// Spec 085 PR 5 (review of #591): a live page whose copy is no formatted page, in a backup of one data
// pass -- SQL Server 2017 under writes copies a page taken into use while it ran as a zero unit, or caught
// being formatted (7 bytes set, the rest zeros, measured in the oracle lane).
//
// The committed full.bak (synthetic; two data passes, as 2019 and later write one under or after writes)
// is copied to a temporary file with one of dbo.events' data pages replaced:
// - all zeros: with a later pass, a Pending page (IsPendingCopy). The page store throws a
//   PendingPageError, ReadUnitDataPage keeps that type, and the scan reads the table's other rows under
//   fuzzy 'warn' and refuses the table, naming the page, under 'error';
// - the 2017 form (header version 0, 7 bytes set): no page and not Pending by the passes, a ZeroCopyError
//   (FormatError's subclass), which ReadUnitDataPage keeps (a150a887's check shipped inert because it did
//   not), and the scan, whose tail formats no such page, refuses the table saying why.
// Whether the tail formats a page is FormattedInTail, over a summary of synthetic records: one before
// LastLSN does, one past it does not. A one-pass Pending page is the 2017 oracle lane's: the samples'
// tails format no page.

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include "backup/backup_fuzzy.hpp"
#include "backup/backup_opened.hpp"
#include "backup/backup_system_tables.hpp"
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

const std::string SAMPLE = "test/data/backup/full.bak";

