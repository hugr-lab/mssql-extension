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
namespace codec {

template <typename T>
inline T FormatValue(const UnifiedVectorFormat &fmt, idx_t row) {
	return UnifiedVectorFormat::GetData<T>(fmt)[fmt.sel->get_index(row)];
}

inline bool FormatIsNull(const UnifiedVectorFormat &fmt, idx_t row) {
	return !fmt.validity.RowIsValid(fmt.sel->get_index(row));
}

}  // namespace codec
}  // namespace mssql
}  // namespace duckdb
