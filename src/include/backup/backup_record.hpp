//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// backup/backup_record.hpp
//
// Spec 085 PR 2b, task 11 (step 3): a FixedVar record on a page, read by its header alone (spec 085
// § Rules, records): statusA (record type in bits 1-3, 0x10 a null bitmap, 0x20 variable columns),
// u16 end of the fixed part @2, the fixed data from 4, then (with 0x10) a u16 column count and the
// null bitmap of ceil(n / 8) bytes, then (with 0x20) a u16 variable column count and that many u16
// end offsets (bit 15 = a complex column), from the record's start. A forwarding stub (type 2), a LOB
// fragment (type 4), an index record (types 3 and 5: keys packed to the page's pminlen, no fixed
// end) and a version ghost (type 7: a heap row deleted under row versioning, E20) are not FixedVar:
// their view holds the status bytes only. A record of a table with row versioning on (RCSI,
// snapshot) has statusA 0x40 and a 14-byte version tag after its last part (E20), which the view
// does not need. Which column is which is the reader's: a record view only bounds every part by the
// page, so a crafted offset or count is a named error, never a read past it. The view allocates
// nothing (its variable columns' ends are read in the page), so the scan builds one per row (spec 085
// PR 2d). DuckDB-free (D15).
//
// And the record of a data file's header page (page 0): its variable columns 2, 3 and 4 are the file
// id (u16), its filegroup (u16) and its Size in pages (u32). By column, not by byte offset: the
// record has 66 columns on SQL Server 2025 and 60 on 2022, so its data starts 13 bytes apart and a
// fixed offset (+167 / +171, read off 2025) points elsewhere on 2022 -- found by the catalog
// fixture's backup, 2026-11-10.
//===----------------------------------------------------------------------===//

#pragma once

#include "backup/backup_bytes.hpp"

#include <cstdint>

namespace duckdb {
namespace mssql {
namespace backup {

//! A record's type, (statusA >> 1) & 7.
constexpr uint8_t RECORD_PRIMARY = 0;
constexpr uint8_t RECORD_FORWARDED = 1;
constexpr uint8_t RECORD_FORWARDING_STUB = 2;
constexpr uint8_t RECORD_INDEX = 3;
constexpr uint8_t RECORD_BLOB_FRAGMENT = 4;
constexpr uint8_t RECORD_GHOST_INDEX = 5;
constexpr uint8_t RECORD_GHOST_DATA = 6;
constexpr uint8_t RECORD_GHOST_VERSION = 7;
//! statusA 0x40: a 14-byte version tag follows the record (E20).
constexpr uint8_t STATUS_VERSION_TAG = 0x40;

struct RecordView {
	const uint8_t *data = nullptr;	// the record's first byte, in its page
	uint16_t offset = 0;			// in its page
	uint16_t limit = 0;				// bytes from the record's start to the page's slot array
	uint8_t status_a = 0;
	uint8_t status_b = 0;
	uint16_t fixed_end = 0;		// where the fixed part ends (the column count follows); 0 for types 2-5
	uint16_t column_count = 0;	// 0 without a null bitmap
	uint16_t null_bitmap = 0;	// where the null bitmap starts (0 without one)
	uint16_t var_count = 0;
	uint16_t var_start = 0;	 // where the first variable column's data starts
	//! The variable columns' u16 end offsets, in the page (raw: bit 15 marks a complex column);
	//! var_count of them, checked by ReadRecord. Null without variable columns.
	const uint8_t *var_ends = nullptr;

	//! (statusA >> 1) & 7: 0 primary, 1 forwarded, 2 forwarding stub, 3 index, 4 blob fragment, 5 ghost
	//! index, 6 ghost data, 7 ghost version.
	uint8_t Type() const {
		return (status_a >> 1) & 7;
	}
	bool HasNullBitmap() const {
		return (status_a & 0x10) != 0;
	}
	bool HasVariableColumns() const {
		return (status_a & 0x20) != 0;
	}
	//! Where a FixedVar record ends, from its start: its last variable column's end (or where variable
	//! data would start), plus the 14-byte version tag statusA 0x40 adds (E20). 0 for a record that
	//! is not FixedVar (its view holds the status bytes only).
	uint16_t End() const;
	//! Column `column` (0-based, of column_count) is NULL in the bitmap; false without a bitmap.
	bool IsNull(size_t column) const;
	//! Variable column `i`'s bytes: [start, end) from the record's start, bit 15 masked off.
	uint16_t VarStart(size_t i) const;
	uint16_t VarEnd(size_t i) const;
	bool VarComplex(size_t i) const {
		return (RawVarEnd(i) & 0x8000) != 0;
	}
	//! Variable column `i`'s end as stored, bit 15 included (i < var_count, unchecked).
	uint16_t RawVarEnd(size_t i) const {
		return static_cast<uint16_t>(var_ends[2 * i] | (var_ends[2 * i + 1] << 8));
	}
	bool HasVersionTag() const {
		return (status_a & STATUS_VERSION_TAG) != 0;
	}
	//! A reader over variable column `i`'s bytes, named `what` (its offsets are the page's).
	ByteReader Var(size_t i, const char *what) const;
	//! The record's fixed part, after its 4-byte header: bytes [4, fixed_end).
	ByteReader Fixed(const char *what) const;
};

//! The record at `offset` of the page `page` (page_size bytes) whose slot array begins at
//! `slot_array` (page_size - 2 * slot count): every part checked to lie before the slot array, the
//! variable columns' ends ascending. `what` names the record in errors.
RecordView ReadRecord(const uint8_t *page, size_t page_size, size_t slot_array, uint16_t offset, const char *what);

//! The record slot `slot` of an 8 KB page points to (u16 at page_size - 2 - 2 * slot); a slot
//! offset below 96 (a freed slot) is a FormatError here: the callers that skip freed slots check it.
RecordView ReadSlot(const uint8_t *page, size_t page_size, uint16_t slot_count, uint16_t slot, const char *what);

struct FileHeaderRecord {
	uint16_t file_id = 0;
	uint16_t filegroup_id = 0;
	uint32_t size = 0;	// pages
};

//! The header record of a data file's page 0 (PAGE_SIZE bytes, one slot).
FileHeaderRecord ReadFileHeaderRecord(const uint8_t *page, size_t page_size);

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb
