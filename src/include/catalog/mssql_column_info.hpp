#pragma once

#include <string>
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/value.hpp"

namespace duckdb {

namespace mssql {
namespace codec {
namespace code_page {
namespace tables {
struct CodePageTable;
}  // namespace tables
}  // namespace code_page
}  // namespace codec
}  // namespace mssql

//! How a `datetime2(7)` column is read (spec 093 D8, DQ-44): `mssql_datetime2_7_as` or the
//! connection's `datetime2_7_as` (ATTACH option, secret, connection string, URI), fixed per catalog at ATTACH.
//! Nanoseconds: TIMESTAMP_NS, every 100 ns tick, a value outside 1677-2262 NULL. Microseconds: TIMESTAMP over the whole
//! 0001-9999 range, the 100 ns digit truncated -- read, compared and keyed as `time(7)` / `datetimeoffset(7)` are.
enum class MSSQLDatetime2Read : uint8_t { Nanoseconds, Microseconds };

//! `'timestamp_ns'` / `'timestamp'` (any case, trimmed); an InvalidInputException naming `source` for
//! anything else.
MSSQLDatetime2Read ParseDatetime2Read(const string &value, const string &source);

//===----------------------------------------------------------------------===//
// MSSQLColumnInfo - Column metadata including SQL Server-specific information
//===----------------------------------------------------------------------===//

struct MSSQLColumnInfo {
	// Basic column info
	string name;			  // Column name
	int32_t column_id;		  // Ordinal position (1-based)
	string sql_type_name;	  // SQL Server type name (e.g., "varchar", "int")
	LogicalType duckdb_type;  // Mapped DuckDB type

	// Size/precision info
	int16_t max_length;	 // Max length in bytes (-1 for MAX types)
	uint8_t precision;	 // Numeric precision
	uint8_t scale;		 // Numeric scale

	// Nullability
	bool is_nullable;  // Allows NULL values

	// Collation info (for text types)
	string collation_name;			 // Column collation (may be empty for non-text)
	bool is_case_sensitive;			 // Derived from collation (_CS_ or _BIN)
	bool is_unicode;				 // True for NVARCHAR/NCHAR/NTEXT
	bool is_utf8;					 // Derived from collation (_UTF8)
	int32_t code_page = 0;			 // COLLATIONPROPERTY(collation, 'CodePage'): the page the column stores
									 // varchar in; 0 = the server has none (non-text, Unicode-only). Set by
									 // the metadata loaders after construction, like is_identity (issue #361)
	int32_t database_code_page = 0;	 // The same for the database collation — a varchar PARAMETER takes
									 // this page, so a constant must fit both (mssql::CodePageCanEncode)
	bool is_cast_required;			 // Unsupported type: needs CAST to NVARCHAR(MAX)
	bool is_geometry;				 // True for SQL Server geometry/geography columns; table scan projects
									 // [col].STAsBinary() AS [col] so the wire delivers OGC WKB which lands
									 // in a LogicalType::GEOMETRY() vector via the Binary codec.
	bool is_identity;				 // sys.columns.is_identity (spec 062 W4, issue #327). Set by the
									 // metadata loaders after construction; the INSERT planner keeps a
									 // column list that names one on the statement path, where the server
									 // decides about the explicit value (error 544 without IDENTITY_INSERT).

	// Default constructor
	MSSQLColumnInfo();

	// Constructor from SQL Server metadata. `datetime2_read`: the catalog's answer (no default: a
	// datetime2(7) column's type follows it).
	MSSQLColumnInfo(const string &name, int32_t column_id, const string &sql_type_name, int16_t max_length,
					uint8_t precision, uint8_t scale, bool is_nullable, const string &collation_name,
					const string &database_collation, MSSQLDatetime2Read datetime2_read);

	//! A datetime2 column read with its 100 ns digit truncated (MSSQLDatetime2Read::Microseconds at scale 7):
	//! not one-to-one, so compared and keyed as time(7) is. Asked of the column's read type.
	bool ReadsDatetime2Truncated() const;

	// Parse collation name to extract sensitivity flags
	static bool IsCaseSensitiveCollation(const string &collation_name);
	static bool IsAccentSensitiveCollation(const string &collation_name);
	static bool IsUTF8Collation(const string &collation_name);
	//! A `_BIN2` collation: pure code-unit comparison. Not `_BIN`, which compares
	//! the first character by code point and the rest byte by byte -- little-
	//! endian on nvarchar, so not code-unit order at all.
	static bool IsBinary2Collation(const string &collation_name);

