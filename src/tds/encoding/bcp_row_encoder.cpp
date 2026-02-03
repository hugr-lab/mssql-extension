#include "tds/encoding/bcp_row_encoder.hpp"

#include "copy/target_resolver.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/vector.hpp"
#include "tds/encoding/utf16.hpp"

#include <cstring>

namespace duckdb {
namespace tds {
namespace encoding {

// Days from 0001-01-01 to 1970-01-01 (Unix epoch)
constexpr int32_t DAYS_FROM_0001_TO_EPOCH = 719162;

// Microseconds per day
constexpr int64_t MICROS_PER_DAY = 86400000000LL;

//===----------------------------------------------------------------------===//
// Helper: Get value from vector (handles both flat and constant vectors)
//===----------------------------------------------------------------------===//

template <typename T>
static T GetVectorValue(Vector &vec, idx_t row_idx) {
	UnifiedVectorFormat format;
	vec.ToUnifiedFormat(1, format);	 // We only need format info, not count
	auto data = UnifiedVectorFormat::GetData<T>(format);
	auto idx = format.sel->get_index(row_idx);
	return data[idx];
}

static bool IsVectorNull(Vector &vec, idx_t row_idx) {
	UnifiedVectorFormat format;
	vec.ToUnifiedFormat(1, format);
	auto idx = format.sel->get_index(row_idx);
	return !format.validity.RowIsValid(idx);
}

//===----------------------------------------------------------------------===//
// Row-Level Encoding
//===----------------------------------------------------------------------===//

void BCPRowEncoder::EncodeRow(vector<uint8_t> &buffer, DataChunk &chunk, idx_t row_idx,
							  const vector<mssql::BCPColumnMetadata> &columns, const vector<int32_t> *column_mapping) {
	for (idx_t target_idx = 0; target_idx < columns.size(); target_idx++) {
		auto &col = columns[target_idx];

		// Determine source column index
		// If column_mapping is provided, use it; otherwise assume 1:1 positional mapping
		int32_t source_idx = column_mapping ? (*column_mapping)[target_idx] : static_cast<int32_t>(target_idx);

		// If source_idx is -1 or out of range, encode NULL for this target column
		if (source_idx < 0 || static_cast<idx_t>(source_idx) >= chunk.ColumnCount()) {
			// Encode NULL based on type
			if (col.IsPLPType()) {
				EncodeNullPLP(buffer);
			} else if (col.IsVariableLengthUSHORT()) {
				EncodeNullVariable(buffer);
			} else {
				EncodeNullFixed(buffer);
			}
			continue;
		}

		auto &vec = chunk.data[source_idx];

		// Check for NULL (handles both flat and constant vectors)
		if (IsVectorNull(vec, row_idx)) {
			// Encode NULL based on type
			if (col.IsPLPType()) {
				EncodeNullPLP(buffer);
			} else if (col.IsVariableLengthUSHORT()) {
				EncodeNullVariable(buffer);
			} else {
				EncodeNullFixed(buffer);
			}
			continue;
		}

		// Get the value and encode based on type
		// Using GetVectorValue to handle both flat and constant vectors
		switch (col.duckdb_type.id()) {
		case LogicalTypeId::BOOLEAN: {
			EncodeBit(buffer, GetVectorValue<bool>(vec, row_idx));
			break;
		}
		case LogicalTypeId::TINYINT: {
			EncodeInt8(buffer, GetVectorValue<int8_t>(vec, row_idx));
			break;
		}
		case LogicalTypeId::UTINYINT: {
			EncodeUInt8(buffer, GetVectorValue<uint8_t>(vec, row_idx));
			break;
		}
		case LogicalTypeId::SMALLINT: {
			EncodeInt16(buffer, GetVectorValue<int16_t>(vec, row_idx));
			break;
		}
		case LogicalTypeId::USMALLINT: {
			// USMALLINT (0-65535) needs 4 bytes (int) to fit without overflow
			EncodeInt32(buffer, static_cast<int32_t>(GetVectorValue<uint16_t>(vec, row_idx)));
			break;
		}
		case LogicalTypeId::INTEGER: {
			EncodeInt32(buffer, GetVectorValue<int32_t>(vec, row_idx));
			break;
		}
		case LogicalTypeId::UINTEGER: {
			// UINTEGER (0-4B) needs 8 bytes (bigint) to fit without overflow
			EncodeInt64(buffer, static_cast<int64_t>(GetVectorValue<uint32_t>(vec, row_idx)));
			break;
		}
		case LogicalTypeId::BIGINT: {
			EncodeInt64(buffer, GetVectorValue<int64_t>(vec, row_idx));
			break;
		}
		case LogicalTypeId::UBIGINT: {
			// UBIGINT (0-18e18) uses DECIMAL(20,0) to handle full range
			// Use two-argument constructor hugeint_t(upper=0, lower=val) to avoid
			// sign issues when val > INT64_MAX (the single-arg constructor takes int64_t)
			uint64_t val = GetVectorValue<uint64_t>(vec, row_idx);
			EncodeDecimal(buffer, hugeint_t(0, val), col.precision, col.scale);
			break;
		}
		case LogicalTypeId::FLOAT: {
			EncodeFloat(buffer, GetVectorValue<float>(vec, row_idx));
			break;
		}
		case LogicalTypeId::DOUBLE: {
			EncodeDouble(buffer, GetVectorValue<double>(vec, row_idx));
			break;
		}
		case LogicalTypeId::DECIMAL: {
			// DuckDB stores DECIMAL in different internal types based on precision:
			// precision <= 4: int16_t, <= 9: int32_t, <= 18: int64_t, > 18: hugeint_t
			auto internal_type = vec.GetType().InternalType();
			hugeint_t decimal_value;
			switch (internal_type) {
			case PhysicalType::INT16:
				decimal_value = hugeint_t(GetVectorValue<int16_t>(vec, row_idx));
				break;
			case PhysicalType::INT32:
				decimal_value = hugeint_t(GetVectorValue<int32_t>(vec, row_idx));
				break;
			case PhysicalType::INT64:
				decimal_value = hugeint_t(GetVectorValue<int64_t>(vec, row_idx));
				break;
			case PhysicalType::INT128:
				decimal_value = GetVectorValue<hugeint_t>(vec, row_idx);
				break;
			default:
				throw InternalException("Unexpected physical type for DECIMAL: %s", TypeIdToString(internal_type));
			}
			EncodeDecimal(buffer, decimal_value, col.precision, col.scale);
			break;
		}
		case LogicalTypeId::VARCHAR: {
			auto str_val = GetVectorValue<string_t>(vec, row_idx);
			if (col.IsPLPType()) {
				EncodeNVarcharPLP(buffer, str_val);
			} else {
				EncodeNVarchar(buffer, str_val);
			}
			break;
		}
		case LogicalTypeId::BLOB: {
			auto blob_val = GetVectorValue<string_t>(vec, row_idx);
			if (col.IsPLPType()) {
				EncodeBinaryPLP(buffer, blob_val);
			} else {
				EncodeBinary(buffer, blob_val);
			}
			break;
		}
		case LogicalTypeId::UUID: {
			EncodeGUID(buffer, GetVectorValue<hugeint_t>(vec, row_idx));
			break;
		}
		case LogicalTypeId::DATE: {
			EncodeDate(buffer, GetVectorValue<date_t>(vec, row_idx));
			break;
		}
		case LogicalTypeId::TIME: {
			EncodeTime(buffer, GetVectorValue<dtime_t>(vec, row_idx), col.scale);
			break;
		}
		case LogicalTypeId::TIMESTAMP:
		case LogicalTypeId::TIMESTAMP_MS:
		case LogicalTypeId::TIMESTAMP_NS:
		case LogicalTypeId::TIMESTAMP_SEC: {
			EncodeDatetime2(buffer, GetVectorValue<timestamp_t>(vec, row_idx), col.scale);
			break;
		}
		case LogicalTypeId::TIMESTAMP_TZ: {
			// DuckDB stores TIMESTAMP_TZ as UTC, send with offset 0
			EncodeDatetimeOffset(buffer, GetVectorValue<timestamp_t>(vec, row_idx), 0, col.scale);
			break;
		}
		default:
			throw NotImplementedException("MSSQL: Unsupported type for BCP encoding: %s", col.duckdb_type.ToString());
		}
	}
}

void BCPRowEncoder::EncodeValue(vector<uint8_t> &buffer, const Value &value, const mssql::BCPColumnMetadata &col) {
	if (value.IsNull()) {
		if (col.IsPLPType()) {
			EncodeNullPLP(buffer);
		} else if (col.IsVariableLengthUSHORT()) {
			EncodeNullVariable(buffer);
		} else {
			EncodeNullFixed(buffer);
		}
		return;
	}

	switch (col.duckdb_type.id()) {
	case LogicalTypeId::BOOLEAN:
		EncodeBit(buffer, value.GetValue<bool>());
		break;
	case LogicalTypeId::TINYINT:
		EncodeInt8(buffer, value.GetValue<int8_t>());
		break;
	case LogicalTypeId::UTINYINT:
		EncodeUInt8(buffer, value.GetValue<uint8_t>());
		break;
	case LogicalTypeId::SMALLINT:
		EncodeInt16(buffer, value.GetValue<int16_t>());
		break;
	case LogicalTypeId::USMALLINT:
		EncodeInt32(buffer, static_cast<int32_t>(value.GetValue<uint16_t>()));
		break;
	case LogicalTypeId::INTEGER:
		EncodeInt32(buffer, value.GetValue<int32_t>());
		break;
	case LogicalTypeId::UINTEGER:
		EncodeInt64(buffer, static_cast<int64_t>(value.GetValue<uint32_t>()));
		break;
	case LogicalTypeId::BIGINT:
		EncodeInt64(buffer, value.GetValue<int64_t>());
		break;
	case LogicalTypeId::UBIGINT: {
		// Use two-argument constructor hugeint_t(upper=0, lower=val) to avoid
		// sign issues when val > INT64_MAX
		uint64_t val = value.GetValue<uint64_t>();
		EncodeDecimal(buffer, hugeint_t(0, val), col.precision, col.scale);
		break;
	}
	case LogicalTypeId::FLOAT:
		EncodeFloat(buffer, value.GetValue<float>());
		break;
	case LogicalTypeId::DOUBLE:
		EncodeDouble(buffer, value.GetValue<double>());
		break;
	case LogicalTypeId::DECIMAL:
		EncodeDecimal(buffer, value.GetValue<hugeint_t>(), col.precision, col.scale);
		break;
	case LogicalTypeId::VARCHAR:
		if (col.IsPLPType()) {
			EncodeNVarcharPLP(buffer, string_t(value.ToString()));
		} else {
			EncodeNVarchar(buffer, string_t(value.ToString()));
		}
		break;
	case LogicalTypeId::BLOB:
		if (col.IsPLPType()) {
			EncodeBinaryPLP(buffer, string_t(value.GetValueUnsafe<string>()));
		} else {
			EncodeBinary(buffer, string_t(value.GetValueUnsafe<string>()));
		}
		break;
	case LogicalTypeId::UUID:
		EncodeGUID(buffer, value.GetValue<hugeint_t>());
		break;
	case LogicalTypeId::DATE:
		EncodeDate(buffer, value.GetValue<date_t>());
		break;
	case LogicalTypeId::TIME:
		EncodeTime(buffer, value.GetValue<dtime_t>(), col.scale);
		break;
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_MS:
	case LogicalTypeId::TIMESTAMP_NS:
	case LogicalTypeId::TIMESTAMP_SEC:
		EncodeDatetime2(buffer, value.GetValue<timestamp_t>(), col.scale);
		break;
	case LogicalTypeId::TIMESTAMP_TZ:
		EncodeDatetimeOffset(buffer, value.GetValue<timestamp_t>(), 0, col.scale);
		break;
	default:
		throw NotImplementedException("MSSQL: Unsupported type for BCP encoding: %s", col.duckdb_type.ToString());
	}
}

//===----------------------------------------------------------------------===//
// Integer Types (INTNTYPE 0x26)
//===----------------------------------------------------------------------===//

void BCPRowEncoder::EncodeInt8(vector<uint8_t> &buffer, int8_t value) {
	buffer.push_back(1);  // length
	buffer.push_back(static_cast<uint8_t>(value));
}

void BCPRowEncoder::EncodeInt16(vector<uint8_t> &buffer, int16_t value) {
	buffer.push_back(2);  // length
	buffer.push_back(static_cast<uint8_t>(value & 0xFF));
	buffer.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
}

void BCPRowEncoder::EncodeInt32(vector<uint8_t> &buffer, int32_t value) {
	buffer.push_back(4);  // length
	for (int i = 0; i < 4; i++) {
		buffer.push_back(static_cast<uint8_t>((value >> (i * 8)) & 0xFF));
	}
}

void BCPRowEncoder::EncodeInt64(vector<uint8_t> &buffer, int64_t value) {
	buffer.push_back(8);  // length
	for (int i = 0; i < 8; i++) {
		buffer.push_back(static_cast<uint8_t>((value >> (i * 8)) & 0xFF));
	}
}

void BCPRowEncoder::EncodeUInt8(vector<uint8_t> &buffer, uint8_t value) {
	buffer.push_back(1);  // length
	buffer.push_back(value);
}

//===----------------------------------------------------------------------===//
// Bit Type (BITNTYPE 0x68)
//===----------------------------------------------------------------------===//

void BCPRowEncoder::EncodeBit(vector<uint8_t> &buffer, bool value) {
	buffer.push_back(1);  // length
	buffer.push_back(value ? 0x01 : 0x00);
}

//===----------------------------------------------------------------------===//
// Float Types (FLTNTYPE 0x6D)
//===----------------------------------------------------------------------===//

void BCPRowEncoder::EncodeFloat(vector<uint8_t> &buffer, float value) {
	buffer.push_back(4);  // length
	uint32_t bits;
	memcpy(&bits, &value, sizeof(bits));
	for (int i = 0; i < 4; i++) {
		buffer.push_back(static_cast<uint8_t>((bits >> (i * 8)) & 0xFF));
	}
}

void BCPRowEncoder::EncodeDouble(vector<uint8_t> &buffer, double value) {
	buffer.push_back(8);  // length
	uint64_t bits;
	memcpy(&bits, &value, sizeof(bits));
	for (int i = 0; i < 8; i++) {
		buffer.push_back(static_cast<uint8_t>((bits >> (i * 8)) & 0xFF));
	}
}

//===----------------------------------------------------------------------===//
// Decimal Type (DECIMALNTYPE 0x6A)
//===----------------------------------------------------------------------===//

void BCPRowEncoder::EncodeDecimal(vector<uint8_t> &buffer, const hugeint_t &value, uint8_t precision, uint8_t scale) {
	// Determine the byte size based on precision
	uint8_t byte_size = GetDecimalByteSize(precision);
	buffer.push_back(byte_size);

	// Determine sign and absolute value
	bool is_negative = value.upper < 0;
	hugeint_t abs_value = is_negative ? Hugeint::Negate(value) : value;

	// Write sign byte: 0x00 = negative, 0x01 = non-negative
	buffer.push_back(is_negative ? 0x00 : 0x01);

	// Write mantissa as little-endian
	// For decimal(p,s), mantissa size is byte_size - 1 (excluding sign byte)
	uint8_t mantissa_size = byte_size - 1;

	// Convert hugeint to bytes (little-endian)
	for (uint8_t i = 0; i < mantissa_size; i++) {
		uint8_t byte_val;
		if (i < 8) {
			byte_val = static_cast<uint8_t>((abs_value.lower >> (i * 8)) & 0xFF);
		} else {
			byte_val = static_cast<uint8_t>((static_cast<uint64_t>(abs_value.upper) >> ((i - 8) * 8)) & 0xFF);
		}
		buffer.push_back(byte_val);
	}
}

//===----------------------------------------------------------------------===//
// Unicode String (NVARCHARTYPE 0xE7)
//===----------------------------------------------------------------------===//

void BCPRowEncoder::EncodeNVarchar(vector<uint8_t> &buffer, const string_t &value) {
	// Optimized: encode directly to buffer without intermediate allocation
	size_t input_len = value.GetSize();
	const char *input = value.GetData();

	// Reserve space: 2 bytes for length + max UTF-16 size (input_len * 2 for ASCII, may be less for non-ASCII)
	size_t start_pos = buffer.size();
	buffer.resize(start_pos + 2 + input_len * 2);

	// Encode directly to buffer (skip 2 bytes for length prefix)
	size_t utf16_len = Utf16LEEncodeDirect(input, input_len, buffer.data() + start_pos + 2);

	// Write actual length
	buffer[start_pos] = static_cast<uint8_t>(utf16_len & 0xFF);
	buffer[start_pos + 1] = static_cast<uint8_t>((utf16_len >> 8) & 0xFF);

	// Shrink buffer to actual size (in case non-ASCII used less space)
	buffer.resize(start_pos + 2 + utf16_len);
}

//===----------------------------------------------------------------------===//
// Binary Data (BIGVARBINARYTYPE 0xA5)
//===----------------------------------------------------------------------===//

void BCPRowEncoder::EncodeBinary(vector<uint8_t> &buffer, const string_t &value) {
	uint16_t len = static_cast<uint16_t>(value.GetSize());

	// Write length as USHORT (little-endian)
	buffer.push_back(static_cast<uint8_t>(len & 0xFF));
	buffer.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));

