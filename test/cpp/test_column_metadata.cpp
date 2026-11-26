// COLMETADATA's TYPE_INFO (ColumnMetadataParser::Parse): what each form of it reads, what a token cut
// anywhere answers, and what a declaration no server sends is refused as -- by the column's name, which
// is why the checks run after the name is read. No SQL Server, no DuckDB.

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "tds/tds_column_metadata.hpp"

using namespace duckdb::tds;

namespace {

int failures = 0;

void Check(bool ok, const std::string &what) {
	std::cout << (ok ? "  ok   " : "  FAIL ") << what << std::endl;
	if (!ok) {
		failures++;
	}
}

using Bytes = std::vector<uint8_t>;

Bytes Cat(Bytes a, const Bytes &b) {
	a.insert(a.end(), b.begin(), b.end());
	return a;
}

//! B_VARCHAR: a character count, then UTF-16LE (ASCII here).
Bytes Name(const std::string &name) {
	Bytes b = {static_cast<uint8_t>(name.size())};
	for (char c : name) {
		b.push_back(static_cast<uint8_t>(c));
		b.push_back(0);
	}
	return b;
}

//! One column: UserType 0, flags 0x0009, the TYPE_INFO bytes, the name.
Bytes Column(const Bytes &type_info, const std::string &name) {
	return Cat(Cat({0, 0, 0, 0, 0x09, 0x00}, type_info), Name(name));
}

Bytes Token(const std::vector<Bytes> &columns) {
	Bytes b = {static_cast<uint8_t>(columns.size()), 0};
	for (auto &column : columns) {
		b = Cat(b, column);
	}
	return b;
}

//! "OK" / "MORE", or the exception's message.
std::string Answer(const Bytes &token, size_t length, std::vector<ColumnMetadata> &columns, size_t &consumed) {
	try {
		return ColumnMetadataParser::Parse(token.data(), length, consumed, columns) ? "OK" : "MORE";
	} catch (const std::exception &e) {
		return e.what();
	}
}

std::string Answer(const Bytes &token) {
	std::vector<ColumnMetadata> columns;
	size_t consumed = 0;
	return Answer(token, token.size(), columns, consumed);
}

bool Has(const std::string &text, const std::string &part) {
	return text.find(part) != std::string::npos;
}

// A token with one column of every TYPE_INFO form.
std::vector<Bytes> EveryForm() {
	return {
		Column({TDS_TYPE_INT}, "i"),
		Column({TDS_TYPE_INTN, 8}, "n"),
		Column({TDS_TYPE_NUMERIC, 17, 38, 10}, "d"),
		Column({TDS_TYPE_NVARCHAR, 0x40, 0x1F, 0x09, 0x04, 0xD0, 0x00, 0x34}, "s"),
		Column({TDS_TYPE_BIGVARBINARY, 0xFF, 0xFF}, "b"),
		Column({TDS_TYPE_DATETIME2, 7}, "t"),
		Column(Cat(Cat(Cat({TDS_TYPE_XML, 1}, Name("db")), Name("dbo")), {2, 0, 'c', 0, 'c', 0}), "x"),
		Column(
			Cat(Cat(Cat(Cat({TDS_TYPE_UDT, 0xFF, 0xFF}, Name("db")), Name("sys")), Name("geometry")), {1, 0, 'a', 0}),
			"u"),
		Column({TDS_TYPE_SQL_VARIANT, 0x49, 0x1F, 0, 0}, "v"),
		Column({TDS_TYPE_VECTOR, 0x14, 0x00, 0x00}, "e"),
		Column({TDS_TYPE_NTEXT, 0xFE, 0xFF, 0xFF, 0x7F, 0x09, 0x04, 0xD0, 0x00, 0x34, 1, 1, 0, 't', 0}, "l"),
		Column({TDS_TYPE_IMAGE, 0xFF, 0xFF, 0xFF, 0x7F, 0}, "g"),
		Column({TDS_TYPE_DATE}, "day"),
	};
}

void TestEveryForm() {
	std::cout << "[1] one column of every TYPE_INFO form" << std::endl;
	const Bytes token = Token(EveryForm());
	std::vector<ColumnMetadata> c;
	size_t consumed = 0;
	Check(Answer(token, token.size(), c, consumed) == "OK" && consumed == token.size() && c.size() == 13,
		  "the token parses whole");
	if (c.size() != 13) {
		return;
	}
	Check(c[0].name == "i" && c[0].GetTypeName() == "INT" && c[0].GetFixedSize() == 4 && c[0].flags == 9,
		  "INT: nothing after the type byte");
	Check(c[1].max_length == 8 && c[1].IsNullableVariant() && c[1].GetFixedSize() == 0, "INTN: a byte length");
	Check(c[2].max_length == 17 && c[2].precision == 38 && c[2].scale == 10 && c[2].GetTypeName() == "NUMERIC",
		  "NUMERIC: length, precision, scale");
	Check(c[3].max_length == 8000 && c[3].collation == 0x00D00409 && c[3].collation_sort_id == 0x34 &&
			  c[3].IsVariableLength() && !c[3].IsPLPType(),
		  "NVARCHAR: a two-byte length, the collation and its sort id");
	Check(c[4].max_length == 0xFFFF && c[4].IsPLPType() && c[4].collation == 0, "VARBINARY(MAX): PLP, no collation");
	Check(c[5].scale == 7 && c[5].name == "t", "DATETIME2: the scale");
	Check(c[6].max_length == 0xFFFF && c[6].IsPLPType() && c[6].name == "x", "XML: the schema collection skipped");
	Check(c[7].udt_schema == "sys" && c[7].udt_type_name == "geometry" && c[7].IsPLPType() && c[7].name == "u",
		  "UDT: the type's schema and name kept");
	Check(c[8].max_length == 8009, "SQL_VARIANT: the LONGLEN maximum");
	Check(c[9].max_length == 20 && c[9].vector_element_type == 0, "VECTOR: length and element type");
	Check(c[10].max_length == 0xFFFF && c[10].collation_sort_id == 0x34 && c[10].name == "l" && !c[10].IsPLPType(),
		  "NTEXT: the collation kept, the table name skipped");
	Check(c[11].max_length == 0xFFFF && c[11].collation == 0 && c[11].name == "g", "IMAGE: no collation");
	Check(c[12].GetFixedSize() == 3 && c[12].name == "day", "DATE: three bytes, nothing declared");
}

void TestPrefixes() {
	std::cout << "[2] a token cut anywhere asks for more" << std::endl;
	const Bytes token = Token(EveryForm());
	size_t wrong = 0;
	for (size_t cut = 0; cut < token.size(); cut++) {
		std::vector<ColumnMetadata> columns;
		size_t consumed = 0xDEAD;
		if (Answer(token, cut, columns, consumed) != "MORE" || consumed != 0xDEAD) {
			wrong++;
		}
	}
	Check(wrong == 0, "every prefix: need more data, nothing consumed, no exception");
	Check(Answer({0xFF, 0xFF}) == "OK", "a count of 0xFFFF: no metadata");
	std::vector<ColumnMetadata> columns;
	size_t consumed = 0;
	const Bytes trailing = Cat(Token({Column({TDS_TYPE_INT}, "i")}), {0xD1, 0x01});
	Check(Answer(trailing, trailing.size(), columns, consumed) == "OK" && consumed == trailing.size() - 2,
		  "bytes after the token are left");
}

void TestTypeNames() {
	std::cout << "[3] type names" << std::endl;
	ColumnMetadata column;
	column.type_id = 0xEB;
	Check(column.GetTypeName() == "UNKNOWN(0xEB)" && column.GetFixedSize() == 0, "an unknown type byte in hex");
	Check(Answer(Token({Column({0xEB}, "c")})) == "Unsupported SQL Server type: UNKNOWN(0xEB)",
		  "and refused where it is met: nothing after it can be framed");
	column.type_id = TDS_TYPE_BIGVARCHAR;
	Check(column.GetTypeName() == "VARCHAR", "BIGVARCHAR is VARCHAR");
	column.type_id = TDS_TYPE_MONEY;
	Check(column.GetTypeName() == "MONEY" && column.GetFixedSize() == 8, "MONEY, 8 bytes");
}

void TestMalformedDeclarations() {
	std::cout << "[4] a declaration no server sends is refused by the column's name" << std::endl;
	// The scale's message used to be built before the name was read: "Column '' declares ...".
	const std::string scale = Answer(Token({Column({TDS_TYPE_INT}, "id"), Column({TDS_TYPE_TIME, 8}, "ts")}));
	Check(Has(scale, "Column 'ts' declares fractional-second scale 8") && Has(scale, "malformed"),
		  "scale 8 names the column: " + scale);
	Check(Answer(Token({Column({TDS_TYPE_DATETIMEOFFSET, 7}, "ts")})) == "OK", "scale 7 parses");

	// DECIMAL(p, s) becomes DuckDB's: a precision past 38 was an InternalException there.
	const std::string wide = Answer(Token({Column({TDS_TYPE_DECIMAL, 17, 39, 0}, "amount")}));
	Check(Has(wide, "Column 'amount' declares precision 39 and scale 0") && Has(wide, "malformed"),
		  "precision 39: " + wide);
	Check(Has(Answer(Token({Column({TDS_TYPE_NUMERIC, 5, 0, 0}, "amount")})), "precision 0"), "precision 0");
	Check(Has(Answer(Token({Column({TDS_TYPE_NUMERIC, 5, 5, 9}, "amount")})), "precision 5 and scale 9"),
		  "a scale past the precision");
	Check(Answer(Token({Column({TDS_TYPE_DECIMAL, 17, 38, 38}, "amount")})) == "OK", "decimal(38, 38) parses");
	Check(Answer(Token({Column({TDS_TYPE_DECIMAL, 5, 1, 0}, "amount")})) == "OK", "decimal(1, 0) parses");

	const std::string element = Answer(Token({Column({TDS_TYPE_VECTOR, 0x14, 0x00, 0x02}, "emb")}));
	Check(Has(element, "Column 'emb' declares vector element type 2"), "a vector's element type: " + element);
	const std::string length = Answer(Token({Column({TDS_TYPE_VECTOR, 0x09, 0x00, 0x00}, "emb")}));
	Check(Has(length, "Column 'emb' declares a vector's maximum length as 9 bytes"), "a vector's length: " + length);

	// Until the name has arrived the answer is "more", then the error with the name.
	const Bytes token = Token({Column({TDS_TYPE_TIME, 8}, "ts")});
	std::vector<ColumnMetadata> columns;
	size_t consumed = 0;
	Check(Answer(token, token.size() - 1, columns, consumed) == "MORE", "cut inside the name: need more data");
}

}  // namespace

int main() {
	TestEveryForm();
	TestPrefixes();
	TestTypeNames();
	TestMalformedDeclarations();
	if (failures != 0) {
		std::cout << failures << " check(s) FAILED" << std::endl;
		return 1;
	}
	std::cout << "all checks passed" << std::endl;
	return 0;
}
