//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension — spec 054
//
// codec/vector_format.hpp
//
// W1 (spec 054 D8): shared accessors for reading vector values through a
// PRE-COMPUTED UnifiedVectorFormat. The format is built once per column per
// chunk by BCPRowEncoder::EncodeChunk and threaded into every family
// codec's EncodeToBcp — replacing the per-cell ToUnifiedFormat
// reconstruction each family used to carry (the deleted GetVectorValue /
// GetVectorBool / IsVectorNull helpers).
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/types/vector.hpp"

namespace duckdb {
namespace mssql {

struct BCPColumnMetadata;

namespace codec {

template <typename T>
inline T FormatValue(const UnifiedVectorFormat &fmt, idx_t row) {
	return UnifiedVectorFormat::GetData<T>(fmt)[fmt.sel->get_index(row)];
}

inline bool FormatIsNull(const UnifiedVectorFormat &fmt, idx_t row) {
	return !fmt.validity.RowIsValid(fmt.sel->get_index(row));
}

// Signature shared by every family's fmt-threaded EncodeToBcp overload.
using BcpEncodeFmtFn = void (*)(Vector &, const UnifiedVectorFormat &, idx_t, const BCPColumnMetadata &,
								duckdb::vector<uint8_t> &);

// Per-row compat shim behind every family's EncodeToBcp(Vector&, idx_t, ...)
// wrapper (unit-test API; production goes through BCPRowEncoder::EncodeChunk).
// Keeps the format-construction protocol — the `row + 1` count — in ONE place
// so no family can silently drift from the fmt-overload contract.
inline void EncodeToBcpViaFormat(BcpEncodeFmtFn encode, Vector &in, idx_t row, const BCPColumnMetadata &col,
								 duckdb::vector<uint8_t> &buf) {
	UnifiedVectorFormat fmt;
	in.ToUnifiedFormat(row + 1, fmt);
	encode(in, fmt, row, col, buf);
}

}  // namespace codec
}  // namespace mssql
}  // namespace duckdb