std::vector<uint8_t> ReadFile(const std::string &path) {
	std::ifstream in(path, std::ios::binary);
	return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void WriteFile(const std::string &path, const std::vector<uint8_t> &bytes) {
	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

//! What reading `page` of `path` through the page store and then ReadUnitDataPage throws: "zero", "format"
//! or "none", and the last message.
struct Thrown {
	std::string store, unit, message;
};

Thrown ReadPage(ClientContext &context, const std::string &path, const PagePointer &at) {
	Thrown t;
	auto headers = OpenBackupHeaders(context, {path});
	auto pages = OpenBackupPages(context, *headers);
	Page page;
	auto kind = [&](const std::function<void()> &f) -> std::string {
		try {
			f();
		} catch (PendingPageError &e) {
			t.message = e.what();
			return "pending";
		} catch (ZeroCopyError &e) {
			t.message = e.what();
			return "zero";
		} catch (FormatError &e) {
			t.message = e.what();
			return "format";
		}
		return "none";
	};
	t.store = kind([&] { pages->store->Read(at.file, at.page, page); });
	t.unit = kind([&] { ReadUnitDataPage(*pages->store, SysAllocUnit(), at, "dbo.events", page); });
	return t;
}

}  // namespace

int main() {
	std::cout << "a live page copied unformatted in a backup of one data pass" << std::endl;
	DuckDB db(nullptr);
	db.LoadStaticExtension<MssqlExtension>();
	Connection con(db);
	auto &context = *con.context;

	// A data page of dbo.events, and where its copy lies in the file (the plain sample holds pages as they are).
	auto found = con.Query("SELECT min(_mssql_page) FROM mssql_backup_scan('" + SAMPLE + "', 'dbo', 'events')");
	if (found->HasError()) {
		std::cout << "  query failed: " << found->GetError() << std::endl;
		return 1;
	}
	PagePointer at;
	auto chunk = found->Fetch();
	if (!chunk || chunk->size() != 1) {
		std::cout << "  no page of dbo.events" << std::endl;
		return 1;
	}
	// '01', then the file (4 hex digits) and the page (8).
	const std::string text = chunk->GetValue(0, 0).ToString();
	at.file = static_cast<uint16_t>(std::stoul(text.substr(2, 4), nullptr, 16));
	at.page = static_cast<uint32_t>(std::stoul(text.substr(6, 8), nullptr, 16));
	auto headers = OpenBackupHeaders(context, {SAMPLE});
	auto pages = OpenBackupPages(context, *headers);
	Page page;
	Check(pages->store->Read(at.file, at.page, page), "page " + at.ToString() + " of dbo.events is live and read");
	auto file = ReadFile(SAMPLE);
	auto copy = std::search(file.begin(), file.end(), page.bytes.begin(), page.bytes.end());
	Check(copy != file.end() && std::search(copy + 1, file.end(), page.bytes.begin(), page.bytes.end()) == file.end(),
		  "its copy is in the file once");
	if (copy == file.end()) {
		return 1;
	}
	const size_t offset = static_cast<size_t>(copy - file.begin());

	const auto dir = std::filesystem::temp_directory_path() / "mssql_zero_copy_test";
	std::filesystem::create_directories(dir);
	struct Form {
		const char *what;
		std::vector<uint8_t> unit;
	};
	std::vector<uint8_t> partial(PAGE_SIZE, 0);
	// The 2017 copy (oracle lane, page 1:44592): header version 0, 7 bytes set from offset 5.
	const uint8_t set[] = {0x02, 0x30, 0xae, 0x01, 0x01, 0x01, 0x01};
	partial[5] = set[0];
	partial[32] = set[1];
	partial[33] = set[2];
	partial[36] = set[3];
	partial[40] = set[4];
	partial[44] = set[5];
	partial[48] = set[6];
	const std::string path = (dir / "full_zero_copy.bak").string();
	con.Query("SET mssql_backup_cache = false");
	const int64_t rows = [&] {
		auto r = con.Query("SELECT count(*) FROM mssql_backup_scan('" + SAMPLE + "', 'dbo', 'events')");
		auto c = r->HasError() ? nullptr : r->Fetch();
		return c ? c->GetValue(0, 0).GetValue<int64_t>() : int64_t(-1);
	}();
	const int64_t on_page = static_cast<int64_t>(page.header.slot_count);
	for (const Form &form : {Form{"all zeros", std::vector<uint8_t>(PAGE_SIZE, 0)}, Form{"7 bytes set", partial}}) {
		auto bytes = file;
		std::copy(form.unit.begin(), form.unit.end(), bytes.begin() + static_cast<std::ptrdiff_t>(offset));
		WriteFile(path, bytes);
		const Thrown t = ReadPage(context, path, at);
		const bool zeros = form.unit == std::vector<uint8_t>(PAGE_SIZE, 0);
		const std::string kind = zeros ? "pending" : "zero";
		Check(t.store == kind, std::string(form.what) + ": the page store throws " + kind + " (" + t.store + ")");
		Check(t.unit == kind && t.message.find("(reading dbo.events)") != std::string::npos,
			  std::string(form.what) + ": ReadUnitDataPage keeps the type, naming the table (" + t.unit + ")");
		auto scan = con.Query("SELECT count(*) FROM mssql_backup_scan('" + path + "', 'dbo', 'events')");
		const std::string error = scan->HasError() ? scan->GetError() : "";
		if (zeros) {
			auto c = error.empty() ? scan->Fetch() : nullptr;
			const int64_t read = c ? c->GetValue(0, 0).GetValue<int64_t>() : -1;
			Check(read >= 0 && read < rows && read >= rows - on_page,
				  "all zeros: the scan reads the table's other pages under 'warn' (" + std::to_string(read) + " of " +
					  std::to_string(rows) + " rows, the page held at most " + std::to_string(on_page) + ")" +
					  (error.empty() ? "" : ": " + error));
			auto refused =
				con.Query("SELECT count(*) FROM mssql_backup_scan('" + path + "', 'dbo', 'events', fuzzy := 'error')");
			const std::string why = refused->HasError() ? refused->GetError() : "";
			Check(why.find("became live while the backup ran") != std::string::npos &&
					  why.find(at.ToString()) != std::string::npos,
				  "all zeros: fuzzy := 'error' refuses the table, naming the page: " + why);
		} else {
			Check(error.find("formats no such page") != std::string::npos,
				  "7 bytes set: the scan refuses it, the tail formatting no such page: " + error);
		}
	}
	std::filesystem::remove_all(dir);

	std::cout << "FormattedInTail" << std::endl;
	{
		auto record = [](uint32_t block, uint8_t op, uint32_t page) {
			LogRecord r;
			r.lsn.vlf = 1;
			r.lsn.block = block;
			r.lsn.slot = 1;
			r.op = op;
			r.has_page = true;
			r.file = 1;
			r.page = page;
			return r;
		};
		Lsn first, last, checkpoint;
		first.vlf = last.vlf = checkpoint.vlf = 1;
		first.block = checkpoint.block = 1;
		last.block = 9;
		const FuzzySummary tail = SummarizeLogRecords(
			{record(2, log_op::FORMAT_PAGE, 44586), record(9, log_op::FORMAT_PAGE, 44592)}, first, last, checkpoint);
		Check(FormattedInTail(tail, 1, 44586), "a page formatted before LastLSN: taken into use while the backup ran");
		Check(!FormattedInTail(tail, 1, 44592), "one formatted at LastLSN or after: not the backup's");
		Check(!FormattedInTail(tail, 2, 44586), "the same page number in another file: not it");
	}
	std::cout << (failures ? "FAILED" : "all passed") << std::endl;
	return failures ? 1 : 0;
}
