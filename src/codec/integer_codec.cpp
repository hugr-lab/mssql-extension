//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension — spec 045
//
// codec/integer_codec.cpp
//
// Integer family implementation. See codec/integer_codec.hpp.
//
// Behavior parity:
//   - DecodeFromTds mirrors TypeConverter::ConvertInteger (size-dispatched
//     on bytes.size(): 1->u8, 2->i16, 4->i32, 8->i64).
//   - EncodeToBcp mirrors BCPRowEncoder Integer arms for TINYINT..UBIGINT
//     (UBIGINT delegates to BCPRowEncoder::EncodeDecimal). HUGEINT and
//     UHUGEINT also delegate to EncodeDecimal as DECIMAL(38,0) (issue #177),
//     guarded client-side: 39-digit values overflow DECIMAL(38,0) and raise
//     InvalidInputException before the row enters the BCP batch.
//   - FormatSqlLiteral produces identical output in Filter and InsertValues
//     contexts (FR-020 (b) — HUGEINT correctness fix; previously Filter
//     fell through filter_encoder's VARCHAR default arm and rendered
//     HUGEINT as N'<digits>').
//   - FormatDdlTypeName is byte-identical in CreateTable and CtasCreateTable
//     contexts (FR-025 / FR-028 — DDL unification). HUGEINT and UHUGEINT
//     now map to DECIMAL(38,0) in both contexts; previously CTAS threw.
//===----------------------------------------------------------------------===//

#include "codec/integer_codec.hpp"

#include "codec/vector_format.hpp"
#include "copy/target_resolver.hpp"
#include "dml/insert/mssql_value_serializer.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/common/types/uhugeint.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "tds/encoding/bcp_row_encoder.hpp"
#include "tds/encoding/type_converter.hpp"

#include <cstdint>
#include <cstring>
#include <limits>
#include <string>

