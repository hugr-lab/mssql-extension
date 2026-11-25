// Spec 085 PR 2d, task 14: the record forms of user tables (E20), on records copied from the E20
// backup (SQL Server 2022, row versioning on) or built after them: a version tag after the record, the
// uniquifier, bit columns sharing a byte, a forwarded record's back pointer, the stub and the version
// ghost a page also holds (neither split as FixedVar, neither a row), every state LocateValue answers,
// and sysrscols.ti's fields.
#include <functional>
#include <iostream>
#include <string>
#include <vector>

#include "backup/backup_cd_record.hpp"
#include "backup/backup_record.hpp"
#include "backup/backup_system_tables.hpp"

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

std::vector<uint8_t> Hex(const std::string &hex) {
	std::vector<uint8_t> out;
	for (size_t i = 0; i + 1 < hex.size(); i += 2) {
		out.push_back(static_cast<uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
	}
	return out;
}

// An 8 KB data page holding `records` one after another from offset 96, slot i at record i.
Page PageOf(const std::vector<std::vector<uint8_t>> &records) {
	Page page;
	page.bytes.assign(PAGE_SIZE, 0);
	size_t at = 96;
	for (size_t i = 0; i < records.size(); i++) {
		std::copy(records[i].begin(), records[i].end(), page.bytes.begin() + at);
		page.bytes[PAGE_SIZE - 2 - 2 * i] = static_cast<uint8_t>(at);
		page.bytes[PAGE_SIZE - 1 - 2 * i] = static_cast<uint8_t>(at >> 8);
		at += records[i].size();
	}
	page.header.slot_count = static_cast<uint16_t>(records.size());
	page.header.type = 1;
	return page;
}

RecordView Slot(const Page &page, uint16_t slot) {
	return ReadSlot(page.bytes.data(), PAGE_SIZE, page.header.slot_count, slot, "the test page");
}

PhysicalColumn Column(const char *name, uint8_t xtype, int16_t offset, uint16_t null_bit, uint32_t status = 0) {
	PhysicalColumn c;
	c.name = name;
	c.xtype = xtype;
	c.offset = offset;
	c.null_bit = null_bit;
	c.status = status;
	return c;
}

int32_t I32(const ValueRef &v) {
	return static_cast<int32_t>(v.data[0] | (v.data[1] << 8) | (v.data[2] << 16) |
								(static_cast<uint32_t>(v.data[3]) << 24));
}

std::string Text(const ValueRef &v) {
	return std::string(reinterpret_cast<const char *>(v.data), v.size);
}

// E20's records (2022). dbo.ver (id int PRIMARY KEY, v int, s varchar(20)), row 1, never updated:
// a version tag whose pointer is zero.
const char *VER_ROW = "70000c0001000000010000000300000100150076310000000000000000a60300000000";
// The same table's row 4, updated: its tag points into tempdb's version store.
const char *VER_UPDATED =
	"70000c0004000000ec030000"
	"0300"
	"00"
	"0100"
	"1500"
	"7634"
	"0900000009003b00a90300000000";
// dbo.uniq (k int, v varchar(20), n int NULL; a non-unique clustered index on k): the first row of key 0,
// its uniquifier (the first variable column) empty.
const char *UNIQ_FIRST = "70000c00000000000a0000000400000200150018007531300000000000000000a00300000000";
// dbo.bits (b1 bit NULL, b2 bit, i int NULL, b3..b9 bit NULL): (1, 0, 5, 1, 0, 1, 0, 1, 0, 1).
const char *BITS_ROW =
	"50000a00550500000001"
	"0a000000"
	"0000000000000000e20300000000";
// ... and (NULL, 1, NULL, 0, 1, 0, 1, 0, 1, 0).
const char *BITS_NULLS =
	"50000a00aa0500000000"
	"0a000500"
	"0000000000000000e20300000000";
// dbo.ghost_heap: a heap row deleted under row versioning, a version ghost (type 7), its first bytes.
const char *VERSION_GHOST = "4e0b00000009002400b9030000000067330000000000000000b60300000000";
// dbo.fwd: a forwarding stub (type 2), 9 bytes.
const char *STUB = "04510100000100a600";

}  // namespace

