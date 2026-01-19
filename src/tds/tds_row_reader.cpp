#include "tds/tds_row_reader.hpp"
#include <cstring>
#include <stdexcept>
#include "tds/tds_types.hpp"

namespace duckdb {
namespace tds {

RowReader::RowReader(const std::vector<ColumnMetadata> &columns) : columns_(columns) {}

bool RowReader::ReadRow(const uint8_t *data, size_t length, size_t &bytes_consumed, RowData &row) {
	// Use Prepare to allocate/clear efficiently (preserves capacity)
	row.Prepare(columns_.size());

	size_t offset = 0;

	for (size_t col_idx = 0; col_idx < columns_.size(); col_idx++) {
		bool is_null = false;

		// Read directly into the pre-allocated vector (clears it first, preserves capacity)
		row.values[col_idx].clear();
		size_t consumed = ReadValue(data + offset, length - offset, col_idx, row.values[col_idx], is_null);
		if (consumed == 0) {
			return false;  // Need more data
		}

		row.null_mask[col_idx] = is_null;
		offset += consumed;
	}

	bytes_consumed = offset;
	return true;
}

bool RowReader::SkipRow(const uint8_t *data, size_t length, size_t &bytes_consumed) {
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

bool RowReader::ReadNBCRow(const uint8_t *data, size_t length, size_t &bytes_consumed, RowData &row) {
	// Use Prepare to allocate/clear efficiently (preserves capacity)
	row.Prepare(columns_.size());

	// Calculate null bitmap size: ceil(columns / 8)
	size_t bitmap_bytes = (columns_.size() + 7) / 8;

	if (length < bitmap_bytes) {
		return false;  // Need more data for bitmap
	}

	// Read null bitmap and set null_mask
	for (size_t col_idx = 0; col_idx < columns_.size(); col_idx++) {
		size_t byte_idx = col_idx / 8;
		size_t bit_idx = col_idx % 8;
		bool is_null = (data[byte_idx] & (1 << bit_idx)) != 0;
		row.null_mask[col_idx] = is_null;
	}

	size_t offset = bitmap_bytes;

	// Read values for non-NULL columns only
	for (size_t col_idx = 0; col_idx < columns_.size(); col_idx++) {
		if (row.null_mask[col_idx]) {
			// Column is NULL - no data to read (already cleared by Prepare)
			continue;
		}

		bool is_null = false;

		// Read directly into the pre-allocated vector
		row.values[col_idx].clear();
		size_t consumed = ReadValueNBC(data + offset, length - offset, col_idx, row.values[col_idx], is_null);
		if (consumed == 0) {
			return false;  // Need more data
		}

		// Note: is_null should always be false here since we already checked bitmap
		offset += consumed;
	}

	bytes_consumed = offset;
	return true;
}

bool RowReader::SkipNBCRow(const uint8_t *data, size_t length, size_t &bytes_consumed) {
	// Calculate null bitmap size
	size_t bitmap_bytes = (columns_.size() + 7) / 8;

	if (length < bitmap_bytes) {
		return false;
	}

	size_t offset = bitmap_bytes;

	// Skip values for non-NULL columns only
	for (size_t col_idx = 0; col_idx < columns_.size(); col_idx++) {
		size_t byte_idx = col_idx / 8;
		size_t bit_idx = col_idx % 8;
		bool is_null = (data[byte_idx] & (1 << bit_idx)) != 0;

		if (is_null) {
			continue;  // No data for NULL column
		}

		size_t consumed = SkipValueNBC(data + offset, length - offset, col_idx);
		if (consumed == 0) {
			return false;
		}
		offset += consumed;
	}

	bytes_consumed = offset;
	return true;
}

size_t RowReader::SkipValue(const uint8_t *data, size_t length, size_t col_idx) {
	const ColumnMetadata &col = columns_[col_idx];

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
		if (length < 1)
			return 0;
		uint8_t data_length = data[0];
		return length >= 1 + data_length ? 1 + data_length : 0;
	}

	// Variable-length (2-byte length prefix, or PLP for MAX types)
	case TDS_TYPE_BIGCHAR:
	case TDS_TYPE_BIGVARCHAR:
	case TDS_TYPE_NCHAR:
	case TDS_TYPE_NVARCHAR:
	case TDS_TYPE_BIGBINARY:
	case TDS_TYPE_BIGVARBINARY: {
		// Check for MAX types (PLP encoding)
		if (col.IsPLPType()) {
			return SkipPLPType(data, length);
		}
		if (length < 2)
			return 0;
		uint16_t data_length = static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8);
		if (data_length == 0xFFFF)
			return 2;  // NULL
		return length >= 2 + data_length ? 2 + data_length : 0;
	}

