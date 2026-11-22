// Spec 093 T0: the COLMETADATA and rows of UDT (0xF0) and sql_variant (0x62) columns, as SQL Server
// 2022 sent them (experiment G2, specs/093-type-kernels/measured/G2.md: a throwaway container, synthetic
// values). The parser takes each response's TYPE_INFO -- UDT_INFO's schema and type name, the variant's
// LONGLEN maximum -- and the row reader frames every value (UDT as PLP, hierarchyid's MAX_BYTE_SIZE of
// 892 included; sql_variant behind a u32 length), ROW and NBCROW alike, to the bytes the server's own
// CAST(x AS varbinary(max)) gives. No SQL Server; DuckDB only for the vector the last case decodes into.

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "codec/variant/variant_codec.hpp"
#include "duckdb/common/types/vector.hpp"
#include "tds/encoding/type_converter.hpp"
#include "tds/tds_column_metadata.hpp"
#include "tds/tds_row_reader.hpp"
#include "tds/tds_token_parser.hpp"

using namespace duckdb::tds;

namespace {

int failures = 0;

void Check(bool ok, const std::string &what) {
	std::cout << (ok ? "  ok   " : "  FAIL ") << what << std::endl;
	if (!ok) {
		failures++;
	}
}

std::vector<uint8_t> FromHex(const std::string &h) {
	std::vector<uint8_t> b;
	for (size_t i = 0; i + 1 < h.size(); i += 2) {
		b.push_back(static_cast<uint8_t>(std::stoi(h.substr(i, 2), nullptr, 16)));
	}
	return b;
}

std::string ToHex(const std::vector<uint8_t> &b) {
	static const char *digits = "0123456789ABCDEF";
	std::string h;
	for (uint8_t c : b) {
		h.push_back(digits[c >> 4]);
		h.push_back(digits[c & 15]);
	}
	return h;
}

struct Response {
	const char *query;
	const char *payload;  // the response's token stream, packet headers removed
};

const Response RESPONSES[] = {
	{"SELECT id, g FROM dbo.t ORDER BY id",
	 "810200000000000800380269006400000000000900f0ffff02670032000373007900730008670065006f006d00650074"
	 "007200790084004d006900630072006f0073006f00660074002e00530071006c005300650072007600650072002e0054"
	 "0079007000650073002e00530071006c00470065006f006d0065007400720079002c0020004d006900630072006f0073"
	 "006f00660074002e00530071006c005300650072007600650072002e00540079007000650073002c0020005600650072"
	 "00730069006f006e003d00310031002e0030002e0030002e0030002c002000430075006c0074007500720065003d006e"
	 "00650075007400720061006c002c0020005000750062006c00690063004b006500790054006f006b0065006e003d0038"
	 "00390038003400350064006300640038003000380030006300630039003100016700a902000100d10100000016000000"
	 "000000001600000000000000010c000000000000f03f000000000000004000000000d20202000000d103000000360000"
	 "000000000036000000110f00000115000000000000000000000000000000000000000000000840000000000000104000"
	 "0000000000f03f000000000000004000000000d104000000700000000000000070000000000000000104050000000000"
	 "000000000000000000000000000000000000000010400000000000000000000000000000104000000000000010400000"
	 "00000000000000000000000010400000000000000000000000000000000001000000020000000001000000ffffffff00"
	 "0000000300000000d20205000000fd1000c1000500000000000000"},
	{"SELECT id, gg FROM dbo.t ORDER BY id",
	 "810200000000000800380269006400000000000900f0ffff02670032000373007900730009670065006f006700720061"
	 "0070006800790085004d006900630072006f0073006f00660074002e00530071006c005300650072007600650072002e"
	 "00540079007000650073002e00530071006c00470065006f006700720061007000680079002c0020004d006900630072"
	 "006f0073006f00660074002e00530071006c005300650072007600650072002e00540079007000650073002c00200056"
	 "0065007200730069006f006e003d00310031002e0030002e0030002e0030002c002000430075006c0074007500720065"
	 "003d006e00650075007400720061006c002c0020005000750062006c00690063004b006500790054006f006b0065006e"
	 "003d0038003900380034003500640063006400380030003800300063006300390031000267006700a902000100d10100"
	 "0000160000000000000016000000e6100000010c0000000000004940000000000000244000000000d20202000000d103"
	 "000000260000000000000026000000e61000000114000000000000494000000000000024400000000000804940000000"
	 "000000264000000000d20204000000d20205000000fd1000c1000500000000000000"},
	{"SELECT id, h FROM dbo.t ORDER BY id",
	 "810200000000000800380269006400000000000900f07c030267003200037300790073000b6800690065007200610072"
	 "006300680079006900640087004d006900630072006f0073006f00660074002e00530071006c00530065007200760065"
	 "0072002e00540079007000650073002e00530071006c00480069006500720061007200630068007900490064002c0020"
	 "004d006900630072006f0073006f00660074002e00530071006c005300650072007600650072002e0054007900700065"
	 "0073002c002000560065007200730069006f006e003d00310031002e0030002e0030002e0030002c002000430075006c"
	 "0074007500720065003d006e00650075007400720061006c002c0020005000750062006c00690063004b006500790054"
	 "006f006b0065006e003d003800390038003400350064006300640038003000380030006300630039003100016800a902"
	 "000100d1010000000200000000000000020000005b4000000000d20202000000d1030000000000000000000000000000"
	 "00d1040000000300000000000000030000006467d000000000d20205000000fd1000c1000500000000000000"},
	{"SELECT id, v FROM dbo.t ORDER BY id",
	 "81020000000000080038026900640000000000090062491f0000017600a902000100d101000000060000003800050000"
	 "00d20202000000d1030000000f000000e7070904d000341400610062006300d104000000150000006a02090301393000"
	 "00000000000000000000000000d1050000000a0000003d0001b5000071987500fd1000c1000500000000000000"},
};

// CAST(<column> AS varbinary(max)) per id 1..5, "" for NULL; for v, SQL_VARIANT_PROPERTY(v, 'BaseType').
const char *const SERVER[5][4] = {
	{"00000000010C000000000000F03F0000000000000040", "E6100000010C00000000000049400000000000002440", "5B40", "int"},
	{"", "", "", ""},
	{"110F000001150000000000000000000000000000000000000000000008400000000000001040000000000000F03F0000000000000040",
	 "E610000001140000000000004940000000000000244000000000008049400000000000002640", "", "nvarchar"},
	{"00000000010405000000000000000000000000000000000000000000000000001040000000000000000000000000000010400000000000001"
	 "040000000000000000000000000000010400000000000000000000000000000000001000000020000000001000000FFFFFFFF0000000003",
	 "", "6467D0", "decimal"},
	{"", "", "", "datetime"},
};

// The value bytes of the variants, as they came: base type, property bytes, properties, value.
const char *const VARIANTS[5] = {"380005000000", "", "E7070904D000341400610062006300",
								 "6A0209030139300000000000000000000000000000", "3D0001B5000071987500"};

}  // namespace

