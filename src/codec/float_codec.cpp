//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension — spec 045
//
// codec/float_codec.cpp
//
// Float family implementation. See codec/float_codec.hpp.
//
// Behavior parity (vs pre-spec-045 baseline):
//   - DecodeFromTds mirrors TypeConverter::ConvertFloat (size-dispatched
//     on bytes.size(): 4 -> float, 8 -> double).
//   - EncodeToBcp mirrors BCPRowEncoder::EncodeFloat / EncodeDouble
//     (length-prefixed LE IEEE 754).
//   - FormatSqlLiteral is byte-identical for both LiteralContext values
//     (FR-020 (b)): setprecision(9) for FLOAT, setprecision(17) for
//     DOUBLE, ".0" suffix forced on integer-valued floats, NaN / +Inf /
//     -Inf rejected client-side with InvalidInputException naming the
//     value type.
//   - FormatDdlTypeName: FLOAT -> "REAL", DOUBLE -> "FLOAT", byte-
//     identical in both DdlContext values.
//   - EstimateLiteralSize: 20 (float) / 30 (double) — matches the pre-
//     spec-045 MSSQLValueSerializer::EstimateSerializedSize upper bounds.
//===----------------------------------------------------------------------===//

#include "codec/float_codec.hpp"

#include "codec/vector_format.hpp"
#include "copy/target_resolver.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/vector/flat_vector.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <string>

