//===----------------------------------------------------------------------===//
// utf8_guard.hpp - shared UTF-8 validation for single-byte text columns
//
// DuckDB VARCHAR is UTF-8 by contract. SQL Server's single-byte text types
// (CHAR / VARCHAR / TEXT) carry bytes in the COLUMN'S CODE PAGE, and the TDS
// type token does not say which -- with UTF8SUPPORT negotiated a UTF-8
// collated column and a CP1252 one arrive as the same type. Publishing the
// latter unchecked produced an invalid string that every downstream function
// then mangled: issue #224, where upper() on a scanned 'naive' with a
// diaeresis returned "NA".
//
// Two decoders publish these bytes -- binary::DecodeChunkFromStaging for the
// staged scan path, string::DecodeFromTds for the per-value one -- so the
// check and its message live here rather than being written twice.
//===----------------------------------------------------------------------===//
#pragma once

#include "duckdb/common/exception.hpp"
#include "tds/tds_column_metadata.hpp"

#include <cstdint>
#include <cstring>

namespace duckdb {
namespace mssql {
namespace codec {

//! Is every byte ASCII? Inlined deliberately: it replaces a simdutf
//! validate_utf8 call, and the point is that a CALL costs ~3.1 ns on a short
//! value while the byte test costs a fraction of that. Eight bytes at a time
//! through a uint64 mask -- the same trick simdutf uses at its tail, minus the
//! dispatch.
//!
//! For a staged payload this is the ONLY sound whole-buffer fast path. Testing
//! the concatenation with validate_utf8 is not: values are packed contiguously
//! with no separator, so a lead byte ending one value and continuation bytes
//! starting the next form a valid sequence ACROSS the boundary. CP1252 'A-tilde'
//! (0xC3) followed by a value starting 0xA9 stages as C3 A9 ... , which
//! validate_utf8 accepts although neither value is valid alone. All-ASCII has
//! no such failure mode: every byte is a complete character, so an all-ASCII
//! payload means every value in it is valid.
inline bool IsAsciiRun(const char *data, size_t size) {
	size_t i = 0;
	for (; i + 8 <= size; i += 8) {
		uint64_t w;
		std::memcpy(&w, data + i, 8);
		if (w & 0x8080808080808080ULL) {
			return false;
		}
	}
	for (; i < size; i++) {
		if (static_cast<uint8_t>(data[i]) & 0x80) {
			return false;
		}
	}
	return true;
}

//! Does this column publish single-byte text into a VARCHAR vector?
//!
//! IMAGE shares the staging arm with TEXT but lands in a BLOB, where arbitrary
//! bytes are the point, so it is excluded -- as are the N-types, whose bytes
//! are UTF-16 and decoded rather than copied.
inline bool IsSingleByteTextColumn(const tds::ColumnMetadata &col) {
	return col.type_id == tds::TDS_TYPE_BIGCHAR || col.type_id == tds::TDS_TYPE_BIGVARCHAR ||
		   col.type_id == tds::TDS_TYPE_TEXT;
}

//! Raise the #224 error, naming the column, its collation and the way out.
//!
//! `chunk_row` is the index within the CURRENT CHUNK, not the result set, and
//! says so: a stream reports row N of whichever chunk failed, and a reader
//! chasing row N of the result would look in the wrong place. Pass SIZE_MAX
//! when there is no row to name.
[[noreturn]] inline void ThrowNonUtf8Column(const tds::ColumnMetadata &col, size_t chunk_row) {
	// NVARCHAR(MAX), not NVARCHAR(4000): SQL Server does not raise on a
	// narrowing character CAST, so suggesting a fixed width would silently
	// truncate a varchar(8000) or varchar(max). The extension's own catalog
	// path uses MAX for exactly those widths (GetNVarcharLength).
	const std::string where = chunk_row == SIZE_MAX
								  ? std::string()
								  : StringUtil::Format("; first bad value at row %llu of the current chunk",
													   static_cast<unsigned long long>(chunk_row));
	throw InvalidInputException(
		"Column \"%s\" is single-byte text whose bytes are not valid UTF-8 (collation 0x%08X, sort id %u%s). "
		"DuckDB VARCHAR must be UTF-8, and this extension does not transcode legacy code pages. Either CAST it "
		"in the query -- CAST(\"%s\" AS NVARCHAR(MAX)) -- or read the table through the attached catalog, which "
		"casts non-Unicode string columns server-side (except a declared VARCHAR(MAX) when "
		"mssql_convert_varchar_max is off).",
		col.name, col.collation, static_cast<uint32_t>(col.collation_sort_id), where, col.name);
}

}  // namespace codec
}  // namespace mssql
}  // namespace duckdb
