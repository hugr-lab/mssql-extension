#pragma once

#include <cstdint>
#include <vector>
#include "tds_column_metadata.hpp"
#include "tds_token_parser.hpp"
#include "tds_types.hpp"

namespace duckdb {
namespace tds {

//===----------------------------------------------------------------------===//
// RowReader - Extracts typed values from ROW token data
//===----------------------------------------------------------------------===//

class RowReader {
public:
	explicit RowReader(const std::vector<ColumnMetadata> &columns);
	~RowReader() = default;

	// Read a complete row from the data buffer
	// Returns true if row was read, false if more data needed
	// Throws on parse error
	bool ReadRow(const uint8_t *data, size_t length, size_t &bytes_consumed, RowData &row);

	// Read a Null Bitmap Compressed (NBC) row from the data buffer
	// NBC rows have a bitmap indicating NULL columns, followed by data for non-NULL columns
	bool ReadNBCRow(const uint8_t *data, size_t length, size_t &bytes_consumed, RowData &row);

	// Skip a row without parsing (fast path for drain)
	// Returns true if row was skipped, false if more data needed
	bool SkipRow(const uint8_t *data, size_t length, size_t &bytes_consumed);

	// Skip a NBC row without parsing (fast path for drain)
	bool SkipNBCRow(const uint8_t *data, size_t length, size_t &bytes_consumed);

	// Single-value entry points. Public because the staged read path
	// (codec::staging::RowStager, spec 055) walks a row column by column and
	// falls back to these for families that do not yet have a batch kernel —
	// reusing the framing logic rather than duplicating it, which is what keeps
	// the two paths byte-identical.
	// All return bytes consumed, or 0 if more data is needed.
	size_t SkipValue(const uint8_t *data, size_t length, size_t col_idx);
	size_t ReadValue(const uint8_t *data, size_t length, size_t col_idx, std::vector<uint8_t> &value, bool &is_null);

	// NBC variants - for non-NULL columns in NBC rows (no length prefix for nullable types)
	size_t SkipValueNBC(const uint8_t *data, size_t length, size_t col_idx);
	size_t ReadValueNBC(const uint8_t *data, size_t length, size_t col_idx, std::vector<uint8_t> &value, bool &is_null);

	// Spec 058 D1-alt: hand this reader the per-column skip forms the parser
	// resolved at COLMETADATA (see SkipForm in tds_column_metadata.hpp). The
	// skip walks then dispatch on the 2-bit form instead of the per-value
	// type_id switch; SLOW columns keep the legacy path. NBC uses the SAME
	// forms — non-NULL values keep their normal framing, and NULL columns
	// carry no bytes so the walk never reaches the descriptor.
	//! `descs` must outlive this reader and hold one entry per column, in
	//! column order (the parser owns it, recomputed per COLMETADATA).
	void SetSkipDescriptors(const SkipDesc *descs) {
		skip_descs_ = descs;
	}

private:
	const SkipDesc *skip_descs_ = nullptr;

	// Type-specific readers
	size_t ReadFixedType(const uint8_t *data, size_t length, uint8_t type_id, std::vector<uint8_t> &value);

	size_t ReadNullableFixedType(const uint8_t *data, size_t length, uint8_t type_id, uint8_t declared_length,
								 std::vector<uint8_t> &value, bool &is_null);

	size_t ReadVariableLengthType(const uint8_t *data, size_t length, uint8_t type_id, std::vector<uint8_t> &value,
								  bool &is_null);

	size_t ReadDecimalType(const uint8_t *data, size_t length, std::vector<uint8_t> &value, bool &is_null);

	size_t ReadDateType(const uint8_t *data, size_t length, std::vector<uint8_t> &value, bool &is_null);

	size_t ReadTimeType(const uint8_t *data, size_t length, uint8_t scale, std::vector<uint8_t> &value, bool &is_null);

	size_t ReadDateTime2Type(const uint8_t *data, size_t length, uint8_t scale, std::vector<uint8_t> &value,
							 bool &is_null);

	size_t ReadDateTimeOffsetType(const uint8_t *data, size_t length, uint8_t scale, std::vector<uint8_t> &value,
								  bool &is_null);

	size_t ReadGuidType(const uint8_t *data, size_t length, std::vector<uint8_t> &value, bool &is_null);

	// PLP (Partially Length-Prefixed) type reader for MAX types
	// MAX types use: 8-byte total length + chunks (4-byte length + data) + terminator (4-byte 0)
	size_t ReadPLPType(const uint8_t *data, size_t length, std::vector<uint8_t> &value, bool &is_null);

	// PLP type skipper for MAX types
	size_t SkipPLPType(const uint8_t *data, size_t length);

	const std::vector<ColumnMetadata> &columns_;
};

}  // namespace tds
}  // namespace duckdb
