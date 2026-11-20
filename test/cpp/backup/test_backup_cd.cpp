// Spec 086 PR 1: the CD record (ROW compression) and its values, against the records E22 read off SQL
// Server 2022's pages (specs/086-backup-row-formats-and-types/measured/E22-cd-records.md) and the
// uncompressed forms those values have. No SQL Server, no DuckDB: ReadCdRecord walks the record, CdRow
// puts each value in the form a FixedVar record holds it, and a malformed record is a named error.

#include <cstring>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

#include "backup/backup_cd_record.hpp"

using namespace duckdb::mssql::backup;

namespace {

int failures = 0;

void Check(bool ok, const std::string &what) {
	std::cout << (ok ? "  ok   " : "  FAIL ") << what << std::endl;
	if (!ok) {
		failures++;
	}
}

std::vector<uint8_t> Hex(const std::string &hex) {
	std::vector<uint8_t> out;
	for (size_t i = 0; i + 1 < hex.size(); i += 2) {
		out.push_back(static_cast<uint8_t>(std::stoi(hex.substr(i, 2), nullptr, 16)));
	}
	return out;
}

std::string ToHex(const uint8_t *data, size_t size) {
	static const char *digits = "0123456789abcdef";
	std::string out;
	for (size_t i = 0; i < size; i++) {
		out += digits[data[i] >> 4];
		out += digits[data[i] & 15];
	}
	return out;
}

constexpr size_t PAGE = 8192;

//! A page with `record` at offset 96 and one slot; the record's CD walk.
struct OnPage {
	std::vector<uint8_t> page = std::vector<uint8_t>(PAGE, 0);
	CdRecord record;
	explicit OnPage(const std::vector<uint8_t> &bytes, size_t limit = 0) {
		std::memcpy(page.data() + 96, bytes.data(), bytes.size());
		const size_t slot_array = limit ? 96 + limit : PAGE - 2;
		ReadCdRecord(page.data(), PAGE, slot_array, 96, "the test record", record);
	}
};

PhysicalColumn Column(const std::string &name, uint8_t xtype, int16_t offset, uint16_t length = 0, uint8_t precision = 0,
					  uint8_t scale = 0) {
	PhysicalColumn c;
	c.name = name;
	c.xtype = xtype;
	c.offset = offset;
	c.length = length;
	c.precision = precision;
	c.scale = scale;
	return c;
}

PhysicalColumn Unpadded(PhysicalColumn c) {
	c.unpadded = true;
	return c;
}

//! (id int, v <column>): v's value in record `hex`, put in its uncompressed form, as hex.
std::string Value(const std::string &hex, const PhysicalColumn &v) {
	OnPage p(Hex(hex));
	RowsetLayout layout;
	layout.table = "t";
	layout.compression = 1;
	layout.columns.push_back(Column("id", XTYPE_INT, -1));
	layout.columns.push_back(v);
	CdRow row;
	row.Read(p.record, layout);
	const ValueRef &r = row.Value(1);
	if (r.state == ValueState::Null) {
		return "NULL";
	}
	return ToHex(r.data, r.size);
}

//! A two-column record (id = 3, then v) of v's CD code and short or long bytes.
std::string Record(uint8_t code, const std::string &bytes) {
	std::string hex;
	const bool is_long = code == CD_LONG;
	hex += is_long ? "21" : "01";
	hex += "02";
	char cd[3];
	snprintf(cd, sizeof(cd), "%x2", code);
	hex += cd;
	hex += "83";  // id = 3
	if (is_long) {
		char end[5];
		const size_t n = bytes.size() / 2;
		snprintf(end, sizeof(end), "%02x%02x", static_cast<unsigned>(n & 0xFF), static_cast<unsigned>(n >> 8));
		hex += std::string("0101") + "00" + end + bytes;
	} else {
		hex += bytes;
	}
	while (hex.size() < 18) {
		hex += "00";  // padded to 9 bytes, as every record is
	}
	return hex;
}

bool Throws(const std::function<void()> &f, const std::string &needle) {
	try {
		f();
	} catch (FormatError &e) {
		if (std::string(e.what()).find(needle) == std::string::npos) {
			std::cout << "    (message: " << e.what() << ")" << std::endl;
			return false;
		}
		return true;
	}
	return false;
}

}  // namespace

