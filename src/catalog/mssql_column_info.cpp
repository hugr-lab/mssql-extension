#include "catalog/mssql_column_info.hpp"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include "codec/target_string_type.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/extension_type_info.hpp"
#include "query/mssql_identifier.hpp"

namespace duckdb {

MSSQLColumnInfo::MSSQLColumnInfo()
	: column_id(0),
	  max_length(0),
	  precision(0),
	  scale(0),
	  is_nullable(true),
	  is_case_sensitive(false),
	  is_unicode(false),
	  is_utf8(false),
	  code_page(0),
	  database_code_page(0),
	  is_cast_required(false),
	  is_geometry(false),
	  is_identity(false) {}

MSSQLColumnInfo::MSSQLColumnInfo(const string &name, int32_t column_id, const string &sql_type_name, int16_t max_length,
								 uint8_t precision, uint8_t scale, bool is_nullable, const string &collation_name,
								 const string &database_collation)
	: name(name),
	  column_id(column_id),
	  sql_type_name(sql_type_name),
	  max_length(max_length),
	  precision(precision),
	  scale(scale),
	  is_nullable(is_nullable),
	  is_identity(false) {
	// Use database collation as fallback if column collation is empty
	if (collation_name.empty() && IsTextType(sql_type_name)) {
		this->collation_name = database_collation;
	} else {
		this->collation_name = collation_name;
	}

	// Derive collation flags
	is_case_sensitive = IsCaseSensitiveCollation(this->collation_name);
	is_unicode = IsUnicodeType(sql_type_name);
	is_utf8 = IsUTF8Collation(this->collation_name);

	// Map to DuckDB type
	duckdb_type = MapSQLServerTypeToDuckDB(sql_type_name, max_length, precision, scale);

	// Detect geometry/geography UDTs — table scan rewrites these to .STAsBinary()
	// so the wire delivers OGC WKB bytes (varbinary(max)) instead of MS's
	// proprietary Spatial Type Binary Format. Catalog reports LogicalType::GEOMETRY().
	{
		string lower_type = sql_type_name;
		std::transform(lower_type.begin(), lower_type.end(), lower_type.begin(),
					   [](unsigned char c) { return std::tolower(c); });
		is_geometry = IsSpatialType(lower_type);
	}

	// Mark columns with unsupported SQL Server types for auto-CAST in pushdown.
	// Geometry/geography are "known" (we handle them via STAsBinary rewrite), not auto-CAST.
	is_cast_required = !IsKnownSQLServerType(sql_type_name);
}

//===----------------------------------------------------------------------===//
// Native type reporting (spec 060)
//===----------------------------------------------------------------------===//

LogicalType MSSQLColumnInfo::NativeDuckDBType() const {
	// Only a bounded character column has anything to state. A MAX column
	// (max_length -1) is already what a plain VARCHAR means, and a column the
	// scan has to CAST — geometry, hierarchyid, sql_variant — does not arrive as
	// the type the catalog names anyway.
	//
	// text/ntext are NOT caught by this guard, despite being MAX by nature:
	// sys.columns reports max_length 16 for them (the in-row pointer size, which
	// is why GetNVarcharLength has to special-case them to MAX rather than derive
	// a CAST length from it — issue #197, already fixed), so they pass it and
	// fall through the
	// unmatched-type-name `else` at the end of the if/else-if chain below. Same
	// outcome, different
	// route — worth stating because the guard's clause list reads like a
	// complete enumeration and is not one.
	if (duckdb_type.id() != LogicalTypeId::VARCHAR || max_length <= 0 || is_cast_required || is_geometry) {
		return duckdb_type;
	}

	string lower_type = sql_type_name;
	std::transform(lower_type.begin(), lower_type.end(), lower_type.begin(),
				   [](unsigned char c) { return std::tolower(c); });

	mssql::codec::TargetStringType spec;
	if (lower_type == "nvarchar" || lower_type == "nchar") {
		// sys.columns reports these in bytes, two per UTF-16 code unit.
		spec.unicode = true;
		spec.length = max_length / 2;
		// A MAX column never reaches here: sys.columns reports max_length = -1
		// for it and the `max_length <= 0` clause of the guard above returns
		// first. That is the guard to move for the open half of issue #321 —
		// reporting MSSQL_VARCHAR(MAX) so a varchar(max) COLLATE X source keeps
		// X instead of arriving as a plain VARCHAR, which is the round trip
		// mssql_catalog_native_types promises and does not currently complete
		// for MAX columns. The `< 1` test below is the SECOND one that would
		// have to move, and it is not merely tidy now that #321 gave 0 a
		// meaning: -1 / 2 truncates to 0, which IS the MAX sentinel. The cost
		// of doing it is a changed type name in DESCRIBE for every MAX column
		// in every attached database.
		if (spec.length < 1 || spec.length > mssql::codec::MAX_NVARCHAR_LENGTH) {
			return duckdb_type;
		}
	} else if (lower_type == "varchar" || lower_type == "char") {
		spec.unicode = false;
		spec.length = max_length;
		if (spec.length < 1 || spec.length > mssql::codec::MAX_VARCHAR_LENGTH) {
			return duckdb_type;
		}
		// Carry the source's OWN collation, so a target created from this column
		// is the column: same capacity for the same values. Inheriting the
		// session's UTF-8 collation instead would silently change how many
		// characters fit, since this length is in bytes either way.
		if (mssql::codec::IsValidCollationName(collation_name)) {
			spec.collation = collation_name;
		}
	} else {
		return duckdb_type;
	}

	return mssql::codec::MakeTargetStringType(spec);
}

//===----------------------------------------------------------------------===//
// Collation Detection
//===----------------------------------------------------------------------===//

bool MSSQLColumnInfo::IsCaseSensitiveCollation(const string &collation_name) {
	if (collation_name.empty()) {
		return false;  // Assume case-insensitive if unknown
	}

	// Convert to uppercase for comparison
	string upper_collation = collation_name;
	std::transform(upper_collation.begin(), upper_collation.end(), upper_collation.begin(),
				   [](unsigned char c) { return std::toupper(c); });

	// Check for _CS_ (case-sensitive) or _BIN (binary)
	if (upper_collation.find("_CS_") != string::npos || upper_collation.find("_CS") == upper_collation.length() - 3) {
		return true;
	}

	// Binary collations are case-sensitive
	if (upper_collation.find("_BIN") != string::npos) {
		return true;
	}

	// _CI_ indicates case-insensitive
	return false;
}

bool MSSQLColumnInfo::IsAccentSensitiveCollation(const string &collation_name) {
	if (collation_name.empty()) {
		return true;  // Assume accent-sensitive if unknown
	}

	// Convert to uppercase for comparison
	string upper_collation = collation_name;
	std::transform(upper_collation.begin(), upper_collation.end(), upper_collation.begin(),
				   [](unsigned char c) { return std::toupper(c); });

	// Check for _AI_ (accent-insensitive)
	if (upper_collation.find("_AI_") != string::npos || upper_collation.find("_AI") == upper_collation.length() - 3) {
		return false;
	}

	// _AS_ indicates accent-sensitive (default)
	return true;
}

bool MSSQLColumnInfo::IsUTF8Collation(const string &collation_name) {
	if (collation_name.empty()) {
		return false;
	}

	// Convert to uppercase for comparison
	string upper_collation = collation_name;
	std::transform(upper_collation.begin(), upper_collation.end(), upper_collation.begin(),
				   [](unsigned char c) { return std::toupper(c); });

	// Check for _UTF8 suffix
	return upper_collation.find("_UTF8") != string::npos;
}

//===----------------------------------------------------------------------===//
// Type Mapping
//===----------------------------------------------------------------------===//

LogicalType MSSQLColumnInfo::MapSQLServerTypeToDuckDB(const string &sql_type_name, int16_t max_length,
													  uint8_t precision, uint8_t scale) {
	// Convert to lowercase for comparison
	string lower_type = sql_type_name;
	std::transform(lower_type.begin(), lower_type.end(), lower_type.begin(),
				   [](unsigned char c) { return std::tolower(c); });

	// Integer types
	if (lower_type == "bit") {
		return LogicalType::BOOLEAN;
	}
	if (lower_type == "tinyint") {
		return LogicalType::UTINYINT;
	}
	if (lower_type == "smallint") {
		return LogicalType::SMALLINT;
	}
	if (lower_type == "int") {
		return LogicalType::INTEGER;
	}
	if (lower_type == "bigint") {
		return LogicalType::BIGINT;
	}

	// Floating point types
	if (lower_type == "real") {
		return LogicalType::FLOAT;
	}
	if (lower_type == "float") {
		return LogicalType::DOUBLE;
	}

	// Decimal/numeric types
	if (lower_type == "decimal" || lower_type == "numeric") {
		return LogicalType::DECIMAL(precision, scale);
	}
	if (lower_type == "money") {
		return LogicalType::DECIMAL(19, 4);
	}
	if (lower_type == "smallmoney") {
		return LogicalType::DECIMAL(10, 4);
	}

	// Character types
	if (lower_type == "char" || lower_type == "varchar" || lower_type == "text") {
		return LogicalType::VARCHAR;
	}
	if (lower_type == "nchar" || lower_type == "nvarchar" || lower_type == "ntext") {
		return LogicalType::VARCHAR;  // Unicode also maps to VARCHAR in DuckDB
	}

	// Date/time types
	if (lower_type == "date") {
		return LogicalType::DATE;
	}
	if (lower_type == "time") {
		return LogicalType::TIME;
	}
	if (lower_type == "datetime" || lower_type == "smalldatetime") {
		// Fixed wire precision (~3 ms for DATETIME, 1 min for SMALLDATETIME) —
		// always fits in DuckDB's µs TIMESTAMP.
		return LogicalType::TIMESTAMP;
	}
	if (lower_type == "datetime2") {
		// Pick the narrowest DuckDB TIMESTAMP variant that can losslessly hold
		// the column's wire precision (spec 045 — type round-trip transparency).
		// scale 0       → TIMESTAMP_S  (seconds)
		// scale 1-3     → TIMESTAMP_MS (milliseconds)
		// scale 4-6     → TIMESTAMP    (microseconds — DuckDB native)
		// scale 7       → TIMESTAMP_NS (DuckDB ns can hold 100-ns wire ticks losslessly)
		if (scale == 0) {
			return LogicalType::TIMESTAMP_S;
		}
		if (scale <= 3) {
			return LogicalType::TIMESTAMP_MS;
		}
		if (scale <= 6) {
			return LogicalType::TIMESTAMP;
		}
		return LogicalType::TIMESTAMP_NS;
	}
	if (lower_type == "datetimeoffset") {
		// DuckDB has no nanosecond-precision time-zone-aware type; collapse to
		// µs TIMESTAMP_TZ regardless of source scale (lossless for ≤6, drops
		// the trailing digit for scale 7).
		return LogicalType::TIMESTAMP_TZ;
	}

	// Binary types
	if (lower_type == "binary" || lower_type == "varbinary" || lower_type == "image") {
		return LogicalType::BLOB;
	}

	// rowversion, whose type name in sys.types is the misleading `timestamp`:
	// nothing to do with time, an 8-byte counter the server bumps on every
	// write (issue #296). It arrives as BIGBINARY(8) — `sp_describe_first_result_set`
	// reports tds_type_id 173, length 8, and the describe path in
	// mssql_functions.cpp has always mapped it that way — so the binary codec
	// decodes it with no help. Until this line it fell through to the VARCHAR
	// default below and was therefore CAST, which SQL Server refuses outright:
	// `[529] Explicit conversion from data type timestamp to nvarchar(max) is
	// not allowed`, making every table with such a column unreadable. Both
	// spellings are accepted because a user writes ROWVERSION and sys.types
	// answers timestamp.
	if (lower_type == "timestamp" || lower_type == "rowversion") {
		return LogicalType::BLOB;
	}

	// The native JSON type of SQL Server 2025. On the wire it is plain
	// varchar(max) under a UTF-8 BIN2 collation — `sp_describe_first_result_set`
	// answers system_type_name `varchar(max)`, tds_type_id 167 — so the bytes
	// are already what a DuckDB VARCHAR wants and the binary kernel reads them
	// as they are. Naming it here is only about NOT treating it as unknown:
	// without the name it is CAST to NVARCHAR(MAX), which works and costs a
	// server-side conversion of every value for nothing.
	if (lower_type == "json") {
		return LogicalType::VARCHAR;
	}

	// Special types
	if (lower_type == "uniqueidentifier") {
		return LogicalType::UUID;
	}

	// Spatial types — geometry and geography both arrive via STAsBinary() rewrite
	// (see is_geometry handling in the constructor + table_scan::BuildColumnExpression).
	// DuckDB's first-class GEOMETRY type stores WKB bytes — same physical storage as BLOB.
	if (IsSpatialType(lower_type)) {
		return LogicalType::GEOMETRY();
	}

	// Default to VARCHAR for unknown types
	return LogicalType::VARCHAR;
}

//===----------------------------------------------------------------------===//
// Type Checks
//===----------------------------------------------------------------------===//

bool MSSQLColumnInfo::IsBinary2Collation(const string &collation_name) {
	string upper_collation = collation_name;
	std::transform(upper_collation.begin(), upper_collation.end(), upper_collation.begin(),
				   [](unsigned char c) { return std::toupper(c); });
	return upper_collation.find("_BIN2") != string::npos;
}

bool MSSQLColumnInfo::OrdersLikeDuckDB() const {
	if (is_cast_required || is_geometry) {
		return false;
	}
	string lower_type = sql_type_name;
	std::transform(lower_type.begin(), lower_type.end(), lower_type.begin(),
				   [](unsigned char c) { return std::tolower(c); });
	if (lower_type == "varchar" || lower_type == "char") {
		return is_utf8 && IsBinary2Collation(collation_name);
	}
	static const char *const ORDERED_TYPES[] = {
		"bit",	 "tinyint", "smallint", "int",	"bigint",	"decimal",	 "numeric",		  "money",		   "smallmoney",
		"float", "real",	"date",		"time", "datetime", "datetime2", "smalldatetime", "datetimeoffset"};
	for (auto type_name : ORDERED_TYPES) {
		if (lower_type == type_name) {
			return true;
		}
	}
	return false;
}

bool MSSQLColumnInfo::IsSpatialType(const string &sql_type_name) {
	string lower_type = sql_type_name;
	std::transform(lower_type.begin(), lower_type.end(), lower_type.begin(),
				   [](unsigned char c) { return std::tolower(c); });
	return lower_type == "geometry" || lower_type == "geography";
}

bool MSSQLColumnInfo::IsKnownSQLServerType(const string &sql_type_name) {
	string lower_type = sql_type_name;
	std::transform(lower_type.begin(), lower_type.end(), lower_type.begin(),
				   [](unsigned char c) { return std::tolower(c); });

	// All types explicitly handled in MapSQLServerTypeToDuckDB
	return lower_type == "bit" || lower_type == "tinyint" || lower_type == "smallint" || lower_type == "int" ||
		   lower_type == "bigint" || lower_type == "real" || lower_type == "float" || lower_type == "decimal" ||
		   lower_type == "numeric" || lower_type == "money" || lower_type == "smallmoney" || lower_type == "char" ||
		   lower_type == "varchar" || lower_type == "text" || lower_type == "nchar" || lower_type == "nvarchar" ||
		   lower_type == "ntext" || lower_type == "date" || lower_type == "time" || lower_type == "datetime" ||
		   lower_type == "datetime2" || lower_type == "smalldatetime" || lower_type == "datetimeoffset" ||
		   lower_type == "binary" || lower_type == "varbinary" || lower_type == "image" ||
		   lower_type == "uniqueidentifier" ||
		   // rowversion: BIGBINARY(8) on the wire, and CASTing it is not merely
		   // wasteful but rejected by the server with error 529 (issue #296).
		   lower_type == "timestamp" || lower_type == "rowversion" ||
		   // The 2025 JSON type: varchar(max) with a UTF-8 collation on the wire.
		   lower_type == "json" ||
		   // XML has dedicated TDS-level support (0xF1) and works without CAST
		   lower_type == "xml" ||
		   // Spatial UDTs — handled by table-scan rewrite to STAsBinary() (spec 045 / sub-phase 5).
		   IsSpatialType(lower_type);
}

bool MSSQLColumnInfo::IsTextType(const string &sql_type_name) {
	string lower_type = sql_type_name;
	std::transform(lower_type.begin(), lower_type.end(), lower_type.begin(),
				   [](unsigned char c) { return std::tolower(c); });

	return lower_type == "char" || lower_type == "varchar" || lower_type == "text" || lower_type == "nchar" ||
		   lower_type == "nvarchar" || lower_type == "ntext";
}

bool MSSQLColumnInfo::IsUnicodeType(const string &sql_type_name) {
	string lower_type = sql_type_name;
	std::transform(lower_type.begin(), lower_type.end(), lower_type.begin(),
				   [](unsigned char c) { return std::tolower(c); });

	return lower_type == "nchar" || lower_type == "nvarchar" || lower_type == "ntext";
}

//===----------------------------------------------------------------------===//
// Read expression (shared by the scan's SELECT list and INSERT's OUTPUT list)
//===----------------------------------------------------------------------===//

namespace {

//! TEXT / NTEXT / IMAGE — the pre-2005 LOB types. Their wire tokens are not
//! the varchar/nvarchar/varbinary ones and no codec decodes them.
enum class LegacyLob { None, Text, NText, Image };

LegacyLob LegacyLobKind(const string &sql_type_name) {
	string lower_type = sql_type_name;
	std::transform(lower_type.begin(), lower_type.end(), lower_type.begin(),
				   [](unsigned char c) { return std::tolower(c); });
	if (lower_type == "text") {
		return LegacyLob::Text;
	}
	if (lower_type == "ntext") {
		return LegacyLob::NText;
	}
	if (lower_type == "image") {
		return LegacyLob::Image;
	}
	return LegacyLob::None;
}

//! Does this column need the CHAR/VARCHAR/TEXT -> NVARCHAR rewrite?
//!
//! Only non-Unicode text under a non-UTF-8 collation: its bytes are in the
//! column's code page, and a DuckDB VARCHAR is UTF-8 by contract.
bool NeedsNVarcharConversion(const string &sql_type_name, int16_t max_length, const string &collation_name,
							 bool convert_varchar_max) {
	if (MSSQLColumnInfo::IsUnicodeType(sql_type_name)) {
		return false;  // already Unicode
	}
	if (!MSSQLColumnInfo::IsTextType(sql_type_name)) {
		return false;  // not a string type
	}
	if (MSSQLColumnInfo::IsUTF8Collation(collation_name)) {
		return false;  // UTF-8 is safe to pass through
	}
	// The setting governs *declared*-MAX columns only (max_length == -1), matching its name and
	// documented purpose. A varchar(4001..8000) is not VARCHAR(MAX): the length helper promotes it
	// to NVARCHAR(MAX) because no shorter NVARCHAR could hold it, not because the user asked for
	// MAX, so the opt-out does not apply to it.
	//
	// TEXT is never opted out. Unlike varchar it has no decodable uncast wire form: it is a known
	// type (so is_cast_required is false) and dropping the CAST would put TDS_TYPE_TEXT (0x23) on
	// the wire, which no codec handles — the read would fail outright rather than degrade.
	if (LegacyLobKind(sql_type_name) == LegacyLob::None && max_length == -1 && !convert_varchar_max) {
		return false;
	}
	return true;
}

//! Length for the NVARCHAR CAST. MAX for VARCHAR(MAX), for the legacy LOBs, and
//! for any CHAR/VARCHAR wider than the 4000-character inline NVARCHAR limit —
//! such a column has no valid inline length, so it must go over as PLP.
//! (Mirrors the >4000 PLP fallback on the BCP write path in
//! SQLServerTypeMaxLength, src/copy/target_resolver.cpp.)
string NVarcharLength(const string &sql_type_name, int16_t max_length) {
	if (max_length == -1) {
		return "MAX";
	}
	if (LegacyLobKind(sql_type_name) != LegacyLob::None) {
		return "MAX";  // TEXT/NTEXT -> NVARCHAR(MAX); their max_length of 16 is the pointer size
	}
	if (max_length > 4000) {
		return "MAX";  // varchar(4001..8000) -> NVARCHAR(MAX); NVARCHAR(4000) would truncate
	}
	return std::to_string(max_length);
}

}  // namespace

string MSSQLColumnInfo::BuildReadExpression(const string &col_name, const string &sql_type_name, int16_t max_length,
											const string &collation_name, bool convert_varchar_max,
											const string &qualifier) {
	const string escaped_name = mssql::QuoteIdentifier(col_name);
	const string reference = qualifier + escaped_name;

	if (IsSpatialType(sql_type_name)) {
		return reference + ".STAsBinary() AS " + escaped_name;
	}

	const LegacyLob lob_kind = LegacyLobKind(sql_type_name);
	if (lob_kind == LegacyLob::NText) {
		return "CAST(" + reference + " AS NVARCHAR(MAX)) AS " + escaped_name;
	}
	if (lob_kind == LegacyLob::Image) {
		return "CAST(" + reference + " AS VARBINARY(MAX)) AS " + escaped_name;
	}

	// Unsupported SQL Server types (hierarchyid, sql_variant, CLR UDTs) must be
	// CAST to NVARCHAR(MAX) so the server sends text instead of a native wire
	// form nothing decodes.
	if (!IsKnownSQLServerType(sql_type_name)) {
		return "CAST(" + reference + " AS NVARCHAR(MAX)) AS " + escaped_name;
	}

	if (NeedsNVarcharConversion(sql_type_name, max_length, collation_name, convert_varchar_max)) {
		return "CAST(" + reference + " AS NVARCHAR(" + NVarcharLength(sql_type_name, max_length) + ")) AS " +
			   escaped_name;
	}

	// Nothing to rewrite. Unqualified this is just the column; qualified it needs
	// the alias, or the result would come back named after the qualifier's table.
	return qualifier.empty() ? escaped_name : reference + " AS " + escaped_name;
}

}  // namespace duckdb
