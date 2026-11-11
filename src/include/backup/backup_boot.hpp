//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// backup/backup_boot.hpp
//
// Spec 085 task 11 (PR 2b step 2): the boot page, 1:9 -- one primary record of fixed columns only
// (statusA 0x00), whose length grows with the version (1644 bytes on 2017, 1768 on 2019, 1786 on
// 2022, 1832 on 2025) while the fields the reader takes keep their place, measured on the lane's
// 2017-2025 sets of the samples' database in R2 (2026-11-10): dbi_version u16 +4, dbi_dbname UTF-16 at +52 (128 units,
// padded) with its length in bytes, u16 +308, dbi_cmptlevel u8 +314, dbi_collation (a CollationId) u32 +392,
// dbi_familyGUID +408, dbi_firstSysIndexes (the first sysallocunits page, a 6-byte pointer) +516.
//
// The trailer (SCIN) records the same database's name, version, level, collation and family GUID:
// CheckBootAgainstSet refuses a backup whose two disagree, by field. DuckDB-free (D15).
//===----------------------------------------------------------------------===//

#pragma once

#include "backup/backup_page.hpp"
#include "backup/backup_set.hpp"

#include <cstdint>
#include <string>

namespace duckdb {
namespace mssql {
namespace backup {

struct BootPage {
	uint16_t version = 0;  // dbi_version: 869 2017, 904 2019, 957 2022, 998 2025
	std::string database_name;
	uint8_t compatibility_level = 0;
	int32_t collation_id = 0;  // a CollationId (codec::code_page::FindCollationById)
	Guid family_guid;
	PagePointer first_sys_indexes;	// dbi_firstSysIndexes: sysallocunits' first page
};

//! The boot page `page` (1:9, of type 13) read.
BootPage ReadBootPage(const Page &page);

//! The boot page and the backup set's trailer describe one database: name, version, compatibility
//! level, family GUID and collation (the boot page's CollationId one the trailer's TDS form names).
//! FormatError naming the field that disagrees. A field the trailer does not record (0, or an empty name), or a
//! CollationId the reader's table lacks, is not compared. The boot page follows a rename, a level
//! change and a RESTORE under a new name (E18); a level changed while a backup runs is not tested.
void CheckBootAgainstSet(const BootPage &boot, const BackupSetHeader &set);

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb
