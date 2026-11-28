#include "tds/tds_column_metadata.hpp"
#include <cstdio>
#include <stdexcept>
#include "tds/encoding/utf16.hpp"

namespace duckdb {
namespace tds {

namespace {

//! How a type's TYPE_INFO is laid out after the type byte (MS-TDS 2.2.5.4): what the parser reads for it.
enum class TypeInfoForm : uint8_t {
	NONE,		  // nothing: the fixed-length types and DATE
	BYTE_LENGTH,  // 1 byte length: the nullable fixed-length types and UNIQUEIDENTIFIER
	DECIMAL,	  // 1 byte length, precision, scale
	STRING,		  // 2 bytes length, 5 bytes collation
	BINARY,		  // 2 bytes length
	SCALE,		  // 1 byte fractional-second scale
	XML,		  // SCHEMA_PRESENT and, when set, the schema collection's names
	UDT,		  // MAX_BYTE_SIZE and the type's names
	VARIANT,	  // a LONGLEN maximum
	VECTOR,		  // 2 bytes length, the element type
	LOB			  // a LONGLEN, a collation for the text forms, the source table's name
};

//! One wire type: its name in messages, its TYPE_INFO, and a value's size where the type alone fixes it.
struct TypeEntry {
	uint8_t type_id;
	const char *name;
	TypeInfoForm form;
	uint8_t fixed_size;
};

constexpr TypeEntry TYPES[] = {
	{TDS_TYPE_NULL, "NULL", TypeInfoForm::NONE, 0},
	{TDS_TYPE_TINYINT, "TINYINT", TypeInfoForm::NONE, 1},
	{TDS_TYPE_BIT, "BIT", TypeInfoForm::NONE, 1},
	{TDS_TYPE_SMALLINT, "SMALLINT", TypeInfoForm::NONE, 2},
	{TDS_TYPE_INT, "INT", TypeInfoForm::NONE, 4},
	{TDS_TYPE_BIGINT, "BIGINT", TypeInfoForm::NONE, 8},
	{TDS_TYPE_REAL, "REAL", TypeInfoForm::NONE, 4},
	{TDS_TYPE_FLOAT, "FLOAT", TypeInfoForm::NONE, 8},
	{TDS_TYPE_MONEY, "MONEY", TypeInfoForm::NONE, 8},
	{TDS_TYPE_SMALLMONEY, "SMALLMONEY", TypeInfoForm::NONE, 4},
	{TDS_TYPE_DATETIME, "DATETIME", TypeInfoForm::NONE, 8},
	{TDS_TYPE_SMALLDATETIME, "SMALLDATETIME", TypeInfoForm::NONE, 4},
	{TDS_TYPE_DATE, "DATE", TypeInfoForm::NONE, 3},
	{TDS_TYPE_INTN, "INTN", TypeInfoForm::BYTE_LENGTH, 0},
	{TDS_TYPE_BITN, "BITN", TypeInfoForm::BYTE_LENGTH, 0},
	{TDS_TYPE_FLOATN, "FLOATN", TypeInfoForm::BYTE_LENGTH, 0},
	{TDS_TYPE_MONEYN, "MONEYN", TypeInfoForm::BYTE_LENGTH, 0},
	{TDS_TYPE_DATETIMEN, "DATETIMEN", TypeInfoForm::BYTE_LENGTH, 0},
	{TDS_TYPE_UNIQUEIDENTIFIER, "UNIQUEIDENTIFIER", TypeInfoForm::BYTE_LENGTH, 0},
	{TDS_TYPE_DECIMAL, "DECIMAL", TypeInfoForm::DECIMAL, 0},
	{TDS_TYPE_NUMERIC, "NUMERIC", TypeInfoForm::DECIMAL, 0},
	{TDS_TYPE_BIGCHAR, "CHAR", TypeInfoForm::STRING, 0},
	{TDS_TYPE_BIGVARCHAR, "VARCHAR", TypeInfoForm::STRING, 0},
	{TDS_TYPE_NCHAR, "NCHAR", TypeInfoForm::STRING, 0},
	{TDS_TYPE_NVARCHAR, "NVARCHAR", TypeInfoForm::STRING, 0},
	{TDS_TYPE_BIGBINARY, "BINARY", TypeInfoForm::BINARY, 0},
	{TDS_TYPE_BIGVARBINARY, "VARBINARY", TypeInfoForm::BINARY, 0},
	{TDS_TYPE_TIME, "TIME", TypeInfoForm::SCALE, 0},
	{TDS_TYPE_DATETIME2, "DATETIME2", TypeInfoForm::SCALE, 0},
	{TDS_TYPE_DATETIMEOFFSET, "DATETIMEOFFSET", TypeInfoForm::SCALE, 0},
	{TDS_TYPE_XML, "XML", TypeInfoForm::XML, 0},
	{TDS_TYPE_UDT, "UDT", TypeInfoForm::UDT, 0},
	{TDS_TYPE_SQL_VARIANT, "SQL_VARIANT", TypeInfoForm::VARIANT, 0},
	{TDS_TYPE_VECTOR, "VECTOR", TypeInfoForm::VECTOR, 0},
	{TDS_TYPE_IMAGE, "IMAGE", TypeInfoForm::LOB, 0},
	{TDS_TYPE_TEXT, "TEXT", TypeInfoForm::LOB, 0},
	{TDS_TYPE_NTEXT, "NTEXT", TypeInfoForm::LOB, 0},
};

//! The entry of a type byte, or null for one the extension does not read.
const TypeEntry *FindType(uint8_t type_id) {
	for (auto &entry : TYPES) {
		if (entry.type_id == type_id) {
			return &entry;
		}
	}
	return nullptr;
}

}  // namespace

//===----------------------------------------------------------------------===//
// ColumnMetadata Implementation
//===----------------------------------------------------------------------===//

std::string ColumnMetadata::GetTypeName() const {
	if (const TypeEntry *entry = FindType(type_id)) {
		return entry->name;
	}
	// Hex prefix, hex digits: 0xAF, not 0x175. The parser's unknown-token
	// message had the same defect (spec 072 W5); this is the type-byte twin,
	// and it reaches users through "Unsupported SQL Server type" and
	// "Unsupported type in RowReader" -- the lines the next #323 gets pasted from.
	char buf[16];
	std::snprintf(buf, sizeof(buf), "UNKNOWN(0x%02X)", static_cast<unsigned>(type_id));
	return buf;
}

bool ColumnMetadata::IsVariableLength() const {
	switch (type_id) {
	case TDS_TYPE_BIGCHAR:
	case TDS_TYPE_BIGVARCHAR:
	case TDS_TYPE_NCHAR:
	case TDS_TYPE_NVARCHAR:
	case TDS_TYPE_BIGBINARY:
	case TDS_TYPE_BIGVARBINARY:
		return true;
	default:
		return false;
	}
}

bool ColumnMetadata::IsNullableVariant() const {
	switch (type_id) {
	case TDS_TYPE_INTN:
	case TDS_TYPE_BITN:
	case TDS_TYPE_FLOATN:
	case TDS_TYPE_MONEYN:
	case TDS_TYPE_DATETIMEN:
		return true;
	default:
		return false;
	}
}

bool ColumnMetadata::IsPLPType() const {
	// XML is always PLP regardless of max_length, and so is a UDT, hierarchyid's MAX_BYTE_SIZE of 892
	// included (spec 093 G2, measured on 2022).
	if (type_id == TDS_TYPE_XML || type_id == TDS_TYPE_UDT) {
		return true;
	}
	// PLP (Partially Length-Prefixed) encoding is used for MAX types, where max_length == 0xFFFF (65535):
	// VARCHAR / NVARCHAR / VARBINARY(MAX), and CHAR / NCHAR / BINARY should a server ever declare one so.
	return max_length == 0xFFFF && IsVariableLength();
}

size_t ColumnMetadata::GetFixedSize() const {
	const TypeEntry *entry = FindType(type_id);
	return entry ? entry->fixed_size : 0;  // 0: variable length, or behind a length prefix
}

//===----------------------------------------------------------------------===//
// ColumnMetadataParser Implementation
//===----------------------------------------------------------------------===//

namespace {

//! The token's bytes and how far the parse has read. Every read answers false, taking nothing, when
//! the bytes it needs have not arrived: the caller then asks again with more of the token.
struct Cursor {
	const uint8_t *data;
	size_t length;
	size_t offset;