namespace duckdb {
namespace mssql {
namespace codec {
namespace float_family {

namespace {

void AppendFloatBcp(duckdb::vector<uint8_t> &buf, float value) {
	buf.push_back(4);
	uint32_t bits;
	std::memcpy(&bits, &value, sizeof(bits));
	for (int i = 0; i < 4; ++i) {
		buf.push_back(static_cast<uint8_t>((bits >> (i * 8)) & 0xFF));
	}
}

void AppendDoubleBcp(duckdb::vector<uint8_t> &buf, double value) {
	buf.push_back(8);
	uint64_t bits;
	std::memcpy(&bits, &value, sizeof(bits));
	for (int i = 0; i < 8; ++i) {
		buf.push_back(static_cast<uint8_t>((bits >> (i * 8)) & 0xFF));
	}
}

void ValidateFiniteOrThrow(double value, const char *type_name) {
	if (std::isnan(value)) {
		throw InvalidInputException(
			"MSSQL: %s NaN values cannot be rendered as a SQL literal — "
			"SQL Server has no canonical NaN literal form. Filter out NaN values "
			"before sending them to SQL Server, or use a NULL placeholder.",
			type_name);
	}
	if (std::isinf(value)) {
		const char *sign = value > 0 ? "+Inf" : "-Inf";
		throw InvalidInputException(
			"MSSQL: %s infinity (%s) cannot be rendered as a SQL literal — "
			"SQL Server has no canonical infinity literal form. Filter out "
			"infinity values before sending them to SQL Server, or use a NULL "
			"placeholder.",
			type_name, sign);
	}
}

template <typename T>
std::string RenderFloatLiteral(T value, int precision, const char *type_name) {
	ValidateFiniteOrThrow(static_cast<double>(value), type_name);

	std::ostringstream oss;
	oss << std::setprecision(precision) << value;
	std::string result = oss.str();

	// Force SQL Server to parse as floating-point, not bare integer.
	if (result.find('.') == std::string::npos && result.find('e') == std::string::npos &&
		result.find('E') == std::string::npos) {
		result += ".0";
	}
	return result;
}

}  // namespace

void DecodeFromTds(const std::vector<uint8_t> &bytes, const tds::ColumnMetadata & /*col*/, Vector &out, idx_t row) {
	if (bytes.size() == 4) {
		float f = 0;
		std::memcpy(&f, bytes.data(), 4);
		FlatVector::GetDataMutableUnsafe<float>(out)[row] = f;
	} else if (bytes.size() == 8) {
		double d = 0;
		std::memcpy(&d, bytes.data(), 8);
		FlatVector::GetDataMutableUnsafe<double>(out)[row] = d;
	}
	// Other sizes silently skip (mirror legacy ConvertFloat — defensive only).
}

// The wire form comes from the TARGET and the value from the SOURCE, which are
// two different types whenever the target table already existed — this used to
// ignore `col` entirely and write whatever width the source stored. Both pairs
// IsTypeCompatible advertises were broken by it: a DOUBLE into `real` sent eight
// bytes for a four-byte column and SQL Server aborted the batch with "Received an
// invalid column length from the bcp client", and FLOAT into `float` sent four
// for eight. Issue #153's shape in the float family.
//
// Narrowing DOUBLE -> real loses precision, and unlike the integer path that is
// not an error: it is what CAST does on the server, and the column's declared
// precision IS the request. Nothing here refuses a value.
void EncodeToBcp(Vector &in, const UnifiedVectorFormat &fmt, idx_t row, const mssql::BCPColumnMetadata &col,
				 duckdb::vector<uint8_t> &buf) {
	const LogicalTypeId source_id = in.GetType().id();
	if (source_id != LogicalTypeId::FLOAT && source_id != LogicalTypeId::DOUBLE) {
		throw InternalException("codec::float_family::EncodeToBcp: unexpected LogicalType '%s'",
								in.GetType().ToString());
	}
	const double value = source_id == LogicalTypeId::FLOAT ? static_cast<double>(FormatValue<float>(fmt, row))
														   : FormatValue<double>(fmt, row);
	if (col.duckdb_type.id() == LogicalTypeId::FLOAT) {
		AppendFloatBcp(buf, static_cast<float>(value));
		return;
	}
	AppendDoubleBcp(buf, value);
}

void EncodeToBcp(Vector &in, idx_t row, const mssql::BCPColumnMetadata &col, duckdb::vector<uint8_t> &buf) {
	EncodeToBcpViaFormat(EncodeToBcp, in, row, col, buf);
}

void EncodeToBcp(const Value &value, const mssql::BCPColumnMetadata & /*col*/, duckdb::vector<uint8_t> &buf) {
	switch (value.type().id()) {
	case LogicalTypeId::FLOAT:
		AppendFloatBcp(buf, value.GetValue<float>());
		break;
	case LogicalTypeId::DOUBLE:
		AppendDoubleBcp(buf, value.GetValue<double>());
		break;
	default:
		throw InternalException("codec::float_family::EncodeToBcp(Value): unexpected LogicalType '%s'",
								value.type().ToString());
	}
}

std::string FormatSqlLiteral(const Value &v, const LogicalType &type, LiteralContext /*ctx*/) {
	switch (type.id()) {
	case LogicalTypeId::FLOAT:
		return RenderFloatLiteral<float>(v.GetValue<float>(), 9, "FLOAT");
	case LogicalTypeId::DOUBLE:
		return RenderFloatLiteral<double>(v.GetValue<double>(), 17, "DOUBLE");
	default:
		throw InternalException("codec::float_family::FormatSqlLiteral: unexpected LogicalType '%s'", type.ToString());
	}
}

std::string FormatDdlTypeName(const LogicalType &type, const mssql::CTASConfig & /*cfg*/, DdlContext /*ctx*/) {
	switch (type.id()) {
	case LogicalTypeId::FLOAT:
		return "REAL";	// 32-bit float
	case LogicalTypeId::DOUBLE:
		return "FLOAT";	 // 64-bit float in SQL Server
	default:
		throw InternalException("codec::float_family::FormatDdlTypeName: unexpected LogicalType '%s'", type.ToString());
	}
}

size_t EstimateLiteralSize(const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::FLOAT:
		return 20;	// setprecision(9) + sign + dot + e+nnn ~ 15; pad for safety.
	case LogicalTypeId::DOUBLE:
		return 30;	// setprecision(17) + sign + dot + e+nnn ~ 24; pad for safety.
	default:
		throw InternalException("codec::float_family::EstimateLiteralSize: unexpected LogicalType '%s'",
								type.ToString());
	}
}

}  // namespace float_family
}  // namespace codec
}  // namespace mssql
}  // namespace duckdb
