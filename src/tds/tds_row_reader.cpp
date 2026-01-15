#include "tds/tds_row_reader.hpp"
#include "tds/tds_types.hpp"
#include <cstring>
#include <stdexcept>

namespace duckdb {
namespace tds {

RowReader::RowReader(const std::vector<ColumnMetadata>& columns)
    : columns_(columns) {
}

bool RowReader::ReadRow(const uint8_t* data, size_t length, size_t& bytes_consumed, RowData& row) {
	row.Clear();
	row.values.resize(columns_.size());
	row.null_mask.resize(columns_.size(), false);

	size_t offset = 0;

	for (size_t col_idx = 0; col_idx < columns_.size(); col_idx++) {
		std::vector<uint8_t> value;
		bool is_null = false;

		size_t consumed = ReadValue(data + offset, length - offset, col_idx, value, is_null);
		if (consumed == 0) {
			return false;  // Need more data
		}

		row.values[col_idx] = std::move(value);
		row.null_mask[col_idx] = is_null;
		offset += consumed;
	}

	bytes_consumed = offset;
	return true;
}

bool RowReader::SkipRow(const uint8_t* data, size_t length, size_t& bytes_consumed) {
	// Fast path: just calculate byte size without copying data
	size_t offset = 0;

	for (size_t col_idx = 0; col_idx < columns_.size(); col_idx++) {
		size_t consumed = SkipValue(data + offset, length - offset, col_idx);
		if (consumed == 0) {
			return false;  // Need more data
		}
		offset += consumed;
	}

	bytes_consumed = offset;
	return true;
}

size_t RowReader::SkipValue(const uint8_t* data, size_t length, size_t col_idx) {
	const ColumnMetadata& col = columns_[col_idx];

	switch (col.type_id) {
	// Fixed-length types
	case TDS_TYPE_TINYINT:
	case TDS_TYPE_BIT:
		return length >= 1 ? 1 : 0;
	case TDS_TYPE_SMALLINT:
		return length >= 2 ? 2 : 0;
	case TDS_TYPE_INT:
	case TDS_TYPE_REAL:
	case TDS_TYPE_SMALLMONEY:
	case TDS_TYPE_SMALLDATETIME:
		return length >= 4 ? 4 : 0;
	case TDS_TYPE_BIGINT:
	case TDS_TYPE_FLOAT:
	case TDS_TYPE_MONEY:
	case TDS_TYPE_DATETIME:
		return length >= 8 ? 8 : 0;

	// Nullable fixed-length (1-byte length prefix)
	case TDS_TYPE_INTN:
	case TDS_TYPE_BITN:
	case TDS_TYPE_FLOATN:
	case TDS_TYPE_MONEYN:
	case TDS_TYPE_DATETIMEN:
	case TDS_TYPE_DECIMAL:
	case TDS_TYPE_NUMERIC:
	case TDS_TYPE_DATE:
	case TDS_TYPE_TIME:
	case TDS_TYPE_DATETIME2:
	case TDS_TYPE_UNIQUEIDENTIFIER: {
		if (length < 1) return 0;
		uint8_t data_length = data[0];
		return length >= 1 + data_length ? 1 + data_length : 0;
	}

	// Variable-length (2-byte length prefix)
	case TDS_TYPE_BIGCHAR:
	case TDS_TYPE_BIGVARCHAR:
	case TDS_TYPE_NCHAR:
	case TDS_TYPE_NVARCHAR:
	case TDS_TYPE_BIGBINARY:
	case TDS_TYPE_BIGVARBINARY: {
		if (length < 2) return 0;
		uint16_t data_length = static_cast<uint16_t>(data[0]) |
		                       (static_cast<uint16_t>(data[1]) << 8);
		if (data_length == 0xFFFF) return 2;  // NULL
		return length >= 2 + data_length ? 2 + data_length : 0;
	}

	default:
		return 0;  // Unknown type - can't skip
	}
}

size_t RowReader::ReadValue(const uint8_t* data, size_t length, size_t col_idx,
                             std::vector<uint8_t>& value, bool& is_null) {
	const ColumnMetadata& col = columns_[col_idx];
	is_null = false;

	switch (col.type_id) {
	// Fixed-length types (no length prefix)
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
		return ReadFixedType(data, length, col.type_id, value);

	// Nullable fixed-length variants
	case TDS_TYPE_INTN:
	case TDS_TYPE_BITN:
	case TDS_TYPE_FLOATN:
	case TDS_TYPE_MONEYN:
	case TDS_TYPE_DATETIMEN:
		return ReadNullableFixedType(data, length, col.type_id, col.max_length, value, is_null);

	// Variable-length types
	case TDS_TYPE_BIGCHAR:
	case TDS_TYPE_BIGVARCHAR:
	case TDS_TYPE_NCHAR:
	case TDS_TYPE_NVARCHAR:
	case TDS_TYPE_BIGBINARY:
	case TDS_TYPE_BIGVARBINARY:
		return ReadVariableLengthType(data, length, col.type_id, value, is_null);

	// DECIMAL/NUMERIC
	case TDS_TYPE_DECIMAL:
	case TDS_TYPE_NUMERIC:
		return ReadDecimalType(data, length, value, is_null);

	// DATE
	case TDS_TYPE_DATE:
		return ReadDateType(data, length, value, is_null);

	// TIME
	case TDS_TYPE_TIME:
		return ReadTimeType(data, length, col.scale, value, is_null);

	// DATETIME2
	case TDS_TYPE_DATETIME2:
		return ReadDateTime2Type(data, length, col.scale, value, is_null);

	// UNIQUEIDENTIFIER
	case TDS_TYPE_UNIQUEIDENTIFIER:
		return ReadGuidType(data, length, value, is_null);

	default:
		throw std::runtime_error("Unsupported type in RowReader: " + col.GetTypeName());
	}
}

size_t RowReader::ReadFixedType(const uint8_t* data, size_t length, uint8_t type_id,
                                 std::vector<uint8_t>& value) {
	size_t size = 0;
	switch (type_id) {
	case TDS_TYPE_TINYINT:
	case TDS_TYPE_BIT:
		size = 1;
		break;
	case TDS_TYPE_SMALLINT:
		size = 2;
		break;
	case TDS_TYPE_INT:
	case TDS_TYPE_REAL:
	case TDS_TYPE_SMALLMONEY:
	case TDS_TYPE_SMALLDATETIME:
		size = 4;
		break;
	case TDS_TYPE_BIGINT:
	case TDS_TYPE_FLOAT:
	case TDS_TYPE_MONEY:
	case TDS_TYPE_DATETIME:
		size = 8;
		break;
	default:
		throw std::runtime_error("Unknown fixed type: " + std::to_string(type_id));
	}

	if (length < size) {
		return 0;  // Need more data
	}

	value.assign(data, data + size);
	return size;
}

size_t RowReader::ReadNullableFixedType(const uint8_t* data, size_t length, uint8_t type_id,
                                         uint8_t declared_length,
                                         std::vector<uint8_t>& value, bool& is_null) {
	if (length < 1) {
		return 0;  // Need more data
	}

	uint8_t actual_length = data[0];

	if (actual_length == 0) {
		// NULL value
		is_null = true;
		value.clear();
		return 1;
	}

	if (length < 1 + actual_length) {
		return 0;  // Need more data
	}

	value.assign(data + 1, data + 1 + actual_length);
	return 1 + actual_length;
}

size_t RowReader::ReadVariableLengthType(const uint8_t* data, size_t length, uint8_t type_id,
                                          std::vector<uint8_t>& value, bool& is_null) {
	if (length < 2) {
		return 0;  // Need length field
	}

	uint16_t data_length = static_cast<uint16_t>(data[0]) |
	                       (static_cast<uint16_t>(data[1]) << 8);

	// 0xFFFF indicates NULL
	if (data_length == 0xFFFF) {
		is_null = true;
		value.clear();
		return 2;
	}

	if (length < 2 + data_length) {
		return 0;  // Need more data
	}

	value.assign(data + 2, data + 2 + data_length);
	return 2 + data_length;
}

size_t RowReader::ReadDecimalType(const uint8_t* data, size_t length,
                                   std::vector<uint8_t>& value, bool& is_null) {
	if (length < 1) {
		return 0;  // Need length byte
	}

	uint8_t data_length = data[0];

	if (data_length == 0) {
		is_null = true;
		value.clear();
		return 1;
	}

	if (length < 1 + data_length) {
		return 0;  // Need more data
	}

	value.assign(data + 1, data + 1 + data_length);
	return 1 + data_length;
}

size_t RowReader::ReadDateType(const uint8_t* data, size_t length,
                                std::vector<uint8_t>& value, bool& is_null) {
	// DATE has a 1-byte length prefix (0=NULL, 3=data)
	if (length < 1) {
		return 0;
	}

	uint8_t data_length = data[0];

	if (data_length == 0) {
		is_null = true;
		value.clear();
		return 1;
	}

	// DATE data is always 3 bytes
	if (length < 1 + data_length) {
		return 0;
	}

	value.assign(data + 1, data + 1 + data_length);
	return 1 + data_length;
}

size_t RowReader::ReadTimeType(const uint8_t* data, size_t length, uint8_t scale,
                                std::vector<uint8_t>& value, bool& is_null) {
	// TIME has length prefix even though it's "fixed" size based on scale
	if (length < 1) {
		return 0;
	}

	uint8_t data_length = data[0];

	if (data_length == 0) {
		is_null = true;
		value.clear();
		return 1;
	}

	if (length < 1 + data_length) {
		return 0;
	}

	value.assign(data + 1, data + 1 + data_length);
	return 1 + data_length;
}

size_t RowReader::ReadDateTime2Type(const uint8_t* data, size_t length, uint8_t scale,
                                     std::vector<uint8_t>& value, bool& is_null) {
	// DATETIME2 has length prefix
	if (length < 1) {
		return 0;
	}

	uint8_t data_length = data[0];

	if (data_length == 0) {
		is_null = true;
		value.clear();
		return 1;
	}

	if (length < 1 + data_length) {
		return 0;
	}

	value.assign(data + 1, data + 1 + data_length);
	return 1 + data_length;
}

size_t RowReader::ReadGuidType(const uint8_t* data, size_t length,
                                std::vector<uint8_t>& value, bool& is_null) {
	if (length < 1) {
		return 0;
	}

	uint8_t data_length = data[0];

	if (data_length == 0) {
		is_null = true;
		value.clear();
		return 1;
	}

	// GUID is always 16 bytes
	if (length < 1 + 16) {
		return 0;
	}

	value.assign(data + 1, data + 1 + 16);
	return 1 + 16;
}

}  // namespace tds
}  // namespace duckdb