	bool Has(size_t bytes) const {
		return bytes <= length - offset;
	}
	bool U8(uint8_t &value) {
		if (!Has(1)) {
			return false;
		}
		value = data[offset++];
		return true;
	}
	bool U16(uint16_t &value) {
		if (!Has(2)) {
			return false;
		}
		value = static_cast<uint16_t>(data[offset] | (data[offset + 1] << 8));
		offset += 2;
		return true;
	}
	bool U32(uint32_t &value) {
		if (!Has(4)) {
			return false;
		}
		value = static_cast<uint32_t>(data[offset]) | (static_cast<uint32_t>(data[offset + 1]) << 8) |
				(static_cast<uint32_t>(data[offset + 2]) << 16) | (static_cast<uint32_t>(data[offset + 3]) << 24);
		offset += 4;
		return true;
	}
	bool Skip(size_t bytes) {
		if (!Has(bytes)) {
			return false;
		}
		offset += bytes;
		return true;
	}
	//! B_VARCHAR: a 1-byte character count, then UTF-16LE.
	bool BVarchar(std::string &text) {
		uint8_t chars;
		if (!U8(chars) || !Has(static_cast<size_t>(chars) * 2)) {
			return false;
		}
		text = encoding::Utf16LEDecode(data + offset, static_cast<size_t>(chars) * 2);
		offset += static_cast<size_t>(chars) * 2;
		return true;
	}
	bool SkipBVarchar() {
		uint8_t chars;
		return U8(chars) && Skip(static_cast<size_t>(chars) * 2);
	}
	//! US_VARCHAR: a 2-byte character count, then UTF-16LE.
	bool SkipUsVarchar() {
		uint16_t chars;
		return U16(chars) && Skip(static_cast<size_t>(chars) * 2);
	}
};

bool ReadByteLength(Cursor &in, ColumnMetadata &column) {
	uint8_t bytes;
	if (!in.U8(bytes)) {
		return false;
	}
	column.max_length = bytes;
	return true;
}

bool ReadDecimal(Cursor &in, ColumnMetadata &column) {
	return ReadByteLength(in, column) && in.U8(column.precision) && in.U8(column.scale);
}

//! The 5 collation bytes. The 5th is the SortId, which is what names the code page for a SQL_*
//! collation: discarding it meant a message about such a column could not say which collation it had
//! met (issue #224).
bool ReadCollation(Cursor &in, ColumnMetadata &column) {
	return in.U32(column.collation) && in.U8(column.collation_sort_id);
}

bool ReadString(Cursor &in, ColumnMetadata &column) {
	return in.U16(column.max_length) && ReadCollation(in, column);
}

//! XML: the SCHEMA_PRESENT flag and, when set, the schema collection's database and owning schema
//! (B_VARCHAR) and its name (US_VARCHAR), none of which the extension keeps.
bool ReadXml(Cursor &in, ColumnMetadata &column) {
	column.max_length = 0xFFFF;	 // PLP indicator (XML is always PLP)
	uint8_t schema_present;
	if (!in.U8(schema_present)) {
		return false;
	}
	return !schema_present || (in.SkipBVarchar() && in.SkipBVarchar() && in.SkipUsVarchar());
}

//! UDT (CLR types: geometry, geography, hierarchyid, user types), MS-TDS 2.2.5.5.2, measured on 2022
//! (spec 093 G2): u16 MAX_BYTE_SIZE, DB_NAME / SCHEMA_NAME / TYPE_NAME as B_VARCHAR, then the
//! ASSEMBLY_QUALIFIED_NAME as US_VARCHAR. Values are PLP.
bool ReadUdt(Cursor &in, ColumnMetadata &column) {
	std::string database;
	return in.U16(column.max_length) && in.BVarchar(database) && in.BVarchar(column.udt_schema) &&
		   in.BVarchar(column.udt_type_name) && in.SkipUsVarchar();
}

//! SSVARIANT, MS-TDS 2.2.5.5.4: a LONGLEN maximum (8009, spec 093 G2). Values are a u32 length, 0 for
//! NULL, then the base type, its property bytes, the properties and the value.
bool ReadVariant(Cursor &in, ColumnMetadata &column) {
	uint32_t declared;
	if (!in.U32(declared)) {
		return false;
	}
	column.max_length = static_cast<uint16_t>(declared < SQL_VARIANT_MAX_BYTES ? declared : SQL_VARIANT_MAX_BYTES);
	return true;
}

//! VECTORTYPE (spec 093 D4, measured V1): a u16 max length -- 8 + dimensions x element size -- and the
//! element type.
bool ReadVector(Cursor &in, ColumnMetadata &column) {
	return in.U16(column.max_length) && in.U8(column.vector_element_type);
}

//! Legacy LOBs (issue #197). Their TYPE_INFO is unlike anything else: a 4-byte LONGLEN, a collation
//! for the two text forms, and then the source TABLE NAME in parts -- a field no other type carries.
//! Without parsing all of it the COLMETADATA stream desynchronises, which is why these columns used
//! to kill the connection before a single row arrived rather than fail on decode.
bool ReadLob(Cursor &in, ColumnMetadata &column) {
	// LONGLEN runs to 2^31-1. max_length is the 16-bit declared width elsewhere, so flag these as
	// PLP-like instead of truncating it.
	if (!in.Skip(4)) {
		return false;
	}
	column.max_length = 0xFFFF;
	if (column.type_id != TDS_TYPE_IMAGE && !ReadCollation(in, column)) {
		return false;
	}
	// TableName: 1 byte part count, then each part as US_VARCHAR.
	uint8_t parts;
	if (!in.U8(parts)) {
		return false;
	}
	for (uint8_t part = 0; part < parts; part++) {
		if (!in.SkipUsVarchar()) {
			return false;
		}
	}
	return true;
}

//! TYPE_INFO: the type byte and what its form puts after it. Throws for a type the extension does
//! not read: nothing after it could be framed.
bool ReadTypeInfo(Cursor &in, ColumnMetadata &column) {
	if (!in.U8(column.type_id)) {
		return false;
	}
	column.max_length = 0;
	column.precision = 0;
	column.scale = 0;
	column.collation = 0;
	column.collation_sort_id = 0;

	const TypeEntry *entry = FindType(column.type_id);
	if (!entry) {
		throw std::runtime_error("Unsupported SQL Server type: " + column.GetTypeName());
	}
	switch (entry->form) {
	case TypeInfoForm::NONE:
		return true;
	case TypeInfoForm::BYTE_LENGTH:
		return ReadByteLength(in, column);
	case TypeInfoForm::DECIMAL:
		return ReadDecimal(in, column);
	case TypeInfoForm::STRING:
		return ReadString(in, column);
	case TypeInfoForm::BINARY:
		return in.U16(column.max_length);
	case TypeInfoForm::SCALE:
		return in.U8(column.scale);
	case TypeInfoForm::XML:
		return ReadXml(in, column);
	case TypeInfoForm::UDT:
		return ReadUdt(in, column);
	case TypeInfoForm::VARIANT:
		return ReadVariant(in, column);
	case TypeInfoForm::VECTOR:
		return ReadVector(in, column);
	case TypeInfoForm::LOB:
		return ReadLob(in, column);
	}
	return true;
}

[[noreturn]] void Malformed(const ColumnMetadata &column, const std::string &declares) {
	throw std::runtime_error("Column '" + column.name + "' declares " + declares + " The TDS stream is malformed.");
}

//! A vector's element type and maximum length: the read divides by the element size and sizes the
//! output's arrays by the dimensions.
void CheckVector(const ColumnMetadata &column) {
	if (column.vector_element_type != VECTOR_ELEMENT_FLOAT32 && column.vector_element_type != VECTOR_ELEMENT_FLOAT16) {
		Malformed(column,
				  "vector element type " + std::to_string(column.vector_element_type) + " (0 float32, 1 float16).");
	}
	const uint16_t element = VectorElementBytes(column.vector_element_type);
	// #644: the limit is the element type's -- a float16 vector holds up to 3996 dimensions.
	const uint16_t max_dimensions = VectorMaxDimensions(column.vector_element_type);
	if (column.max_length <= VECTOR_HEADER_BYTES || (column.max_length - VECTOR_HEADER_BYTES) % element != 0 ||
		(column.max_length - VECTOR_HEADER_BYTES) / element > max_dimensions) {
		Malformed(column, "a vector's maximum length as " + std::to_string(column.max_length) + " bytes, which no " +
							  (column.vector_element_type == VECTOR_ELEMENT_FLOAT16 ? "float16" : "float32") +
							  " vector(1.." + std::to_string(max_dimensions) + ") has.");
	}
}

// What a declaration may say, checked HERE, once per column, because the consumers trust it on the
// per-value path, where a test would catch only what the metadata already knows:
//   - a fractional-second scale: everything downstream scales by ten to that power, and `Pow10(scale)`
//     overflows int64 past 18, which is undefined behaviour; the fuzz harness reached it (spec 059 D4).
//     MS-TDS caps TIME / DATETIME2 / DATETIMEOFFSET at 7.
//   - a DECIMAL / NUMERIC precision and scale: they become DuckDB's DECIMAL(p, s), and a precision past
//     38 is an InternalException there (measured), where a malformed stream is an error of the stream.
//   - a vector's element type and length.
// After the name, so the error can say which column.
void CheckDeclaration(const TypeEntry &type, const ColumnMetadata &column) {
	switch (type.form) {
	case TypeInfoForm::SCALE:
		if (column.scale > 7) {
			Malformed(column, "fractional-second scale " + std::to_string(column.scale) +
								  "; TIME, DATETIME2 and DATETIMEOFFSET allow 0..7.");
		}
		break;
	case TypeInfoForm::DECIMAL:
		if (column.precision < 1 || column.precision > 38 || column.scale > column.precision) {
			Malformed(column, "precision " + std::to_string(column.precision) + " and scale " +
								  std::to_string(column.scale) +
								  "; DECIMAL and NUMERIC allow a precision of 1..38 and a scale up to it.");
		}
		break;
	case TypeInfoForm::VECTOR:
		CheckVector(column);
		break;
	default:
		break;
	}
}

//! One column: UserType (255 marks a vector column in every form it streams, spec 093 T3), flags,
//! TYPE_INFO and the name (B_VARCHAR).
bool ReadColumn(Cursor &in, ColumnMetadata &column) {
	if (!in.U32(column.user_type) || !in.U16(column.flags) || !ReadTypeInfo(in, column) || !in.BVarchar(column.name)) {
		return false;
	}
	CheckDeclaration(*FindType(column.type_id), column);
	return true;
}

}  // namespace

bool ColumnMetadataParser::Parse(const uint8_t *data, size_t length, size_t &bytes_consumed,
								 std::vector<ColumnMetadata> &columns) {
	columns.clear();
	Cursor in{data, length, 0};

	uint16_t count;
	if (!in.U16(count)) {
		return false;
	}
	// 0xFFFF: no metadata (no result set).
	if (count != 0xFFFF) {
		columns.reserve(count);
		for (uint16_t i = 0; i < count; i++) {
			ColumnMetadata column;
			if (!ReadColumn(in, column)) {
				return false;  // Need more data
			}
			columns.push_back(std::move(column));
		}
	}
	bytes_consumed = in.offset;
	return true;
}

}  // namespace tds
}  // namespace duckdb
