#include "tds/encoding/type_converter.hpp"
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <sstream>
#include "codec/binary_codec.hpp"
#include "codec/boolean_codec.hpp"
#include "codec/clr/hierarchyid_codec.hpp"
#include "codec/clr/spatial_codec.hpp"
#include "codec/datetime_codec.hpp"
#include "codec/decimal_codec.hpp"
#include "codec/float_codec.hpp"
#include "codec/integer_codec.hpp"
#include "codec/money_codec.hpp"
#include "codec/sql_vector/vector_codec.hpp"
#include "codec/string_codec.hpp"
#include "codec/uuid_codec.hpp"
#include "codec/variant/variant_codec.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/decimal.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/string_vector.hpp"
#include "tds/encoding/datetime_encoding.hpp"
#include "tds/encoding/decimal_encoding.hpp"
#include "tds/encoding/utf16.hpp"
#include "tds/tds_types.hpp"

// Debug logging controlled by MSSQL_DEBUG environment variable
static int GetTypeConverterDebugLevel() {
	static const int level = []() {
		const char *env = std::getenv("MSSQL_DEBUG");
		return env ? std::atoi(env) : 0;
	}();
	return level;
}

#define MSSQL_TC_DEBUG_LOG(level, fmt, ...)                         \
	do {                                                            \
		if (GetTypeConverterDebugLevel() >= level) {                \
			fprintf(stderr, "[MSSQL TC] " fmt "\n", ##__VA_ARGS__); \
		}                                                           \
	} while (0)

namespace duckdb {
namespace tds {
namespace encoding {

namespace {

//! The codec family a wire type's value belongs to: ConvertValue decodes by it and
//! WriteAsStringFallback renders by it. NONE is a type the per-value path does not read.
enum class WireFamily : uint8_t {
	NONE,
	INTEGER,
	BOOLEAN,
	FLOAT,
	DECIMAL,
	MONEY,
	STRING,
	VARIANT,
	UDT,
	BINARY,
	DATETIME,
	UUID
};

using TypeOfColumn = LogicalType (*)(const ColumnMetadata &column);
using DecodeValue = void (*)(const std::vector<uint8_t> &value, const ColumnMetadata &column, Vector &vector,
							 idx_t row_idx);

//! What this file knows of one wire type id. A type's DuckDB type is `duckdb_type`, or what
//! `type_of_column` makes of the column where the id alone does not say; a type with neither is unknown.
//! `decode` is the family's codec (DecoderOf), kept in the row so that ConvertValue reaches it in one
//! load and one jump, as a switch over the type id does.
struct WireType {
	const char *name = "UNKNOWN";
	bool supported = false;
	WireFamily family = WireFamily::NONE;
	LogicalTypeId duckdb_type = LogicalTypeId::INVALID;
	TypeOfColumn type_of_column = nullptr;
	DecodeValue decode = nullptr;
};

//! The codec that reads a family's value, or none: a sql_variant and a CLR type take more than the
//! value and the column (DecodeRareForm), and NONE has no reader. Every family is listed and there is no
//! default, so a family added to the enum and not here is a -Wswitch warning.
DecodeValue DecoderOf(WireFamily family) {
	switch (family) {
	case WireFamily::INTEGER:
		return mssql::codec::integer::DecodeFromTds;
	case WireFamily::BOOLEAN:
		return mssql::codec::boolean::DecodeFromTds;
	case WireFamily::FLOAT:
		return mssql::codec::float_family::DecodeFromTds;
	case WireFamily::DECIMAL:
		return mssql::codec::decimal::DecodeFromTds;
	case WireFamily::MONEY:
		return mssql::codec::money::DecodeFromTds;
	case WireFamily::STRING:
		return mssql::codec::string::DecodeFromTds;
	case WireFamily::BINARY:
		return mssql::codec::binary::DecodeFromTds;
	case WireFamily::DATETIME:
		return mssql::codec::datetime::DecodeFromTds;
	case WireFamily::UUID:
		return mssql::codec::uuid::DecodeFromTds;
	case WireFamily::VARIANT:
	case WireFamily::UDT:
	case WireFamily::NONE:
		break;
	}
	return nullptr;
}

// Nullable integer variants
LogicalType IntNType(const ColumnMetadata &column) {
	switch (column.max_length) {
	case 1:
		return LogicalType::UTINYINT;  // SQL Server TINYINT is unsigned
	case 2:
		return LogicalType::SMALLINT;
	case 4:
		return LogicalType::INTEGER;
	case 8:
		return LogicalType::BIGINT;
	default:
		throw InvalidInputException("Invalid INTN length: %d", column.max_length);
	}
}

LogicalType FloatNType(const ColumnMetadata &column) {
	return (column.max_length == 4) ? LogicalType::FLOAT : LogicalType::DOUBLE;
}

// Decimal/Numeric
LogicalType DecimalType(const ColumnMetadata &column) {
	return LogicalType::DECIMAL(column.precision, column.scale);
}

// Money types -> DECIMAL(19,4) or DECIMAL(10,4)
LogicalType MoneyType(const ColumnMetadata &) {
	return LogicalType::DECIMAL(19, 4);
}

LogicalType SmallMoneyType(const ColumnMetadata &) {
	return LogicalType::DECIMAL(10, 4);
}

LogicalType MoneyNType(const ColumnMetadata &column) {
	return (column.max_length == 8) ? LogicalType::DECIMAL(19, 4) : LogicalType::DECIMAL(10, 4);
}

// A CLR type: hierarchyid decoded on the client into its ToString() text (spec 093 T4), geometry /
// geography into GEOMETRY of the ISO WKB, geography's labelled OGC:CRS84 (DQ-45), or a BLOB under the
// catalog's geometry_as 'wkb' / 'native' (T5, D6), and a user CLR type as its bytes (opaque by design).
LogicalType UdtType(const ColumnMetadata &column) {
	if (column.IsHierarchyIdUdt()) {
		return LogicalType::VARCHAR;
	}
	if (column.IsSpatialUdt()) {
		if (column.geometry_as != GeometryAs::Geometry) {
			return LogicalType::BLOB;
		}
		return column.IsGeographyUdt() ? LogicalType::GEOMETRY("OGC:CRS84") : LogicalType::GEOMETRY();
	}
	return LogicalType::BLOB;  // IsUserClrUdt
}

// sql_variant (spec 093 T1, D3): each value a VARIANT of its base type.
LogicalType VariantType(const ColumnMetadata &) {
	return LogicalType::VARIANT();
}

// vector(n) as VECTORTYPE (spec 093 T3, D4): FLOAT[n], n from the TYPE_INFO. Its JSON form is a
// varchar(max) and reads VARCHAR here, the server's text: no n is on the wire (D4); a catalog,
// which knows n, reads it into FLOAT[n] (ResolveColumnOps).
LogicalType VectorType(const ColumnMetadata &column) {
	return mssql::codec::sql_vector::FloatArrayType(column.VectorDimensions());
}

//! Every wire type id's row; an id no Set names keeps WireType's defaults, the unknown type.
//! A row is what this file answers for the id: its name (GetTypeName), whether IsSupported lists it,
//! the family that reads its value (ConvertValue, WriteAsStringFallback) and its DuckDB type
//! (GetDuckDBType).
struct WireTypes {
	WireType by_type[256];

	WireTypes() {
		// Integer types
		// Note: SQL Server TINYINT is unsigned (0-255), maps to UTINYINT
		Set(TDS_TYPE_TINYINT, "TINYINT", true, WireFamily::INTEGER, LogicalTypeId::UTINYINT);
		Set(TDS_TYPE_SMALLINT, "SMALLINT", true, WireFamily::INTEGER, LogicalTypeId::SMALLINT);
		Set(TDS_TYPE_INT, "INT", true, WireFamily::INTEGER, LogicalTypeId::INTEGER);
		Set(TDS_TYPE_BIGINT, "BIGINT", true, WireFamily::INTEGER, LogicalTypeId::BIGINT);
		Set(TDS_TYPE_INTN, "INTN", true, WireFamily::INTEGER, IntNType);

		// Boolean
		Set(TDS_TYPE_BIT, "BIT", true, WireFamily::BOOLEAN, LogicalTypeId::BOOLEAN);
		Set(TDS_TYPE_BITN, "BITN", true, WireFamily::BOOLEAN, LogicalTypeId::BOOLEAN);

		// Floating-point
		Set(TDS_TYPE_REAL, "REAL", true, WireFamily::FLOAT, LogicalTypeId::FLOAT);
		Set(TDS_TYPE_FLOAT, "FLOAT", true, WireFamily::FLOAT, LogicalTypeId::DOUBLE);
		Set(TDS_TYPE_FLOATN, "FLOATN", true, WireFamily::FLOAT, FloatNType);

		Set(TDS_TYPE_DECIMAL, "DECIMAL", true, WireFamily::DECIMAL, DecimalType);
		Set(TDS_TYPE_NUMERIC, "NUMERIC", true, WireFamily::DECIMAL, DecimalType);

		Set(TDS_TYPE_MONEY, "MONEY", true, WireFamily::MONEY, MoneyType);
		Set(TDS_TYPE_SMALLMONEY, "SMALLMONEY", true, WireFamily::MONEY, SmallMoneyType);
		Set(TDS_TYPE_MONEYN, "MONEYN", true, WireFamily::MONEY, MoneyNType);

		// String types -> VARCHAR
		Set(TDS_TYPE_BIGCHAR, "CHAR", true, WireFamily::STRING, LogicalTypeId::VARCHAR);
		Set(TDS_TYPE_BIGVARCHAR, "VARCHAR", true, WireFamily::STRING, LogicalTypeId::VARCHAR);
		Set(TDS_TYPE_NCHAR, "NCHAR", true, WireFamily::STRING, LogicalTypeId::VARCHAR);
		Set(TDS_TYPE_NVARCHAR, "NVARCHAR", true, WireFamily::STRING, LogicalTypeId::VARCHAR);

		// Binary types -> BLOB
		Set(TDS_TYPE_BIGBINARY, "BINARY", true, WireFamily::BINARY, LogicalTypeId::BLOB);
		Set(TDS_TYPE_BIGVARBINARY, "VARBINARY", true, WireFamily::BINARY, LogicalTypeId::BLOB);

		// Date/Time
		Set(TDS_TYPE_DATE, "DATE", true, WireFamily::DATETIME, LogicalTypeId::DATE);
		Set(TDS_TYPE_TIME, "TIME", true, WireFamily::DATETIME, LogicalTypeId::TIME);
		Set(TDS_TYPE_DATETIME, "DATETIME", true, WireFamily::DATETIME, LogicalTypeId::TIMESTAMP);
		Set(TDS_TYPE_SMALLDATETIME, "SMALLDATETIME", true, WireFamily::DATETIME, LogicalTypeId::TIMESTAMP);
		Set(TDS_TYPE_DATETIME2, "DATETIME2", true, WireFamily::DATETIME, LogicalTypeId::TIMESTAMP);
		Set(TDS_TYPE_DATETIMEN, "DATETIMEN", true, WireFamily::DATETIME, LogicalTypeId::TIMESTAMP);
		Set(TDS_TYPE_DATETIMEOFFSET, "DATETIMEOFFSET", true, WireFamily::DATETIME, LogicalTypeId::TIMESTAMP_TZ);

		// GUID
		Set(TDS_TYPE_UNIQUEIDENTIFIER, "UNIQUEIDENTIFIER", true, WireFamily::UUID, LogicalTypeId::UUID);

		// XML -> VARCHAR (PLP + UTF-16LE, same as NVARCHAR(MAX))
		Set(TDS_TYPE_XML, "XML", true, WireFamily::STRING, LogicalTypeId::VARCHAR);

		// Legacy LOBs (issue #197). Decoded natively since spec 055: the wire bytes
		// behind their text-pointer framing are ordinary CHAR / NCHAR / BINARY.
		// A catalog scan still casts them server-side, which also normalises TEXT's
		// collation; this mapping is what makes raw mssql_scan work on them too.
		// The staged path reads them; ConvertValue has no arm for them, so no family.
		Set(TDS_TYPE_TEXT, "TEXT", true, WireFamily::NONE, LogicalTypeId::VARCHAR);
		Set(TDS_TYPE_NTEXT, "NTEXT", true, WireFamily::NONE, LogicalTypeId::VARCHAR);
		Set(TDS_TYPE_IMAGE, "IMAGE", true, WireFamily::NONE, LogicalTypeId::BLOB);

		// IsSupported has never listed UDT, though it has a type and a decoder.
		Set(TDS_TYPE_UDT, "UDT", false, WireFamily::UDT, UdtType);
		Set(TDS_TYPE_SQL_VARIANT, "SQL_VARIANT", true, WireFamily::VARIANT, VariantType);
		// A VECTORTYPE value is read into FLOAT[n] before ConvertValue looks at the family.
		Set(TDS_TYPE_VECTOR, "VECTOR", true, WireFamily::NONE, VectorType);
	}

	void Set(uint8_t type_id, const char *name, bool supported, WireFamily family, LogicalTypeId duckdb_type) {
		WireType &type = by_type[type_id];
		type.name = name;
		type.supported = supported;
		type.family = family;
		type.duckdb_type = duckdb_type;
		type.decode = DecoderOf(family);
	}

	void Set(uint8_t type_id, const char *name, bool supported, WireFamily family, TypeOfColumn type_of_column) {
		Set(type_id, name, supported, family, LogicalTypeId::INVALID);
		by_type[type_id].type_of_column = type_of_column;
	}

	const WireType &Of(uint8_t type_id) const {
		return by_type[type_id];
	}
};

//! Built when the library loads. Nothing converts a value during another file's static initialisation,
//! so no reader can see it unbuilt; a function-local static would put a guard test on every value.
const WireTypes WIRE_TYPES;

// A CLR type: hierarchyid as its text (spec 093 T4); geometry / geography the WKB the client decodes,
// or under geometry_as 'native' the stored bytes into a BLOB -- into a GEOMETRY always the WKB (T5); a
// user CLR type as its bytes.
void DecodeUdt(const std::vector<uint8_t> &value, const ColumnMetadata &column, Vector &vector, idx_t row_idx,
			   mssql::codec::spatial::SpatialColumnNotes *spatial_notes) {
	if (column.IsHierarchyIdUdt()) {
		mssql::codec::hierarchyid::DecodeFromTds(value, column, vector, row_idx);
	} else if (column.IsSpatialUdt() &&
			   !(vector.GetType().id() == LogicalTypeId::BLOB && column.geometry_as == GeometryAs::Native)) {
		mssql::codec::spatial::DecodeFromTds(value, column, vector, row_idx, spatial_notes);
	} else {
		mssql::codec::binary::DecodeFromTds(value, column, vector, row_idx);
	}
}

//! A value whose family has no plain codec: a sql_variant and a CLR type, which count into the
//! column's notes; any other family is a type the per-value path does not read.
void DecodeRareForm(WireFamily family, const std::vector<uint8_t> &value, const ColumnMetadata &column, Vector &vector,
					idx_t row_idx, mssql::codec::variant::VariantColumnNotes *variant_notes,
					mssql::codec::spatial::SpatialColumnNotes *spatial_notes) {
	if (family == WireFamily::VARIANT) {
		mssql::codec::variant::DecodeFromTds(value, column, vector, row_idx, variant_notes);
		return;
	}
	if (family == WireFamily::UDT) {
		DecodeUdt(value, column, vector, row_idx, spatial_notes);
		return;
	}
	throw InvalidInputException("Type conversion not implemented for type 0x%02X", column.type_id);
}

}  // namespace

LogicalType TypeConverter::GetDuckDBType(const ColumnMetadata &column) {
	const WireType &type = WIRE_TYPES.Of(column.type_id);
	if (type.type_of_column) {
		return type.type_of_column(column);
	}
	if (type.duckdb_type == LogicalTypeId::INVALID) {
		throw InvalidInputException("MSSQL Error: Unknown SQL Server type (0x%02X) for column '%s'.", column.type_id,
									column.name.c_str());
	}
	return LogicalType(type.duckdb_type);
}

bool TypeConverter::IsSupported(uint8_t type_id) {
	return WIRE_TYPES.Of(type_id).supported;
}

std::string TypeConverter::GetTypeName(uint8_t type_id) {
	return WIRE_TYPES.Of(type_id).name;
}

void TypeConverter::ConvertValue(const std::vector<uint8_t> &value, bool is_null, const ColumnMetadata &column,
								 Vector &vector, idx_t row_idx,
								 mssql::codec::variant::VariantColumnNotes *variant_notes,
								 mssql::codec::spatial::SpatialColumnNotes *spatial_notes) {
	if (is_null) {
		FlatVector::SetNull(vector, row_idx, true);
		return;
	}
	// Spec 093 T3: a vector column into FLOAT[n], from VECTORTYPE or its JSON text (whose wire type is a
	// string's, so before the type's codec).
	if (vector.GetType().id() == LogicalTypeId::ARRAY && (column.IsVectorWire() || column.IsVectorText())) {
		mssql::codec::sql_vector::DecodeFromTds(value, column, vector, row_idx);
		return;
	}

	// Issue #89: catalog vs runtime type divergence. SQL Server views can project a column
	// at a different type than sys.columns reports (typically via CAST/CONVERT inside the
	// view definition). When the destination vector was allocated as VARCHAR from the catalog
	// but TDS sends back a non-string type, render the value as a string instead of crashing
	// inside FlatVector::GetData<T> with a vector-type assertion.
	// A hierarchyid's VARCHAR is its own decoded text (spec 093 T4), not a divergence.
	if (vector.GetType().id() == LogicalTypeId::VARCHAR && !IsStringTdsType(column.type_id) &&
		!column.IsHierarchyIdUdt()) {
		WriteAsStringFallback(value, column, vector, row_idx);
		return;
	}

	const WireType &type = WIRE_TYPES.Of(column.type_id);
	if (type.decode) {
		type.decode(value, column, vector, row_idx);
		return;
	}
	DecodeRareForm(type.family, value, column, vector, row_idx, variant_notes, spatial_notes);
}

//===----------------------------------------------------------------------===//
// Issue #89 — VARCHAR fallback for catalog-vs-runtime type divergence
//===----------------------------------------------------------------------===//

bool TypeConverter::IsStringTdsType(uint8_t type_id) {
	switch (type_id) {
	case TDS_TYPE_BIGCHAR:
	case TDS_TYPE_BIGVARCHAR:
	case TDS_TYPE_NCHAR:
	case TDS_TYPE_NVARCHAR:
	case TDS_TYPE_XML:
	case TDS_TYPE_TEXT:
	case TDS_TYPE_NTEXT:
		return true;
	default:
		return false;
	}
}

namespace {

std::string FormatIntegerFallback(const uint8_t *value, size_t size) {
	// Mirror codec::integer::DecodeFromTds size-dispatch — TINYINT is unsigned (0-255),
	// SMALLINT/INT/BIGINT are signed.
	switch (size) {
	case 1:
		return std::to_string(static_cast<unsigned int>(value[0]));
	case 2: {
		int16_t v = 0;
		std::memcpy(&v, value, 2);
		return std::to_string(v);
	}
	case 4: {
		int32_t v = 0;
		std::memcpy(&v, value, 4);
		return std::to_string(v);
	}
	case 8: {
		int64_t v = 0;
		std::memcpy(&v, value, 8);
		return std::to_string(v);
	}
	default:
		throw InvalidInputException("VARCHAR fallback: unexpected integer length %zu", size);
	}
}

std::string FormatFloatFallback(const uint8_t *value, size_t size) {
	std::ostringstream oss;
	if (size == 4) {
		float f = 0;
		std::memcpy(&f, value, 4);
		oss << std::setprecision(9) << f;
	} else if (size == 8) {
		double d = 0;
		std::memcpy(&d, value, 8);
		oss << std::setprecision(17) << d;
	} else {
		throw InvalidInputException("VARCHAR fallback: unexpected float length %zu", size);
	}
	return oss.str();
}

std::string FormatBitFallback(const uint8_t *value, size_t size) {
	return (size > 0 && value[0] != 0) ? "1" : "0";
}

//! The value as text, by its family; a family with no text form here (a string is no divergence, and
//! sql_variant, a CLR type, the legacy LOBs and vector have none) is refused by the wire type's id.
std::string RenderFallback(const uint8_t *value, size_t size, const ColumnMetadata &column) {
	switch (WIRE_TYPES.Of(column.type_id).family) {
	case WireFamily::INTEGER:
		return FormatIntegerFallback(value, size);
	case WireFamily::BOOLEAN:
		return FormatBitFallback(value, size);
	case WireFamily::FLOAT:
		return FormatFloatFallback(value, size);
	case WireFamily::DECIMAL:
		return mssql::codec::decimal::RenderAsString(value, size, column.precision, column.scale);
	case WireFamily::MONEY:
		return mssql::codec::decimal::RenderMoneyAsString(value, size);
	case WireFamily::UUID:
		return mssql::codec::uuid::RenderAsString(value, size);
	case WireFamily::BINARY:
		return mssql::codec::binary::RenderAsString(value, size);
	case WireFamily::DATETIME:
		return mssql::codec::datetime::RenderAsString(value, size, column);
	default:
		throw InvalidInputException(
			"MSSQL: catalog reported VARCHAR for this column but SQL Server returned TDS type 0x%02X — "
			"this typically happens with VIEWs that CAST/CONVERT a column to a different type. "
			"Workaround: re-attach the database after the view definition changed, or use mssql_scan() "
			"with an explicit CAST in the query.",
			column.type_id);
	}
}

}  // namespace

void TypeConverter::WriteAsStringFallback(const uint8_t *value, size_t size, const ColumnMetadata &column,
										  Vector &vector, idx_t row_idx) {
	const std::string rendered = RenderFallback(value, size, column);
	FlatVector::GetDataMutable<string_t>(vector)[row_idx] = StringVector::AddString(vector, rendered);
}

void TypeConverter::WriteAsStringFallback(const std::vector<uint8_t> &value, const ColumnMetadata &column,
										  Vector &vector, idx_t row_idx) {
	WriteAsStringFallback(value.data(), value.size(), column, vector, row_idx);
}

}  // namespace encoding
}  // namespace tds
}  // namespace duckdb
