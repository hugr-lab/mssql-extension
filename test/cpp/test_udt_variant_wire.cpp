// Spec 093 T0: the COLMETADATA and rows of UDT (0xF0) and sql_variant (0x62) columns, as SQL Server
// 2022 sent them (experiment G2, specs/093-type-kernels/measured/G2.md: a throwaway container, synthetic
// values). The parser takes each response's TYPE_INFO -- UDT_INFO's schema and type name, the variant's
// LONGLEN maximum -- and the row reader frames every value (UDT as PLP, hierarchyid's MAX_BYTE_SIZE of
// 892 included; sql_variant behind a u32 length), ROW and NBCROW alike, to the bytes the server's own
// CAST(x AS varbinary(max)) gives. No SQL Server, no DuckDB.

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

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
		std::string message;
		try {
			duckdb::tds::encoding::TypeConverter::GetDuckDBType(columns[0]);
		} catch (std::exception &e) {
			message = e.what();
		}
		Check(message.find("column 'u' is of the CLR type dbo.MyType") != std::string::npos,
			  "a raw scan's refusal names the type: " + message);
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

	std::cout << (failures ? "FAILED" : "PASSED") << " (" << failures << " failure(s))" << std::endl;
	return failures ? 1 : 0;
}
