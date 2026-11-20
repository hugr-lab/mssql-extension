// Spec 086 PR 2 (tasks 5-6): PAGE compression -- a page's compression-information record (anchor,
// dictionary) and the values it rebuilds -- on E23's measured bytes (SQL Server 2025, DBCC PAGE (…, 3) of
// PAGE-compressed tables, specs/086-backup-row-formats-and-types/measured/E23-page-compression.md): every
// value below is what the server printed for it. And the records and records the reader refuses. A ghost
// forwarded record (E23: a forwarded record deleted under row versioning). DuckDB-free.

#include <cstdint>
#include <cstdio>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

#include "backup/backup_bytes.hpp"
#include "backup/backup_cd_record.hpp"
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

std::vector<uint8_t> Hex(const std::string &hex) {
	std::vector<uint8_t> out;
	for (size_t i = 0; i + 1 < hex.size(); i += 2) {
		out.push_back(static_cast<uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
	}
	return out;
}

std::string Text(const ValueRef &v) {
	return v.state == ValueState::Null ? "NULL" : std::string(reinterpret_cast<const char *>(v.data), v.size);
}

PhysicalColumn Column(const std::string &name, uint8_t xtype, int16_t offset, uint16_t length = 0) {
	PhysicalColumn c;
	c.name = name;
	c.xtype = xtype;
	c.offset = offset;
	c.length = length;
	return c;
}

//! An 8 KB page: its compression information at 96 and the records after it, a slot each.
struct TestPage {
	std::vector<uint8_t> bytes = std::vector<uint8_t>(8192, 0);
	std::vector<uint16_t> slots;

	TestPage(const std::string &ci, const std::vector<std::string> &records) {
		auto c = Hex(ci);
		std::copy(c.begin(), c.end(), bytes.begin() + 96);
		size_t at = 96 + c.size();
		for (const auto &r : records) {
			auto b = Hex(r);
			std::copy(b.begin(), b.end(), bytes.begin() + at);
			slots.push_back(static_cast<uint16_t>(at));
			at += b.size();
		}
		for (size_t s = 0; s < slots.size(); s++) {
			bytes[8192 - 2 - 2 * s] = static_cast<uint8_t>(slots[s]);
			bytes[8192 - 1 - 2 * s] = static_cast<uint8_t>(slots[s] >> 8);
		}
	}
	size_t SlotArray() const {
		return 8192 - 2 * slots.size();
	}
};

std::string ErrorOf(const std::function<void()> &f) {
	try {
		f();
	} catch (FormatError &e) {
		return e.what();
	}
	return "";
}

// E23's t_dict page: (id int, c varchar(20), d int, e nvarchar(20), f char(10)), its anchor ('charlie' for c,
// 'zebra' for e) and a dictionary of 16 entries.
const char *DICT_CI =
	"060000180076000105806010636861726c69657a6562726110002400260029002c002f003200350038003b003e0041004600"
	"4c00520058005e00787879798186a0830d408493e0861a8087a1208927c08aae608c35008dbba000e5652c670012b6c3"
	"ba1000616c70686100627261766f0064656c7461";

}  // namespace