	// Write bytes
	const uint8_t *data = reinterpret_cast<const uint8_t *>(value.GetData());
	buffer.insert(buffer.end(), data, data + len);
}

//===----------------------------------------------------------------------===//
// PLP (Partially Length-prefixed) Encoding for MAX types
//===----------------------------------------------------------------------===//

void BCPRowEncoder::EncodeNVarcharPLP(vector<uint8_t> &buffer, const string_t &value) {
	// Optimized: encode directly to buffer without intermediate allocation
	size_t input_len = value.GetSize();
	const char *input = value.GetData();

	// Handle empty string: PLP with no chunks, just terminator
	// PLP chunks must have length > 0, so empty string = no chunks
	if (input_len == 0) {
		// Write UNKNOWN_PLP_LEN (0xFFFFFFFFFFFFFFFE)
		constexpr uint64_t UNKNOWN_PLP_LEN = 0xFFFFFFFFFFFFFFFEULL;
		for (int i = 0; i < 8; i++) {
			buffer.push_back(static_cast<uint8_t>((UNKNOWN_PLP_LEN >> (i * 8)) & 0xFF));
		}
		// Write terminator (4 bytes of 0x00) - no chunks for empty string
		buffer.push_back(0x00);
		buffer.push_back(0x00);
		buffer.push_back(0x00);
		buffer.push_back(0x00);
		return;
	}

	// PLP format: 8 bytes (UNKNOWN_LEN) + 4 bytes (chunk len) + data + 4 bytes (terminator)
	size_t start_pos = buffer.size();
	size_t max_utf16_len = input_len * 2;
	buffer.resize(start_pos + 8 + 4 + max_utf16_len + 4);

	uint8_t *out = buffer.data() + start_pos;

	// Write UNKNOWN_PLP_LEN (0xFFFFFFFFFFFFFFFE)
	constexpr uint64_t UNKNOWN_PLP_LEN = 0xFFFFFFFFFFFFFFFEULL;
	for (int i = 0; i < 8; i++) {
		out[i] = static_cast<uint8_t>((UNKNOWN_PLP_LEN >> (i * 8)) & 0xFF);
	}
	out += 8;

	// Reserve 4 bytes for chunk length (will fill in after encoding)
	uint8_t *chunk_len_ptr = out;
	out += 4;

	// Encode UTF-16 directly
	size_t utf16_len = Utf16LEEncodeDirect(input, input_len, out);
	out += utf16_len;

	// Write actual chunk length
	uint32_t chunk_len = static_cast<uint32_t>(utf16_len);
	chunk_len_ptr[0] = static_cast<uint8_t>(chunk_len & 0xFF);
	chunk_len_ptr[1] = static_cast<uint8_t>((chunk_len >> 8) & 0xFF);
	chunk_len_ptr[2] = static_cast<uint8_t>((chunk_len >> 16) & 0xFF);
	chunk_len_ptr[3] = static_cast<uint8_t>((chunk_len >> 24) & 0xFF);

	// Write terminator (4 bytes of 0x00)
	out[0] = 0x00;
	out[1] = 0x00;
	out[2] = 0x00;
	out[3] = 0x00;

	// Shrink buffer to actual size
	buffer.resize(start_pos + 8 + 4 + utf16_len + 4);
}

void BCPRowEncoder::EncodeBinaryPLP(vector<uint8_t> &buffer, const string_t &value) {
	// Use UNKNOWN_PLP_LEN (0xFFFFFFFFFFFFFFFE) instead of actual length
	// This is how Microsoft BCP and FreeTDS handle varbinary(max) in bulk load
	constexpr uint64_t UNKNOWN_PLP_LEN = 0xFFFFFFFFFFFFFFFEULL;
	for (int i = 0; i < 8; i++) {
		buffer.push_back(static_cast<uint8_t>((UNKNOWN_PLP_LEN >> (i * 8)) & 0xFF));
	}

	// Handle empty binary: PLP with no chunks, just terminator
	// PLP chunks must have length > 0, so empty binary = no chunks
	uint32_t chunk_len = static_cast<uint32_t>(value.GetSize());
	if (chunk_len > 0) {
		// Write single chunk: chunk length (4 bytes) + data
		for (int i = 0; i < 4; i++) {
			buffer.push_back(static_cast<uint8_t>((chunk_len >> (i * 8)) & 0xFF));
		}
		const uint8_t *data = reinterpret_cast<const uint8_t *>(value.GetData());
		buffer.insert(buffer.end(), data, data + chunk_len);
	}

	// Write terminator (4 bytes of 0x00) to signal end of PLP chunks
	buffer.push_back(0x00);
	buffer.push_back(0x00);
	buffer.push_back(0x00);
	buffer.push_back(0x00);
}

//===----------------------------------------------------------------------===//
// GUID (GUIDTYPE 0x24)
//===----------------------------------------------------------------------===//

void BCPRowEncoder::EncodeGUID(vector<uint8_t> &buffer, const hugeint_t &uuid) {
	// Write length (always 16 for GUID)
	buffer.push_back(16);

	// DuckDB stores UUID with high bit flipped for sortability
	// We need to unflip it first
	uint64_t upper = static_cast<uint64_t>(uuid.upper) ^ (uint64_t(1) << 63);
	uint64_t lower = uuid.lower;

	// Convert big-endian UUID to TDS mixed-endian GUID format
	// Standard UUID (big-endian): bytes 0-3=Data1, 4-5=Data2, 6-7=Data3, 8-15=Data4
	// TDS GUID (mixed-endian): Data1 LE, Data2 LE, Data3 LE, Data4 BE

	// Extract bytes in big-endian order
	uint8_t be_bytes[16];
	for (int i = 0; i < 8; i++) {
		be_bytes[i] = static_cast<uint8_t>((upper >> (56 - i * 8)) & 0xFF);
		be_bytes[i + 8] = static_cast<uint8_t>((lower >> (56 - i * 8)) & 0xFF);
	}

	// Write Data1 (bytes 0-3) as little-endian
	buffer.push_back(be_bytes[3]);
	buffer.push_back(be_bytes[2]);
	buffer.push_back(be_bytes[1]);
	buffer.push_back(be_bytes[0]);

	// Write Data2 (bytes 4-5) as little-endian
	buffer.push_back(be_bytes[5]);
	buffer.push_back(be_bytes[4]);

	// Write Data3 (bytes 6-7) as little-endian
	buffer.push_back(be_bytes[7]);
	buffer.push_back(be_bytes[6]);

	// Write Data4 (bytes 8-15) as-is (big-endian)
	for (int i = 8; i < 16; i++) {
		buffer.push_back(be_bytes[i]);
	}
}

//===----------------------------------------------------------------------===//
// Date/Time Types
//===----------------------------------------------------------------------===//

void BCPRowEncoder::EncodeDate(vector<uint8_t> &buffer, date_t value) {
	// DATE: 3 bytes unsigned little-endian, days since 0001-01-01
	// Convert from DuckDB date_t (days since 1970-01-01)
	int32_t days = value.days + DAYS_FROM_0001_TO_EPOCH;

	// Write length
	buffer.push_back(3);

	// Write 3 bytes little-endian
	buffer.push_back(static_cast<uint8_t>(days & 0xFF));
	buffer.push_back(static_cast<uint8_t>((days >> 8) & 0xFF));
	buffer.push_back(static_cast<uint8_t>((days >> 16) & 0xFF));
}

void BCPRowEncoder::EncodeTime(vector<uint8_t> &buffer, dtime_t value, uint8_t scale) {
	// TIME: 3-5 bytes depending on scale, stored as 10^(-scale) seconds since midnight
	// DuckDB dtime_t is microseconds since midnight

	// Convert microseconds to the appropriate scale
	int64_t scaled_value;
	if (scale <= 6) {
		// Divide for scales 0-6
		int64_t divisor = 1;
		for (int i = 0; i < 6 - scale; i++) {
			divisor *= 10;
		}
		scaled_value = value.micros / divisor;
	} else {
		// Multiply for scale 7 (100ns units)
		scaled_value = value.micros * 10;
	}

	uint8_t byte_size = GetTimeByteSize(scale);
	buffer.push_back(byte_size);

	// Write little-endian bytes
	for (uint8_t i = 0; i < byte_size; i++) {
		buffer.push_back(static_cast<uint8_t>((scaled_value >> (i * 8)) & 0xFF));
	}
}

void BCPRowEncoder::EncodeDatetime2(vector<uint8_t> &buffer, timestamp_t ts, uint8_t scale) {
	// DATETIME2: time (3-5 bytes) + date (3 bytes)
	uint64_t time_value;
	uint32_t date_value;
	TimestampToDatetime2Components(ts, scale, time_value, date_value);

	uint8_t time_size = GetTimeByteSize(scale);
	uint8_t total_size = time_size + 3;

	buffer.push_back(total_size);

	// Write time portion (little-endian)
	for (uint8_t i = 0; i < time_size; i++) {
		buffer.push_back(static_cast<uint8_t>((time_value >> (i * 8)) & 0xFF));
	}

	// Write date portion (3 bytes little-endian)
	buffer.push_back(static_cast<uint8_t>(date_value & 0xFF));
	buffer.push_back(static_cast<uint8_t>((date_value >> 8) & 0xFF));
	buffer.push_back(static_cast<uint8_t>((date_value >> 16) & 0xFF));
}

void BCPRowEncoder::EncodeDatetimeOffset(vector<uint8_t> &buffer, timestamp_t ts, int16_t offset_minutes,
										 uint8_t scale) {
	// DATETIMEOFFSET: time (3-5 bytes) + date (3 bytes) + offset (2 bytes signed)
	uint64_t time_value;
	uint32_t date_value;
	TimestampToDatetime2Components(ts, scale, time_value, date_value);

	uint8_t time_size = GetTimeByteSize(scale);
	uint8_t total_size = time_size + 3 + 2;

	buffer.push_back(total_size);

	// Write time portion (little-endian)
	for (uint8_t i = 0; i < time_size; i++) {
		buffer.push_back(static_cast<uint8_t>((time_value >> (i * 8)) & 0xFF));
	}

	// Write date portion (3 bytes little-endian)
	buffer.push_back(static_cast<uint8_t>(date_value & 0xFF));
	buffer.push_back(static_cast<uint8_t>((date_value >> 8) & 0xFF));
	buffer.push_back(static_cast<uint8_t>((date_value >> 16) & 0xFF));

	// Write offset (2 bytes signed little-endian)
	buffer.push_back(static_cast<uint8_t>(offset_minutes & 0xFF));
	buffer.push_back(static_cast<uint8_t>((offset_minutes >> 8) & 0xFF));
}

//===----------------------------------------------------------------------===//
// NULL Encoding
//===----------------------------------------------------------------------===//

void BCPRowEncoder::EncodeNullFixed(vector<uint8_t> &buffer) {
	buffer.push_back(0x00);	 // length = 0 indicates NULL
}

void BCPRowEncoder::EncodeNullVariable(vector<uint8_t> &buffer) {
	buffer.push_back(0xFF);	 // 0xFFFF indicates NULL for USHORTLEN
	buffer.push_back(0xFF);
}

void BCPRowEncoder::EncodeNullGUID(vector<uint8_t> &buffer) {
	buffer.push_back(0x00);	 // length = 0 indicates NULL
}

void BCPRowEncoder::EncodeNullDateTime(vector<uint8_t> &buffer) {
	buffer.push_back(0x00);	 // length = 0 indicates NULL
}

void BCPRowEncoder::EncodeNullPLP(vector<uint8_t> &buffer) {
	// PLP NULL is 8 bytes of 0xFF
	for (int i = 0; i < 8; i++) {
		buffer.push_back(0xFF);
	}
}

//===----------------------------------------------------------------------===//
// Helper Methods
//===----------------------------------------------------------------------===//

vector<uint8_t> BCPRowEncoder::StringToUTF16LE(const string_t &str) {
	// Use existing UTF-16 encoding utility
	return Utf16LEEncode(str.GetString());
}

uint8_t BCPRowEncoder::GetTimeByteSize(uint8_t scale) {
	if (scale <= 2) {
		return 3;
	} else if (scale <= 4) {
		return 4;
	} else {
		return 5;
	}
}

uint8_t BCPRowEncoder::GetDecimalByteSize(uint8_t precision) {
	if (precision <= 9) {
		return 5;  // 1 sign + 4 mantissa
	} else if (precision <= 19) {
		return 9;  // 1 sign + 8 mantissa
	} else if (precision <= 28) {
		return 13;	// 1 sign + 12 mantissa
	} else {
		return 17;	// 1 sign + 16 mantissa
	}
}

void BCPRowEncoder::TimestampToDatetime2Components(timestamp_t ts, uint8_t scale, uint64_t &time_value,
												   uint32_t &date_value) {
	// DuckDB timestamp_t is microseconds since 1970-01-01 00:00:00
	int64_t total_micros = ts.value;

	// Split into days and time-of-day
	int32_t days;
	int64_t time_micros;

	if (total_micros >= 0) {
		days = static_cast<int32_t>(total_micros / MICROS_PER_DAY);
		time_micros = total_micros % MICROS_PER_DAY;
	} else {
		// Handle negative timestamps (before 1970)
		days = static_cast<int32_t>((total_micros - MICROS_PER_DAY + 1) / MICROS_PER_DAY);
		time_micros = total_micros - (static_cast<int64_t>(days) * MICROS_PER_DAY);
	}

	// Convert days to SQL Server format (days since 0001-01-01)
	date_value = static_cast<uint32_t>(days + DAYS_FROM_0001_TO_EPOCH);

	// Convert time to the appropriate scale
	if (scale <= 6) {
		// Divide for scales 0-6
		int64_t divisor = 1;
		for (int i = 0; i < 6 - scale; i++) {
			divisor *= 10;
		}
		time_value = static_cast<uint64_t>(time_micros / divisor);
	} else {
		// Multiply for scale 7 (100ns units)
		time_value = static_cast<uint64_t>(time_micros * 10);
	}
}

}  // namespace encoding
}  // namespace tds
}  // namespace duckdb