int main() {
	std::cout << "UDT and sql_variant on the wire (spec 093 T0, G2)" << std::endl;
	const char *const want_type[4] = {"geometry", "geography", "hierarchyid", ""};
	const uint16_t want_max[4] = {0xFFFF, 0xFFFF, 892, SQL_VARIANT_MAX_BYTES};
	for (size_t r = 0; r < 4; r++) {
		const std::vector<uint8_t> b = FromHex(RESPONSES[r].payload);
		std::cout << RESPONSES[r].query << std::endl;
		Check(!b.empty() && b[0] == 0x81, "a COLMETADATA token first");
		std::vector<ColumnMetadata> columns;
		size_t consumed = 0;
		const bool parsed = ColumnMetadataParser::Parse(b.data() + 1, b.size() - 1, consumed, columns);
		Check(parsed && columns.size() == 2, "COLMETADATA parsed: two columns");
		if (!parsed || columns.size() != 2) {
			continue;
		}
		const ColumnMetadata &c = columns[1];
		if (r < 3) {
			Check(c.type_id == TDS_TYPE_UDT && c.udt_schema == "sys" && c.udt_type_name == want_type[r] &&
					  c.max_length == want_max[r] && c.IsPLPType(),
				  "UDT_INFO: sys." + c.udt_type_name + ", MAX_BYTE_SIZE " + std::to_string(c.max_length) + ", PLP");
		} else {
			Check(c.type_id == TDS_TYPE_SQL_VARIANT && c.max_length == want_max[r], "SSVARIANT: a maximum of 8009");
		}
		RowReader reader(columns);
		size_t at = 1 + consumed;
		// An ORDER BY query's ORDER token (0xA9) follows COLMETADATA: a u16 length, then the column ids.
		while (at + 3 <= b.size() && b[at] == 0xA9) {
			at += 3 + (b[at + 1] | (b[at + 2] << 8));
		}
		int rows = 0;
		bool all_equal = true;
		// Review of #557 (roborev job 2416): the null mask has to be checked APART from the bytes. `got` below renders
		// a NULL and a non-NULL value of zero bytes alike (both ""), so without these a 0-length PLP
		// UDT read as NULL -- or a real value read as NULL -- compares equal and passes.
		int nulls = 0;
		int empty_non_null = 0;
		while (at < b.size() && (b[at] == 0xD1 || b[at] == 0xD2)) {
			const bool nbc = b[at] == 0xD2;
			RowData row;
			size_t used = 0;
			const bool ok = nbc ? reader.ReadNBCRow(b.data() + at + 1, b.size() - at - 1, used, row)
								: reader.ReadRow(b.data() + at + 1, b.size() - at - 1, used, row);
			if (!ok) {
				Check(false, "row " + std::to_string(rows + 1) + " read");
				break;
			}
			const uint8_t *id_bytes = row.values[0].data();
			const int id = id_bytes[0] | (id_bytes[1] << 8);
			const std::string got = row.null_mask[1] ? "" : ToHex(row.values[1]);
			const std::string want = r < 3 ? SERVER[id - 1][r] : VARIANTS[id - 1];
			if (got != want) {
				all_equal = false;
				std::cout << "    id " << id << " got " << got << " want " << want << std::endl;
			}
			if (row.null_mask[1]) {
				nulls++;
				if (!want.empty()) {  // a value the server gave bytes for must not read as NULL
					all_equal = false;
					std::cout << "    id " << id << " read as NULL, want " << want << std::endl;
				}
			} else if (row.values[1].empty()) {
				empty_non_null++;
			}
			rows++;
			at += 1 + used;
			// Skipped the same way, for the drain path.
			size_t skipped = 0;
			const bool skip_ok = nbc ? reader.SkipNBCRow(b.data() + at - used, used, skipped)
									 : reader.SkipRow(b.data() + at - used, used, skipped);
			if (!skip_ok || skipped != used) {
				all_equal = false;
				std::cout << "    id " << id << " skipped " << skipped << " of " << used << std::endl;
			}
		}
		Check(rows == 5 && all_equal, std::to_string(rows) + " rows (ROW and NBCROW), every value the server's bytes");
		// G2 measured that the ROOT hierarchyid is a non-NULL value of ZERO bytes -- the one value in
		// these responses that `got` cannot tell from a NULL. Asserting the COUNT rather than naming
		// the id keeps this independent of which row holds it, and it is what fails if a 0-length PLP
		// UDT is ever read as NULL (review of #557, roborev job 2416). The other three columns have no such value.
		Check(empty_non_null == (r == 2 ? 1 : 0), "null mask apart from the bytes: " + std::to_string(nulls) +
													  " NULL, " + std::to_string(empty_non_null) +
													  " non-NULL of zero bytes (the root hierarchyid in " +
													  std::string(r == 2 ? "this" : "no other") + " response)");
	}

	std::cout << "UDT_INFO cut short, and a user CLR type" << std::endl;
	{
		// One UDT column of a user type dbo.MyType (synthetic: the layout G2 measured, other names).
		auto bvarchar = [](std::vector<uint8_t> &b, const std::string &s) {
			b.push_back(static_cast<uint8_t>(s.size()));
			for (char c : s) {
				b.push_back(static_cast<uint8_t>(c));
				b.push_back(0);
			}
		};
		std::vector<uint8_t> meta = {0x01, 0x00, 0, 0, 0, 0, 0x09, 0x00, TDS_TYPE_UDT, 0x14, 0x00};	 // MAX_BYTE_SIZE 20
		const size_t after_max = meta.size();
		bvarchar(meta, "db");
		const size_t after_db = meta.size();
		bvarchar(meta, "dbo");
		bvarchar(meta, "MyType");
		const size_t after_type = meta.size();
		const std::string aqn = "MyAssembly.MyType, MyAssembly";
		meta.push_back(static_cast<uint8_t>(aqn.size()));
		meta.push_back(0);
		for (char c : aqn) {
			meta.push_back(static_cast<uint8_t>(c));
			meta.push_back(0);
		}
		const size_t after_aqn = meta.size();
		bvarchar(meta, "u");
		std::vector<ColumnMetadata> columns;
		size_t consumed = 0;
		const bool whole = ColumnMetadataParser::Parse(meta.data(), meta.size(), consumed, columns);
		Check(whole && consumed == meta.size() && columns.size() == 1 && columns[0].udt_schema == "dbo" &&
				  columns[0].udt_type_name == "MyType" && columns[0].max_length == 20 && columns[0].IsPLPType(),
			  "a user type: dbo.MyType, MAX_BYTE_SIZE 20, PLP");
		bool short_ok = true;
		for (size_t cut : {after_max - 1, after_max, after_max + 3, after_db + 2, after_type, after_type + 3,
						   after_aqn - 1, after_aqn}) {
			std::vector<ColumnMetadata> c;
			size_t used = 0;
			if (ColumnMetadataParser::Parse(meta.data(), cut, used, c)) {
				short_ok = false;
				std::cout << "    a cut at " << cut << " parsed" << std::endl;
			}
		}
		Check(short_ok, "cut inside any UDT_INFO field: more data needed, not a parse");
		// Spec 093 T4: a ColumnMetadata starts with every scalar 0. The describe built one per row, and
		// with the scalars uninitialised the row after a UDT one took its stale 0xF0 and read as an
		// unnamed user CLR type (a BLOB named C1) -- the shape of that loop:
		bool fresh_ok = true;
		for (int i = 0; i < 3; i++) {
			ColumnMetadata c;
			fresh_ok = fresh_ok && c.type_id == 0 && c.max_length == 0 && c.flags == 0 && !c.IsUserClrUdt();
			c.type_id = TDS_TYPE_UDT;
			c.max_length = 892;
			c.flags = 1;
		}
		Check(fresh_ok, "a default ColumnMetadata: type 0, length 0, flags 0, not a UDT");
		// Spec 093 T4: a user CLR type's bytes are opaque, read as a BLOB.
		Check(columns[0].IsUserClrUdt() &&
				  duckdb::tds::encoding::TypeConverter::GetDuckDBType(columns[0]) == duckdb::LogicalType::BLOB,
			  "a user CLR type reads as a BLOB");
		// A user type that borrows a system name in another schema is still a user type.
		ColumnMetadata borrowed = columns[0];
		borrowed.udt_type_name = "hierarchyid";
		Check(borrowed.IsUserClrUdt() && !borrowed.IsHierarchyIdUdt(), "dbo.hierarchyid is a user type, not sys's");
		// hierarchyid is decoded into its text (T4), geometry / geography into GEOMETRY (T5).
		ColumnMetadata h = columns[0];
		h.udt_schema = "sys";
		h.udt_type_name = "hierarchyid";
		Check(duckdb::tds::encoding::TypeConverter::GetDuckDBType(h) == duckdb::LogicalType::VARCHAR,
			  "sys.hierarchyid reads as VARCHAR");
		ColumnMetadata g = columns[0];
		g.udt_schema = "sys";
		g.udt_type_name = "geometry";
		Check(!g.IsUserClrUdt() &&
				  duckdb::tds::encoding::TypeConverter::GetDuckDBType(g) == duckdb::LogicalType::GEOMETRY(),
			  "sys.geometry reads as GEOMETRY");
	}

	std::cout << "An NBCROW variant present by its bitmap but of length 0" << std::endl;
	{
		std::vector<ColumnMetadata> columns(1);
		columns[0].type_id = TDS_TYPE_SQL_VARIANT;
		columns[0].max_length = SQL_VARIANT_MAX_BYTES;
		columns[0].flags = 0;
		RowReader reader(columns);
		const std::vector<uint8_t> nbc = {0x00, 0x00, 0x00, 0x00, 0x00};  // bitmap: present; length 0
		RowData row;
		size_t used = 0;
		const bool ok = reader.ReadNBCRow(nbc.data(), nbc.size(), used, row);
		Check(ok && used == nbc.size() && row.null_mask[0], "read, as NULL, all five bytes consumed");
	}

	// Review of #557: the same honouring reaches the types that had it dropped before T0 -- a PLP
	// value carrying PLP_NULL and a non-MAX string of length 0xFFFF, each under a present bit.
	std::cout << "An NBCROW value present by its bitmap but NULL by itself, older types" << std::endl;
	{
		std::vector<ColumnMetadata> columns(3);
		columns[0].type_id = TDS_TYPE_NVARCHAR;	 // nvarchar(max): PLP
		columns[0].max_length = 0xFFFF;
		columns[1].type_id = TDS_TYPE_XML;		 // always PLP
		columns[2].type_id = TDS_TYPE_NVARCHAR;	 // nvarchar(10): a u16 length
		columns[2].max_length = 20;
		for (auto &c : columns) {
			c.flags = 0;
		}
		RowReader reader(columns);
		std::vector<uint8_t> nbc = {0x00};	// bitmap: all three present
		for (int i = 0; i < 2; i++) {
			nbc.insert(nbc.end(), 8, 0xFF);	 // PLP_NULL
		}
		nbc.insert(nbc.end(), {0xFF, 0xFF});  // CHARBIN_NULL
		RowData row;
		size_t used = 0;
		const bool ok = reader.ReadNBCRow(nbc.data(), nbc.size(), used, row);
		Check(ok && used == nbc.size() && row.null_mask[0] && row.null_mask[1] && row.null_mask[2],
			  "nvarchar(max) PLP_NULL, xml PLP_NULL, nvarchar(10) 0xFFFF: each read as NULL, every byte consumed");
	}

	std::cout << "A malformed variant length" << std::endl;
	{
		std::vector<ColumnMetadata> columns(1);
		columns[0].type_id = TDS_TYPE_SQL_VARIANT;
		columns[0].max_length = SQL_VARIANT_MAX_BYTES;
		columns[0].flags = 0;
		RowReader reader(columns);
		const std::vector<uint8_t> past = {0x4A, 0x1F, 0x00, 0x00};	 // 8010
		RowData row;
		size_t used = 0;
		std::string error;
		try {
			reader.ReadRow(past.data(), past.size(), used, row);
		} catch (std::exception &e) {
			error = e.what();
		}
		Check(error.find("past the 8009-byte maximum") != std::string::npos, "a length past 8009 is refused: " + error);
		const std::vector<uint8_t> partial = {0x06, 0x00, 0x00, 0x00, 0x38, 0x00};
		Check(!reader.ReadRow(partial.data(), partial.size(), used, row), "a value not all arrived is not read");
	}

	// Review of #564: the per-value path (TypeConverter::ConvertValue, used with target vectors -- a
	// composite rowid) counts a value read as its bytes for the stream's WARNING, as the chunk path does.
	std::cout << "The per-value path counts a variant read as its bytes" << std::endl;
	{
		ColumnMetadata col;
		col.name = "v";
		col.type_id = TDS_TYPE_SQL_VARIANT;
		col.max_length = SQL_VARIANT_MAX_BYTES;
		// varchar (0xA7) under Latin1_General_100_CI_AS_SC_UTF8 (props: collation 0904d02400, length 1400),
		// bytes 61 FF: not UTF-8.
		const std::vector<uint8_t> envelope = FromHex("A7070904D02400140061FF");
		duckdb::Vector out(duckdb::LogicalType::VARIANT(), 2);
		duckdb::mssql::codec::variant::VariantColumnNotes notes;
		encoding::TypeConverter::ConvertValue(envelope, false, col, out, 0, &notes, nullptr);
		encoding::TypeConverter::ConvertValue(envelope, false, col, out, 1, nullptr, nullptr);
		Check(notes.invalid_utf8 == 1 && notes.unknown_collation == 0,
			  "invalid UTF-8 counted once, only where notes were passed");
	}

	std::cout << (failures ? "FAILED" : "PASSED") << " (" << failures << " failure(s))" << std::endl;
	return failures ? 1 : 0;
}