namespace duckdb {
namespace mssql {
namespace codec {
namespace integer {

namespace {

void AppendInt8Bcp(duckdb::vector<uint8_t> &buf, int8_t value) {
	buf.push_back(1);
	buf.push_back(static_cast<uint8_t>(value));
}

void AppendUInt8Bcp(duckdb::vector<uint8_t> &buf, uint8_t value) {
	buf.push_back(1);
	buf.push_back(value);
}

void AppendInt16Bcp(duckdb::vector<uint8_t> &buf, int16_t value) {
	buf.push_back(2);
	buf.push_back(static_cast<uint8_t>(value & 0xFF));
	buf.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
}

void AppendInt32Bcp(duckdb::vector<uint8_t> &buf, int32_t value) {
	buf.push_back(4);
	for (int i = 0; i < 4; ++i) {
		buf.push_back(static_cast<uint8_t>((value >> (i * 8)) & 0xFF));
	}
}

void AppendInt64Bcp(duckdb::vector<uint8_t> &buf, int64_t value) {
	buf.push_back(8);
	for (int i = 0; i < 8; ++i) {
		buf.push_back(static_cast<uint8_t>((value >> (i * 8)) & 0xFF));
	}
}

// #177: HUGEINT/UHUGEINT travel as DECIMAL(38,0) on the BCP wire (matching
// FormatDdlTypeName / FormatSqlLiteral). DECIMAL(38,0) holds at most 38 digits
// while int128 reaches ~1.70e38 and uint128 ~3.40e38 (39 digits), so
// out-of-range values must fail client-side before the row enters the batch —
// a server-side overflow would abort the whole BCP transfer mid-stream.
const hugeint_t &MaxDecimal38() {
	static const hugeint_t max = Hugeint::POWERS_OF_TEN[38] - 1;
	return max;
}

void CheckHugeintFitsDecimal38(const hugeint_t &value, const std::string &col_name) {
	// Bound both ends directly instead of negating `value`: Hugeint::Negate on
	// HUGEINT min (-2^127) is itself out of int128 range, so the old
	// abs-then-compare let that one value slip the guard (or threw a raw
	// overflow). +/-(10^38-1) are both representable (< 2^127), so the range
	// check is exact and keeps the column-named message for every input.
	const hugeint_t &max = MaxDecimal38();
	const hugeint_t min = Hugeint::Negate(max);
	if (Hugeint::GreaterThan(value, max) || Hugeint::GreaterThan(min, value)) {
		throw InvalidInputException(
			"MSSQL: HUGEINT value %s in column \"%s\" is out of range for DECIMAL(38,0) (max 38 digits)",
			Hugeint::ToString(value), col_name);
	}
}

void CheckUhugeintFitsDecimal38(const uhugeint_t &value, const std::string &col_name) {
	const uhugeint_t max(static_cast<uint64_t>(MaxDecimal38().upper), MaxDecimal38().lower);
	if (value > max) {
		throw InvalidInputException(
			"MSSQL: UHUGEINT value %s in column \"%s\" is out of range for DECIMAL(38,0) (max 38 digits)",
			Uhugeint::ToString(value), col_name);
	}
}

// Read the value at the width the SOURCE vector actually stores, widened to
// hugeint so every integer physical type fits without a second dispatch.
//
// The encode switch below chooses the WIRE form from the target column, which is
// right — but until spec 057 it also chose the READ width from the target, and
// the source is under no obligation to match. `COPY (SELECT 1::INTEGER)` into a
// `bigint` column asked for eight bytes from a four-byte array and died with
// `INTERNAL Error: Expected unified vector format of type INT64, but found type
// INT32`. Every widening the schema validator advertises (TINYINT -> int,
// SMALLINT -> bigint, INTEGER -> bigint) failed that way, so the compatibility
// table was documenting conversions the encoder could not perform (issue #153).
hugeint_t ReadSourceInteger(Vector &in, const UnifiedVectorFormat &fmt, idx_t row) {
	switch (in.GetType().InternalType()) {
	case PhysicalType::BOOL:
	case PhysicalType::INT8:
		return hugeint_t(FormatValue<int8_t>(fmt, row));
	case PhysicalType::UINT8:
		return hugeint_t(FormatValue<uint8_t>(fmt, row));
	case PhysicalType::INT16:
		return hugeint_t(FormatValue<int16_t>(fmt, row));
	case PhysicalType::UINT16:
		return hugeint_t(FormatValue<uint16_t>(fmt, row));
	case PhysicalType::INT32:
		return hugeint_t(FormatValue<int32_t>(fmt, row));
	case PhysicalType::UINT32:
		return hugeint_t(FormatValue<uint32_t>(fmt, row));
	case PhysicalType::INT64:
		return hugeint_t(FormatValue<int64_t>(fmt, row));
	case PhysicalType::UINT64:
		// Two-argument form: a value above INT64_MAX must not become negative.
		return hugeint_t(0, FormatValue<uint64_t>(fmt, row));
	case PhysicalType::INT128:
		return FormatValue<hugeint_t>(fmt, row);
	default:
		throw InternalException("codec::integer::EncodeToBcp: unexpected source PhysicalType for an integer column");
	}
}

// Narrow to the target's width, refusing what will not fit.
//
// An error, not a wraparound: an integer that does not fit is a different number,
// and SQL Server itself refuses the row. Contrast the string path, where the
// column states a bound and truncating to it is the documented intent — nothing
// about `bigint -> int` says the user wanted the low 32 bits.
int64_t NarrowSourceInteger(const hugeint_t &value, int64_t min, int64_t max, const mssql::BCPColumnMetadata &col) {
	if (Hugeint::GreaterThan(value, hugeint_t(max)) || Hugeint::GreaterThan(hugeint_t(min), value)) {
		throw InvalidInputException(
			"MSSQL: integer value %s in column \"%s\" is out of range for the target column (accepts %lld..%lld)",
			Hugeint::ToString(value), col.name, (long long)min, (long long)max);
	}
	return Hugeint::Cast<int64_t>(value);
}

}  // namespace

void DecodeFromTds(const std::vector<uint8_t> &bytes, const tds::ColumnMetadata &col, Vector &out, idx_t row) {
	(void)col;	// size-dispatched
	switch (bytes.size()) {
	case 1:
		// SQL Server TINYINT is unsigned (0-255).
		FlatVector::GetDataMutableUnsafe<uint8_t>(out)[row] = bytes[0];
		return;
	case 2: {
		int16_t v = 0;
		std::memcpy(&v, bytes.data(), 2);
		FlatVector::GetDataMutableUnsafe<int16_t>(out)[row] = v;
		return;
	}
	case 4: {
		int32_t v = 0;
		std::memcpy(&v, bytes.data(), 4);
		FlatVector::GetDataMutableUnsafe<int32_t>(out)[row] = v;
		return;
	}
	case 8: {
		int64_t v = 0;
		std::memcpy(&v, bytes.data(), 8);
		FlatVector::GetDataMutableUnsafe<int64_t>(out)[row] = v;
		return;
	}
	default:
		throw InvalidInputException("codec::integer::DecodeFromTds: invalid integer length %zu", bytes.size());
	}
}

void EncodeToBcp(Vector &in, const UnifiedVectorFormat &fmt, idx_t row, const mssql::BCPColumnMetadata &col,
				 duckdb::vector<uint8_t> &buf) {
	// The source's storage width, which the target is under no obligation to
	// match (issue #153). Each arm below keeps its exact-match case byte-identical
	// and only converts when the widths actually differ — so the common path pays
	// one integer comparison, and step 3 resolves even that once per column.
	const PhysicalType src = in.GetType().InternalType();
	switch (col.duckdb_type.id()) {
	case LogicalTypeId::UTINYINT:
		// SQL Server's `tinyint`: one byte, UNSIGNED 0..255. A UINT8 source is
		// exactly that and skips the check because every value of it fits.
		if (src == PhysicalType::UINT8) {
			AppendUInt8Bcp(buf, FormatValue<uint8_t>(fmt, row));
			return;
		}
		// Anything else must satisfy 0..255 — notably a SIGNED int8, whose byte
		// used to be copied straight through: -1 landed on the server as 255, with
		// no error and no warning, just a different number. It was left that way
		// deliberately as a "wire-compatibility question", but there was no
		// compatibility to keep — the catalog reads the column back as UTINYINT, so
		// the round trip never returned -1 either. Same principle as
		// `bigint -> int`: a value that does not fit is a different number.
		AppendUInt8Bcp(buf, static_cast<uint8_t>(NarrowSourceInteger(ReadSourceInteger(in, fmt, row), 0, 255, col)));
		return;
	case LogicalTypeId::TINYINT:
		// A SIGNED TINYINT source travels as a smallint, because SQL Server has no
		// signed one-byte integer to put it in — the same widening USMALLINT and
		// UINTEGER already do. Its source is int8, never int16, so it always takes
		// the conversion arm below and always fits.
	case LogicalTypeId::SMALLINT:
		if (src == PhysicalType::INT16) {
			AppendInt16Bcp(buf, FormatValue<int16_t>(fmt, row));
			return;
		}
		AppendInt16Bcp(
			buf, static_cast<int16_t>(NarrowSourceInteger(ReadSourceInteger(in, fmt, row), INT16_MIN, INT16_MAX, col)));
		return;
	case LogicalTypeId::USMALLINT:
		// USMALLINT (0-65535) widens to int32 to fit without overflow.
		AppendInt32Bcp(buf, static_cast<int32_t>(FormatValue<uint16_t>(fmt, row)));
		return;
	case LogicalTypeId::INTEGER:
		if (src == PhysicalType::INT32) {
			AppendInt32Bcp(buf, FormatValue<int32_t>(fmt, row));
			return;
		}
		AppendInt32Bcp(
			buf, static_cast<int32_t>(NarrowSourceInteger(ReadSourceInteger(in, fmt, row), INT32_MIN, INT32_MAX, col)));
		return;
	case LogicalTypeId::UINTEGER:
		// UINTEGER (0-4B) widens to int64 to fit without overflow.
		AppendInt64Bcp(buf, static_cast<int64_t>(FormatValue<uint32_t>(fmt, row)));
		return;
	case LogicalTypeId::BIGINT:
		if (src == PhysicalType::INT64) {
			AppendInt64Bcp(buf, FormatValue<int64_t>(fmt, row));
			return;
		}
		AppendInt64Bcp(buf, NarrowSourceInteger(ReadSourceInteger(in, fmt, row), INT64_MIN, INT64_MAX, col));
		return;
	case LogicalTypeId::UBIGINT: {
		// UBIGINT (0-18e18) uses DECIMAL(20,0) on the wire — SQL Server BIGINT is signed.
		// Two-argument hugeint_t(upper=0, lower=val) avoids sign issues when val > INT64_MAX.
		uint64_t val = FormatValue<uint64_t>(fmt, row);
		tds::encoding::BCPRowEncoder::EncodeDecimal(buf, hugeint_t(0, val), col.precision, col.scale);
		return;
	}
	case LogicalTypeId::HUGEINT: {
		// #177: HUGEINT (e.g. SUM() over integers) encodes as DECIMAL(38,0),
		// consistent with the DDL and literal paths.
		hugeint_t val = FormatValue<hugeint_t>(fmt, row);
		CheckHugeintFitsDecimal38(val, col.name);
		tds::encoding::BCPRowEncoder::EncodeDecimal(buf, val, col.precision, col.scale);
		return;
	}
	case LogicalTypeId::UHUGEINT: {
		uhugeint_t val = FormatValue<uhugeint_t>(fmt, row);
		CheckUhugeintFitsDecimal38(val, col.name);
		// Guarded value is < 2^127, so the signed reinterpretation is lossless.
		tds::encoding::BCPRowEncoder::EncodeDecimal(buf, hugeint_t(static_cast<int64_t>(val.upper), val.lower),
													col.precision, col.scale);
		return;
	}
	default:
		throw NotImplementedException("codec::integer::EncodeToBcp: unsupported type %s", col.duckdb_type.ToString());
	}
}

void EncodeToBcp(Vector &in, idx_t row, const mssql::BCPColumnMetadata &col, duckdb::vector<uint8_t> &buf) {
	EncodeToBcpViaFormat(EncodeToBcp, in, row, col, buf);
}

void EncodeToBcp(const Value &value, const mssql::BCPColumnMetadata &col, duckdb::vector<uint8_t> &buf) {
	switch (col.duckdb_type.id()) {
	case LogicalTypeId::TINYINT:
		// Widened to smallint, as in the Vector overload above.
		AppendInt16Bcp(buf, value.GetValue<int16_t>());
		return;
	case LogicalTypeId::UTINYINT:
		// GetValue<uint8_t> throws on a negative rather than wrapping it, which is
		// the answer the range check gives there.
		AppendUInt8Bcp(buf, value.GetValue<uint8_t>());
		return;
	case LogicalTypeId::SMALLINT:
		AppendInt16Bcp(buf, value.GetValue<int16_t>());
		return;
	case LogicalTypeId::USMALLINT:
		AppendInt32Bcp(buf, static_cast<int32_t>(value.GetValue<uint16_t>()));
		return;
	case LogicalTypeId::INTEGER:
		AppendInt32Bcp(buf, value.GetValue<int32_t>());
		return;
	case LogicalTypeId::UINTEGER:
		AppendInt64Bcp(buf, static_cast<int64_t>(value.GetValue<uint32_t>()));
		return;
	case LogicalTypeId::BIGINT:
		AppendInt64Bcp(buf, value.GetValue<int64_t>());
		return;
	case LogicalTypeId::UBIGINT: {
		uint64_t val = value.GetValue<uint64_t>();
		tds::encoding::BCPRowEncoder::EncodeDecimal(buf, hugeint_t(0, val), col.precision, col.scale);
		return;
	}
	case LogicalTypeId::HUGEINT: {
		hugeint_t val = value.GetValue<hugeint_t>();
		CheckHugeintFitsDecimal38(val, col.name);
		tds::encoding::BCPRowEncoder::EncodeDecimal(buf, val, col.precision, col.scale);
		return;
	}
	case LogicalTypeId::UHUGEINT: {
		uhugeint_t val = value.GetValue<uhugeint_t>();
		CheckUhugeintFitsDecimal38(val, col.name);
		tds::encoding::BCPRowEncoder::EncodeDecimal(buf, hugeint_t(static_cast<int64_t>(val.upper), val.lower),
													col.precision, col.scale);
		return;
	}
	default:
		throw NotImplementedException("codec::integer::EncodeToBcp(Value): unsupported type %s",
									  col.duckdb_type.ToString());
	}
}

std::string FormatSqlLiteral(const Value &v, const LogicalType &type, LiteralContext ctx) {
	(void)ctx;	// Both Filter and InsertValues produce identical output (FR-020 (b)).
	if (v.IsNull()) {
		return "NULL";
	}
	switch (type.id()) {
	case LogicalTypeId::TINYINT:
		return std::to_string(TinyIntValue::Get(v));
	case LogicalTypeId::SMALLINT:
		return std::to_string(SmallIntValue::Get(v));
	case LogicalTypeId::INTEGER:
		return std::to_string(IntegerValue::Get(v));
	case LogicalTypeId::BIGINT:
		return std::to_string(BigIntValue::Get(v));
	case LogicalTypeId::UTINYINT:
		return std::to_string(static_cast<int64_t>(UTinyIntValue::Get(v)));
	case LogicalTypeId::USMALLINT:
		return std::to_string(static_cast<int64_t>(USmallIntValue::Get(v)));
	case LogicalTypeId::UINTEGER:
		return std::to_string(static_cast<int64_t>(UIntegerValue::Get(v)));
	case LogicalTypeId::UBIGINT: {
		// Values > INT64_MAX render as CAST(N AS DECIMAL(20,0)); smaller values render as bare digits.
		// SQL Server BIGINT is signed, so unsigned values above INT64_MAX must use DECIMAL(20,0).
		uint64_t uval = UBigIntValue::Get(v);
		if (uval <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
			return std::to_string(uval);
		}
		return StringUtil::Format("CAST(%llu AS DECIMAL(20,0))", uval);
	}
	case LogicalTypeId::HUGEINT:
		return MSSQLValueSerializer::SerializeDecimal(HugeIntValue::Get(v), 38, 0);
	case LogicalTypeId::UHUGEINT:
		// UHUGEINT not currently produced by any DuckDB path that reaches Filter/InsertValues; keep
		// the arm for completeness with the Decimal family's overflow handling (FR-025).
		throw NotImplementedException("codec::integer::FormatSqlLiteral: UHUGEINT literal not yet implemented");
	default:
		throw NotImplementedException("codec::integer::FormatSqlLiteral: unsupported type %s", type.ToString());
	}
}

std::string FormatDdlTypeName(const LogicalType &type, const mssql::CTASConfig &cfg, DdlContext ctx) {
	(void)cfg;
	(void)ctx;	// Output is byte-identical in CreateTable and CtasCreateTable (FR-025 / FR-028).
	switch (type.id()) {
	case LogicalTypeId::UTINYINT:
		// UTINYINT (0-255) fits SQL Server TINYINT exactly (also 0-255).
		return "TINYINT";
	case LogicalTypeId::TINYINT:
		// Signed TINYINT (-128..127) does NOT fit, because SQL Server's tinyint is
		// UNSIGNED — it is the only unsigned integer the server has. Creating a
		// tinyint here and copying the byte stored -1 as 255, silently. So it
		// widens, exactly as USMALLINT widens to INT and UINTEGER to BIGINT for
		// the same reason in the other direction.
		return "SMALLINT";
	case LogicalTypeId::SMALLINT:
		return "SMALLINT";
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::INTEGER:
		// USMALLINT widens to INT to fit the full range.
		return "INT";
	case LogicalTypeId::UINTEGER:
	case LogicalTypeId::BIGINT:
		// UINTEGER widens to BIGINT to fit the full range.
		return "BIGINT";
	case LogicalTypeId::UBIGINT:
		return "DECIMAL(20,0)";
	case LogicalTypeId::HUGEINT:
	case LogicalTypeId::UHUGEINT:
		return "DECIMAL(38,0)";
	default:
		throw NotImplementedException("codec::integer::FormatDdlTypeName: unsupported type %s", type.ToString());
	}
}

size_t EstimateLiteralSize(const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::UTINYINT:
		return 4;  // max 3 digits + sign
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::USMALLINT:
		return 6;  // max 5 digits + sign
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::UINTEGER:
		return 11;	// max 10 digits + sign
	case LogicalTypeId::BIGINT:
		return 20;	// max 19 digits + sign
	case LogicalTypeId::UBIGINT:
		// "CAST(18446744073709551615 AS DECIMAL(20,0))" = 43 chars; round up to 50 for safety.
		// Legacy MSSQLValueSerializer::EstimateSerializedSize undercounted at 40 (spec-045 fix).
		return 50;
	case LogicalTypeId::HUGEINT:
	case LogicalTypeId::UHUGEINT:
		return 45;	// max 39 digits + sign
	default:
		throw NotImplementedException("codec::integer::EstimateLiteralSize: unsupported type %s", type.ToString());
	}
}

}  // namespace integer
}  // namespace codec
}  // namespace mssql
}  // namespace duckdb
