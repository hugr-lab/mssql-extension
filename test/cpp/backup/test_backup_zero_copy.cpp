// Spec 085 PR 5 (review of #591): a live page whose copy is no formatted page, in a backup of one data
// pass -- SQL Server 2017 under writes copies a page taken into use while it ran as a zero unit, or caught
// being formatted (7 bytes set, the rest zeros, measured in the oracle lane).
//
// The committed full.bak (synthetic) is copied to a temporary file with one of dbo.events' data pages
// replaced. Every committed sample has two data passes (2019 and later write one), so beside the later
// pass:
// - all zeros, and the 2017 form (header version 0, 7 bytes set, its own id): a Pending page
//   (IsPendingCopy). The page store throws a PendingPageError, ReadUnitDataPage keeps that type, and the
//   scan reads the table's other rows under fuzzy 'warn' and refuses the table, naming the page, under
//   'error';
// - the 2017 form naming another page: a misplaced copy, a plain FormatError, refused as not a page with
//   the tail never asked (review of #591).
// A one-pass backup (2017's) makes the same copy a ZeroCopyError, which the scan asks the tail about: the
// 2017 oracle lane's, as no sample has one pass. What keeps it one through ReadUnitDataPage -- a150a887's
// check shipped inert without it -- is FormatError::AddContext, rethrowing the error itself: checked here
// over a ZeroCopyError, with FormattedInTail over synthetic records (one before LastLSN does, one past it
// does not).

#include <algorithm>
#include <chrono>
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

	// One directory per run: two runs at once must not write each other's fixture (review of #591).
	const auto dir =
		std::filesystem::temp_directory_path() /
		("mssql_zero_copy_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	std::filesystem::create_directories(dir);
	struct Form {
		const char *what;
		std::vector<uint8_t> unit;
	};
	// The 2017 copy (oracle lane, page 1:44592): header version 0, 7 bytes set -- byte 5 of the flags, the
	// page's own id (page u32 at 32, file u16 at 36) and an LSN of 1/1/1 (at 40 / 44 / 48) -- built here for
	// the page under test. The same bytes naming another page are a misplaced copy, refused (review of #591).
	auto unformatted = [&](const PagePointer &names) {
		std::vector<uint8_t> unit(PAGE_SIZE, 0);
		unit[5] = 0x02;
		for (int k = 0; k < 4; k++) {
			unit[32 + k] = static_cast<uint8_t>(names.page >> (8 * k));
		}
		unit[36] = static_cast<uint8_t>(names.file);
		unit[37] = static_cast<uint8_t>(names.file >> 8);
		unit[40] = unit[44] = unit[48] = 0x01;
		return unit;
	};
	const std::vector<uint8_t> partial = unformatted(at);
	PagePointer other = at;
	other.page += 1;
	const std::vector<uint8_t> misplaced = unformatted(other);
	const std::string path = (dir / "full_zero_copy.bak").string();
	con.Query("SET mssql_backup_cache = false");
	const int64_t rows = [&] {
		auto r = con.Query("SELECT count(*) FROM mssql_backup_scan('" + SAMPLE + "', 'dbo', 'events')");
		auto c = r->HasError() ? nullptr : r->Fetch();
		return c ? c->GetValue(0, 0).GetValue<int64_t>() : int64_t(-1);
	}();
	const int64_t on_page = static_cast<int64_t>(page.header.slot_count);
	for (const Form &form : {Form{"all zeros", std::vector<uint8_t>(PAGE_SIZE, 0)}, Form{"7 bytes set", partial},
							 Form{"7 bytes set naming another page", misplaced}}) {
		auto bytes = file;
		std::copy(form.unit.begin(), form.unit.end(), bytes.begin() + static_cast<std::ptrdiff_t>(offset));
		WriteFile(path, bytes);
		const Thrown t = ReadPage(context, path, at);
		// Beside a later pass a copy of version 0 that is all zeros or names itself is Pending (IsPendingCopy);
		// one naming another page is a misplaced copy.
		const bool elsewhere = form.unit == misplaced;
		const bool zeros = !elsewhere;
		const std::string kind = elsewhere ? "format" : "pending";
		Check(t.store == kind, std::string(form.what) + ": the page store throws " + kind + " (" + t.store + ")");
		Check(t.unit == kind && t.message.find("(reading dbo.events)") != std::string::npos,
			  std::string(form.what) + ": ReadUnitDataPage keeps the type, naming the table (" + t.unit + ")");
		auto scan = con.Query("SELECT count(*) FROM mssql_backup_scan('" + path + "', 'dbo', 'events')");
		const std::string error = scan->HasError() ? scan->GetError() : "";
		if (zeros) {
			auto c = error.empty() ? scan->Fetch() : nullptr;
			const int64_t read = c ? c->GetValue(0, 0).GetValue<int64_t>() : -1;
			Check(read >= 0 && read < rows && read >= rows - on_page,
				  std::string(form.what) + ": the scan reads the table's other pages under 'warn' (" +
					  std::to_string(read) + " of " + std::to_string(rows) + " rows, the page held at most " +
					  std::to_string(on_page) + ")" + (error.empty() ? "" : ": " + error));
			auto refused =
				con.Query("SELECT count(*) FROM mssql_backup_scan('" + path + "', 'dbo', 'events', fuzzy := 'error')");
			const std::string why = refused->HasError() ? refused->GetError() : "";
			Check(why.find("became live while the backup ran") != std::string::npos &&
					  why.find(at.ToString()) != std::string::npos,
				  std::string(form.what) + ": fuzzy := 'error' refuses the table, naming the page: " + why);
		} else {
			Check(error.find("its copy is not a page") != std::string::npos &&
					  error.find("formats no such page") == std::string::npos,
				  "naming another page: a misplaced copy, refused as not a page, the tail never asked: " + error);
		}
	}
	std::filesystem::remove_all(dir);

	std::cout << "AddContext keeps the error's type" << std::endl;
	{
		std::string kept;
		try {
			try {
				PagePointer p;
				p.file = 1;
				p.page = 44592;
				throw ZeroCopyError("backup: page 1:44592 is live, and its copy is not a page", p);
			} catch (FormatError &e) {
				e.AddContext(" (reading dbo.events)");
				throw;
			}
		} catch (ZeroCopyError &e) {
			kept = std::string(e.what()) + " @" + e.page.ToString();
		} catch (FormatError &) {
			kept = "a FormatError";
		}
		Check(kept == "backup: page 1:44592 is live, and its copy is not a page (reading dbo.events) @1:44592",
			  "a ZeroCopyError rethrown with its context is still one, its page kept: " + kept);
	}

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
