//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// backup/backup_cd_record.hpp
//
// Spec 086 PR 1 (tasks 1-3): a ROW-compressed (CD) record and its values, by E22's measurements
// (specs/086-backup-row-formats-and-types/measured/E22-cd-records.md). The record: a header byte
// (bit 0 CD, bit 1 a 14-byte version tag at the end, bits 2-4 the record type, bit 5 a long region),
// a column count (u8, or u16 big-endian when its top bit is set), a CD
// array of 4-bit codes, a cluster array when there are more than 30 columns, the short region of
// 1-8-byte values and the long region (flags, count, end offsets). Every part is bounded by the
// record's limit, so a crafted count, code or offset is a named error, never a read past the page.
//
// A value is then put in the form the uncompressed (FixedVar) record holds it (CdRow): the reader's
// kernels, the key bytes of _mssql_rid and everything after the record are the uncompressed path's,
// unchanged. Integers from their sign-flipped big-endian form, decimals from vardecimal, the IEEE
// types padded back, datetime from its one integer, char / binary / nchar padded to the column, and
// Unicode compression (SCSU, UTS #6) decoded to UTF-16LE. DuckDB-free (085 D15).
//===----------------------------------------------------------------------===//

#pragma once

#include "backup/backup_system_tables.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace duckdb {
namespace mssql {
namespace backup {

//! The CD codes (E22): 0 NULL, 1 EMPTY, 2-9 1-8 bytes in the short region, 10 in the long region,
//! 11 bit = 1, 12 a page-dictionary symbol (PAGE compression, spec 086 PR 2).
constexpr uint8_t CD_NULL = 0;
constexpr uint8_t CD_EMPTY = 1;
constexpr uint8_t CD_LONG = 10;
constexpr uint8_t CD_BIT_ONE = 11;
constexpr uint8_t CD_SYMBOL = 12;
//! Columns per cluster of the CD array (E22).
constexpr size_t CD_CLUSTER_COLUMNS = 30;
//! The record types of a CD header's bits 2-4 (E22): a primary record, a version ghost (a heap row
//! deleted under row versioning: the header and its version tag only), a forwarding stub (the header
//! and the RID it forwards to), a ghost, a forwarded record (its back pointer after its data).
constexpr uint8_t CD_TYPE_PRIMARY = 0;
constexpr uint8_t CD_TYPE_GHOST_VERSION = 1;
constexpr uint8_t CD_TYPE_STUB = 2;
constexpr uint8_t CD_TYPE_GHOST = 3;
constexpr uint8_t CD_TYPE_FORWARDED = 4;
//! Header bit 1: a 14-byte version tag ends the record (row versioning: RCSI, snapshot isolation).
constexpr uint8_t CD_VERSION_TAG = 0x02;
constexpr size_t CD_VERSION_TAG_BYTES = 14;

//! One column of a CD record: its code and, for a stored value, where its bytes lie in the record.
struct CdValue {
	uint8_t code = CD_NULL;
	bool complex = false;  // a long value whose end offset has bit 15 set: an off-row pointer
	uint16_t offset = 0;   // from the record's start
	uint16_t size = 0;
};

//! A CD record on a page. Kind() says what the scan does with it: a row (primary or forwarded),
//! something that is no row of its own (a ghost, a forwarding stub: the row is read where it lives).
struct CdRecord {
	enum class Kind : uint8_t { Row, Forwarded, Ghost, Stub };

	const uint8_t *data = nullptr;
	uint16_t offset = 0;  // in its page
	uint16_t limit = 0;	  // bytes from the record's start to the page's slot array
	uint8_t header = 0;
	Kind kind = Kind::Row;
	uint16_t column_count = 0;
	//! Where the record ends, from its start (a forwarded record's back pointer and a version tag
	//! included; at least the 9 bytes every record is padded to).
	uint16_t end = 0;
	//! Where a forwarded record's back pointer starts, from its start (after its data, before a tag).
	uint16_t back_pointer = 0;
	//! column_count of them (the vector is reused from record to record).
	std::vector<CdValue> values;

