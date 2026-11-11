// Spec 085 PR 2b, task 11b: OpenBackupPages and the PageStore over the committed samples
// (test/data/backup, synthetic, SQL Server 2025).
//
// The five forms of one full backup -- plain, MS_XPRESS, ZSTD, striped in two, ZSTD striped in two,
// taken one after another with no data changed between them (samples.sql) -- must have the same live
// pages, of the same types: each read through the PageStore with its header checked (it names
// itself). Not the same bytes: the plain one is the only non-copy-only full, which rewrites the boot
// page (its differential base) and clears the DCM pages. The allocation pages sit where the format puts them (file
// header 0, PFS 1, GAM 2, the boot page 1:9). A page that is not live is not read; a differential
// and a log backup are refused by name.

#include <functional>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "backup/backup_opened.hpp"
#include "duckdb.hpp"

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

//! Every live page of every data file, as "file:page type" in page order.
struct LivePages {
	std::vector<std::string> pages;
	std::map<uint8_t, size_t> by_type;

	size_t Count(uint8_t type) const {
		auto it = by_type.find(type);
		return it == by_type.end() ? 0 : it->second;
	}
};

LivePages ReadAllLive(ClientContext &context, const std::vector<std::string> &files) {
	std::vector<std::string> paths;
	for (const auto &f : files) {
		paths.push_back(DIR + f);
	}
	auto headers = OpenBackupHeaders(context, paths);
	auto pages = OpenBackupPages(context, *headers);
	LivePages live;
	Page page;
	for (const auto &file : headers->Set().files) {
		if (file.type != 'D') {
			continue;
		}
		const auto id = static_cast<uint16_t>(file.file_id);
		const uint32_t size = pages->allocation->FileSize(id);
		for (uint32_t p = 0; p < size; p++) {
			if (pages->store->Read(id, p, page)) {
				live.pages.push_back(std::to_string(id) + ":" + std::to_string(p) + " " +
									 std::to_string(page.header.type));
				live.by_type[page.header.type]++;
			}
		}
	}
	return live;
}

std::string ErrorOf(const std::function<void()> &f) {
	try {
		f();
	} catch (std::exception &e) {
		return e.what();
	}
	return std::string();
}

}  // namespace

int main() {
	std::cout << "OpenBackupPages and the PageStore (spec 085 task 11b)" << std::endl;
	DuckDB db(nullptr);
	Connection con(db);
	auto &context = *con.context;

	const std::vector<std::pair<std::string, std::vector<std::string>>> forms = {
		{"plain", {"full.bak"}},
		{"MS_XPRESS", {"full_xpress.bak"}},
		{"ZSTD", {"full_zstd.bak"}},
		{"striped", {"stripe_1.bak", "stripe_2.bak"}},
		{"ZSTD striped", {"stripe_zstd_1.bak", "stripe_zstd_2.bak"}},
	};
	std::map<std::string, LivePages> live;
	for (const auto &form : forms) {
		std::string error = ErrorOf([&]() { live[form.first] = ReadAllLive(context, form.second); });
		Check(error.empty(),
			  form.first + ": every live page read, its header checked" + (error.empty() ? "" : " -- " + error));
	}

	const auto &plain = live["plain"];
	Check(plain.pages.size() > 100, "the plain full has live pages (" + std::to_string(plain.pages.size()) + ")");
	for (const auto &form : forms) {
		if (form.first != "plain") {
			Check(live[form.first].pages == plain.pages,
				  form.first + " has the plain full's live pages, page for page, of the same types (" +
					  std::to_string(live[form.first].pages.size()) + ")");
		}
	}

	auto has = [&](const std::string &page_and_type) {
		for (const auto &p : plain.pages) {
			if (p == page_and_type) {
				return true;
			}
		}
		return false;
	};
	Check(has("1:0 15"), "1:0 is the file header page");
	Check(has("1:1 11"), "1:1 is a PFS page");
	Check(has("1:2 8"), "1:2 is a GAM page");
	Check(has("1:9 13"), "1:9 is the boot page");
	Check(plain.Count(1) > 0, "there are data pages (" + std::to_string(plain.Count(1)) + ")");
	Check(plain.Count(10) > 0, "there are IAM pages (" + std::to_string(plain.Count(10)) + ")");

	{
		auto headers = OpenBackupHeaders(context, {DIR + "full.bak"});
		auto pages = OpenBackupPages(context, *headers);
		// The last page of file 1 is past every allocated extent in a database this small.
		const uint32_t last = pages->allocation->FileSize(1) - 1;
		Page page;
		Check(!pages->allocation->IsLive(1, last) && !pages->store->Read(1, last, page),
			  "a page that is not live (1:" + std::to_string(last) + ") is not read");
		std::string error = ErrorOf([&]() { pages->store->ReadLive(1, last, page, "the test"); });
		Check(error.find("not live") != std::string::npos, "ReadLive names a page that is not live: " + error);
	}

	std::string diff = ErrorOf([&]() {
		auto headers = OpenBackupHeaders(context, {DIR + "diff.bak"});
		OpenBackupPages(context, *headers);
	});
	Check(diff.find("differential") != std::string::npos, "a differential alone is refused: " + diff);
	std::string log = ErrorOf([&]() {
		auto headers = OpenBackupHeaders(context, {DIR + "log.trn"});
		OpenBackupPages(context, *headers);
	});
	Check(log.find("log backup") != std::string::npos, "a log backup is refused: " + log);

	std::cout << (failures ? "FAILED" : "PASSED") << " (" << failures << " failure(s))" << std::endl;
	return failures ? 1 : 0;
}
