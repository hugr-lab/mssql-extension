#include "tds/encoding/bcp_row_encoder.hpp"

#include "codec/write_column_ops.hpp"

#include "tds/tds_types.hpp"

#include "codec/binary_codec.hpp"
#include "codec/boolean_codec.hpp"
#include "codec/datetime_codec.hpp"
#include "codec/decimal_codec.hpp"
#include "codec/float_codec.hpp"
#include "codec/integer_codec.hpp"
#include "codec/string_codec.hpp"
#include "codec/type_family.hpp"
#include "codec/uuid_codec.hpp"
#include "codec/vector_format.hpp"
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

// Money family is decode-only (DuckDB has no MONEY LogicalType — SQL Server
// MONEY/SMALLMONEY/MONEYN tokens surface as DECIMAL on decode). The Money arm
// in BCP encode is a compile-required exhaustiveness placeholder + runtime
// assertion: FamilyFromLogicalType never returns Money, so reaching this
// throw indicates the LogicalType→family mapping has drifted.
[[noreturn]] static void ThrowMoneyUnreachable(const LogicalType &type) {
	throw InternalException(
		"BCP encoder reached Money family arm for DuckDB type '%s' — "
		"Money is decode-only and has no LogicalType mapping",
		type.ToString());
}

//===----------------------------------------------------------------------===//
// W1+W2 (spec 054): per-column encode state, hoisted once per chunk.
//===----------------------------------------------------------------------===//

namespace {

// TDS BCP ROW token (written by EncodeChunk, one per row).
constexpr uint8_t BCP_TOKEN_ROW = 0xD1;

// Family encoder signature — the format-threaded codec overloads.
using EncodeFn = void (*)(Vector &, const UnifiedVectorFormat &, idx_t, const mssql::BCPColumnMetadata &,
						  vector<uint8_t> &);

// Wire form of NULL for a column, resolved once (was an IsPLPType /
// IsVariableLengthUSHORT branch pair per cell).
enum class NullWireKind : uint8_t { Plp, VariableUShort, Fixed };

[[noreturn]] static void EncodeMoneyUnreachable(Vector &vec, const UnifiedVectorFormat &, idx_t,
												const mssql::BCPColumnMetadata &, vector<uint8_t> &) {
	ThrowMoneyUnreachable(vec.GetType());
}

// W2: resolve the family encoder once per column (was a 9-way switch per row).
EncodeFn ResolveEncoder(const mssql::BCPColumnMetadata &col) {
	mssql::codec::TypeFamily family;
	try {
		family = mssql::codec::FamilyFromLogicalType(col.duckdb_type);
	} catch (const NotImplementedException &) {
		throw NotImplementedException("MSSQL: Unsupported type for BCP encoding: %s", col.duckdb_type.ToString());
	}
	switch (family) {
	case mssql::codec::TypeFamily::Boolean:
		return mssql::codec::boolean::EncodeToBcp;
	case mssql::codec::TypeFamily::Integer:
		return mssql::codec::integer::EncodeToBcp;
	case mssql::codec::TypeFamily::Float:
		return mssql::codec::float_family::EncodeToBcp;
	case mssql::codec::TypeFamily::Decimal:
		return mssql::codec::decimal::EncodeToBcp;
	case mssql::codec::TypeFamily::String:
		// Spec 060: a UTF-8 target takes the bytes as they are; anything else is
		// transcoded to UTF-16. Resolved HERE, once per column, so the row loop
		// carries no test — the same reason the 9-way family switch moved here.
		// Measured: a per-value test cost the untouched nvarchar path ~20 ns.
		if (col.tds_type_token == TDS_TYPE_BIGVARCHAR) {
			return static_cast<EncodeFn>(mssql::codec::string::EncodeToBcpUtf8);
		}
		return static_cast<EncodeFn>(mssql::codec::string::EncodeToBcp);
	case mssql::codec::TypeFamily::Binary:
		return mssql::codec::binary::EncodeToBcp;
	case mssql::codec::TypeFamily::Uuid:
		return mssql::codec::uuid::EncodeToBcp;
	case mssql::codec::TypeFamily::DateTime:
		return mssql::codec::datetime::EncodeToBcp;
	case mssql::codec::TypeFamily::Money:
		return EncodeMoneyUnreachable;
	}
	return EncodeMoneyUnreachable;	// unreachable — switch is exhaustive
}

struct ColumnEncodeState {
	Vector *vec = nullptr;	// nullptr → source column missing: NULL every row
	UnifiedVectorFormat fmt;
	EncodeFn encode = nullptr;
	NullWireKind null_kind = NullWireKind::Fixed;
};

void EncodeNullOfKind(vector<uint8_t> &buffer, NullWireKind kind) {
	switch (kind) {
	case NullWireKind::Plp:
		BCPRowEncoder::EncodeNullPLP(buffer);
		return;
	case NullWireKind::VariableUShort:
		BCPRowEncoder::EncodeNullVariable(buffer);
		return;
	case NullWireKind::Fixed:
		BCPRowEncoder::EncodeNullFixed(buffer);
		return;
	}
}

// Build the hoisted per-column state. `format_count` is the row count the
// UnifiedVectorFormat is computed for (chunk size for EncodeChunk; row+1 for
// the per-row EncodeRow compatibility path).
void PrepareColumnStates(DataChunk &chunk, idx_t format_count, const vector<mssql::BCPColumnMetadata> &columns,
						 const vector<int32_t> *column_mapping, vector<ColumnEncodeState> &states) {
	states.resize(columns.size());
	for (idx_t target_idx = 0; target_idx < columns.size(); target_idx++) {
		auto &col = columns[target_idx];
		auto &state = states[target_idx];
		state.null_kind = col.IsPLPType()
							  ? NullWireKind::Plp
							  : (col.IsVariableLengthUSHORT() ? NullWireKind::VariableUShort : NullWireKind::Fixed);
		// If source_idx is -1 or out of range, the column stays NULL for every row.
		int32_t source_idx = column_mapping ? (*column_mapping)[target_idx] : static_cast<int32_t>(target_idx);
		if (source_idx < 0 || static_cast<idx_t>(source_idx) >= chunk.ColumnCount()) {
			continue;
		}
		state.vec = &chunk.data[source_idx];
		state.vec->ToUnifiedFormat(format_count, state.fmt);
		state.encode = ResolveEncoder(col);
	}
}

// Encode one row from prepared states (no 0xD1 token byte).
void EncodeRowFromStates(vector<uint8_t> &buffer, idx_t row_idx, const vector<mssql::BCPColumnMetadata> &columns,
						 vector<ColumnEncodeState> &states) {
	for (idx_t target_idx = 0; target_idx < columns.size(); target_idx++) {
		auto &state = states[target_idx];
		if (!state.vec || mssql::codec::FormatIsNull(state.fmt, row_idx)) {
			EncodeNullOfKind(buffer, state.null_kind);
			continue;
		}
		state.encode(*state.vec, state.fmt, row_idx, columns[target_idx], buffer);
	}
}

}  // namespace

