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
#include <string>
#include <thread>
#include <vector>

#include "backup/backup_opened.hpp"
#include "duckdb.hpp"
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
		Check(first->model.tables.size() == second->model.tables.size() && !second->model.tables.empty(),
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
		const auto dir = std::filesystem::temp_directory_path() / ("mssql_backup_cache_" + std::to_string(reinterpret_cast<uintptr_t>(&context)));
		std::filesystem::create_directories(dir);
		const auto copy = dir / "full.bak";
		std::filesystem::copy_file(DIR + "full.bak", copy, std::filesystem::copy_options::overwrite_existing);
		auto before = OpenBackupCatalog(context, {copy.string()});
		std::filesystem::last_write_time(copy, std::filesystem::last_write_time(copy) + std::chrono::seconds(10));
		auto after = OpenBackupCatalog(context, {copy.string()});
		Check(before->opened.backup.get() != after->opened.backup.get() &&
				  before->opened.cache_key != after->opened.cache_key,
			  "a file whose version tag changed is opened anew, never trusted");
		Check(after->model.tables.size() == before->model.tables.size(), "and reads as before");
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
		const std::string count =
			"SELECT count(*) FROM mssql_backup_scan('" + DIR + "full_zstd.bak', 'dbo', 'items')";
		const int64_t expected = ScalarInt(con, count);
		Check(expected > 0, "the scan reads the sample's rows (" + std::to_string(expected) + ")");
		std::atomic<int> wrong {0};
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
		std::atomic<int> ready {0};
		std::vector<int64_t> counts(4, 0);
		std::vector<std::thread> threads;
		for (int t = 0; t < 4; t++) {
			threads.emplace_back([&, t]() {
				Connection mine(cold_db);
				ready++;
				while (ready.load() < 4) {
				}
				counts[t] =
					ScalarInt(mine, "SELECT count(*) FROM mssql_backup_scan('" + DIR + "stripe_zstd_*.bak', 'dbo', 'items')");
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

	if (failures) {
		std::cout << failures << " check(s) failed" << std::endl;
		return 1;
	}
	std::cout << "all checks passed" << std::endl;
	return 0;
}
