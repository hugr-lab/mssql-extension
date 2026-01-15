#include "tds/tds_column_metadata.hpp"
#include "encoding/utf16.hpp"
#include <stdexcept>

namespace duckdb {
namespace tds {

//===----------------------------------------------------------------------===//
// ColumnMetadata Implementation
//===----------------------------------------------------------------------===//

std::string ColumnMetadata::GetTypeName() const {
	switch (type_id) {
	case TDS_TYPE_NULL: return "NULL";
	case TDS_TYPE_TINYINT: return "TINYINT";
	case TDS_TYPE_BIT: return "BIT";
	case TDS_TYPE_SMALLINT: return "SMALLINT";
	case TDS_TYPE_INT: return "INT";
	case TDS_TYPE_BIGINT: return "BIGINT";
	case TDS_TYPE_REAL: return "REAL";
	case TDS_TYPE_FLOAT: return "FLOAT";
	case TDS_TYPE_MONEY: return "MONEY";
	case TDS_TYPE_SMALLMONEY: return "SMALLMONEY";
	case TDS_TYPE_DATETIME: return "DATETIME";
	case TDS_TYPE_SMALLDATETIME: return "SMALLDATETIME";
	case TDS_TYPE_INTN: return "INTN";
	case TDS_TYPE_BITN: return "BITN";
	case TDS_TYPE_FLOATN: return "FLOATN";
	case TDS_TYPE_MONEYN: return "MONEYN";
	case TDS_TYPE_DATETIMEN: return "DATETIMEN";
	case TDS_TYPE_DECIMAL: return "DECIMAL";
	case TDS_TYPE_NUMERIC: return "NUMERIC";
	case TDS_TYPE_UNIQUEIDENTIFIER: return "UNIQUEIDENTIFIER";
	case TDS_TYPE_BIGCHAR: return "CHAR";
	case TDS_TYPE_BIGVARCHAR: return "VARCHAR";
	case TDS_TYPE_NCHAR: return "NCHAR";
	case TDS_TYPE_NVARCHAR: return "NVARCHAR";
	case TDS_TYPE_BIGBINARY: return "BINARY";
	case TDS_TYPE_BIGVARBINARY: return "VARBINARY";
	case TDS_TYPE_DATE: return "DATE";
	case TDS_TYPE_TIME: return "TIME";
	case TDS_TYPE_DATETIME2: return "DATETIME2";
	case TDS_TYPE_DATETIMEOFFSET: return "DATETIMEOFFSET";
	case TDS_TYPE_XML: return "XML";
	case TDS_TYPE_UDT: return "UDT";
	case TDS_TYPE_SQL_VARIANT: return "SQL_VARIANT";
	case TDS_TYPE_IMAGE: return "IMAGE";
	case TDS_TYPE_TEXT: return "TEXT";
	case TDS_TYPE_NTEXT: return "NTEXT";
	default: return "UNKNOWN(0x" + std::to_string(type_id) + ")";
	}
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

size_t ColumnMetadata::GetFixedSize() const {
	switch (type_id) {
	case TDS_TYPE_TINYINT: return 1;
	case TDS_TYPE_BIT: return 1;
	case TDS_TYPE_SMALLINT: return 2;
	case TDS_TYPE_INT: return 4;
	case TDS_TYPE_BIGINT: return 8;
	case TDS_TYPE_REAL: return 4;
	case TDS_TYPE_FLOAT: return 8;
	case TDS_TYPE_MONEY: return 8;
	case TDS_TYPE_SMALLMONEY: return 4;
	case TDS_TYPE_DATETIME: return 8;
	case TDS_TYPE_SMALLDATETIME: return 4;
	case TDS_TYPE_DATE: return 3;
	default:
		return 0;  // Variable length or has length prefix
	}
}

//===----------------------------------------------------------------------===//
// ColumnMetadataParser Implementation
//===----------------------------------------------------------------------===//

bool ColumnMetadataParser::Parse(const uint8_t* data, size_t length, size_t& bytes_consumed,
                                  std::vector<ColumnMetadata>& columns) {
	columns.clear();
	size_t offset = 0;

	// Need at least 2 bytes for column count
	if (length < 2) {
		return false;
	}

	// Read column count (uint16_t LE)
	uint16_t count = static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8);
	offset = 2;

	// Special case: 0xFFFF means no metadata (no result set)
	if (count == 0xFFFF) {
		bytes_consumed = offset;
		return true;
	}

	columns.reserve(count);

	// Parse each column
	for (uint16_t i = 0; i < count; i++) {
		ColumnMetadata column;
		if (!ParseColumn(data + offset, length - offset, offset, column)) {
			return false;  // Need more data
		}
		columns.push_back(std::move(column));
	}

	bytes_consumed = offset;
	return true;
}

bool ColumnMetadataParser::ParseColumn(const uint8_t* data, size_t length, size_t& offset,
                                        ColumnMetadata& column) {
	size_t local_offset = 0;

	// Need at least 4 bytes for UserType
	if (length < 4) {
		return false;
	}

	// Skip UserType (4 bytes, legacy)
	local_offset += 4;

	// Need 2 more bytes for flags
	if (length < local_offset + 2) {
		return false;
	}

	// Read flags (uint16_t LE)
	column.flags = static_cast<uint16_t>(data[local_offset]) |
	               (static_cast<uint16_t>(data[local_offset + 1]) << 8);
	local_offset += 2;

	// Parse type info
	size_t type_offset = local_offset;
	if (!ParseTypeInfo(data, length, type_offset, column)) {
		return false;
	}
	local_offset = type_offset;

	// Parse column name (B_VARCHAR format)
	std::string name;
	if (!ParseColumnName(data, length, local_offset, name)) {
		return false;
	}
	column.name = std::move(name);

	offset += local_offset;
	return true;
}

bool ColumnMetadataParser::ParseTypeInfo(const uint8_t* data, size_t length, size_t& offset,
                                          ColumnMetadata& column) {
	if (offset >= length) {
		return false;
	}

	column.type_id = data[offset++];
	column.max_length = 0;
	column.precision = 0;
	column.scale = 0;
	column.collation = 0;

	switch (column.type_id) {
	// Fixed-length types (no additional metadata)
	case TDS_TYPE_NULL:
	case TDS_TYPE_TINYINT:
	case TDS_TYPE_BIT:
	case TDS_TYPE_SMALLINT:
	case TDS_TYPE_INT:
	case TDS_TYPE_BIGINT:
	case TDS_TYPE_REAL:
	case TDS_TYPE_FLOAT:
	case TDS_TYPE_MONEY:
	case TDS_TYPE_SMALLMONEY:
	case TDS_TYPE_DATETIME:
	case TDS_TYPE_SMALLDATETIME:
		break;

	// Nullable fixed-length types (1 byte length)
	case TDS_TYPE_INTN:
	case TDS_TYPE_BITN:
	case TDS_TYPE_FLOATN:
	case TDS_TYPE_MONEYN:
	case TDS_TYPE_DATETIMEN:
		if (offset >= length) return false;
		column.max_length = data[offset++];
		break;

	// DECIMAL/NUMERIC (1 byte length, 1 byte precision, 1 byte scale)
	case TDS_TYPE_DECIMAL:
	case TDS_TYPE_NUMERIC:
		if (offset + 3 > length) return false;
		column.max_length = data[offset++];
		column.precision = data[offset++];
		column.scale = data[offset++];
		break;

	// UNIQUEIDENTIFIER (1 byte length, always 16)
	case TDS_TYPE_UNIQUEIDENTIFIER:
		if (offset >= length) return false;
		column.max_length = data[offset++];
		break;

	// Variable-length string types (2 bytes length + 5 bytes collation)
	case TDS_TYPE_BIGCHAR:
	case TDS_TYPE_BIGVARCHAR:
	case TDS_TYPE_NCHAR:
	case TDS_TYPE_NVARCHAR:
		if (offset + 7 > length) return false;
		column.max_length = static_cast<uint16_t>(data[offset]) |
		                    (static_cast<uint16_t>(data[offset + 1]) << 8);
		offset += 2;
		// Collation (5 bytes)
		column.collation = static_cast<uint32_t>(data[offset]) |
		                   (static_cast<uint32_t>(data[offset + 1]) << 8) |
		                   (static_cast<uint32_t>(data[offset + 2]) << 16) |
		                   (static_cast<uint32_t>(data[offset + 3]) << 24);
		offset += 5;  // collation is 5 bytes but we only store 4
		break;

	// Variable-length binary types (2 bytes length)
	case TDS_TYPE_BIGBINARY:
	case TDS_TYPE_BIGVARBINARY:
		if (offset + 2 > length) return false;
		column.max_length = static_cast<uint16_t>(data[offset]) |
		                    (static_cast<uint16_t>(data[offset + 1]) << 8);
		offset += 2;
		break;

	// DATE (no additional metadata)
	case TDS_TYPE_DATE:
		break;

	// TIME (1 byte scale)
	case TDS_TYPE_TIME:
		if (offset >= length) return false;
		column.scale = data[offset++];
		break;

	// DATETIME2 (1 byte scale)
	case TDS_TYPE_DATETIME2:
		if (offset >= length) return false;
		column.scale = data[offset++];
		break;

	// DATETIMEOFFSET (1 byte scale)
	case TDS_TYPE_DATETIMEOFFSET:
		if (offset >= length) return false;
		column.scale = data[offset++];
		break;

	default:
		throw std::runtime_error("Unsupported SQL Server type: " + column.GetTypeName());
	}

	return true;
}

bool ColumnMetadataParser::ParseColumnName(const uint8_t* data, size_t length, size_t& offset,
                                            std::string& name) {
	if (offset >= length) {
		return false;
	}

	// B_VARCHAR: 1 byte character count + UTF-16LE data
	uint8_t char_count = data[offset++];
	size_t byte_length = char_count * 2;

	if (offset + byte_length > length) {
		return false;
	}

	// Decode UTF-16LE to UTF-8
	name = encoding::Utf16LEDecode(data + offset, byte_length);
	offset += byte_length;

	return true;
}

}  // namespace tds
}  // namespace duckdb