//===----------------------------------------------------------------------===//
// Chunk- and Row-Level Encoding
//===----------------------------------------------------------------------===//

//===----------------------------------------------------------------------===//
// Columnar scatter for fixed-width, direct-copy columns (spec 057 step 3)
//
// The row loop below hoists per-column state (spec 054 W1+W2) but still appends
// one value at a time through push_back, which costs a capacity check per byte —
// measured at ~10 ns/value on the string path, more than the UTF-16 conversion
// itself. This path removes it for the case where it is removable without any
// conversion at all: every column fixed-width, and its wire bytes a plain
// little-endian copy of what the vector already stores.
//
// The shape is: size the buffer from METADATA and the validity masks (never from
// a value), resize once, then walk COLUMNS writing through a raw pointer. That
// is the direction measured in test/cpp/bench_materialize.cpp — 8.14 -> 0.53
// ns/value at one column, 7.83 -> 0.36 at 44 with blocking.
//
// Deliberately narrow for now. DECIMAL, temporal and GUID need transformation
// kernels and variable-width families need staging; both are later commits, and
// until they exist those chunks take the row loop unchanged.
//
// Little-endian is assumed, as it already is by the byte-wise appenders this
// replaces and by DecodeFromTds's memcpy. Every platform this ships to is LE.
//===----------------------------------------------------------------------===//

//! Encode the whole chunk as a columnar scatter. Returns false if any column
//! lacks a scatter arm, having written nothing.
bool TryEncodeChunkColumnar(vector<uint8_t> &buffer, idx_t row_count, const vector<mssql::BCPColumnMetadata> &columns,
							vector<ColumnEncodeState> &states) {
	const idx_t ncols = columns.size();
	if (ncols == 0) {
		return false;
	}

	vector<uint8_t> widths(ncols);
	size_t stride = 1;	// the 0xD1 ROW token
	bool all_valid = true;
	for (idx_t c = 0; c < ncols; c++) {
		// A missing source column is NULL every row: no payload, so it scatters
		// for free. Everything else answers to ResolveWriteColumnOps, which is
		// the single place that decides what this path can do.
		if (!states[c].vec) {
			widths[c] = 0;
			stride += 1;
			continue;
		}
		const auto ops = mssql::codec::ResolveWriteColumnOps(states[c].vec->GetType(), columns[c]);
		if (!ops.CanScatter()) {
			return false;
		}
		widths[c] = static_cast<uint8_t>(ops.wire_width);
		stride += 1 + widths[c];
		if (states[c].vec && !states[c].fmt.validity.AllValid()) {
			all_valid = false;
		}
	}

	// Sizing reads metadata and the masks only — never a value. A NULL keeps its
	// 1-byte marker and drops its payload, so it can only make a row shorter.
	const size_t base = buffer.size();
	vector<size_t> row_at;
	size_t total;
	if (all_valid) {
		total = row_count * stride;
	} else {
		row_at.resize(row_count);
		size_t acc = 0;
		for (idx_t r = 0; r < row_count; r++) {
			row_at[r] = acc;
			size_t sz = 1;
			for (idx_t c = 0; c < ncols; c++) {
				const auto &st = states[c];
				const bool valid = st.vec && st.fmt.validity.RowIsValid(st.fmt.sel->get_index(r));
				sz += 1 + (valid ? widths[c] : 0);
			}
			acc += sz;
		}
		total = acc;
	}

	buffer.resize(base + total);
	uint8_t *const dst = buffer.data() + base;

	if (all_valid) {
		for (idx_t r = 0; r < row_count; r++) {
			dst[r * stride] = BCP_TOKEN_ROW;
		}
		size_t col_off = 1;
		for (idx_t c = 0; c < ncols; c++) {
			const auto &st = states[c];
			const uint8_t w = widths[c];
			if (w == 0) {
				for (idx_t r = 0; r < row_count; r++) {
					dst[r * stride + col_off] = 0x00;  // missing column: NULL marker
				}
				col_off += 1;
				continue;
			}
			const uint8_t *src = reinterpret_cast<const uint8_t *>(st.fmt.data);
			const auto *sel = st.fmt.sel;
			for (idx_t r = 0; r < row_count; r++) {
				uint8_t *out = dst + r * stride + col_off;
				*out = w;
				std::memcpy(out + 1, src + sel->get_index(r) * w, w);
			}
			col_off += 1 + w;
		}
		return true;
	}

	// NULL-bearing: the row offsets are not a stride, so carry a per-column
	// cursor. The branch per value is deliberate — the branchless form was
	// measured WORSE (null50 5.5 -> 7.6 ns/value) because it trades a
	// well-predicted branch for an unconditional write and a multiply that stops
	// the loop vectorising. Do not retry it.
	for (idx_t r = 0; r < row_count; r++) {
		dst[row_at[r]] = BCP_TOKEN_ROW;
	}
	vector<size_t> cursor(row_count);
	for (idx_t r = 0; r < row_count; r++) {
		cursor[r] = row_at[r] + 1;
	}
	for (idx_t c = 0; c < ncols; c++) {
		const auto &st = states[c];
		const uint8_t w = widths[c];
		const uint8_t *src = st.vec ? reinterpret_cast<const uint8_t *>(st.fmt.data) : nullptr;
		for (idx_t r = 0; r < row_count; r++) {
			uint8_t *out = dst + cursor[r];
			const idx_t sidx = st.vec ? st.fmt.sel->get_index(r) : 0;
			if (src && w != 0 && st.fmt.validity.RowIsValid(sidx)) {
				*out = w;
				std::memcpy(out + 1, src + sidx * w, w);
				cursor[r] += 1 + w;
			} else {
				*out = 0x00;
				cursor[r] += 1;
			}
		}
	}
	return true;
}

void BCPRowEncoder::EncodeChunk(vector<uint8_t> &buffer, DataChunk &chunk,
								const vector<mssql::BCPColumnMetadata> &columns,
								const vector<int32_t> *column_mapping) {
	const idx_t row_count = chunk.size();
	if (row_count == 0) {
		return;
	}

	// W4 (spec 054): reserve the accumulator once per chunk from a cheap
	// per-row estimate, so the per-value appends below rarely reallocate.
	// Fixed-width columns are exact (+1 length byte); variable/PLP columns
	// use a modest payload guess — an under-estimate only costs a vector
	// grow, an exact max_length bound could over-reserve by orders of
	// magnitude (2048 rows x nvarchar(4000) = 16 MB).
	size_t per_row_estimate = 1;  // 0xD1 ROW token
	for (auto &col : columns) {
		if (col.IsPLPType()) {
			per_row_estimate += 8 + 4 + 64 + 4;
		} else if (col.IsVariableLengthUSHORT()) {
			per_row_estimate += 2 + MinValue<size_t>(col.max_length, 64);
		} else {
			per_row_estimate += 1 + col.max_length;
		}
	}
	// Amplify to at least 2x the current capacity: reserve() allocates
	// exactly what is requested, so asking for size+estimate every chunk
	// would make capacity track size and memcpy the whole accumulator per
	// chunk (quadratic across a large batch).
	const size_t needed = buffer.size() + row_count * per_row_estimate;
	if (needed > buffer.capacity()) {
		buffer.reserve(MaxValue<size_t>(needed, 2 * buffer.capacity()));
	}

	vector<ColumnEncodeState> states;
	PrepareColumnStates(chunk, row_count, columns, column_mapping, states);
	if (TryEncodeChunkColumnar(buffer, row_count, columns, states)) {
		return;
	}
	for (idx_t row_idx = 0; row_idx < row_count; row_idx++) {
		buffer.push_back(BCP_TOKEN_ROW);
		EncodeRowFromStates(buffer, row_idx, columns, states);
	}
}