int main() {
	std::cout << "a dictionary page (E23's t_dict)" << std::endl;
	{
		TestPage page(DICT_CI, {"0105c2cc1c810e020b01", "0105121c1182036f89"});
		PageCompression ci;
		ReadPageCompression(page.bytes.data(), 8192, page.SlotArray(), "t_dict", ci);
		Check(ci.present && ci.has_anchor && ci.dictionary.size() == 16 && ci.end == 96 + 118,
			  "the record: an anchor, 16 entries, 118 bytes");
		RowsetLayout layout;
		layout.table = "t_dict";
		layout.compression = 2;
		layout.columns = {Column("id", XTYPE_INT, -1), Column("c", XTYPE_VARCHAR, -2, 20), Column("d", XTYPE_INT, -3),
						  Column("e", XTYPE_NVARCHAR, -4, 40), Column("f", XTYPE_CHAR, -5, 10)};
		CdRecord record;
		CdRow row;
		ReadCdRecord(page.bytes.data(), 8192, page.SlotArray(), page.slots[0], "t_dict", record);
		row.Read(record, layout, nullptr, &ci);
		const ValueRef &d = row.Value(2);
		Check(Text(row.Value(1)) == "bravo", "slot 0: c a symbol: 'bravo' (its entry '\\0bravo': a prefix of 0)");
		Check(d.size == 4 && (d.data[0] | (d.data[1] << 8) | (d.data[2] << 16)) == 100000,
			  "slot 0: d a symbol: 100000");
		Check(Text(row.Value(3)) == std::string("\xe5\x65\x2c\x67", 4), "slot 0: e a symbol: N'日本' (UTF-16LE)");
		Check(Text(row.Value(4)) == "yy        ", "slot 0: f a symbol: 'yy', padded to char(10)");
		ReadCdRecord(page.bytes.data(), 8192, page.SlotArray(), page.slots[1], "t_dict", record);
		row.Read(record, layout, nullptr, &ci);
		Check(Text(row.Value(1)) == "charlie", "slot 1: c EMPTY beside an anchor: the anchor's value whole");
		Check(Text(row.Value(3)) == std::string("z\0e\0b\0r\0a\0", 10), "slot 1: e EMPTY: the anchor's N'zebra'");
		Check(Text(row.Value(4)) == "          ", "slot 1: f EMPTY with no anchor: an empty char(10)");
		// With no compression information the symbol is refused, as on a ROW page.
		Check(
			ErrorOf([&] { row.Read(record, layout); }).find("page-dictionary symbol outside PAGE") != std::string::npos,
			"a symbol read with no compression information is refused");
	}
	std::cout << "a two-byte prefix length (E23's t_long)" << std::endl;
	{
		// (id int, s varchar(400), b varbinary(300)): the anchor 'p' x 200 + '-10' and 'q' x 150 + '10'; slot 0
		// 's' a prefix of 0x80ca = 202 bytes and no suffix, 'b' a prefix of 0x0097 = 151... as the server read it.
		std::string anchor_s(200, 'p');
		anchor_s += "-10";
		std::string anchor_b(150, 'q');
		anchor_b += "10";
		std::string hex_s, hex_b;
		char buf[3];
		for (unsigned char ch : anchor_s) {
			snprintf(buf, sizeof(buf), "%02x", ch);
			hex_s += buf;
		}
		for (unsigned char ch : anchor_b) {
			snprintf(buf, sizeof(buf), "%02x", ch);
			hex_b += buf;
		}
		// The anchor record: header 0x21 (a long region), 3 columns, codes NULL / LONG / LONG, its long region.
		char ends[16];
		snprintf(ends, sizeof(ends), "%02x%02x%02x%02x", static_cast<unsigned>(anchor_s.size() & 0xff),
				 static_cast<unsigned>(anchor_s.size() >> 8),
				 static_cast<unsigned>((anchor_s.size() + anchor_b.size()) & 0xff),
				 static_cast<unsigned>((anchor_s.size() + anchor_b.size()) >> 8));
		const std::string anchor = std::string("2103a00a") + "010200" + ends + hex_s + hex_b;
		const size_t size = 5 + anchor.size() / 2;
		char head[16];
		snprintf(head, sizeof(head), "020000%02x%02x", static_cast<unsigned>(size & 0xff),
				 static_cast<unsigned>(size >> 8));
		TestPage page(std::string(head) + anchor, {"010332138180ca8097"});
		PageCompression ci;
		ReadPageCompression(page.bytes.data(), 8192, page.SlotArray(), "t_long", ci);
		RowsetLayout layout;
		layout.table = "t_long";
		layout.compression = 2;
		layout.columns = {Column("id", XTYPE_INT, -1), Column("s", XTYPE_VARCHAR, -2, 400),
						  Column("b", XTYPE_VARBINARY, -3, 300)};
		CdRecord record;
		CdRow row;
		ReadCdRecord(page.bytes.data(), 8192, page.SlotArray(), page.slots[0], "t_long", record);
		row.Read(record, layout, nullptr, &ci);
		Check(Text(row.Value(1)) == std::string(200, 'p') + "-1",
			  "s: 202 bytes of the anchor (0x80ca), as the server read it");
		Check(Text(row.Value(2)) == std::string(150, 'q') + "1", "b: 151 bytes of the anchor (0x8097)");
	}
	std::cout << "a ghost forwarded record (E23: deleted under row versioning)" << std::endl;
	{
		TestPage page("", {"17003002000001006000080000000900"
						   "1b00210400000000"});
		CdRecord record;
		ReadCdRecord(page.bytes.data(), 8192, page.SlotArray(), 96, "h3", record);
		Check(record.kind == CdRecord::Kind::Ghost && record.end == 24, "type 5: a ghost of 24 bytes (no row)");
	}
	std::cout << "what is refused" << std::endl;
	{
		auto ci_error = [](const std::string &ci) {
			TestPage page(ci, {});
			PageCompression out;
			return ErrorOf([&] { ReadPageCompression(page.bytes.data(), 8192, 8192, "t", out); });
		};
		Check(ci_error("0a0000050000").find("flags 10") != std::string::npos, "a flag the reader does not know");
		Check(ci_error("02000000200000").find("runs past") != std::string::npos, "a size past the page's records");
		Check(ci_error("040000ff00760000").find("places its dictionary") != std::string::npos,
			  "a dictionary placed past the record");
		// Two entries, their ends descending.
		Check(ci_error("040000070011000200000a0008000000").find("dictionary entry") != std::string::npos,
			  "dictionary entries whose ends descend");
		TestPage page(DICT_CI, {"0105c2cc1c81ff020b01"});
		PageCompression ci;
		ReadPageCompression(page.bytes.data(), 8192, page.SlotArray(), "t_dict", ci);
		RowsetLayout layout;
		layout.table = "t_dict";
		layout.compression = 2;
		layout.columns = {Column("id", XTYPE_INT, -1), Column("c", XTYPE_VARCHAR, -2, 20), Column("d", XTYPE_INT, -3),
						  Column("e", XTYPE_NVARCHAR, -4, 40), Column("f", XTYPE_CHAR, -5, 10)};
		CdRecord record;
		CdRow row;
		ReadCdRecord(page.bytes.data(), 8192, page.SlotArray(), page.slots[0], "t_dict", record);
		Check(ErrorOf([&] { row.Read(record, layout, nullptr, &ci); }).find("symbol 255") != std::string::npos,
			  "a symbol past the dictionary, named");
		TestPage longer(DICT_CI, {"0105c2cc1c8114020b01"});	 // c: symbol 0x14 = 20 > 15
		ReadPageCompression(longer.bytes.data(), 8192, longer.SlotArray(), "t_dict", ci);
		ReadCdRecord(longer.bytes.data(), 8192, longer.SlotArray(), longer.slots[0], "t_dict", record);
		Check(!ErrorOf([&] { row.Read(record, layout, nullptr, &ci); }).empty(), "another symbol past the dictionary");
		// A value whose prefix is longer than the anchor's value: c (anchor 'charlie', 7 bytes) of prefix 9.
		TestPage prefixed(DICT_CI, {"010332008109610000"});	 // id = 1 (code 2); c: 2 bytes, prefix 9 then 'a'; d NULL
		ReadPageCompression(prefixed.bytes.data(), 8192, prefixed.SlotArray(), "t_dict", ci);
		RowsetLayout three = layout;
		three.columns.resize(3);
		ReadCdRecord(prefixed.bytes.data(), 8192, prefixed.SlotArray(), prefixed.slots[0], "t_dict", record);
		Check(ErrorOf([&] { row.Read(record, three, nullptr, &ci); }).find("prefix of 9 bytes") != std::string::npos,
			  "a prefix past the anchor's value, named");
	}
	std::cout << (failures ? "FAILED" : "all passed") << std::endl;
	return failures ? 1 : 0;
}
