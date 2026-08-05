//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// codec/scatter_position.hpp
//
// Where a scatter kernel writes row `r`, as a policy rather than as a parameter
// (spec 064 D1).
//
// WHY THIS EXISTS. The batch kernels took `size_t stride` and computed
// `dst + r * stride`. That is a scalar, so "constant stride" was baked into the
// signature — and there are two layouts, not one:
//
//   * strided — every row the same length, so `r * stride` is the position;
//   * cursor  — rows differ in length because SOME column in the chunk is
//               variable-width or carries a NULL, so positions come from a
//               per-row array that was computed up front.
//
// With only a scalar to pass, the cursor path could not express itself and
// called the kernel once PER ROW with `rows = 1, stride = 0`. Not because the
// data needed it — decimal, uuid and datetime all have a fixed width per column,
// and the per-row offsets were already known — but because the API had nowhere
// to put them. 2048 un-inlinable calls across a translation-unit boundary where
// one would do, measured at +0.23 s CPU per 16M values against a `bigint`
// control that costs nothing (its arm is a template in the calling TU).
//
// Both policies are empty or near-empty structs and every method is inline, so
// the kernel instantiates to the same code it had — the strided form still
// computes `dst + r * stride` with nothing dispatched at run time.
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/types.hpp"

namespace duckdb {
namespace mssql {
namespace codec {

//! Rows at a constant stride: the layout when every column is fixed-width and
//! nothing is NULL.
struct StridePos {
	uint8_t *dst;
	size_t stride;

	//! Where row `r` of this block writes. `r` is block-relative.
	inline uint8_t *At(idx_t r) const {
		return dst + r * stride;
	}
	//! No-op: the next row's position is implied by the stride, so nothing has
	//! to be recorded.
	inline void Advance(idx_t, size_t) const {
	}
};

//! Rows at positions held in a per-row cursor: the layout when the chunk has a
//! variable-width column or a NULL anywhere.
//!
//! The cursor is indexed by ABSOLUTE row, while `At`/`Advance` take a
//! block-relative one, so `row_begin` is carried here rather than added by every
//! caller.
struct CursorPos {
	uint8_t *dst;
	size_t *cursor;
	idx_t row_begin;

	inline uint8_t *At(idx_t r) const {
		return dst + cursor[row_begin + r];
	}
	//! Record how many bytes row `r` consumed, so the NEXT column of the same
	//! row starts after them. This is what the caller used to do around each
	//! per-value call.
	inline void Advance(idx_t r, size_t bytes) const {
		cursor[row_begin + r] += bytes;
	}
};

}  // namespace codec
}  // namespace mssql
}  // namespace duckdb