int main() {
	std::cout << "test_backup_rows (spec 085 PR 2d, task 14)" << std::endl;

	// A version tag: statusA 0x40, 14 bytes after the record, the layout before it unchanged.
	{
		RowsetLayout layout;
		layout.table = "ver";
		layout.columns = {Column("id", XTYPE_INT, 4, 1, 0x80), Column("v", XTYPE_INT, 8, 2, 0x80),
						  Column("s", XTYPE_VARCHAR, -1, 3)};
		const Page page = PageOf({Hex(VER_ROW), Hex(VER_UPDATED)});
		for (uint16_t slot = 0; slot < 2; slot++) {
			const RecordView r = Slot(page, slot);
			const size_t vars = RowVarCount(r, layout.table);
			const ValueRef id = LocateValue(r, vars, SparseVector(), layout, 0),
						   v = LocateValue(r, vars, SparseVector(), layout, 1),
						   s = LocateValue(r, vars, SparseVector(), layout, 2);
			const int32_t want_id = slot == 0 ? 1 : 4, want_v = slot == 0 ? 1 : 1004;
			Check(r.HasVersionTag() && r.Type() == RECORD_PRIMARY && I32(id) == want_id && I32(v) == want_v &&
					  Text(s) == (slot == 0 ? "v1" : "v4"),
				  std::string("a version-tagged record decodes as without its tag (") +
					  (slot == 0 ? "never updated" : "updated, its tag pointing to tempdb") + ")");
		}
	}

	// The uniquifier: the first variable column, empty for 0, else an int; the table's own variable
	// columns after it.
	{
		RowsetLayout layout;
		layout.table = "uniq";
		layout.columns = {Column("", XTYPE_INT, -1, 1, 0x10 | 0x80), Column("k", XTYPE_INT, 4, 2, 0x80),
						  Column("v", XTYPE_VARCHAR, -2, 3, 0x80), Column("n", XTYPE_INT, 8, 4)};
		const Page first = PageOf({Hex(UNIQ_FIRST)});
		const RecordView r = Slot(first, 0);
		const size_t vars = RowVarCount(r, layout.table);
		const ValueRef u = LocateValue(r, vars, SparseVector(), layout, 0),
					   k = LocateValue(r, vars, SparseVector(), layout, 1),
					   v = LocateValue(r, vars, SparseVector(), layout, 2),
					   n = LocateValue(r, vars, SparseVector(), layout, 3);
		Check(u.state == ValueState::Inline && u.size == 0 && I32(k) == 0 && Text(v) == "u10" && I32(n) == 10,
			  "the first row of a key: an empty uniquifier, then the table's columns");
		// The same record with uniquifier 3 (4 bytes before "u10").
		std::vector<uint8_t> second = Hex(UNIQ_FIRST);
		second[17] = 0x19;	// the uniquifier now ends at 25
		second[19] = 0x1c;	// and v at 28
		second.insert(second.begin() + 21, {0x03, 0x00, 0x00, 0x00});
		const Page next = PageOf({second});
		const RecordView r2 = Slot(next, 0);
		const ValueRef u2 = LocateValue(r2, RowVarCount(r2, layout.table), SparseVector(), layout, 0);
		const ValueRef v2 = LocateValue(r2, RowVarCount(r2, layout.table), SparseVector(), layout, 2);
		Check(u2.size == 4 && I32(u2) == 3 && Text(v2) == "u10", "a later row of the key: its uniquifier an int");
		const std::vector<SystemValue> decoded = DecodeRecord(r2, layout);
		Check(decoded[0].is_null && ValueText(layout, 2, decoded[2]) == "u10",
			  "DecodeRecord leaves the uniquifier out, as before");
	}

	// Bit columns: eight share the byte at offset 4 by bit position, the ninth starts the next; a NULL
	// is the null bitmap's, whatever its bit holds.
	{
		RowsetLayout layout;
		layout.table = "bits";
		layout.columns.push_back(Column("b1", XTYPE_BIT, 4, 1));
		layout.columns.push_back(Column("b2", XTYPE_BIT, 4, 2, 0x80));
		layout.columns.back().bit_position = 1;
		layout.columns.push_back(Column("i", XTYPE_INT, 5, 3));
		for (uint8_t b = 3; b <= 8; b++) {
			const std::string name = "b" + std::to_string(b);
			layout.columns.push_back(Column("", XTYPE_BIT, 4, static_cast<uint16_t>(b + 1)));
			layout.columns.back().name = name;
			layout.columns.back().bit_position = static_cast<uint8_t>(b - 1);
		}
		layout.columns.push_back(Column("b9", XTYPE_BIT, 9, 10));
		const Page page = PageOf({Hex(BITS_ROW), Hex(BITS_NULLS)});
		auto values = [&](uint16_t slot) {
			const std::vector<SystemValue> row = DecodeRecord(Slot(page, slot), layout);
			std::string out;
			for (size_t k = 0; k < row.size(); k++) {
				if (row[k].is_null) {
					out += "N";
				} else if (layout.columns[k].xtype == XTYPE_BIT) {
					out += row[k].bytes == std::string(1, '\1') ? "1" : "0";
				} else {
					out += std::to_string(ValueInt(layout, k, row[k]));
				}
			}
			return out;
		};
		Check(values(0) == "1051010101", "(1, 0, 5, 1, 0, 1, 0, 1, 0, 1): " + values(0));
		Check(values(1) == "N1N0101010", "(NULL, 1, NULL, 0, 1, 0, 1, 0, 1, 0): " + values(1));
		layout.columns[1].bit_position = 8;
		Check(ErrorOf([&]() { DecodeRecord(Slot(page, 0), layout); }).find("bit position 8") != std::string::npos,
			  "a bit position past 7 is refused by name");
	}

	// A version ghost (type 7) and a stub (type 2) beside a row: neither split as FixedVar, neither a
	// row of the page. Before E20 a version ghost was read as FixedVar and failed ("a fixed part ending
	// at 0").
	{
		const Page page = PageOf({Hex(VER_ROW), Hex(VERSION_GHOST), Hex(STUB)});
		const RecordView ghost = Slot(page, 1), stub = Slot(page, 2);
		Check(ghost.Type() == RECORD_GHOST_VERSION && ghost.fixed_end == 0 && ghost.var_count == 0,
			  "a version ghost is not split as FixedVar");
		Check(stub.Type() == RECORD_FORWARDING_STUB && stub.fixed_end == 0, "a stub is not either");
		RowsetLayout layout;
		layout.table = "ver";
		layout.columns = {Column("id", XTYPE_INT, 4, 1, 0x80), Column("v", XTYPE_INT, 8, 2, 0x80),
						  Column("s", XTYPE_VARCHAR, -1, 3)};
		int rows = 0;
		ForEachPageRow(page, layout, [&](const SystemRow &) { rows++; });
		Check(rows == 1, "a page of a row, a version ghost and a stub holds one row");
	}

	// A forwarded record's back pointer: the row's place before it moved.
	{
		// id int, v varchar; "ab", then the back pointer: tag 0x0400, page 0x151, file 1, slot 0xa6.
		std::vector<uint8_t> forwarded =
			Hex("3200"
				"0800"
				"07000000"
				"0200"
				"00"
				"0200"
				"1300"
				"1d80"
				"6162"
				"0004"
				"51010000"
				"0100"
				"a600");
		RowsetLayout layout;
		layout.table = "fwd";
		layout.columns = {Column("id", XTYPE_INT, 4, 1, 0x80), Column("v", XTYPE_VARCHAR, -1, 2)};
		const Page page = PageOf({forwarded, Hex(VER_ROW)});
		const RecordView r = Slot(page, 0);
		const RowLocation from = ForwardedFrom(r, layout.table);
		Check(r.Type() == RECORD_FORWARDED && RowVarCount(r, layout.table) == 1 && from.page.page == 0x151 &&
				  from.page.file == 1 && from.slot == 0xa6,
			  "a forwarded record: one variable column of its own, and where it came from");
		Check(Text(LocateValue(r, 1, SparseVector(), layout, 1)) == "ab", "its own column reads");
		Check(
			ErrorOf([&]() { ForwardedFrom(Slot(page, 1), "ver"); }).find("not a forwarded record") != std::string::npos,
			"a primary record has no place it came from");
	}

	// LocateValue's other states: dropped, added (a default, else NULL), past the variable count,
	// off-row.
	{
		// id int, a dropped int at 8 (99), v and w varchar: four columns, one variable column, complex
		// (E20's 24-byte row-overflow pointer); then two columns added after the record was written,
		// the first taking the dropped column's offset (spec 085 § Rules, records: the order matters).
		std::vector<uint8_t> record =
			Hex("3000"
				"0c00"
				"01000000"
				"63000000"
				"0400"
				"00"
				"0100"
				"2b80"
				"0200000001000000eb01000088130000c001000001000000");
		RowsetLayout layout;
		layout.table = "t";
		layout.columns = {Column("id", XTYPE_INT, 4, 1, 0x80),	   Column("", XTYPE_INT, 8, 2, 0x2),
						  Column("v", XTYPE_VARCHAR, -1, 3),	   Column("w", XTYPE_VARCHAR, -2, 4),
						  Column("added", XTYPE_INT, 8, 5, 0x200), Column("added_null", XTYPE_INT, 8, 6)};
		layout.columns[4].has_default = true;
		layout.columns[4].default_bytes = std::string("\x2a\x00\x00\x00", 4);
		const Page page = PageOf({record});
		const RecordView r = Slot(page, 0);
		const size_t vars = RowVarCount(r, layout.table);
		Check(LocateValue(r, vars, SparseVector(), layout, 1).state == ValueState::Null, "a dropped column: Null");
		const ValueRef v = LocateValue(r, vars, SparseVector(), layout, 2);
		Check(v.state == ValueState::OffRow && v.size == 24 && v.data[0] == 2,
			  "a complex column: OffRow, its in-row pointer");
		const ValueRef w = LocateValue(r, vars, SparseVector(), layout, 3);
		Check(w.state == ValueState::Inline && w.size == 0, "a variable column past the record's count: empty");
		const ValueRef added = LocateValue(r, vars, SparseVector(), layout, 4);
		Check(added.state == ValueState::Default && I32(added) == 42,
			  "a column added after the record: its default, not the dropped column's bytes in its place");
		Check(LocateValue(r, vars, SparseVector(), layout, 5).state == ValueState::Null,
			  "added without a default: Null");
	}

	// sysrscols.ti: the length, a decimal's precision and scale, a time type's scale, MAX as 0.
	{
		PhysicalColumn c;
		SetTypeInfo(c, (0x0412u << 8) | XTYPE_DECIMAL);
		Check(c.xtype == XTYPE_DECIMAL && c.precision == 18 && c.scale == 4 && c.length == 0 && c.FixedLength("t") == 9,
			  "0x04126a: decimal(18, 4), 9 bytes");
		SetTypeInfo(c, (0x07u << 8) | XTYPE_DATETIME2);
		Check(c.scale == 7 && c.FixedLength("t") == 8, "0x072a: datetime2(7), 8 bytes");
		SetTypeInfo(c, (0x03u << 8) | XTYPE_DATETIMEOFFSET);
		Check(c.scale == 3 && c.FixedLength("t") == 9, "datetimeoffset(3), 9 bytes");
		SetTypeInfo(c, (0x03u << 8) | XTYPE_TIME);
		Check(c.FixedLength("t") == 4, "time(3), 4 bytes");
		SetTypeInfo(c, (200u << 8) | XTYPE_NVARCHAR);
		Check(c.length == 200 && c.precision == 0, "nvarchar(100): 200 bytes");
		SetTypeInfo(c, XTYPE_VARBINARY);
		Check(c.length == 0, "varbinary(max): length 0");
		const struct {
			uint8_t xtype;
			uint16_t bytes;
		} widths[] = {{XTYPE_TINYINT, 1},	 {XTYPE_BIT, 1},
					  {XTYPE_SMALLINT, 2},	 {XTYPE_DATE, 3},
					  {XTYPE_INT, 4},		 {XTYPE_REAL, 4},
					  {XTYPE_SMALLMONEY, 4}, {XTYPE_SMALLDATETIME, 4},
					  {XTYPE_BIGINT, 8},	 {XTYPE_FLOAT, 8},
					  {XTYPE_MONEY, 8},		 {XTYPE_DATETIME, 8},
					  {XTYPE_TIMESTAMP, 8},	 {XTYPE_UNIQUEIDENTIFIER, 16}};
		bool all = true;
		for (const auto &w : widths) {
			SetTypeInfo(c, w.xtype);
			all = all && c.FixedLength("t") == w.bytes;
		}
		Check(all, "every fixed type's width");
		SetTypeInfo(c, (0x0828u << 8) | XTYPE_DECIMAL);	 // precision 40
		Check(ErrorOf([&]() { c.FixedLength("t"); }).find("precision 40") != std::string::npos,
			  "a decimal of precision 40 is refused by name");
		SetTypeInfo(c, (0x08u << 8) | XTYPE_TIME);
		Check(ErrorOf([&]() { c.FixedLength("t"); }).find("past 7") != std::string::npos,
			  "a time of scale 8 is refused by name");
	}

	// Spec 086 PR 3: the sparse vector, on E24's records (SQL Server 2025). dbo.t_types (id int, r1 varchar(10),
	// sparse columns of hbcolid 3 - 32, r2 nvarchar(10)): the vector is the last variable column, after the
	// regular ones holding a value -- with r1 and r2 NULL, variable column 0.
	{
		auto sparse = [](const char *name, uint8_t xtype, int32_t hbcolid, uint16_t length = 0) {
			PhysicalColumn c = Column(name, xtype, 0, 0, 0x100);
			c.hbcolid = hbcolid;
			c.length = length;
			return c;
		};
		RowsetLayout types;
		types.table = "t_types";
		types.columns = {Column("id", XTYPE_INT, 4, 1, 0x80),	Column("r1", XTYPE_VARCHAR, -1, 2),
						 sparse("s_bit", XTYPE_BIT, 3),			sparse("s_int", XTYPE_INT, 6),
						 sparse("s_char", XTYPE_CHAR, 22, 6),	sparse("s_vc", XTYPE_VARCHAR, 23),
						 sparse("s_nvc", XTYPE_NVARCHAR, 25),	sparse("s_bin", XTYPE_BINARY, 26, 5),
						 sparse("s_vbin", XTYPE_VARBINARY, 27), sparse("s_vcmax", XTYPE_VARCHAR, 28),
						 Column("r2", XTYPE_NVARCHAR, -2, 3)};
		IndexSparse(types);
		Check(types.sparse && types.var_null_bits == std::vector<uint16_t>({2, 3}),
			  "a layout with a sparse column: its variable columns' null bits indexed");
		// Row 3: s_int = 7 alone.
		const char *ONE = "300008000300000003003601001b800500010006000c0007000000";
		// Row 4: '' / N'' / 0x and a char(6), a binary(5) padded: entries of length 0 beside full ones.
		const char *EMPTY =
			"300008000400000003000601003280050005001600170019001a001b001e001e001e0023002300202020202020"
			"0000000000";
		// Row 6: a varchar(max) of 10,000 bytes, its entry an in-row LOB root (bit 15 of its end).
		const char *OFF_ROW =
			"300008000600000003007e01003b80050001001c002c80040000a001000000e14a0000681f0000790100000100"
			"0000102700007801000001000000";
		const Page page = PageOf({Hex(ONE), Hex(EMPTY), Hex(OFF_ROW)});
		auto locate = [&](const RowsetLayout &layout, uint16_t slot, size_t k) {
			const RecordView r = Slot(page, slot);
			const size_t vars = RowVarCount(r, layout.table);
			return LocateValue(r, vars, FindSparseVector(r, vars, layout), layout, k);
		};
		const RecordView one = Slot(page, 0);
		const SparseVector v = FindSparseVector(one, RowVarCount(one, "t"), types);
		Check(v.data && v.count == 1 && v.size == 12, "a vector of one entry at variable column 0");
		Check(I32(locate(types, 0, 3)) == 7 && locate(types, 0, 2).state == ValueState::Null &&
				  locate(types, 0, 1).state == ValueState::Null && locate(types, 0, 10).state == ValueState::Null,
			  "s_int read from its entry; a sparse column with none and the regular NULLs are NULL");
		Check(Text(locate(types, 1, 4)) == "      " && locate(types, 1, 5).state == ValueState::Inline &&
				  locate(types, 1, 5).size == 0 && locate(types, 1, 6).size == 0 && locate(types, 1, 7).size == 5 &&
				  locate(types, 1, 8).state == ValueState::Inline && locate(types, 1, 8).size == 0 &&
				  locate(types, 1, 3).state == ValueState::Null,
			  "an empty value is an entry of length 0, not NULL; a char / binary entry padded to its width");
		const ValueRef lob = locate(types, 2, 9);
		Check(lob.state == ValueState::OffRow && lob.size == 36 && lob.data[0] == 4,
			  "an off-row value inside the vector: OffRow, its 36-byte in-row root");

		// The uniquifier is not stored when 0: the vector at its index is the vector, the uniquifier empty.
		RowsetLayout uniq = types;
		uniq.columns[1] = Column("", XTYPE_INT, -1, 2, 0x10);
		IndexSparse(uniq);
		const RecordView r = Slot(page, 0);
		const RowRecord row = RowRecord::Fixed(r, uniq);
		Check(row.sparse.data && row.var_count == 0 && row.Locate(uniq, 1).size == 0 && I32(row.Locate(uniq, 3)) == 7,
			  "a uniquifier of 0 beside the vector: not stored, and the vector at its index read");

		// An empty r1 ('', its null bit clear) closing the regular columns is not stored: the vector takes its index,
		// and r1 reads empty (review of #626, fixture sparse.sql). Where a tagged column there is no whole vector,
		// it is r1's value, not an error.
		std::vector<uint8_t> empty_r1 = Hex(ONE);
		empty_r1[10] = 0x34;  // r1's null bit cleared
		std::vector<uint8_t> broken_r1 = empty_r1;
		broken_r1[21] = 0x40;  // the vector's one value ends past it
		const Page empties = PageOf({empty_r1, broken_r1});
		const RecordView e0 = Slot(empties, 0), e1 = Slot(empties, 1);
		const RowRecord erow = RowRecord::Fixed(e0, types);
		const ValueRef r1 = erow.Locate(types, 1);
		Check(erow.sparse.data && erow.sparse.count == 1 && erow.var_count == 0 && r1.state == ValueState::Inline &&
				  r1.size == 0 && I32(erow.Locate(types, 3)) == 7,
			  "an empty regular column before the vector: not stored, the vector at its index, the column empty");
		Check(FindSparseVector(e1, RowVarCount(e1, "t"), types).data == nullptr,
			  "a tagged column that is no whole vector where the bitmap says a regular value: that value");
		// There, an id that is no sparse column's: that value too; with r1 NULL the same vector is read.
		std::vector<uint8_t> foreign_id = empty_r1;
		foreign_id[19] = 0x2a;	// the entry's id, 6 -> 42
		std::vector<uint8_t> foreign_null = Hex(ONE);
		foreign_null[19] = 0x2a;
		const Page foreigns = PageOf({foreign_id, foreign_null});
		const RecordView f0 = Slot(foreigns, 0), f1 = Slot(foreigns, 1);
		Check(FindSparseVector(f0, RowVarCount(f0, "t"), types).data == nullptr &&
				  FindSparseVector(f1, RowVarCount(f1, "t"), types).data != nullptr,
			  "an id of no sparse column beside a regular value: that value; beside its NULL, the vector");

		// No regular variable column: the last one must be the vector. With its tag changed it is refused by
		// name; beside a regular column's NULL it is that column, kept (no vector).
		RowsetLayout bare = types;
		bare.columns = {types.columns[0], types.columns[3]};
		IndexSparse(bare);
		std::vector<uint8_t> untagged = Hex(ONE);
		untagged[15] = 0x06;
		const Page bad = PageOf({untagged});
		const RecordView b = Slot(bad, 0);
		Check(ErrorOf([&]() { FindSparseVector(b, RowVarCount(b, "t"), bare); }).find("not a sparse vector") !=
				  std::string::npos,
			  "a variable column past the table's that is not a vector is refused by name");
		Check(FindSparseVector(b, RowVarCount(b, "t"), types).data == nullptr,
			  "an untagged column at a regular column's index is that column (its NULL), no vector");
		// A vector whose ids do not ascend, and one whose value ends past it.
		std::vector<uint8_t> unordered = Hex(EMPTY);
		unordered[19] = 0x30;  // the first id, 22, past the second (23)
		std::vector<uint8_t> past = Hex(ONE);
		past[21] = 0x40;  // the end of its one value, past the vector
		const Page broken = PageOf({unordered, past});
		const RecordView b0 = Slot(broken, 0), b1 = Slot(broken, 1);
		Check(ErrorOf([&]() { FindSparseVector(b0, RowVarCount(b0, "t"), types); }).find("do not ascend") !=
					  std::string::npos &&
				  ErrorOf([&]() { FindSparseVector(b1, RowVarCount(b1, "t"), types); }).find("ends at 64") !=
					  std::string::npos,
			  "a vector's ids that do not ascend and a value past its end are refused by name");
		// A fixed type's entry of another width (an int's 4 bytes declared bigint) is refused, not read short.
		RowsetLayout wide = types;
		wide.columns[3].xtype = XTYPE_BIGINT;
		Check(ErrorOf([&]() { locate(wide, 0, 3); }).find("sparse entry is 4 bytes, where its type takes 8") !=
				  std::string::npos,
			  "a fixed type's entry of another width is refused by name");
	}
	// After ALTER (E24): DROP SPARSE moves y into the fixed part (hbcolid 4) and keeps the old entry under the dropped
	// column's hbcolid 2; ADD SPARSE moves x into the vector under a new hbcolid (5), the old fixed x dropped.
	{
		const char *UNSPARSE = "30000c00010000000100000002000001002680050002000200030010001300010000006f6e65";
		const char *ALTERED = "30000c00010000000a00000003000002001b0027806265666f72650500010005000c000a000000";
		const Page page = PageOf({Hex(UNSPARSE), Hex(ALTERED)});
		PhysicalColumn w = Column("w", XTYPE_VARCHAR, 0, 0, 0x100), old_y = Column("", XTYPE_INT, 0, 0, 0x102);
		w.hbcolid = 3;
		old_y.hbcolid = 2;
		PhysicalColumn y = Column("y", XTYPE_INT, 8, 2);
		y.hbcolid = 4;
		RowsetLayout unsparse;
		unsparse.table = "t_unsparse";
		unsparse.columns = {Column("id", XTYPE_INT, 4, 1, 0x80), y, w, old_y};
		IndexSparse(unsparse);
		const RecordView r0 = Slot(page, 0);
		const RowRecord row0 = RowRecord::Fixed(r0, unsparse);
		Check(I32(row0.Locate(unsparse, 1)) == 1 && Text(row0.Locate(unsparse, 2)) == "one" &&
				  row0.Locate(unsparse, 3).state == ValueState::Null,
			  "DROP SPARSE: y from the fixed part, the dropped column's entry passed over");
		PhysicalColumn old_x = Column("", XTYPE_INT, 8, 2, 0x2), v = Column("v", XTYPE_VARCHAR, -1, 3);
		PhysicalColumn sp = Column("sp", XTYPE_INT, 0, 0, 0x100), x = Column("x", XTYPE_INT, 0, 0, 0x100);
		old_x.hbcolid = 2;
		v.hbcolid = 3;
		sp.hbcolid = 4;
		x.hbcolid = 5;
		RowsetLayout altered;
		altered.table = "t_alter";
		altered.columns = {Column("id", XTYPE_INT, 4, 1, 0x80), x, v, sp, old_x};
		IndexSparse(altered);
		const RecordView r1 = Slot(page, 1);
		const RowRecord row1 = RowRecord::Fixed(r1, altered);
		Check(I32(row1.Locate(altered, 1)) == 10 && Text(row1.Locate(altered, 2)) == "before" &&
				  row1.Locate(altered, 3).state == ValueState::Null && row1.var_count == 1,
			  "ADD SPARSE: x from the vector under its new hbcolid, after v");
	}
	// ANSI_PADDING OFF: '' and 0x read as one space / one zero byte, as the server reads them (DATALENGTH 1).
	{
		PhysicalColumn c = Column("c", XTYPE_CHAR, 4, 1);
		c.unpadded = true;
		const uint8_t spaces[] = {' ', ' ', ' ', ' '}, ab[] = {'a', 'b', ' ', ' '};
		Check(c.Unpadded(spaces, 4) == 1 && c.Unpadded(ab, 4) == 2,
			  "an unpadded char: trailing spaces left out, one kept for ''");
	}

	std::cout << (failures ? "FAILED" : "PASSED") << " (" << failures << " failure(s))" << std::endl;
	return failures ? 1 : 0;
}