	//! Whether the SERVER, ordering by this column, gives the order DuckDB gives
	//! the values it reads (issue #362, spec 079 D4 "orders are DuckDB's"). THE
	//! predicate MSSQLOptimizer's ORDER BY / TOP pushdown asks; spec 079's
	//! remote-pushdown writer is to ask the same one.
	//!
	//! An allow-list: a type is on it because both sides order its values the
	//! same way, and anything not named -- a type added to SQL Server later
	//! included -- is refused. On it: the integer types, bit, decimal/numeric,
	//! money/smallmoney, float/real, and the date/time types (datetimeoffset by
	//! its UTC instant on both sides) -- except datetime2(7), which DuckDB reads
	//! as TIMESTAMP_NS and whose values outside 1677-2262 it reads as NULL.
	//!
	//! No string is on it. Refused, among others: `nvarchar`/`nchar` under any
	//! collation (UTF-16 code units put a character above the BMP before
	//! U+E000-U+FFFF, DuckDB after), `varchar`/`char` ordered as text (even
	//! under `_BIN2` UTF-8 the server pads with spaces, so `ab` = `ab ` and `ab`
	//! + TAB sorts before `ab` -- see OrdersLikeDuckDBAsBytes for how such a
	//! column is pushed), `uniqueidentifier` (the server compares its last six
	//! bytes first), `binary`/`varbinary` as a column type, `json`,
	//! `sql_variant` and every cast-required type, and the types the server
	//! cannot sort (text, ntext, image, xml, geometry, geography).
	bool OrdersLikeDuckDB() const;
	//! A bounded `varchar` whose bytes are UTF-8 (a UTF-8 collation, any
	//! sensitivity): ordered by `CAST(col AS varbinary(n))` the server compares
	//! those bytes, which is code-point order, DuckDB's -- measured: `aa, ab,
	//! ab`+TAB`, ab ` on both sides. The one residual is a trailing NUL: the
	//! binary comparison pads with 0x00, so `ab` and `ab`+NUL tie. The key is not
	//! sargable, so the optimizer uses it only under a LIMIT (review of #387).
	//! Not `char(n)`: it is stored blank-padded and read trimmed, so its bytes
	//! are not the values DuckDB orders (`ab` + TAB is stored `ab\t ` and sorts
	//! before `ab  `). Not `varchar(max)`: a sort on an unbounded key.
	bool OrdersLikeDuckDBAsBytes() const;

	// Map SQL Server type to DuckDB LogicalType
	//! Spec 060: the type to REPORT for this column — MSSQL_VARCHAR(n) /
	//! MSSQL_NVARCHAR(n) where the column is a bounded character one, and plain
	//! `duckdb_type` otherwise. Values are ordinary DuckDB strings either way;
	//! the annotation exists so a target created from this column inherits its
	//! declared type instead of becoming nvarchar(max). Gated by
	//! mssql_catalog_native_types at the point the table entry is built.
	LogicalType NativeDuckDBType() const;

	static LogicalType MapSQLServerTypeToDuckDB(const string &sql_type_name, int16_t max_length, uint8_t precision,
												uint8_t scale, MSSQLDatetime2Read datetime2_read);

	// The two spatial CLR UDTs, by name. ONE predicate, because four things key
	// on the same pair and they have to agree: the catalog sets `is_geometry`
	// from it, the type map answers GEOMETRY() from it, `IsKnownSQLServerType`
	// admits them from it, and the DML literal renderer decides from it whether
	// to wrap a value in the server's WKB reader. If the routing and the
	// rendering ever disagreed the failure would be silent in one direction
	// (WKB sent as text on the bulk wire) and a server error in the other.
	static bool IsSpatialType(const string &sql_type_name);

	// Check if SQL Server type is natively supported (has explicit mapping or TDS-level support)
	static bool IsKnownSQLServerType(const string &sql_type_name);

	// Check if type is a text type that has collation
	static bool IsTextType(const string &sql_type_name);

	// Check if type is Unicode (NVARCHAR, NCHAR, NTEXT)
	static bool IsUnicodeType(const string &sql_type_name);

