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
// Unicode compression (SCSU, UTS #6) decoded to UTF-16LE.
//
// Spec 086 PR 2 (tasks 5-6): PAGE compression, by E23's measurements
// (specs/086-backup-row-formats-and-types/measured/E23-page-compression.md). A page whose header flags
// carry 0x80 holds a compression-information record at offset 96 (ReadPageCompression): an anchor
// record (a CD record of each column's prefix) and a dictionary (entries in their CD form). A record's
// value is then a dictionary symbol (code 12: an entry), its prefix rebuilt from the anchor (a prefix
// length, one byte or two with 0x80, then the suffix), or EMPTY: the anchor's value whole -- and then put in
// the uncompressed form as a ROW value is. A page without the flag in a PAGE rowset is a ROW page.
// DuckDB-free (085 D15).
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
//! A forwarded record deleted under row versioning (E23: header 0x17): its column count (0), its back
//! pointer and its version tag; no row (a ghost).
constexpr uint8_t CD_TYPE_GHOST_FORWARDED = 5;
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
//! into `out` (its vectors reused). A record type the reader does not know (6, 7; 5 is a ghost
//! forwarded record, E23), or a header bit
//! it does not (0x40, 0x80), is a FormatError, never skipped: it could be a row. `what` names the
//! record in errors. `padded`: the record is padded to 9 bytes as every row record is (E22); a PAGE
//! page's anchor record is not (E23: one of 5 bytes).
void ReadCdRecord(const uint8_t *page, size_t page_size, size_t slot_array, uint16_t offset, const char *what,
				  CdRecord &out, bool padded = true);

//! A PAGE-compressed page's compression-information record (E23), at offset 96: u8 flags (0x02 an anchor
//! record, 0x04 a dictionary), u16 a modification count, then with a dictionary u16 its offset from the
//! record's start, then u16 the record's size; the anchor record (a CD record: per column its prefix, or
//! NULL for none); the dictionary: u16 its entry count, u16 each entry's end offset from the dictionary's
//! start, the entries (each value in its CD form, an anchored column's with its prefix length first).
struct PageCompression {
	//! The page's header carries 0x80: the record is there. Otherwise a ROW page in a PAGE rowset.
	bool present = false;
	bool has_anchor = false;
	CdRecord anchor;
	//! The dictionary's entries: where each lies in the page, and its size.
	std::vector<std::pair<uint16_t, uint16_t>> dictionary;
	const uint8_t *page = nullptr;
	//! Where the record ends in the page: the first row record starts at or past it.
	uint16_t end = 0;
};

//! The compression-information record of `page` (of `page_size` bytes, its slot array at `slot_array`),
//! read into `out`: every part within the record, the record within the page's records, the anchor a CD
//! row of the record, the dictionary's end offsets ascending within it. A flag the reader does not know
//! is a FormatError, as any part out of its bounds; `what` names the page.
void ReadPageCompression(const uint8_t *page, size_t page_size, size_t slot_array, const char *what,
						 PageCompression &out);

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
	//! a few columns does not pay for the others' decoding (measured on 1M rows of six columns, one
	//! thread: the whole ROW table 0.27 s against 0.10 s uncompressed; one bigint of it, with `needed`,
	//! 0.11 s against 0.04 s).
	//! `page`: the record's page's compression information (PAGE compression), or none (a ROW page, a ROW
	//! rowset): a symbol is its dictionary entry, a value of an anchored column its prefix rebuilt.
	void Read(const CdRecord &record, const RowsetLayout &layout, const std::vector<bool> *needed = nullptr,
			  const PageCompression *page = nullptr);

	const ValueRef &Value(size_t k) const {
		return values_[k];
	}
	size_t Size() const {
		return values_.size();
	}

private:
	//! One value being staged: its column, its CD value, and where its bytes lie. Passed by reference
	//! so the staging functions below share one description instead of eight parameters each.
	struct Staging {
		const RowsetLayout &layout;
		const PhysicalColumn &c;
		const CdValue &cd;
		size_t k;
		size_t start;
		const uint8_t *p;
		size_t size;
		bool rebuilt;
		//! The column's name, for an error only: built where one is thrown, never per value.
		std::string What() const {
			return layout.table + "." + c.name;
		}
	};

	void StageValue(const Staging &s, const std::vector<bool> *needed);
	bool StageNumeric(const Staging &s);
	bool StageTemporal(const Staging &s);
	void StageBytes(const Staging &s);

	void Store(const Staging &s, size_t bytes);
	void StoreAsIs(const Staging &s);
	void StoreInteger(const Staging &s, size_t width);
	void StorePadded(const Staging &s, uint8_t pad, size_t width);

	//! A value's bytes under PAGE compression: a symbol resolved to its dictionary entry, an anchored
	//! column's value rebuilt from the anchor's prefix and its own suffix (E23).
	void RebuildFromPage(const PageCompression &page, const CdValue &cd, size_t index, const RowsetLayout &layout,
						 const PhysicalColumn &c, const uint8_t *&p, size_t &size, bool &rebuilt);

	std::vector<ValueRef> values_;
	std::vector<size_t> at_;  // where each column's normalized bytes start in buffer_, or SIZE_MAX
	std::vector<uint8_t> buffer_;
	std::vector<uint8_t> rebuilt_;	// a PAGE value's prefix and suffix put together (one value at a time)
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
