#include "catalog/mssql_column_info.hpp"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include "codec/code_page/code_page.hpp"
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
	  read_form(MSSQLReadForm::Bare),
	  comparison_differs(false),
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

	// Spec 093 D2: how the scan reads it, and whether the server compares it unlike DuckDB -- two
	// answers that were one flag (is_cast_required) while the two always coincided.
	read_form = ReadFormFor(sql_type_name);
	comparison_differs = ComparisonDiffersFor(sql_type_name);
}

MSSQLReadForm MSSQLColumnInfo::ReadFormFor(const string &sql_type_name) {
	// Geometry / geography: the table scan rewrites them to .STAsBinary(), so the wire delivers OGC WKB
	// (varbinary(max)) instead of MS-SSCLRT; the catalog reports LogicalType::GEOMETRY().
	if (IsSpatialType(sql_type_name)) {
		return MSSQLReadForm::SpatialBinary;
	}
	return IsKnownSQLServerType(sql_type_name) ? MSSQLReadForm::Bare : MSSQLReadForm::ServerCast;
}

bool MSSQLColumnInfo::ComparisonDiffersFor(const string &sql_type_name) {
	return !IsKnownSQLServerType(sql_type_name);
}

bool MSSQLColumnInfo::BulkWireTakesType(const string &sql_type_name) {
	// A geometry / geography column arrives as WKB, which the statement path renders as a 0x literal
	// the server converts; on the bulk wire FromServerColumn would declare it nvarchar and send the
	// bytes as text. The same for the types read through a cast (hierarchyid, sql_variant): the
	// statement path sends a string literal the server converts, the bulk wire would not.
	return IsKnownSQLServerType(sql_type_name) && !IsSpatialType(sql_type_name);
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
	if (duckdb_type.id() != LogicalTypeId::VARCHAR || max_length <= 0 || read_form != MSSQLReadForm::Bare) {
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
	// (see ReadFormFor's SpatialBinary + table_scan::BuildColumnExpression).
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
	if (comparison_differs || IsSpatial()) {
		return false;
	}
	string lower_type = sql_type_name;
	std::transform(lower_type.begin(), lower_type.end(), lower_type.begin(),
				   [](unsigned char c) { return std::tolower(c); });
	if (lower_type == "datetime2" && scale >= 7) {
		// TIMESTAMP_NS: the codec reads a value outside 1677-2262 -- the
		// 9999-12-31 ValidTo sentinel of a temporal table -- as NULL (#168), so
		// the server orders values DuckDB never sees (review of #387).
		return false;
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

bool MSSQLColumnInfo::OrdersLikeDuckDBAsBytes() const {
	if (comparison_differs || !is_utf8 || max_length <= 0) {
		return false;
	}
	string lower_type = sql_type_name;
	std::transform(lower_type.begin(), lower_type.end(), lower_type.begin(),
				   [](unsigned char c) { return std::tolower(c); });
	return lower_type == "varchar";
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

//! TEXT / NTEXT / IMAGE — the pre-2005 LOB types. Their wire tokens are not the
//! varchar/nvarchar/varbinary ones: the staged path decodes them, the per-value one
//! (TypeConverter::ConvertValue) does not.
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

//! Which page family the client transcodes by default (spec 092 D10). A double-byte
//! page keeps the server's cast: its wire bytes are nearly equal and the client's
//! decode costs ~11.6x simdutf's (owner's decision, measured). A raw scan does not
//! ask: it has no cast to keep.
bool TranscodeOnClientByDefault(const mssql::codec::code_page::CodePageTable &page) {
	return !mssql::codec::code_page::IsDoubleByte(page);
}

//! Does this column need the CHAR/VARCHAR/TEXT -> NVARCHAR rewrite?
//!
//! Only non-Unicode text under a non-UTF-8 collation that the client does not
//! transcode: its bytes are in the column's code page, and a DuckDB VARCHAR is
//! UTF-8 by contract.
bool NeedsNVarcharConversion(const string &sql_type_name, int16_t max_length, const string &collation_name,
							 bool convert_varchar_max, bool client_transcode) {
	if (MSSQLColumnInfo::IsUnicodeType(sql_type_name)) {
		return false;  // already Unicode
	}
	if (!MSSQLColumnInfo::IsTextType(sql_type_name)) {
		return false;  // not a string type
	}
	if (MSSQLColumnInfo::IsUTF8Collation(collation_name)) {
		return false;  // UTF-8 is safe to pass through
	}
	if (MSSQLColumnInfo::ClientTranscodedPage(sql_type_name, collation_name, client_transcode)) {
		return false;  // read as bytes, transcoded on the client
	}
	// The setting governs *declared*-MAX columns only (max_length == -1), matching its name and
	// documented purpose. A varchar(4001..8000) is not VARCHAR(MAX): the length helper promotes it
	// to NVARCHAR(MAX) because no shorter NVARCHAR could hold it, not because the user asked for
	// MAX, so the opt-out does not apply to it.
	//
	// TEXT is never opted out. The staged path decodes its wire form since #197, but the per-value
	// one does not (TypeConverter::ConvertValue has no TEXT arm): dropping the CAST would fail a
	// composite rowid's STRUCT and INSERT ... RETURNING outright rather than degrade.
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

const mssql::codec::code_page::tables::CodePageTable *MSSQLColumnInfo::ClientTranscodedPage(
	const string &sql_type_name, const string &collation_name, bool client_transcode) {
	if (!client_transcode) {
		return nullptr;
	}
	const auto type = StringUtil::Lower(sql_type_name);
	if (type != "char" && type != "varchar") {
		return nullptr;
	}
	const auto *collation = mssql::codec::code_page::FindCollationByName(collation_name.c_str(), collation_name.size());
	const auto *page = collation ? mssql::codec::code_page::FindCodePage(collation->code_page) : nullptr;
	return page && TranscodeOnClientByDefault(*page) ? page : nullptr;
}

string MSSQLColumnInfo::BuildReturningExpression(const string &col_name, const string &sql_type_name,
												 int16_t max_length, const string &collation_name,
												 bool convert_varchar_max, bool client_transcode,
												 const string &qualifier, const string &alias) {
	const auto type = StringUtil::Lower(sql_type_name);
	const auto reference = qualifier + mssql::QuoteIdentifier(col_name);
	const auto as = " AS " + mssql::QuoteIdentifier(alias.empty() ? col_name : alias);
	if (type == "timestamp" || type == "rowversion") {
		return "CAST(" + reference + " AS binary(8))" + as;
	}
	if (type == "char" && !ClientTranscodedPage(sql_type_name, collation_name, client_transcode)) {
		// The read expression is NCHAR(n) (#405), which doubles a char's fixed
		// width in the table this types: two char(2100) columns came to 8400
		// bytes, past the 8060-byte row (1701), where the source table and a
		// bare OUTPUT had no trouble. A trimmed NVARCHAR is variable-length and
		// arrives without the padding, as the NCHAR read would (review of #471).
		return "RTRIM(CAST(" + reference + " AS NVARCHAR(" +
			   (max_length > 0 && max_length <= 4000 ? std::to_string(max_length) : string("MAX")) + ")))" + as;
	}
	return BuildReadExpression(col_name, sql_type_name, max_length, collation_name, convert_varchar_max,
							   client_transcode, qualifier, alias);
}

string MSSQLColumnInfo::BuildReadExpression(const string &col_name, const string &sql_type_name, int16_t max_length,
											const string &collation_name, bool convert_varchar_max,
											bool client_transcode, const string &qualifier, const string &alias,
											bool utf8_on_wire) {
	const string escaped_name = mssql::QuoteIdentifier(alias.empty() ? col_name : alias);
	const string reference = qualifier + mssql::QuoteIdentifier(col_name);

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

	// Issue #354, until sql_variant is decoded on the wire: each value is converted by the style
	// that keeps its base type whole. The plain cast is style 0, which kept the minute of a
	// datetime2 (`Sep 16 2026 10:11AM`), six digits of a float, two decimals of a money, and read
	// a varbinary's bytes as UTF-16. Measured per base type on SQL Server 2025: 3 gives a float
	// or a real its 17 digits, 2 a money its four decimals, 126 the date/time family ISO 8601
	// with every fraction digit, 1 a binary its 0x hex; the rest are exact under style 0. One
	// style cannot serve a column whose type lives in each value (126 refuses a varbinary, 9809).
	// The styles hold on SQL Server 2025 (local), 2022 (the CI lane) and 2019 (15.0.4500,
	// sql_variant_read.test run against it; time(0/3/7) and datetimeoffset by 126 alike).
	// Cost (review of #542, 1M rows of int / float / datetime2 / nvarchar, server under Rosetta):
	// 7.7 s of server CPU against 0.87 s for the plain cast. Each comparison of the base type
	// calls SQL_VARIANT_PROPERTY again (an IN of n names is n calls, and the server does not
	// share them), so a value of a type no WHEN names pays twelve. Six other forms were measured
	// (no cast, a BIN2 compare, a derived table, a CHARINDEX position, a two-level tree, a single
	// guard): none was cheaper than 5%. Decoding sql_variant on the wire (#515) removes the
	// CASE; until then the cost is a sql_variant column's only, and the plain cast lost data.
	if (StringUtil::Lower(sql_type_name) == "sql_variant") {
		const string base_type = "CAST(SQL_VARIANT_PROPERTY(" + reference + ", 'BaseType') AS NVARCHAR(128))";
		const auto styled = [&](const char *style) {
			return "CONVERT(NVARCHAR(MAX), " + reference + ", " + style + ")";
		};
		return "CASE WHEN " + base_type + " IN (N'float', N'real') THEN " + styled("3") + " WHEN " + base_type +
			   " IN (N'money', N'smallmoney') THEN " + styled("2") + " WHEN " + base_type +
			   " IN (N'date', N'time', N'datetime', N'datetime2', N'smalldatetime', N'datetimeoffset') THEN " +
			   styled("126") + " WHEN " + base_type + " IN (N'binary', N'varbinary') THEN " + styled("1") +
			   " ELSE CAST(" + reference + " AS NVARCHAR(MAX)) END AS " + escaped_name;
	}

	// Unsupported SQL Server types (hierarchyid, CLR UDTs) must be CAST to
	// NVARCHAR(MAX) so the server sends text instead of a native wire form
	// nothing decodes.
	if (ReadFormFor(sql_type_name) == MSSQLReadForm::ServerCast) {
		return "CAST(" + reference + " AS NVARCHAR(MAX)) AS " + escaped_name;
	}

	// Issue #405: a char(n) is stored padded with spaces, which SQL Server
	// ignores in a comparison and the codec trims off a fixed-length wire type
	// (BIGCHAR, NCHAR). On a variable-length one -- the NVARCHAR cast of a
	// code-page char, or a UTF-8 char the server transcodes for a connection
	// without UTF8SUPPORT -- the padding reached DuckDB, and a join, GROUP BY or
	// DISTINCT there no longer agreed with the server ('AB  ' of a char(4)
	// against 'AB    ' of a char(6): 0 rows joined, 2 on the server). So it is
	// read as NCHAR(n), whose n characters hold the column's n bytes in any code
	// page or in UTF-8, or, past NCHAR's 4000, trimmed by the server. One the client
	// transcodes is read as itself, BIGCHAR, and the codec trims the output (spec
	// 092 D3).
	if (StringUtil::Lower(sql_type_name) == "char" && (!IsUTF8Collation(collation_name) || !utf8_on_wire) &&
		!ClientTranscodedPage(sql_type_name, collation_name, client_transcode)) {
		if (max_length > 0 && max_length <= 4000) {
			return "CAST(" + reference + " AS NCHAR(" + std::to_string(max_length) + ")) AS " + escaped_name;
		}
		return "RTRIM(CAST(" + reference + " AS NVARCHAR(MAX))) AS " + escaped_name;
	}

	if (NeedsNVarcharConversion(sql_type_name, max_length, collation_name, convert_varchar_max, client_transcode)) {
		return "CAST(" + reference + " AS NVARCHAR(" + NVarcharLength(sql_type_name, max_length) + ")) AS " +
			   escaped_name;
	}

	// Nothing to rewrite. Unqualified this is just the column; qualified it needs
	// the alias, or the result would come back named after the qualifier's table.
	return qualifier.empty() && alias.empty() ? escaped_name : reference + " AS " + escaped_name;
}

}  // namespace duckdb