void BCPRowEncoder::EncodeRow(vector<uint8_t> &buffer, DataChunk &chunk, idx_t row_idx,
							  const vector<mssql::BCPColumnMetadata> &columns, const vector<int32_t> *column_mapping) {
	vector<ColumnEncodeState> states;
	PrepareColumnStates(chunk, row_idx + 1, columns, column_mapping, states);
	EncodeRowFromStates(buffer, row_idx, columns, states);
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

	// Family-level dispatch — see EncodeRow above for the same pattern.
	mssql::codec::TypeFamily family;
	try {
		family = mssql::codec::FamilyFromLogicalType(col.duckdb_type);
	} catch (const NotImplementedException &) {
		throw NotImplementedException("MSSQL: Unsupported type for BCP encoding: %s", col.duckdb_type.ToString());
	}
	switch (family) {
	case mssql::codec::TypeFamily::Boolean:
		mssql::codec::boolean::EncodeToBcp(value, col, buffer);
		break;
	case mssql::codec::TypeFamily::Integer:
		mssql::codec::integer::EncodeToBcp(value, col, buffer);
		break;
	case mssql::codec::TypeFamily::Float:
		mssql::codec::float_family::EncodeToBcp(value, col, buffer);
		break;
	case mssql::codec::TypeFamily::Decimal:
		mssql::codec::decimal::EncodeToBcp(value, col, buffer);
		break;
	case mssql::codec::TypeFamily::String:
		mssql::codec::string::EncodeToBcp(value, col, buffer);
		break;
	case mssql::codec::TypeFamily::Binary:
		mssql::codec::binary::EncodeToBcp(value, col, buffer);
		break;
	case mssql::codec::TypeFamily::Uuid:
		mssql::codec::uuid::EncodeToBcp(value, col, buffer);
		break;
	case mssql::codec::TypeFamily::DateTime:
		mssql::codec::datetime::EncodeToBcp(value, col, buffer);
		break;
	case mssql::codec::TypeFamily::Money:
		ThrowMoneyUnreachable(col.duckdb_type);
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

void BCPRowEncoder::EncodeDatetime2Raw(vector<uint8_t> &buffer, uint64_t time_value, uint32_t date_value,
									   uint8_t scale) {
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

void BCPRowEncoder::EncodeDatetime2(vector<uint8_t> &buffer, timestamp_t ts, uint8_t scale) {
	uint64_t time_value;
	uint32_t date_value;
	TimestampToDatetime2Components(ts, scale, time_value, date_value);
	EncodeDatetime2Raw(buffer, time_value, date_value, scale);
}

void BCPRowEncoder::EncodeDatetimeOffsetRaw(vector<uint8_t> &buffer, uint64_t time_value, uint32_t date_value,
											int16_t offset_minutes, uint8_t scale) {
	uint8_t time_size = GetTimeByteSize(scale);
	uint8_t total_size = time_size + 3 + 2;

	buffer.push_back(total_size);

	for (uint8_t i = 0; i < time_size; i++) {
		buffer.push_back(static_cast<uint8_t>((time_value >> (i * 8)) & 0xFF));
	}
	buffer.push_back(static_cast<uint8_t>(date_value & 0xFF));
	buffer.push_back(static_cast<uint8_t>((date_value >> 8) & 0xFF));
	buffer.push_back(static_cast<uint8_t>((date_value >> 16) & 0xFF));
	buffer.push_back(static_cast<uint8_t>(offset_minutes & 0xFF));
	buffer.push_back(static_cast<uint8_t>((offset_minutes >> 8) & 0xFF));
}

void BCPRowEncoder::EncodeDatetimeOffset(vector<uint8_t> &buffer, timestamp_t ts, int16_t offset_minutes,
										 uint8_t scale) {
	uint64_t time_value;
	uint32_t date_value;
	TimestampToDatetime2Components(ts, scale, time_value, date_value);
	EncodeDatetimeOffsetRaw(buffer, time_value, date_value, offset_minutes, scale);
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
