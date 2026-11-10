// Spec 085 PR 2b step 2: the boot page (1:9) of the committed samples (synthetic, SQL Server 2025),
// read through the PageStore, and checked against the backup set's trailer: the database's name,
// version, compatibility level, family GUID and collation. Every form of the full agrees; a trailer
// that disagrees on any of them is refused naming the field.

#include <functional>
#include <iostream>
#include <string>
#include <vector>

#include "backup/backup_boot.hpp"
#include "backup/backup_collation.hpp"
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

}  // namespace

int main() {
	std::cout << "The boot page (spec 085 PR 2b step 2)" << std::endl;
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
		Page page;
		pages->store->ReadLive(1, 9, page, "the test");
		const BootPage boot = ReadBootPage(page);
		const CollationRow *row = CollationById(boot.collation_id);
		Check(
			boot.version == 998 && boot.database_name == "bak_sample" && boot.compatibility_level == 170 && row &&
				std::string(row->name) == "SQL_Latin1_General_CP1_CI_AS" && boot.first_sys_indexes.file == 1 &&
				boot.first_sys_indexes.page == 20,
			form.front() + ": version 998, bak_sample, level 170, SQL_Latin1_General_CP1_CI_AS, sysallocunits at 1:20");
		Check(ErrorOf([&]() { CheckBootAgainstSet(boot, headers->Set()); }).empty(),
			  form.front() + ": the boot page agrees with the trailer");
	}

	auto headers = OpenBackupHeaders(context, {DIR + "full.bak"});
	auto pages = OpenBackupPages(context, *headers);
	Page page;
	pages->store->ReadLive(1, 9, page, "the test");
	const BootPage boot = ReadBootPage(page);
	auto refused = [&](const std::function<void(BackupSetHeader &)> &alter, const char *field) {
		BackupSetHeader set = headers->Set();
		alter(set);
		const std::string e = ErrorOf([&]() { CheckBootAgainstSet(boot, set); });
		Check(e.find(field) != std::string::npos, std::string("a trailer with another ") + field + " is refused: " + e);
	};
	refused([](BackupSetHeader &s) { s.database_name = "other"; }, "database's name");
	refused([](BackupSetHeader &s) { s.database_version = 957; }, "version");
	refused([](BackupSetHeader &s) { s.compatibility_level = 160; }, "compatibility level");
	refused([](BackupSetHeader &s) { s.family_guid.bytes[0] ^= 1; }, "family GUID");
	// Cyrillic_General_CI_AS (LCID 0x419) where the boot page has SQL_Latin1_General_CP1_CI_AS.
	refused(
		[](BackupSetHeader &s) {
			s.collation_sort_id = 0;
			s.collation_info = 0x00D00419;
		},
		"database's collation");
	// Another SQL_ collation: SortId 51 is SQL_Latin1_General_CP1_CS_AS, the boot page's has 52.
	refused([](BackupSetHeader &s) { s.collation_sort_id = 51; }, "database's collation");
	{
		BackupSetHeader blank = headers->Set();
		blank.database_version = 0;
		blank.compatibility_level = 0;
		blank.family_guid = Guid();
		blank.collation_info = 0;
		blank.collation_sort_id = 0;
		Check(ErrorOf([&]() { CheckBootAgainstSet(boot, blank); }).empty(),
			  "a trailer that records none of version, level, family GUID and collation is not refused");
		BootPage unknown = boot;
		unknown.collation_id = 0x7FFFFFF0;	// no collation has it
		Check(ErrorOf([&]() { CheckBootAgainstSet(unknown, headers->Set()); }).empty() &&
				  CollationById(0x7FFFFFF0) == nullptr,
			  "a CollationId the table lacks is not compared");
	}
	Check(!ErrorOf([&]() {
			   Page data;
			   pages->store->ReadLive(1, 2, data, "the test");
			   ReadBootPage(data);
		   }).empty(),
		  "a page that is not the boot page is refused");

	std::cout << (failures ? "FAILED" : "PASSED") << " (" << failures << " failure(s))" << std::endl;
	return failures ? 1 : 0;
}