	default:
		return 0;  // Unknown type - can't skip
	}
}

size_t RowReader::ReadValue(const uint8_t *data, size_t length, size_t col_idx, std::vector<uint8_t> &value,
							bool &is_null) {
	const ColumnMetadata &col = columns_[col_idx];
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
		// Check for MAX types (PLP encoding)
		if (col.IsPLPType()) {
			return ReadPLPType(data, length, value, is_null);
		}
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

	// DATETIMEOFFSET
	case TDS_TYPE_DATETIMEOFFSET:
		return ReadDateTimeOffsetType(data, length, col.scale, value, is_null);

	// UNIQUEIDENTIFIER
	case TDS_TYPE_UNIQUEIDENTIFIER:
		return ReadGuidType(data, length, value, is_null);

	default:
		throw std::runtime_error("Unsupported type in RowReader: " + col.GetTypeName());
	}
}

size_t RowReader::ReadFixedType(const uint8_t *data, size_t length, uint8_t type_id, std::vector<uint8_t> &value) {
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

size_t RowReader::ReadNullableFixedType(const uint8_t *data, size_t length, uint8_t type_id, uint8_t declared_length,
										std::vector<uint8_t> &value, bool &is_null) {
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

size_t RowReader::ReadVariableLengthType(const uint8_t *data, size_t length, uint8_t type_id,
										 std::vector<uint8_t> &value, bool &is_null) {
	if (length < 2) {
		return 0;  // Need length field
	}

	uint16_t data_length = static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8);

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

size_t RowReader::ReadDecimalType(const uint8_t *data, size_t length, std::vector<uint8_t> &value, bool &is_null) {
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

size_t RowReader::ReadDateType(const uint8_t *data, size_t length, std::vector<uint8_t> &value, bool &is_null) {
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

size_t RowReader::ReadTimeType(const uint8_t *data, size_t length, uint8_t scale, std::vector<uint8_t> &value,
							   bool &is_null) {
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

size_t RowReader::ReadDateTime2Type(const uint8_t *data, size_t length, uint8_t scale, std::vector<uint8_t> &value,
									bool &is_null) {
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

size_t RowReader::ReadDateTimeOffsetType(const uint8_t *data, size_t length, uint8_t scale, std::vector<uint8_t> &value,
										 bool &is_null) {
	// DATETIMEOFFSET has length prefix: time (3-5 bytes) + date (3 bytes) + offset (2 bytes)
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

size_t RowReader::ReadGuidType(const uint8_t *data, size_t length, std::vector<uint8_t> &value, bool &is_null) {
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

// PLP_NULL marker: 0xFFFFFFFFFFFFFFFE
static constexpr uint64_t PLP_NULL_MARKER = 0xFFFFFFFFFFFFFFFEULL;
// PLP_UNKNOWN marker: 0xFFFFFFFFFFFFFFFF (unknown total length)
static constexpr uint64_t PLP_UNKNOWN_MARKER = 0xFFFFFFFFFFFFFFFFULL;

// Debug logging for PLP parsing
static int GetPLPDebugLevel() {
	static int level = -1;
	if (level == -1) {
		const char *env = std::getenv("MSSQL_DEBUG_PLP");
		level = env ? std::atoi(env) : 0;
	}
	return level;
}

#define PLP_DEBUG(level, fmt, ...)                             \
	do {                                                       \
		if (GetPLPDebugLevel() >= level) {                     \
			fprintf(stderr, "[PLP] " fmt "\n", ##__VA_ARGS__); \
		}                                                      \
	} while (0)

size_t RowReader::ReadPLPType(const uint8_t *data, size_t length, std::vector<uint8_t> &value, bool &is_null) {
	// PLP format:
	// 8 bytes: total length (PLP_NULL=null, PLP_UNKNOWN=unknown length, else actual length)
	// Then chunks: 4-byte chunk length + chunk data, until chunk length == 0

	PLP_DEBUG(1, "ReadPLPType: buffer_length=%zu", length);

	// Hex dump first 16 bytes for debugging
	if (GetPLPDebugLevel() >= 2 && length >= 16) {
		fprintf(stderr, "[PLP] ReadPLPType: first 16 bytes: ");
		for (size_t i = 0; i < 16; i++) {
			fprintf(stderr, "%02x ", data[i]);
		}
		fprintf(stderr, "\n");
	}

	if (length < 8) {
		PLP_DEBUG(1, "ReadPLPType: need more data for header (have %zu, need 8)", length);
		return 0;  // Need total length field
	}

	// Read 8-byte total length (little-endian)
	uint64_t total_length = static_cast<uint64_t>(data[0]) | (static_cast<uint64_t>(data[1]) << 8) |
							(static_cast<uint64_t>(data[2]) << 16) | (static_cast<uint64_t>(data[3]) << 24) |
							(static_cast<uint64_t>(data[4]) << 32) | (static_cast<uint64_t>(data[5]) << 40) |
							(static_cast<uint64_t>(data[6]) << 48) | (static_cast<uint64_t>(data[7]) << 56);

	size_t offset = 8;

	PLP_DEBUG(1, "ReadPLPType: total_length=0x%llx (%llu)", (unsigned long long)total_length,
			  (unsigned long long)total_length);

	// Check for NULL
	if (total_length == PLP_NULL_MARKER) {
		PLP_DEBUG(1, "ReadPLPType: NULL value");
		is_null = true;
		value.clear();
		return offset;
	}

	is_null = false;

	// For PLP_UNKNOWN or known length, read chunks
	// Pre-allocate if we know the total length
	if (total_length != PLP_UNKNOWN_MARKER && total_length < 0x7FFFFFFF) {
		value.reserve(static_cast<size_t>(total_length));
	}
	value.clear();

	// Read chunks until terminator (chunk length == 0)
	int chunk_num = 0;
	while (true) {
		if (offset + 4 > length) {
			PLP_DEBUG(1, "ReadPLPType: need more data for chunk header (have %zu, need %zu)", length, offset + 4);
			return 0;  // Need chunk length
		}

		// Read 4-byte chunk length (little-endian)
		uint32_t chunk_length = static_cast<uint32_t>(data[offset]) | (static_cast<uint32_t>(data[offset + 1]) << 8) |
								(static_cast<uint32_t>(data[offset + 2]) << 16) |
								(static_cast<uint32_t>(data[offset + 3]) << 24);
		offset += 4;

		PLP_DEBUG(2, "ReadPLPType: chunk[%d] length=%u, offset=%zu, buffer_length=%zu", chunk_num, chunk_length, offset,
				  length);

		// Terminator: chunk length == 0
		if (chunk_length == 0) {
			PLP_DEBUG(1, "ReadPLPType: terminator found, total_read=%zu bytes in %d chunks", value.size(), chunk_num);
			break;
		}

		// Read chunk data
		if (offset + chunk_length > length) {
			PLP_DEBUG(1, "ReadPLPType: need more data for chunk (have %zu, need %zu)", length, offset + chunk_length);
			return 0;  // Need more chunk data
		}

		// Append chunk data to value
		value.insert(value.end(), data + offset, data + offset + chunk_length);
		offset += chunk_length;
		chunk_num++;
	}

	PLP_DEBUG(1, "ReadPLPType: complete, consumed=%zu bytes, value_size=%zu", offset, value.size());
	return offset;
}

size_t RowReader::SkipPLPType(const uint8_t *data, size_t length) {
	// Same structure as ReadPLPType but without copying data

	if (length < 8) {
		return 0;
	}

	// Read 8-byte total length
	uint64_t total_length = static_cast<uint64_t>(data[0]) | (static_cast<uint64_t>(data[1]) << 8) |
							(static_cast<uint64_t>(data[2]) << 16) | (static_cast<uint64_t>(data[3]) << 24) |
							(static_cast<uint64_t>(data[4]) << 32) | (static_cast<uint64_t>(data[5]) << 40) |
							(static_cast<uint64_t>(data[6]) << 48) | (static_cast<uint64_t>(data[7]) << 56);

	size_t offset = 8;

	// Check for NULL
	if (total_length == PLP_NULL_MARKER) {
		return offset;
	}

	// Skip chunks
	while (true) {
		if (offset + 4 > length) {
			return 0;
		}

		uint32_t chunk_length = static_cast<uint32_t>(data[offset]) | (static_cast<uint32_t>(data[offset + 1]) << 8) |
								(static_cast<uint32_t>(data[offset + 2]) << 16) |
								(static_cast<uint32_t>(data[offset + 3]) << 24);
		offset += 4;

		if (chunk_length == 0) {
			break;
		}

		if (offset + chunk_length > length) {
			return 0;
		}

		offset += chunk_length;
	}

	return offset;
}

// NBC row value reading - for non-NULL columns (no length prefix for nullable types)
size_t RowReader::ReadValueNBC(const uint8_t *data, size_t length, size_t col_idx, std::vector<uint8_t> &value,
							   bool &is_null) {
	const ColumnMetadata &col = columns_[col_idx];
	is_null = false;  // NBC rows don't include data for NULL columns

	switch (col.type_id) {
	// Fixed-length types (same as regular row)
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

	// Nullable fixed-length - in NBC rows, INTN/BITN/FLOATN/MONEYN/DATETIMEN still have a
	// 1-byte length prefix indicating the actual data size, followed by the data bytes.
	// The NULL bitmap only tells us if the column is NULL; non-NULL columns still include length.
	case TDS_TYPE_INTN:
	case TDS_TYPE_BITN:
	case TDS_TYPE_FLOATN:
	case TDS_TYPE_MONEYN:
	case TDS_TYPE_DATETIMEN: {
		if (length < 1)
			return 0;
		uint8_t actual_length = data[0];
		if (length < 1 + actual_length)
			return 0;
		value.assign(data + 1, data + 1 + actual_length);
		return 1 + actual_length;
	}

	// Variable-length types still have 2-byte length prefix (or PLP for MAX types)
	case TDS_TYPE_BIGCHAR:
	case TDS_TYPE_BIGVARCHAR:
	case TDS_TYPE_NCHAR:
	case TDS_TYPE_NVARCHAR:
	case TDS_TYPE_BIGBINARY:
	case TDS_TYPE_BIGVARBINARY:
		// Check for MAX types (PLP encoding)
		if (col.IsPLPType()) {
			return ReadPLPType(data, length, value, is_null);
		}
		return ReadVariableLengthType(data, length, col.type_id, value, is_null);

	// DECIMAL/NUMERIC - has 1-byte length prefix in NBC rows
	case TDS_TYPE_DECIMAL:
	case TDS_TYPE_NUMERIC: {
		if (length < 1)
			return 0;
		uint8_t data_length = data[0];
		if (length < 1 + data_length)
			return 0;
		value.assign(data + 1, data + 1 + data_length);
		return 1 + data_length;
	}

	// DATE - has 1-byte length prefix in NBC rows (0 or 3)
	case TDS_TYPE_DATE: {
		if (length < 1)
			return 0;
		uint8_t data_length = data[0];
		if (length < 1 + data_length)
			return 0;
		value.assign(data + 1, data + 1 + data_length);
		return 1 + data_length;
	}

	// TIME - has 1-byte length prefix in NBC rows
	case TDS_TYPE_TIME: {
		if (length < 1)
			return 0;
		uint8_t data_length = data[0];
		if (length < 1 + data_length)
			return 0;
		value.assign(data + 1, data + 1 + data_length);
		return 1 + data_length;
	}

	// DATETIME2 - has 1-byte length prefix in NBC rows
	case TDS_TYPE_DATETIME2: {
		if (length < 1)
			return 0;
		uint8_t data_length = data[0];
		if (length < 1 + data_length)
			return 0;
		value.assign(data + 1, data + 1 + data_length);
		return 1 + data_length;
	}

	// UNIQUEIDENTIFIER - has 1-byte length prefix in NBC rows (0 or 16)
	case TDS_TYPE_UNIQUEIDENTIFIER: {
		if (length < 1)
			return 0;
		uint8_t data_length = data[0];
		if (length < 1 + data_length)
			return 0;
		value.assign(data + 1, data + 1 + data_length);
		return 1 + data_length;
	}

	default:
		throw std::runtime_error("Unsupported type in NBC RowReader: " + col.GetTypeName());
	}
}

size_t RowReader::SkipValueNBC(const uint8_t *data, size_t length, size_t col_idx) {
	const ColumnMetadata &col = columns_[col_idx];

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

	// Nullable fixed-length - still has 1-byte length prefix in NBC rows
	case TDS_TYPE_INTN:
	case TDS_TYPE_BITN:
	case TDS_TYPE_FLOATN:
	case TDS_TYPE_MONEYN:
	case TDS_TYPE_DATETIMEN: {
		if (length < 1)
			return 0;
		uint8_t actual_length = data[0];
		return length >= 1 + actual_length ? 1 + actual_length : 0;
	}

	// Variable-length (still have 2-byte length prefix, or PLP for MAX types)
	case TDS_TYPE_BIGCHAR:
	case TDS_TYPE_BIGVARCHAR:
	case TDS_TYPE_NCHAR:
	case TDS_TYPE_NVARCHAR:
	case TDS_TYPE_BIGBINARY:
	case TDS_TYPE_BIGVARBINARY: {
		// Check for MAX types (PLP encoding)
		if (col.IsPLPType()) {
			return SkipPLPType(data, length);
		}
		if (length < 2)
			return 0;
		uint16_t data_length = static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8);
		if (data_length == 0xFFFF)
			return 2;  // Should not happen in NBC for non-NULL
		return length >= 2 + data_length ? 2 + data_length : 0;
	}

	// DECIMAL/NUMERIC - has 1-byte length prefix
	case TDS_TYPE_DECIMAL:
	case TDS_TYPE_NUMERIC: {
		if (length < 1)
			return 0;
		uint8_t data_length = data[0];
		return length >= 1 + data_length ? 1 + data_length : 0;
	}

	// DATE - has 1-byte length prefix
	case TDS_TYPE_DATE: {
		if (length < 1)
			return 0;
		uint8_t data_length = data[0];
		return length >= 1 + data_length ? 1 + data_length : 0;
	}

	// TIME - has 1-byte length prefix
	case TDS_TYPE_TIME: {
		if (length < 1)
			return 0;
		uint8_t data_length = data[0];
		return length >= 1 + data_length ? 1 + data_length : 0;
	}

	// DATETIME2 - has 1-byte length prefix
	case TDS_TYPE_DATETIME2: {
		if (length < 1)
			return 0;
		uint8_t data_length = data[0];
		return length >= 1 + data_length ? 1 + data_length : 0;
	}

	// UNIQUEIDENTIFIER - has 1-byte length prefix
	case TDS_TYPE_UNIQUEIDENTIFIER: {
		if (length < 1)
			return 0;
		uint8_t data_length = data[0];
		return length >= 1 + data_length ? 1 + data_length : 0;
	}

	default:
		return 0;  // Unknown type
	}
}

}  // namespace tds
}  // namespace duckdb
