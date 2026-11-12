// Spec 085 PR 2d, task 14: the record forms of user tables (E20), on records copied from the E20
// backup (SQL Server 2022, row versioning on) or built after them: a version tag after the record, the
// uniquifier, bit columns sharing a byte, a forwarded record's back pointer, the stub and the version
// ghost a page also holds (neither split as FixedVar, neither a row), every state LocateValue answers,
// and sysrscols.ti's fields.
#include <functional>
#include <iostream>
#include <string>
#include <vector>

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
			const ValueRef id = LocateValue(r, vars, layout, 0), v = LocateValue(r, vars, layout, 1),
						   s = LocateValue(r, vars, layout, 2);
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
		const ValueRef u = LocateValue(r, vars, layout, 0), k = LocateValue(r, vars, layout, 1),
					   v = LocateValue(r, vars, layout, 2), n = LocateValue(r, vars, layout, 3);
		Check(u.state == ValueState::Inline && u.size == 0 && I32(k) == 0 && Text(v) == "u10" && I32(n) == 10,
			  "the first row of a key: an empty uniquifier, then the table's columns");
		// The same record with uniquifier 3 (4 bytes before "u10").
		std::vector<uint8_t> second = Hex(UNIQ_FIRST);
		second[17] = 0x19;	// the uniquifier now ends at 25
		second[19] = 0x1c;	// and v at 28
		second.insert(second.begin() + 21, {0x03, 0x00, 0x00, 0x00});
		const Page next = PageOf({second});
		const RecordView r2 = Slot(next, 0);
		const ValueRef u2 = LocateValue(r2, RowVarCount(r2, layout.table), layout, 0);
		const ValueRef v2 = LocateValue(r2, RowVarCount(r2, layout.table), layout, 2);
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
		Check(Text(LocateValue(r, 1, layout, 1)) == "ab", "its own column reads");
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
		Check(LocateValue(r, vars, layout, 1).state == ValueState::Null, "a dropped column: Null");
		const ValueRef v = LocateValue(r, vars, layout, 2);
		Check(v.state == ValueState::OffRow && v.size == 24 && v.data[0] == 2,
			  "a complex column: OffRow, its in-row pointer");
		const ValueRef w = LocateValue(r, vars, layout, 3);
		Check(w.state == ValueState::Inline && w.size == 0, "a variable column past the record's count: empty");
		const ValueRef added = LocateValue(r, vars, layout, 4);
		Check(added.state == ValueState::Default && I32(added) == 42,
			  "a column added after the record: its default, not the dropped column's bytes in its place");
		Check(LocateValue(r, vars, layout, 5).state == ValueState::Null, "added without a default: Null");
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

	std::cout << (failures ? "FAILED" : "PASSED") << " (" << failures << " failure(s))" << std::endl;
	return failures ? 1 : 0;
}