	//! The T-SQL expression that reads this column so its bytes are decodable.
	//!
	//! Four rewrites, and the point of having ONE function for them is that two
	//! lists need them and used to carry different subsets. The table scan's
	//! SELECT list is one; the other is the `OUTPUT INSERTED` list of an
	//! `INSERT … RETURNING`, which carries every column of the table because
	//! DuckDB's RETURNING projection sits above the operator and expects the
	//! table's full width — so one undecodable column fails a statement that
	//! never mentioned it. When OUTPUT copied only two of the four, a `text`
	//! value still failed the whole INSERT with `Unsupported type in RowReader:
	//! TEXT` while the very same column read fine through a SELECT.
	//!
	//!   - spatial UDTs -> `.STAsBinary()`, so the wire carries OGC WKB rather
	//!     than SQL Server's own Spatial Type Binary Format;
	//!   - `ntext` -> `NVARCHAR(MAX)`, `image` -> `VARBINARY(MAX)`: the per-value
	//!     decoders have no arm for their wire tokens;
	//!   - a type with no decodable wire form at all (sql_variant, hierarchyid,
	//!     a CLR UDT) -> `NVARCHAR(MAX)`;
	//!   - a non-UTF-8 CHAR/VARCHAR/TEXT -> `NVARCHAR(n)`, because a DuckDB
	//!     VARCHAR is UTF-8 by contract, unless the client transcodes it
	//!     (`client_transcode` below).
	//!
	//! Every flag it needs is a pure function of the type name and the
	//! collation, which is why it takes those rather than an MSSQLColumnInfo:
	//! the INSERT path holds an MSSQLInsertColumn and would otherwise have to
	//! grow a copy of this logic, which is exactly what went wrong.
	//!
	//! @param client_transcode the catalog's mssql_client_transcode (spec 092 D4):
	//!                  a char / varchar under a single-byte page is then read as
	//!                  itself and transcoded on the client (ClientTranscodedPage).
	//!                  No default: a caller must not keep the cast by forgetting it.
	//! @param qualifier prefix for the column REFERENCE, e.g. `"INSERTED."`.
	//!                  The alias is never qualified, so the result always comes
	//!                  back under the column's own name.
	//! @param alias    the name the column comes back under; empty = its own.
	//!                  Spec 079's writer passes a SELECT-list alias.
	//! @param utf8_on_wire the connection was granted UTF8SUPPORT, so a UTF-8
	//!                  char(n) arrives as itself (BIGCHAR) and is trimmed by
	//!                  the codec. False -- the default, right on any
	//!                  connection -- reads it as NCHAR(n) (issue #405).
	static string BuildReadExpression(const string &col_name, const string &sql_type_name, int16_t max_length,
									  const string &collation_name, bool convert_varchar_max, bool client_transcode,
									  const string &qualifier = "", const string &alias = "",
									  bool utf8_on_wire = false);

	//! The code page a column's bare read is transcoded from on the client, or
	//! nullptr when the read expression keeps the server's cast (spec 092 D1, D10):
	//! the catalog does not transcode, the type is not char / varchar (`text` keeps
	//! its cast, D1), the collation names no table, or the page is double-byte. A
	//! pure function of the type, the collation name and the flag, so a caller
	//! holding an MSSQLInsertColumn needs no page of its own (D5).
	static const mssql::codec::code_page::tables::CodePageTable *ClientTranscodedPage(const string &sql_type_name,
																					  const string &collation_name,
																					  bool client_transcode);

	//! A RETURNING column, as `OUTPUT … INTO <table>` writes it and as the
	//! `SELECT … INTO` that creates that table types it. It is the read
	//! expression, with two exceptions:
	//! - a `rowversion` is cast to `binary(8)`: an OUTPUT cannot write into a
	//!   `timestamp` column, and SELECT INTO would make one;
	//! - a `char(n)` read as NCHAR(n) is a trimmed NVARCHAR instead, because
	//!   NCHAR(n) doubles its fixed width past the 8060-byte row. One read bare
	//!   (spec 092) has the source's own width and comes through as it is.
	//! Shared by INSERT … RETURNING (#471) and the staged UPDATE / DELETE …
	//! RETURNING (spec 080 PR 2b).
	static string BuildReturningExpression(const string &col_name, const string &sql_type_name, int16_t max_length,
										   const string &collation_name, bool convert_varchar_max,
										   bool client_transcode, const string &qualifier, const string &alias = "");
};

}  // namespace duckdb
