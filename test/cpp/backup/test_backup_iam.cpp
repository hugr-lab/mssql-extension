// Spec 085 PR 2b step 4 (task 11a, DQ-40): IAM chains over the committed samples (synthetic, SQL
// Server 2025), checked against themselves -- no catalog needed. Every live IAM page that opens a
// chain (no previous page) is walked with the AU its header names; then every live data,
// index and LOB page (types 1 to 4) and every IAM page must lie in exactly one chain, and name that
// chain's AU in its own header. That pins the IAM layout (header and bitmap records, sequence, start
// page, single-page slots) and the AU of a page header in one check, for every form of the full.
// A chain whose pages disagree is refused by name.

#include <algorithm>
#include <functional>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "backup/backup_iam.hpp"
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

std::string ErrorOf(const std::function<void()> &f) {
	try {
		f();
	} catch (std::exception &e) {
		return e.what();
	}
	return std::string();
}

const std::string DIR = "test/data/backup/";

struct Coverage {
	size_t chains = 0;
	size_t pages = 0;  // live pages of types 1-4 and 10
	size_t covered_once = 0;
	size_t uncovered = 0;
	size_t twice = 0;
	size_t wrong_au = 0;
	size_t pending = 0;	 // live, copied as a zero unit (became live while the backup ran)
	size_t longest = 0;	 // IAM pages in the longest chain
};

Coverage Cover(const OpenedHeaders &headers, const OpenedPages &pages) {
	const auto &store = *pages.store;
	typedef std::pair<uint16_t, uint32_t> Key;
	std::vector<uint16_t> files;
	for (const auto &f : headers.Set().files) {
		if (f.type == 'D') {
			files.push_back(static_cast<uint16_t>(f.file_id));
		}
	}
	std::map<Key, uint64_t> owner;	// page -> AU of the chain listing it
	std::map<Key, int> listed;
	Coverage c;
	Page page;
	std::vector<std::pair<Key, uint64_t>> heads;
	for (uint16_t file : files) {
		for (uint32_t p = 0; p < pages.allocation->FileSize(file); p++) {
			// A chain's first page has no previous page; its number need not be 0 (E19).
			if (store.Fetch(file, p, page) == PageState::Present && page.header.type == 10 &&
				page.header.prev.IsNull()) {
				heads.emplace_back(Key(file, p), page.header.AllocationUnit());
			}
		}
	}
	for (const auto &head : heads) {
		PagePointer first;
		first.file = head.first.first;
		first.page = head.first.second;
		const AuPages au = ReadIamChain(store, first, head.second);
		c.chains++;
		c.longest = std::max(c.longest, au.iam_pages.size());
		auto list = [&](uint16_t file, uint32_t p) {
			listed[Key(file, p)]++;
			owner[Key(file, p)] = head.second;
		};
		for (const auto &iam : au.iam_pages) {
			list(iam.file, iam.page);
		}
		for (const auto &single : au.single_pages) {
			list(single.file, single.page);
		}
		for (const auto &run : au.extents) {
			for (uint32_t e = run.first; e < run.first + run.count; e++) {
				for (uint32_t k = 0; k < 8; k++) {
					list(run.file, e * 8 + k);
				}
			}
		}
	}
	for (uint16_t file : files) {
		for (uint32_t p = 0; p < pages.allocation->FileSize(file); p++) {
			const PageState state = store.Fetch(file, p, page);
			if (state == PageState::Pending) {
				// Its header is in the log tail: its AU is not checkable here, but a chain must list it.
				c.pending++;
				if (listed.count(Key(file, p)) == 0) {
					c.uncovered++;
				}
				continue;
			}
			if (state != PageState::Present) {
				continue;
			}
			const uint8_t t = page.header.type;
			if (!(t >= 1 && t <= 4) && t != 10) {
				continue;
			}
			c.pages++;
			auto it = listed.find(Key(file, p));
			if (it == listed.end()) {
				c.uncovered++;
			} else if (it->second > 1) {
				c.twice++;
			} else if (owner[Key(file, p)] != page.header.AllocationUnit()) {
				c.wrong_au++;
			} else {
				c.covered_once++;
			}
		}
	}
	return c;
}

}  // namespace

int main() {
	std::cout << "IAM chains (spec 085 PR 2b step 4)" << std::endl;
	DuckDB db(nullptr);
	Connection con(db);
	auto &context = *con.context;

	const std::vector<std::vector<std::string>> forms = {{"full.bak"},
														 {"full_xpress.bak"},
														 {"full_zstd.bak"},
														 {"stripe_1.bak", "stripe_2.bak"},
														 {"stripe_zstd_1.bak", "stripe_zstd_2.bak"}};
	for (const auto &form : forms) {
		std::vector<std::string> paths;
		for (const auto &f : form) {
			paths.push_back(DIR + f);
		}
		auto headers = OpenBackupHeaders(context, paths);
		auto pages = OpenBackupPages(context, *headers);
		Coverage c;
		const std::string error = ErrorOf([&]() { c = Cover(*headers, *pages); });
		// A quiet backup (one pass): no page is Pending.
		Check(error.empty() && c.chains > 10 && c.pages > 200 && c.covered_once == c.pages && c.pending == 0 &&
				  c.uncovered == 0,
			  form.front() + ": " + std::to_string(c.chains) +
				  " chains list every live data / index / LOB / IAM page (" + std::to_string(c.pages) +
				  ") once, under its own AU, none pending" + (error.empty() ? "" : " -- " + error) +
				  (c.pending ? " -- pending " + std::to_string(c.pending) : "") +
				  (c.uncovered || c.twice || c.wrong_au
					   ? " -- uncovered " + std::to_string(c.uncovered) + ", twice " + std::to_string(c.twice) +
							 ", other AU " + std::to_string(c.wrong_au)
					   : ""));
	}

	auto headers = OpenBackupHeaders(context, {DIR + "full.bak"});
	auto pages = OpenBackupPages(context, *headers);
	Page iam;
	uint32_t first = 0;
	for (uint32_t p = 0; p < 1024; p++) {
		if (pages->store->Read(1, p, iam) && iam.header.type == 10) {
			first = p;
			break;
		}
	}
	Check(iam.header.type == 10, "the sample has an IAM page among its first 1024 pages");
	PagePointer at;
	at.file = 1;
	at.page = first;
	const uint64_t au = iam.header.AllocationUnit();
	Check(ErrorOf([&]() { ReadIamChain(*pages->store, at, au + (1ull << 16)); }).find("belongs to allocation unit") !=
			  std::string::npos,
		  "a chain asked for another AU is refused");
	PagePointer boot;
	boot.file = 1;
	boot.page = 9;
	Check(ErrorOf([&]() { ReadIamChain(*pages->store, boot, au); }).find("not an IAM page") != std::string::npos,
		  "a chain that starts at a page that is not an IAM page is refused");
	Check(ReadIamChain(*pages->store, PagePointer(), au).PageCount() == 0, "an AU with no first IAM page has no pages");

	std::cout << (failures ? "FAILED" : "PASSED") << " (" << failures << " failure(s))" << std::endl;
	return failures ? 1 : 0;
}
