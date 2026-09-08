#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include "tds_types.hpp"

namespace duckdb {
namespace tds {

//===----------------------------------------------------------------------===//
// ColumnMetadata - Describes a single result column from COLMETADATA token
//===----------------------------------------------------------------------===//

struct ColumnMetadata {
	std::string name;	  // Column name (UTF-8)
	uint8_t type_id;	  // TDS type identifier
	uint16_t max_length;  // Maximum length for variable types
	uint8_t precision;	  // Precision for DECIMAL/NUMERIC
	uint8_t scale;		  // Scale for DECIMAL/NUMERIC or TIME
	uint32_t collation;	  // Collation LCID + flags: the first 4 of the 5 wire bytes
	// The 5th collation byte, previously parsed and thrown away. It is the
	// SortId, and for the SQL_* collations it is what names the CODE PAGE --
	// the first four carry LCID and flags only, so without it a message about
	// a collation cannot say which one it met (issue #224, from @oluies' #305).
	uint8_t collation_sort_id;
	uint16_t flags;	 // Column flags (nullable, identity, etc.)

	// Derived properties
	bool IsNullable() const {
		return (flags & COL_FLAG_NULLABLE) != 0;
	}
	bool IsIdentity() const {
		return (flags & COL_FLAG_IDENTITY) != 0;
	}
	bool IsComputed() const {
		return (flags & COL_FLAG_COMPUTED) != 0;
	}

	//! True when this column's collation names a UTF-8 code page, so its bytes
	//! are already what a DuckDB VARCHAR requires.
	//!
	//! MS-TDS 2.2.5.1.2 packs the collation as 20 bits of LCID, 8 bits of
	//! flags, 4 bits of version, then the SortId byte. `collation` holds the
	//! first four bytes, so the flags occupy bits 20-27 and fUTF8 -- 0x40
	//! within them -- lands at 0x04000000 in the word.
	//!
	//! Verified against a live SQL Server 2025 rather than read off the spec,
	//! by dumping the COLMETADATA bytes for known collations:
	//!
	//!     Latin1_General_100_CI_AS_SC_UTF8   0x24D00409   set
	//!     Latin1_General_100_BIN2_UTF8       0x26000409   set
	//!     Latin1_General_CI_AS      (cp1252) 0x00D00409   clear
	//!     Cyrillic_General_CI_AS    (cp1251) 0x00D00419   clear
	//!
	//! Each decodes without remainder (CI -> fIgnoreCase, AS -> fIgnoreAccent
	//! clear, BIN2 -> fBinary2), and it degrades correctly on older servers:
	//! UTF-8 collations are SQL Server 2019+, so the bit is never set before
	//! that. Note that the installation default SQL_Latin1_General_CP1_CI_AS is
	//! CP1252 -- a legacy code page is the majority case, not an edge case.
	bool IsUtf8Collation() const {
		return (collation & 0x04000000u) != 0;
	}

	//! True for the single-byte text types: CHAR, VARCHAR and the deprecated
	//! TEXT. Their bytes are copied to the client verbatim and land in a DuckDB
	//! VARCHAR, which is UTF-8 by contract -- so these are the columns whose
	//! collation decides whether that contract holds. NCHAR / NVARCHAR / NTEXT
	//! are UTF-16 on the wire and always transcoded; BINARY / VARBINARY / IMAGE
	//! are copied verbatim too but land in a BLOB, where arbitrary bytes are
	//! the point.
	bool IsSingleByteTextColumn() const {
		return type_id == TDS_TYPE_BIGCHAR || type_id == TDS_TYPE_BIGVARCHAR || type_id == TDS_TYPE_TEXT;
	}

	// Get human-readable type name for error messages
	std::string GetTypeName() const;

	// Check if this is a variable-length type
	bool IsVariableLength() const;

	// Check if this is a nullable variant (INTN, FLOATN, etc.)
	bool IsNullableVariant() const;

	// Check if this is a PLP (Partially Length-Prefixed) type (MAX types)
	// MAX types have max_length == 0xFFFF and use chunked encoding
	bool IsPLPType() const;

	// Get the fixed size for fixed-length types (0 for variable)
	size_t GetFixedSize() const;
};

//===----------------------------------------------------------------------===//
// ColumnMetadataParser - Parse COLMETADATA token from TDS stream
//===----------------------------------------------------------------------===//

class ColumnMetadataParser {
public:
	// Parse COLMETADATA token and return column definitions
	// Returns true if parsing succeeded, false if more data needed
	// Throws on parse error
	static bool Parse(const uint8_t *data, size_t length, size_t &bytes_consumed, std::vector<ColumnMetadata> &columns);

private:
	// Parse a single column definition
	static bool ParseColumn(const uint8_t *data, size_t length, size_t &offset, ColumnMetadata &column);

	// Parse type-specific metadata (length, precision, scale, collation)
	static bool ParseTypeInfo(const uint8_t *data, size_t length, size_t &offset, ColumnMetadata &column);

	// Parse B_VARCHAR column name
	static bool ParseColumnName(const uint8_t *data, size_t length, size_t &offset, std::string &name);
};

// Spec 058 D1-alt. The skip walk's job per value is tiny — read a length
// prefix, bounds-check, advance — but SkipValue selects it with a ~30-case
// switch on type_id PER VALUE. The wire has only four framing shapes, and
// which one a column uses is fixed at COLMETADATA, so the parser resolves each
// column to a form ONCE and the walk dispatches on a 2-bit form from a
// contiguous array instead. Measured before the change: the whole walk is
// 1.7–2.1 ns/value, ~13% of the entire client-side read of a cheap type
// (spec 058 §3a/T0e).
//
// SLOW keeps the legacy SkipValue/SkipValueNBC as the one implementation of
// the hard forms: PLP chunk lists, the TEXT/NTEXT/IMAGE text-pointer form and
// its MAX_LOB guard, and unknown types. Lives here (not in RowReader) so the
// parser can hold the resolved array without a header cycle — RowData lives in
// the parser's header, which RowReader needs.
enum class SkipForm : uint8_t { FIXED = 0, PREFIX1 = 1, PREFIX2 = 2, SLOW = 3 };

struct SkipDesc {
	SkipForm form;
	uint8_t width;	// FIXED: payload bytes; other forms: unused
};

//! Defined in tds_row_reader.cpp beside the switch it mirrors.
SkipDesc ResolveSkipForm(const ColumnMetadata &col);

}  // namespace tds
}  // namespace duckdb