	//! The row a forwarded record was moved from (its back pointer: page u32, file u16, slot u16).
	void BackPointer(uint32_t &page, uint16_t &file, uint16_t &slot) const;
};

//! The record at `offset` of a page of `page_size` bytes whose slot array begins at `slot_array`, read
//! into `out` (its vectors reused). A record type the reader does not know (5, 6, 7), or a header bit
//! it does not (0x40, 0x80), is a FormatError, never skipped: it could be a row. `what` names the
//! record in errors.
void ReadCdRecord(const uint8_t *page, size_t page_size, size_t slot_array, uint16_t offset, const char *what,
				  CdRecord &out);

//! One row's values in the uncompressed form, through `layout` (a ROW-compressed rowset's: every
//! physical column a CD index, -offset - 1). Reused from row to row; the ValueRefs point into the
//! record or into the row's own buffer, valid until the next Read.
class CdRow {
public:
	//! Every column of `layout` located in `record` and put in its uncompressed form: dropped -> Null;
	//! past the record's column count -> the layout's default (Default) or NULL, as LocateValue; a
	//! complex long value -> OffRow (its pointer as it is). A value that cannot be of its column's
	//! type (a code 12 outside PAGE compression, a length the type cannot have, digits past the
	//! precision, a truncated SCSU sequence) is a FormatError naming the column.
	//! `needed` (by layout index; null: every column): the columns whose values are read. Another column
	//! is located only -- its value stays as stored, and a complex one is OffRow as any -- so a query of
	//! a few columns does not pay for the others' decoding (measured: a 1M-row scan of one bigint of
	//! six columns, 0.27 s against 0.04 s uncompressed before).
	void Read(const CdRecord &record, const RowsetLayout &layout, const std::vector<bool> *needed = nullptr);

	const ValueRef &Value(size_t k) const {
		return values_[k];
	}
	size_t Size() const {
		return values_.size();
	}

private:
	std::vector<ValueRef> values_;
	std::vector<size_t> at_;  // where each column's normalized bytes start in buffer_, or SIZE_MAX
	std::vector<uint8_t> buffer_;
};

//! One row's record as a reader takes its values: a FixedVar record, located column by column
//! (LocateValue), or a ROW-compressed one, located and put in the uncompressed form once (CdRow).
struct RowRecord {
	const uint8_t *data = nullptr;	// the record's first byte, in its page (which record it is)
	const RecordView *fixed = nullptr;
	size_t var_count = 0;  // the table's variable columns of a FixedVar record (RowVarCount)
	const CdRow *cd = nullptr;

	static RowRecord Fixed(const RecordView &record, size_t var_count) {
		RowRecord r;
		r.data = record.data;
		r.fixed = &record;
		r.var_count = var_count;
		return r;
	}
	static RowRecord Compressed(const CdRecord &record, const CdRow &row) {
		RowRecord r;
		r.data = record.data;
		r.cd = &row;
		return r;
	}
	ValueRef Locate(const RowsetLayout &layout, size_t k) const {
		return cd ? cd->Value(k) : LocateValue(*fixed, var_count, layout, k);
	}
};

//! Unicode compression (E22): `size` bytes at `data` of an nchar / nvarchar value, odd: SCSU (UTS #6),
//! even: UTF-16LE as it is; appended to `out` as UTF-16LE. A FormatError naming `what` for a
//! truncated sequence or a reserved tag (`table`.`column` names the value).
void DecodeCompressedUnicode(const uint8_t *data, size_t size, std::vector<uint8_t> &out, const std::string &table,
							 const std::string &column);

//! Vardecimal (E22): `size` bytes at `data` (0: zero) of a decimal(precision, scale) into its
//! uncompressed form -- a sign byte (1 positive, 0 negative) and the unscaled magnitude,
//! little-endian, in 4 / 8 / 12 / 16 bytes by precision -- appended to `out`. A FormatError naming
//! `what` when the value has more digits than the precision or more fraction digits than the scale.
void DecodeVardecimal(const uint8_t *data, size_t size, uint8_t precision, uint8_t scale, std::vector<uint8_t> &out,
					  const std::string &table, const std::string &column);

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb
