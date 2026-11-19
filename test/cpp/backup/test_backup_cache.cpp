// Spec 085 D9, task 22: the opened-file cache over the committed samples (test/data/backup).
//
// A second call on the same files takes the first's OpenedBackup from the DuckDB instance's
// ObjectCache and reads none of what it holds again; a striped set named in another order is the
// same key; a file whose version tag changed is a new key; mssql_backup_cache off opens anew; another
// DuckDB instance has its own cache. The shared state is read only through a statement's files bound
// to the thread: outside them a read is an internal error, never a read through a dead context. And
// statements on several connections read one cached backup at once.

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "backup/backup_opened.hpp"
#include "backup/backup_scan_function.hpp"
#include "catalog/backup/mssql_backup_catalog.hpp"
#include "duckdb.hpp"
#include "duckdb/transaction/transaction.hpp"
#include "mssql_extension.hpp"

using namespace duckdb;
using namespace duckdb::mssql::backup;

namespace {

int failures = 0;

void Check(bool ok, const std::string &what) {
	std::cout << (ok ? "  ok   " : "  FAIL ") << what << std::endl;
	if (!ok) {
		failures++;
	}
}

const std::string DIR = "test/data/backup/";

//! The fetches a statement's files made.
uint64_t Fetches(const OpenedCatalog &catalog) {
	uint64_t n = 0;
	const auto &files = *catalog.opened.files;
	for (size_t i = 0; i < files.Source().FileCount(); i++) {
		n += files.Access(i).Fetches();
	}
	return n;
}

int64_t ScalarInt(Connection &con, const std::string &sql) {
	auto result = con.Query(sql);
	if (result->HasError()) {
		std::cout << "  query failed: " << result->GetError() << std::endl;
		return -1;
	}
	auto chunk = result->Fetch();
	return chunk && chunk->size() == 1 ? chunk->GetValue(0, 0).GetValue<int64_t>() : -1;
}

}  // namespace

int main() {
	std::cout << "test_backup_cache (spec 085 D9, task 22)" << std::endl;
	DuckDB db(nullptr);
	db.LoadStaticExtension<MssqlExtension>();
	Connection con(db);
	auto &context = *con.context;

	{
		auto first = OpenBackupCatalog(context, {DIR + "full.bak"});
		auto second = OpenBackupCatalog(context, {DIR + "full.bak"});
		Check(first->opened.backup.get() == second->opened.backup.get(),
			  "a second open of the same file takes the first's OpenedBackup");
		Check(!first->opened.cache_key.empty() && first->opened.cache_key == second->opened.cache_key,
			  "the two opens have one cache key");
		Check(first->pages == second->pages && first->tables == second->tables,
			  "and its pages and base tables: built once, not again");
		Check(Fetches(*second) < Fetches(*first),
			  "the second open reads less: the headers, pages and base tables are not read again (" +
				  std::to_string(Fetches(*first)) + " fetches, then " + std::to_string(Fetches(*second)) + ")");
		Check(first->Model().tables.size() == second->Model().tables.size() && !second->Model().tables.empty(),
			  "the second open's model lists the same tables");
		Check(first->opened.files.get() != second->opened.files.get(), "each open has its own files");
	}

	{
		auto a = OpenBackupCatalog(context, {DIR + "stripe_1.bak", DIR + "stripe_2.bak"});
		auto b = OpenBackupCatalog(context, {DIR + "stripe_2.bak", DIR + "stripe_1.bak"});
		Check(a->opened.backup.get() == b->opened.backup.get(),
			  "a striped set named in another order is the same backup");
		BindFiles bound(b->Binding());
		bool read = false;
		try {
			Page boot;
			b->pages->store->ReadLive(1, 9, boot, "the boot page");
			read = true;
		} catch (std::exception &e) {
			std::cout << "  " << e.what() << std::endl;
		}
		Check(read, "and reads through the second statement's files, mapped by path");
	}

	{
		auto cached = OpenBackupCatalog(context, {DIR + "full.bak"});
		bool refused = false;
		try {
			Page boot;
			cached->pages->store->ReadLive(1, 9, boot, "the boot page");
		} catch (InternalException &e) {
			refused = std::string(e.what()).find("outside the statement") != std::string::npos;
		}
		Check(refused, "a read of a cached backup with no statement files bound is an internal error");
	}

	{
		con.Query("SET mssql_backup_cache = false");
		auto a = OpenBackupCatalog(context, {DIR + "full.bak"});
		auto b = OpenBackupCatalog(context, {DIR + "full.bak"});
		Check(a->opened.cache_key.empty() && a->opened.backup.get() != b->opened.backup.get(),
			  "mssql_backup_cache off: every open is its own");
		con.Query("RESET mssql_backup_cache");
	}

	{
		// A copy whose modification time moves: the same bytes, another version tag, another key.
		const auto dir = std::filesystem::temp_directory_path() /
						 ("mssql_backup_cache_" + std::to_string(reinterpret_cast<uintptr_t>(&context)));
		std::filesystem::create_directories(dir);
		const auto copy = dir / "full.bak";
		std::filesystem::copy_file(DIR + "full.bak", copy, std::filesystem::copy_options::overwrite_existing);
		auto before = OpenBackupCatalog(context, {copy.string()});
		std::filesystem::last_write_time(copy, std::filesystem::last_write_time(copy) + std::chrono::seconds(10));
		auto after = OpenBackupCatalog(context, {copy.string()});
		Check(before->opened.backup.get() != after->opened.backup.get() &&
				  before->opened.cache_key != after->opened.cache_key,
			  "a file whose version tag changed is opened anew, never trusted");
		Check(after->Model().tables.size() == before->Model().tables.size(), "and reads as before");
		std::filesystem::remove_all(dir);
	}

	{
		DuckDB other_db(nullptr);
		other_db.LoadStaticExtension<MssqlExtension>();
		Connection other(other_db);
		auto here = OpenBackupCatalog(context, {DIR + "full.bak"});
		auto there = OpenBackupCatalog(*other.context, {DIR + "full.bak"});
		Check(here->opened.backup.get() != there->opened.backup.get(),
			  "another DuckDB instance in the process has its own cache");
	}

	{
		const std::string count = "SELECT count(*) FROM mssql_backup_scan('" + DIR + "full_zstd.bak', 'dbo', 'items')";
		const int64_t expected = ScalarInt(con, count);
		Check(expected > 0, "the scan reads the sample's rows (" + std::to_string(expected) + ")");
		std::atomic<int> wrong{0};
		std::vector<std::thread> threads;
		for (int t = 0; t < 4; t++) {
			threads.emplace_back([&]() {
				Connection mine(db);
				for (int i = 0; i < 10; i++) {
					if (ScalarInt(mine, count) != expected) {
						wrong++;
					}
				}
			});
		}
		for (auto &t : threads) {
			t.join();
		}
		Check(wrong.load() == 0, "four connections scanning one cached compressed backup at once read every row");
	}

	{
		// A cold cache raced: four connections of a new instance open and scan one backup at once; one
		// builds its stages, the others wait for it or build their own entry, and every count is whole.
		DuckDB cold_db(nullptr);
		cold_db.LoadStaticExtension<MssqlExtension>();
		std::atomic<int> ready{0};
		std::vector<int64_t> counts(4, 0);
		std::vector<std::thread> threads;
		for (int t = 0; t < 4; t++) {
			threads.emplace_back([&, t]() {
				Connection mine(cold_db);
				ready++;
				while (ready.load() < 4) {
				}
				counts[t] = ScalarInt(
					mine, "SELECT count(*) FROM mssql_backup_scan('" + DIR + "stripe_zstd_*.bak', 'dbo', 'items')");
			});
		}
		for (auto &t : threads) {
			t.join();
		}
		bool same = counts[0] > 0;
		for (auto c : counts) {
			same = same && c == counts[0];
		}
		Check(same, "four connections of a cold instance open one striped compressed backup at once and agree (" +
						std::to_string(counts[0]) + " rows each)");
	}

	{
		// The info function's headers and a catalog call share the entry.
		auto info = OpenBackup(context, {DIR + "full_xpress.bak"});
		auto catalog = OpenBackupCatalog(context, {DIR + "full_xpress.bak"});
		Check(info.backup.get() == catalog->opened.backup.get(),
			  "mssql_backup_info's open and a catalog's share one entry");
	}

	{
		// Task 23: an attached backup's name is the backup; every statement takes the model its ATTACH
		// built and the page lists its scans walked, and opens its own files.
		con.Query("ATTACH '" + DIR + "full.bak' AS bk (TYPE mssql_backup)");
		auto a = OpenBackupCatalog(context, {"bk"});
		auto b = OpenBackupCatalog(context, {"bk"});
		Check(a->opened.backup.get() == b->opened.backup.get() && a->model.get() == b->model.get() && a->unit_pages &&
				  a->unit_pages == b->unit_pages,
			  "two statements on an attached backup share its opened backup, model and page lists");
		Check(a->opened.files.get() != b->opened.files.get(), "and open their own files");
		const CatalogTable *items = a->Model().FindTable("dbo", "items");
		uint64_t in_row = 0;
		if (items) {
			for (const auto &u : items->units) {
				if (u.unit.type == 1) {
					in_row = u.unit.auid;
				}
			}
		}
		Check(ScalarInt(con, "SELECT count(*) FROM bk.dbo.items") == 2000, "a scan of the attached table");
		bool walked = false;
		auto pages = a->unit_pages->Get(in_row, [&walked]() {
			walked = true;
			return std::vector<PagePointer>();
		});
		Check(in_row != 0 && !walked && pages && !pages->empty(),
			  "its in-row unit's pages are kept: a later scan walks no IAM chain");
		// Pinned by the catalog, whatever the cache setting: an ATTACH made with it off is opened once.
		con.Query("SET mssql_backup_cache = false");
		con.Query("ATTACH '" + DIR + "full_xpress.bak' AS bx (TYPE mssql_backup)");
		auto c = OpenBackupCatalog(context, {"bx"});
		auto d = OpenBackupCatalog(context, {"bx"});
		Check(c->opened.backup.get() == d->opened.backup.get(), "with mssql_backup_cache off, the ATTACH's backup");
		con.Query("SET mssql_backup_cache = true");
		con.Query("DETACH bk");
		con.Query("DETACH bx");
	}

	{
		// A case-sensitive database's columns that differ only in case are read under a suffix no column of
		// the table has, never refused; spaces and quotes stay as they are.
		CatalogTable t;
		t.schema_name = "dbo";
		t.name = "cs";
		const char *names[] = {"Id", "ID", "id", "my col", "Other \"Col\"", "ID_1"};
		int32_t id = 1;
		for (const char *n : names) {
			CatalogColumn c;
			c.column_id = id++;
			c.name = n;
			c.system_type_id = 56;	// int
			c.type_name = c.system_type_name = "int";
			c.max_length = 4;
			t.columns.push_back(c);
		}
		AssignDuckDBNames(t.columns);
		const auto columns = ScanColumnsOf(t);
		const std::vector<std::string> want = {"Id", "ID_2", "id_3", "my col", "Other \"Col\"", "ID_1"};
		Check(std::vector<std::string>(columns.names.begin(), columns.names.end()) == want,
			  "columns that differ only in case read as Id, ID_2, id_3: a real column ID_1 keeps its name");
	}

	{
		// Task 24: a differential over its full is its own entry, keyed by both backups' files, whose
		// base is the full's entry; a second statement takes it and reads nothing it holds again.
		auto full = OpenBackupCatalog(context, {DIR + "full.bak"});
		auto a = OpenBackupCatalog(context, {DIR + "full.bak"}, true, CatalogScope(), {DIR + "diff.bak"});
		auto b = OpenBackupCatalog(context, {DIR + "full.bak"}, true, CatalogScope(), {DIR + "diff.bak"});
		Check(a->opened.backup.get() == b->opened.backup.get() && !a->opened.cache_key.empty(),
			  "a differential over its full is cached, keyed by both backups' files");
		Check(a->opened.backup->Base().get() == full->opened.backup.get(),
			  "and its base is the full's entry, not a second open of the full");
		Check(a->opened.backup.get() != full->opened.backup.get() && a->pages != full->pages,
			  "the overlay's pages are its own: the differential's index over the full's");
		const CatalogTable *items = a->Model().FindTable("dbo", "items");
		Check(items && items->row_count == 2500, "the overlay's catalog counts the differential's rows (2500 items)");
		Check(Fetches(*b) < Fetches(*a) || Fetches(*b) == 0, "a second open of the overlay reads less");
		// The full's entry evicted (each entry goes on its own) and opened anew while the overlay's stays:
		// the overlay still reads through its own base, bound to the statement's files of the full.
		ObjectCache::GetObjectCache(context).Delete(full->opened.cache_key);
		auto again = OpenBackupCatalog(context, {DIR + "full.bak"});
		Check(again->opened.backup.get() != full->opened.backup.get(), "the full opened anew after its eviction");
		const int64_t rows = ScalarInt(con, "SELECT count(*) FROM mssql_backup_scan('" + DIR +
												"full.bak', 'dbo', 'events', diff := '" + DIR + "diff.bak')");
		Check(rows == 2572, "and the cached overlay over its old base still reads (" + std::to_string(rows) + " rows)");
		auto entry = ObjectCache::GetObjectCache(context).Get<OpenedBackup>(full->opened.cache_key);
		Check(entry.get() == a->opened.backup->Base().get(),
			  "and its base is the full's entry again: one copy of the full, counted (review of #575)");
	}

	{
		// Task 24b: a history's folder grows; mssql_backup_refresh takes the new backups in and keeps a
		// version read before by its backups and files, not by its number.
		const auto dir = std::filesystem::temp_directory_path() /
						 ("mssql_backup_history_" + std::to_string(reinterpret_cast<uintptr_t>(&db)));
		std::filesystem::remove_all(dir);
		std::filesystem::create_directories(dir);
		std::filesystem::copy_file(DIR + "full.bak", dir / "a_full.bak");
		std::filesystem::copy_file(DIR + "diff.bak", dir / "b_diff.bak");
		std::string glob;
		for (const char ch : (dir / "*.bak").string()) {
			glob += ch;
			if (ch == '\'') {
				glob += ch;	 // inside a SQL literal
			}
		}
		auto attach = con.Query("ATTACH '" + glob + "' AS hr (TYPE mssql_backup, history true)");
		Check(!attach->HasError(),
			  "a history of a full and its differential attached " + (attach->HasError() ? attach->GetError() : ""));
		auto pinned = con.Query("ATTACH '" + glob + "' AS hp (TYPE mssql_backup, history true, snapshot_version 1)");
		Check(!pinned->HasError(), "the same, snapshot_version 1");
		Check(ScalarInt(con, "SELECT count(*) FROM hr.items") == 2500, "the differential's rows");
		auto diff_before = OpenBackupCatalog(context, {"hr"});
		Check(ScalarInt(con, "SELECT versions_before * 10 + versions FROM mssql_backup_refresh('hr')") == 22,
			  "a refresh with nothing new: 2 versions before and after");
		// A copy-only full that finished between the full and the differential: the differential moves from
		// version 2 to 3 and keeps its state.
		std::filesystem::copy_file(DIR + "full_xpress.bak", dir / "a_copy_only.bak");
		Check(ScalarInt(con, "SELECT versions FROM mssql_backup_refresh('hr')") == 3,
			  "a refresh takes a copy-only full in: 3 versions");
		Check(ScalarInt(con, "SELECT snapshot_id FROM hr.snapshots() WHERE backup_type = 'differential'") == 3,
			  "the differential, finished last, is version 3 now");
		auto diff_after = OpenBackupCatalog(context, {"hr"});
		Check(diff_after->opened.backup.get() == diff_before->opened.backup.get(),
			  "and keeps the state read as version 2 (the same opened backup)");
		Check(ScalarInt(con, "SELECT count(*) FROM hr.items AT (VERSION => 1)") == 2000,
			  "version 1, the full, still reads");
		// snapshot_version 1 pins the full by its backup, wherever it lands.
		Check(ScalarInt(con, "SELECT versions FROM mssql_backup_refresh('hp')") == 3, "the pinned history refreshed");
		Check(ScalarInt(con, "SELECT snapshot_id FROM hp.snapshots() WHERE is_default") == 1 &&
				  ScalarInt(con, "SELECT count(*) FROM hp.items") == 2000,
			  "snapshot_version 1 still the full (2000 items)");
		// A file read before, touched: its version is a new state, opened and checked against the refresh.
		std::filesystem::last_write_time(
			dir / "a_full.bak", std::filesystem::last_write_time(dir / "a_full.bak") + std::chrono::seconds(10));
		auto stale = con.Query("SELECT count(*) FROM hr.items AT (VERSION => 1)");
		Check(stale->HasError() && stale->GetError().find("changed since it was attached") != std::string::npos,
			  "a file touched since the refresh refused by name");
		Check(ScalarInt(con, "SELECT versions FROM mssql_backup_refresh('hr')") == 3, "refreshed again");
		Check(ScalarInt(con, "SELECT count(*) FROM hr.items AT (VERSION => 1)") == 2000 &&
				  ScalarInt(con, "SELECT count(*) FROM hr.items") == 2500,
			  "and the touched full reads again, alone and under the differential");
		// A file renamed: the refresh follows it.
		std::filesystem::rename(dir / "b_diff.bak", dir / "c_diff.bak");
		Check(ScalarInt(con, "SELECT versions FROM mssql_backup_refresh('hr')") == 3 &&
				  ScalarInt(con, "SELECT count(*) FROM hr.items") == 2500,
			  "a renamed differential read at its new path after a refresh");
		// A version is resolved when a query is bound, to its backups and files, and the query keeps them to its
		// end; a transaction keeps no versions, so its next statement reads a refresh made meanwhile (owner,
		// 2026-11-19).
		{
			Connection other(db);
			// Submitted, so bound and streaming, before the refresh; read after it.
			auto streaming = con.Submit("SELECT id FROM hr.items AT (VERSION => 3)");
			Check(!streaming->HasError(), "a query of version 3, the differential, bound");
			std::filesystem::copy_file(DIR + "full_zstd.bak", dir / "a_copy_only_2.bak");
			Check(ScalarInt(other, "SELECT versions FROM mssql_backup_refresh('hr')") == 4,
				  "another connection refreshes: 4 versions, the differential now version 4");
			int64_t streamed = 0;
			while (!streaming->HasError()) {
				auto chunk = streaming->Fetch();
				if (!chunk || chunk->size() == 0) {
					break;
				}
				streamed += static_cast<int64_t>(chunk->size());
			}
			Check(!streaming->HasError() && streamed == 2500,
				  "the query bound before the refresh reads the version its bind named (" + std::to_string(streamed) +
					  " items)");
			streaming.reset();
			Check(ScalarInt(con, "SELECT count(*) FROM hr.items AT (VERSION => 3)") == 2000 &&
					  ScalarInt(con, "SELECT count(*) FROM mssql_backup_scan('hr', 'dbo', 'items', version := 3)") ==
						  2000,
				  "the next query's version 3 is the new copy-only full, through the catalog and the file functions");
			con.Query("BEGIN");
			Check(ScalarInt(con, "SELECT count(*) FROM hr.snapshots()") == 4, "a transaction reads 4 versions");
			std::filesystem::copy_file(DIR + "stripe_1.bak", dir / "a_stripe_1.bak");
			std::filesystem::copy_file(DIR + "stripe_2.bak", dir / "a_stripe_2.bak");
			Check(ScalarInt(other, "SELECT versions FROM mssql_backup_refresh('hr')") == 5,
				  "another connection refreshes: 5 versions (a striped full)");
			Check(ScalarInt(con, "SELECT count(*) FROM hr.snapshots()") == 5 &&
					  ScalarInt(con, "SELECT count(*) FROM mssql_backup_snapshots('hr')") == 5,
				  "the transaction's next statement reads 5");
			con.Query("COMMIT");
			// A file read before, touched and refreshed: the next query opens its version anew.
			Check(ScalarInt(con, "SELECT count(*) FROM hr.items AT (VERSION => 1)") == 2000, "version 1, the full");
			std::filesystem::last_write_time(
				dir / "a_full.bak", std::filesystem::last_write_time(dir / "a_full.bak") + std::chrono::seconds(20));
			Check(ScalarInt(other, "SELECT versions FROM mssql_backup_refresh('hr')") == 5,
				  "another connection refreshes past the touched full");
			Check(ScalarInt(con, "SELECT count(*) FROM hr.items AT (VERSION => 1)") == 2000,
				  "the next query reads the touched full as refreshed");
			// A query bound on a version that a refresh then drops (its full touched: a new key) reads it to its
			// end, its state held by the query.
			auto held = con.Submit("SELECT id FROM hr.items AT (VERSION => 1)");
			Check(!held->HasError(), "a query of version 1 bound");
			std::filesystem::last_write_time(
				dir / "a_full.bak", std::filesystem::last_write_time(dir / "a_full.bak") + std::chrono::seconds(20));
			Check(ScalarInt(other, "SELECT versions FROM mssql_backup_refresh('hr')") == 5,
				  "another connection refreshes: version 1's key dropped");
			int64_t held_rows = 0;
			while (!held->HasError()) {
				auto chunk = held->Fetch();
				if (!chunk || chunk->size() == 0) {
					break;
				}
				held_rows += static_cast<int64_t>(chunk->size());
			}
			Check(!held->HasError() && held_rows == 2000, "the dropped version read to the query's end (" +
															  std::to_string(held_rows) + " items)" +
															  (held->HasError() ? held->GetError() : ""));
			held.reset();
			// A file function on the name pins its version at bind, by its key (PinAttachedVersion): what its init
			// and a deserialized plan find the version by. The differential's number pinned, an earlier backup
			// then gone (the numbers move down), the key still finds the differential.
			const int64_t diff_number =
				ScalarInt(con, "SELECT snapshot_id FROM hr.snapshots() WHERE backup_type = 'differential'");
			BackupAt pinned;
			pinned.unit = "version";
			pinned.value = Value::BIGINT(diff_number);
			PinAttachedVersion(context, "hr", pinned);
			const auto diff_backup = FindAttachedBackup(context, "hr", pinned);
			Check(!pinned.key.empty() && diff_backup && diff_backup->backup->Base(),
				  "the differential's number pinned to its key");
			std::filesystem::rename(dir / "a_copy_only.bak", dir / "a_copy_only.bak.away");
			Check(ScalarInt(other, "SELECT versions FROM mssql_backup_refresh('hr')") == 4,
				  "another connection refreshes without a copy-only full: 4 versions");
			Check(ScalarInt(con, "SELECT snapshot_id FROM hr.snapshots() WHERE backup_type = 'differential'") ==
					  diff_number - 1,
				  "the differential is one lower now");
			const auto again = FindAttachedBackup(context, "hr", pinned);
			Check(again && again.get() == diff_backup.get(), "the pinned key finds the differential at its new number");
			// A key no version has and no one holds: refused by name.
			BackupAt gone = pinned;
			gone.key += "/gone";
			bool refused_gone = false;
			try {
				FindAttachedBackup(context, "hr", gone);
			} catch (std::exception &e) {
				refused_gone = std::string(e.what()).find("run the query again") != std::string::npos;
			}
			Check(refused_gone, "a pinned version no longer among the files refused by name");
			// Two versions of one table in one query are two scans (their keys differ), not one merged.
			Check(ScalarInt(con,
							"SELECT count(*) FROM (SELECT id FROM hr.items AT (VERSION => 1) UNION ALL "
							"SELECT id FROM hr.items AT (VERSION => " +
								std::to_string(diff_number - 1) + "))") == 4500,
				  "a UNION ALL of the full and the differential: 2000 + 2500");
			std::filesystem::rename(dir / "a_copy_only.bak.away", dir / "a_copy_only.bak");
			Check(ScalarInt(other, "SELECT versions FROM mssql_backup_refresh('hr')") == 5,
				  "the full back: 5 versions");
			// The default version read, dropped from the catalog by another connection's refresh (its files gone)
			// and back since, is the default again after a refresh: the catalog holds its state (re-review of
			// #579: the refresh found it among the refreshing side's own states and left the catalog's without it,
			// so a lookup with no AT -- the database size, the default schema -- threw).
			con.Query("BEGIN");
			const int64_t default_rows = ScalarInt(con, "SELECT count(*) FROM hr.items");
			auto files = con.Query(
				"SELECT unnest(CASE WHEN len(diff_files) > 0 THEN diff_files ELSE full_files END) "
				"FROM hr.snapshots() WHERE is_default");
			std::vector<std::string> moved;
			while (!files->HasError()) {
				auto chunk = files->Fetch();
				if (!chunk || chunk->size() == 0) {
					break;
				}
				for (idx_t r = 0; r < chunk->size(); r++) {
					moved.push_back(chunk->GetValue(0, r).ToString());
				}
			}
			Check(!moved.empty(), "the default version's files");
			for (const auto &f : moved) {
				std::filesystem::rename(f, f + ".away");
			}
			auto fewer = other.Query("SELECT versions FROM mssql_backup_refresh('hr')");
			Check(!fewer->HasError(), "another connection refreshes without the default's files");
			for (const auto &f : moved) {
				std::filesystem::rename(f + ".away", f);
			}
			Check(ScalarInt(con, "SELECT versions FROM mssql_backup_refresh('hr')") == 5,
				  "the transaction refreshes with them back: 5 versions");
			auto size = other.Query("SELECT count(*) FROM pragma_database_size() WHERE database_name = 'hr'");
			Check(!size->HasError() && ScalarInt(other, "SELECT count(*) FROM hr.items") == default_rows,
				  "the default version read with no AT, its size too " + (size->HasError() ? size->GetError() : ""));
			con.Query("COMMIT");
		}
		// A version of several files -- a striped full whose files sort by name against their family order, a
		// full and its differential -- is pinned by all of them: the key the history gives (by family) is the
		// one its opened backup gives, and a file of it touched makes the version a new one.
		{
			const auto many = std::filesystem::temp_directory_path() /
							  ("mssql_backup_many_" + std::to_string(reinterpret_cast<uintptr_t>(&db)));
			std::filesystem::remove_all(many);
			std::filesystem::create_directories(many);
			std::filesystem::copy_file(DIR + "full.bak", many / "a_full.bak");
			std::filesystem::copy_file(DIR + "diff.bak", many / "b_diff.bak");
			std::filesystem::copy_file(DIR + "stripe_2.bak", many / "c_s2.bak");  // family 2 first by name
			std::filesystem::copy_file(DIR + "stripe_1.bak", many / "d_s1.bak");
			auto attached_many =
				con.Query("ATTACH '" + (many / "*.bak").string() + "' AS hm (TYPE mssql_backup, history true)");
			Check(!attached_many->HasError(), "a history with a striped full attached " +
												  (attached_many->HasError() ? attached_many->GetError() : ""));
			const int64_t striped = ScalarInt(
				con, "SELECT snapshot_id FROM hm.snapshots() WHERE len(full_files) = 2 AND backup_type = 'full'");
			const int64_t with_diff =
				ScalarInt(con, "SELECT snapshot_id FROM hm.snapshots() WHERE backup_type = 'differential'");
			auto pin = [&](int64_t number) {
				BackupAt at;
				at.unit = "version";
				at.value = Value::BIGINT(number);
				PinAttachedVersion(context, "hm", at);
				return at;
			};
			const BackupAt striped_at = pin(striped);
			const BackupAt diff_at = pin(with_diff);
			const auto striped_backup = FindAttachedBackup(context, "hm", striped_at);
			const auto diff_backup = FindAttachedBackup(context, "hm", diff_at);
			Check(striped_backup && striped_backup->backup->Headers().families.size() == 2 &&
					  MSSQLBackupCatalog::StateKey(*striped_backup) == striped_at.key,
				  "the striped full's key from the history is its opened backup's (both files, by family)");
			Check(
				diff_backup && diff_backup->backup->Base() && MSSQLBackupCatalog::StateKey(*diff_backup) == diff_at.key,
				"and the full + differential's (the full's file and the differential's)");
			Check(
				ScalarInt(con, "SELECT count(*) FROM hm.items AT (VERSION => " + std::to_string(striped) + ")") == 2000,
				"the striped full read");
			// One stripe touched: the striped version is a new one; the old key, held by no query, is refused.
			std::filesystem::last_write_time(
				many / "d_s1.bak", std::filesystem::last_write_time(many / "d_s1.bak") + std::chrono::seconds(10));
			Check(ScalarInt(con, "SELECT versions FROM mssql_backup_refresh('hm')") == 3, "refreshed: 3 versions");
			bool striped_refused = false;
			try {
				FindAttachedBackup(context, "hm", striped_at);
			} catch (std::exception &e) {
				striped_refused = std::string(e.what()).find("run the query again") != std::string::npos;
			}
			Check(striped_refused, "a stripe touched: the striped version's old key refused by name");
			Check(FindAttachedBackup(context, "hm", diff_at).get() == diff_backup.get(),
				  "the full + differential, none of its files touched, still found by its key");
			Check(
				ScalarInt(con, "SELECT count(*) FROM hm.items AT (VERSION => " + std::to_string(striped) + ")") == 2000,
				"the next query reads the striped full anew");
			// The full touched: the differential's version (the full's file in its key) is a new one too.
			std::filesystem::last_write_time(
				many / "a_full.bak", std::filesystem::last_write_time(many / "a_full.bak") + std::chrono::seconds(10));
			Check(ScalarInt(con, "SELECT versions FROM mssql_backup_refresh('hm')") == 3, "refreshed again");
			bool diff_refused = false;
			try {
				FindAttachedBackup(context, "hm", diff_at);
			} catch (std::exception &e) {
				diff_refused = std::string(e.what()).find("run the query again") != std::string::npos;
			}
			Check(diff_refused, "its full touched: the differential's old key refused by name");
			Check(ScalarInt(con, "SELECT count(*) FROM hm.items AT (VERSION => " + std::to_string(with_diff) + ")") ==
					  2500,
				  "the next query reads the full + differential anew");
			con.Query("DETACH hm");
			std::filesystem::remove_all(many);
		}
		auto refused = con.Query("SELECT * FROM mssql_backup_refresh('nope')");
		Check(refused->HasError() && refused->GetError().find("is not a backup attached") != std::string::npos,
			  "a refresh of no attached backup refused");
		// A file whose name has glob characters, found by a glob, is opened again as itself (review of #579:
		// `s[1].bak` was taken for a glob that matches `s1.bak`).
		{
			const auto odd = std::filesystem::temp_directory_path() /
							 ("mssql_backup_glob_" + std::to_string(reinterpret_cast<uintptr_t>(&db)));
			std::filesystem::remove_all(odd);
			std::filesystem::create_directories(odd);
			std::filesystem::copy_file(DIR + "full.bak", odd / "s[1].bak");
			std::string pattern;
			for (const char ch : (odd / "*.bak").string()) {
				pattern += ch;
				if (ch == '\'') {
					pattern += ch;
				}
			}
			Check(ScalarInt(con, "SELECT count(*) FROM mssql_backup_scan('" + pattern +
									 "', 'dbo', 'items', version := 1)") == 2000,
				  "a history's file named s[1].bak read as itself");
			std::filesystem::remove_all(odd);
		}
		con.Query("DETACH hp");
		con.Query("DETACH hr");
		std::filesystem::remove_all(dir);
	}

	if (failures) {
		std::cout << failures << " check(s) failed" << std::endl;
		return 1;
	}
	std::cout << "all checks passed" << std::endl;
	return 0;
}