int main() {
	std::cout << "test_backup_cd (spec 086 PR 1)" << std::endl;

	// The records E22 read, verbatim.
	{
		OnPage p(Hex("010222838100000080"));  // id 3, v int 1, padded to 9 bytes
		Check(p.record.kind == CdRecord::Kind::Row && p.record.column_count == 2 && p.record.values[0].code == 2 &&
				  p.record.values[1].code == 2 && p.record.values[1].offset == 4 && p.record.end == 9,
			  "a primary record: two one-byte values, padded to 9 bytes");
	}
	{
		// 31 columns + id: two clusters, the cluster array's 0x19 = 25 short bytes in the first.
		OnPage p(Hex("012022220222222022022222202202222220198181828384867988898b8c8d72909192936b9697989a9b649d9f"));
		Check(p.record.column_count == 32 && p.record.values[0].offset == 19 && p.record.values[31].code == 2,
			  "32 columns: the CD array, a cluster array of one byte, then the short region");
	}
	{
		// 70 columns, long values in columns 6, 7, 41, 42 and 66: the long region's per-cluster counts (02 02).
		std::vector<uint8_t> codes(70, 0);
		codes[0] = 2;
		for (int c : {6, 7, 41, 42, 66}) {
			codes[c - 1] = CD_LONG;
		}
		std::string cd;
		for (size_t i = 0; i < 70; i += 2) {
			char b[3];
			snprintf(b, sizeof(b), "%x%x", codes[i + 1], codes[i]);
			cd += b;
		}
		// id (code 2) is in cluster 0: its short byte counts there (the cluster array 01 00).
		std::string rec = "2146" + cd + "0100" + "81" + "010500" + "0a0014001e0028003200" + "0202";
		for (char ch : std::string("abcde")) {
			for (int k = 0; k < 10; k++) {
				char b[3];
				snprintf(b, sizeof(b), "%02x", static_cast<unsigned>(ch));
				rec += b;
			}
		}
		OnPage p(Hex(rec));
		const CdValue &v41 = p.record.values[40];
		Check(v41.code == CD_LONG && v41.size == 10 && p.record.data[v41.offset] == 'c',
			  "a long region over three clusters: the end offsets, then a long count per cluster but the last");
	}
	{
		// A forwarded record (0x31): its back pointer is its last 8 bytes.
		OnPage p(Hex("3102a28301010004006162636490010000010035" "00"));
		uint32_t page = 0;
		uint16_t file = 0, slot = 0;
		p.record.BackPointer(page, file, slot);
		Check(p.record.kind == CdRecord::Kind::Forwarded && page == 400 && file == 1 && slot == 53,
			  "a forwarded record: page 400, file 1, slot 53 from its last 8 bytes");
	}
	{
		OnPage stub(Hex("099101000001000000"));
		OnPage ghost(Hex("0d0232846734000000"));
		Check(stub.record.kind == CdRecord::Kind::Stub && ghost.record.kind == CdRecord::Kind::Ghost,
			  "a forwarding stub (0x09) and a ghost (type 6) are no rows of their own");
	}

	{
		// Row versioning (RCSI): header bit 1, a 14-byte version tag at the end of every record.
		OnPage row(Hex("03022282940800000009000e00a60300000000"));
		OnPage ghost(Hex("0f022281810800000009000f00a70300000000"));
		OnPage version_ghost(Hex("070800000009000000900300000000"));
		Check(row.record.kind == CdRecord::Kind::Row && row.record.end == 19 && row.record.values[1].offset == 4 &&
				  ghost.record.kind == CdRecord::Kind::Ghost && version_ghost.record.kind == CdRecord::Kind::Ghost &&
				  version_ghost.record.end == 15,
			  "row versioning: a tagged row, a tagged ghost, a version ghost (the header and its tag only)");
		// A tagged forwarded record (0x33): its back pointer after its data, then the tag.
		OnPage fwd(Hex("3302a28301010004006162636490010000010035" "00" "0800000009000a009f0300000000"));
		uint32_t page = 0;
		uint16_t file = 0, slot = 0;
		fwd.record.BackPointer(page, file, slot);
		Check(fwd.record.kind == CdRecord::Kind::Forwarded && page == 400 && file == 1 && slot == 53 &&
				  fwd.record.end == 35,
			  "a tagged forwarded record: the back pointer before the version tag");
	}

	// Values in their uncompressed form.
	struct Case {
		std::string what;
		std::string record;
		PhysicalColumn column;
		std::string expected;
	};
	const std::vector<Case> cases = {
		{"int 1", Record(2, "81"), Column("v", XTYPE_INT, -2), "01000000"},
		{"int 0 (EMPTY)", Record(1, ""), Column("v", XTYPE_INT, -2), "00000000"},
		{"int -2147483648", Record(5, "00000000"), Column("v", XTYPE_INT, -2), "00000080"},
		{"smallint -129", Record(3, "7f7f"), Column("v", XTYPE_SMALLINT, -2), "7fff"},
		{"tinyint 128, not flipped", Record(2, "80"), Column("v", XTYPE_TINYINT, -2), "80"},
		{"bigint 1099511627776", Record(7, "810000000000"), Column("v", XTYPE_BIGINT, -2), "0000000000010000"},
		{"money 12.34", Record(4, "81e208"), Column("v", XTYPE_MONEY, -2), "08e2010000000000"},
		{"smallmoney -1", Record(3, "58f0"), Column("v", XTYPE_SMALLMONEY, -2), "f0d8ffff"},
		{"bit 1 (code 11)", Record(11, ""), Column("v", XTYPE_BIT, -2), "01"},
		{"real 0.5", Record(2, "3f"), Column("v", XTYPE_REAL, -2), "0000003f"},
		{"float 1.0", Record(3, "f03f"), Column("v", XTYPE_FLOAT, -2), "000000000000f03f"},
		{"decimal(5, 2) 123.45", Record(5, "c21edc20"), Column("v", XTYPE_DECIMAL, -2, 0, 5, 2), "0139300000"},
		{"decimal(5, 2) -0.01", Record(3, "3e19"), Column("v", XTYPE_DECIMAL, -2, 0, 5, 2), "0001000000"},
		{"decimal(9, 3) 0.012", Record(3, "be1e"), Column("v", XTYPE_DECIMAL, -2, 0, 9, 3), "010c000000"},
		{"decimal(18, 4) 12345678901234.5678", Record(CD_LONG, "cd1edc8c540c566a60"),
		 Column("v", XTYPE_DECIMAL, -2, 0, 18, 4), "014ef330a64b9bb601"},  // 123456789012345678
		{"decimal(38, 10) -(10^38 - 1) / 10^10", Record(CD_LONG, "5bf9fe7f9fe7f9fe7f9fe7f9fe7f9fe7f780"),
		 Column("v", XTYPE_DECIMAL, -2, 0, 38, 10), "00ffffffff3f228a097ac4865aa84c3b4b"},
		{"datetime 2026-11-16 10:11:12.123", Record(8, "80b50400a7df25"), Column("v", XTYPE_DATETIME, -2),
		 "25dfa70004b50000"},
		{"datetime 1899-12-31 23:59:59.997", Record(6, "7f018b81ff"), Column("v", XTYPE_DATETIME, -2), "ff818b01ffffffff"},
		{"smalldatetime 1900-12-31 (the day alone)", Record(3, "6c01"), Column("v", XTYPE_SMALLDATETIME, -2), "00006c01"},
		{"date as it is", Record(4, "42240b"), Column("v", XTYPE_DATE, -2), "42240b"},
		{"char(10) 'abc', padded", Record(4, "616263"), Column("v", XTYPE_CHAR, -2, 10), "61626320202020202020"},
		{"binary(8) 0x01, padded", Record(2, "01"), Column("v", XTYPE_BINARY, -2, 8), "0100000000000000"},
		{"nchar(10) 'жук' (SCSU), padded", Record(6, "12b6c3ba10"), Column("v", XTYPE_NCHAR, -2, 20),
		 "3604430" "43a04" "2000200020002000200020002000"},
		{"nvarchar 'mixedЖ日𝄞end' (SCSU: SQ2, SQU)", Record(CD_LONG, "6d6978656403960e65e50ed8340edd1e656e64"),
		 Column("v", XTYPE_NVARCHAR, -2, 80), "6d0069007800650064001604e56534d81edd65006e006400"},
		{"nvarchar '𝄞𝄞' (even: UTF-16 as it is)", Record(9, "34d81edd34d81edd"), Column("v", XTYPE_NVARCHAR, -2, 80),
		 "34d81edd34d81edd"},
		{"varchar keeps its trailing spaces", Record(7, "616263202020"), Column("v", XTYPE_VARCHAR, -2, 20),
		 "616263202020"},
		{"NULL", Record(0, ""), Column("v", XTYPE_INT, -2), "NULL"},
		{"rowversion 0x7D1 (big-endian, leading zeros left out)", Record(3, "07d1"), Column("v", XTYPE_TIMESTAMP, -2),
		 "00000000000007d1"},
		{"binary(8) NULL under ANSI_PADDING OFF: as stored", Record(2, "01"), Unpadded(Column("v", XTYPE_BINARY, -2, 8)),
		 "01"},
	};
	for (const auto &c : cases) {
		std::string got;
		try {
			got = Value(c.record, c.column);
		} catch (std::exception &e) {
			got = std::string("error: ") + e.what();
		}
		Check(got == c.expected, c.what + (got == c.expected ? "" : " (got " + got + ")"));
	}

	{
		// A column added after the record was written: its default.
		OnPage p(Hex(Record(2, "81")));
		RowsetLayout layout;
		layout.table = "t";
		layout.compression = 1;
		layout.columns.push_back(Column("id", XTYPE_INT, -1));
		layout.columns.push_back(Column("v", XTYPE_INT, -2));
		PhysicalColumn added = Column("added", XTYPE_INT, -3);
		added.has_default = true;
		added.default_bytes = std::string("\x4d\x00\x00\x00", 4);
		layout.columns.push_back(added);
		CdRow row;
		row.Read(p.record, layout);
		Check(row.Value(2).state == ValueState::Default && row.Value(2).size == 4 && row.Value(2).data[0] == 77,
			  "a column past the record's count: its default");
	}

	{
		// Review of #569: `needed` -- a column not needed is located only (its bytes as stored), the
		// others converted; a column past the vector's end counts as not needed.
		OnPage p(Hex(Record(2, "81")));	 // id = 3, v = 1 (one byte, the sign bit flipped)
		RowsetLayout layout;
		layout.table = "t";
		layout.compression = 1;
		layout.columns.push_back(Column("id", XTYPE_INT, -1));
		layout.columns.push_back(Column("v", XTYPE_INT, -2));
		const auto hex = [](const ValueRef &r) { return ToHex(r.data, r.size); };
		CdRow row;
		const std::vector<bool> only_v = {false, true};
		row.Read(p.record, layout, &only_v);
		Check(row.Value(0).state == ValueState::Inline && hex(row.Value(0)) == "83" && hex(row.Value(1)) == "01000000",
			  "needed v only: v converted, id located as stored");
		const std::vector<bool> only_id = {true};
		row.Read(p.record, layout, &only_id);
		Check(hex(row.Value(0)) == "03000000" && row.Value(1).state == ValueState::Inline && hex(row.Value(1)) == "81",
			  "needed id only (v past the vector): id converted, v located as stored");
		row.Read(p.record, layout);
		Check(hex(row.Value(0)) == "03000000" && hex(row.Value(1)) == "01000000", "no `needed`: every column converted");
	}

	// Malformed records: named, never read past.
	Check(Throws([] { OnPage p(Hex("010222838100000080"), 3); }, "runs past"), "a record past its limit");
	Check(Throws([] { OnPage p(Hex("0102d2838100000080")); }, "CD code 13"), "a CD code no value has");
	Check(Throws([] { OnPage p(Hex("1902228381000000")); }, "record type 6"),
		  "a record type the reader does not know (6; 5 is a ghost forwarded record, E23)");
	Check(Throws([] { OnPage p(Hex("4102228381000000")); }, "not a CD record the reader knows"),
		  "a header bit the reader does not know (0x40)");
	Check(Throws([] { Value(Record(6, "0ff2610010"), Column("v", XTYPE_NVARCHAR, -2, 80)); }, "reserved SCSU tag 242"),
		  "the reserved SCSU tag 0xF2 in Unicode mode");
	Check(Throws([] { OnPage p(Hex("012022220222222022022222202202222220188181828384867988898b8c8d72909192936b9697989a9b649d9f")); },
				 "cluster array says"),
		  "a cluster array that disagrees with the CD array");
	Check(Throws([] { OnPage p(Hex("2102a283010200040061626364")); }, "long region lists"),
		  "a long region that lists another count than the CD array");
	Check(Throws([] { Value(Record(3, "c019"), Column("v", XTYPE_DECIMAL, -2, 0, 5, 0)); }, "past its precision") ==
				  false &&
			  Throws([] { Value(Record(CD_LONG, "e5f9fe7f9fe7f9fe7f9fe7f9fe7f9fe7f780"), Column("v", XTYPE_DECIMAL, -2, 0, 9, 0)); },
					 "past its precision"),
		  "a vardecimal value past its column's precision");
	Check(Throws([] { Value(Record(4, "616e0e"), Column("v", XTYPE_NVARCHAR, -2, 80)); }, "inside an SCSU sequence"),
		  "a truncated SCSU sequence");
	Check(Throws([] { Value(Record(12, ""), Column("v", XTYPE_INT, -2)); }, "page-dictionary symbol"),
		  "a page-dictionary symbol outside PAGE compression");

	if (failures) {
		std::cout << failures << " check(s) failed" << std::endl;
		return 1;
	}
	std::cout << "all checks passed" << std::endl;
	return 0;
}
