#include "backup/backup_boot.hpp"

#include "backup/backup_collation.hpp"
#include "backup/backup_record.hpp"

#include <cstdio>
#include <cstring>

namespace duckdb {
namespace mssql {
namespace backup {

static constexpr uint8_t PAGE_TYPE_BOOT = 13;
static constexpr size_t BOOT_VERSION_AT = 4;
static constexpr size_t BOOT_NAME_AT = 52;
static constexpr size_t BOOT_NAME_BYTES = 256;
static constexpr size_t BOOT_NAME_LENGTH_AT = 308;
static constexpr size_t BOOT_CMPTLEVEL_AT = 314;
static constexpr size_t BOOT_COLLATION_AT = 392;
static constexpr size_t BOOT_FAMILY_GUID_AT = 408;
static constexpr size_t BOOT_FIRST_SYS_INDEXES_AT = 516;
static constexpr size_t BOOT_FIXED_NEEDED = BOOT_FIRST_SYS_INDEXES_AT + 6;

BootPage ReadBootPage(const Page &page) {
	if (page.header.type != PAGE_TYPE_BOOT) {
		throw FormatError("backup: page " + page.header.page_id.ToString() + " is of type " +
						  std::to_string(page.header.type) + ", not a boot page (13)");
	}
	auto r = ReadSlot(page.bytes.data(), page.bytes.size(), page.header.slot_count, 0, "the boot record");
	if (r.Type() != 0 || r.fixed_end < BOOT_FIXED_NEEDED) {
		throw FormatError("backup: the boot record is of type " + std::to_string(r.Type()) + " with a fixed part of " +
						  std::to_string(r.fixed_end) + " bytes, not a primary record of " +
						  std::to_string(BOOT_FIXED_NEEDED) + " or more");
	}
	// Offsets from the record's start (the fixed part begins after its 4-byte header).
	ByteReader b(r.data, r.fixed_end, "the boot record", r.offset);
	BootPage boot;
	boot.version = b.U16(BOOT_VERSION_AT);
	const uint16_t name_bytes = b.U16(BOOT_NAME_LENGTH_AT);
	if (name_bytes > BOOT_NAME_BYTES || name_bytes % 2) {
		b.Fail(BOOT_NAME_LENGTH_AT, "a database name of " + std::to_string(name_bytes) + " bytes, not 0..256 and even");
	}
	boot.database_name = Utf16LeToUtf8(b.Bytes(BOOT_NAME_AT, name_bytes), name_bytes);
	boot.compatibility_level = b.U8(BOOT_CMPTLEVEL_AT);
	boot.collation_id = b.I32(BOOT_COLLATION_AT);
	std::memcpy(boot.family_guid.bytes.data(), b.Bytes(BOOT_FAMILY_GUID_AT, 16), 16);
	boot.first_sys_indexes = ReadPagePointer(b, BOOT_FIRST_SYS_INDEXES_AT);
	return boot;
}

void CheckBootAgainstSet(const BootPage &boot, const BackupSetHeader &set) {
	auto refuse = [](const std::string &field, const std::string &boot_value, const std::string &set_value) {
		throw FormatError("backup: the boot page and the backup set's trailer disagree on the database's " + field +
						  " (" + boot_value + " against " + set_value + ")");
	};
	if (boot.database_name != set.database_name) {
		refuse("name", boot.database_name, set.database_name);
	}
	if (set.database_version != 0 && boot.version != set.database_version) {
		refuse("version", std::to_string(boot.version), std::to_string(set.database_version));
	}
	if (set.compatibility_level != 0 && boot.compatibility_level != set.compatibility_level) {
		refuse("compatibility level", std::to_string(boot.compatibility_level),
			   std::to_string(set.compatibility_level));
	}
	if (!set.family_guid.IsZero() && boot.family_guid.bytes != set.family_guid.bytes) {
		refuse("family GUID", boot.family_guid.ToString(), set.family_guid.ToString());
	}
	// A CollationId the table lacks (a server newer than the reader's tables) cannot be compared: not
	// a disagreement. A varchar column under it is refused when its values are decoded (D13).
	const CollationRow *row = CollationById(boot.collation_id);
	if (row && (set.collation_info != 0 || set.collation_sort_id != 0) &&
		!CollationMatchesTds(*row, set.collation_info, set.collation_sort_id)) {
		std::string named = CollationNameOfTds(set.collation_info, set.collation_sort_id);
		if (named.empty()) {
			char tds[48];
			snprintf(tds, sizeof(tds), "TDS form 0x%08X, SortId %u", set.collation_info, set.collation_sort_id);
			named = tds;
		}
		refuse("collation", row->name, named);
	}
}

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb
