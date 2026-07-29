//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// codec/staging/row_stager.hpp
//
// The staged read path's row walk (spec 055 T5).
//
// WHAT CHANGES
// ------------
// The shipped path decomposes every ROW token into `tds::RowData` — a
// vector<vector<uint8_t>>, one heap buffer per column — and then walks that a
// second time, dispatching on the TDS type for every single cell. Two passes
// over the same bytes and a type switch in each.
//
// Here the row is walked ONCE. Per-column dispatch was resolved after
// COLMETADATA (`ResolveColumnOps`), so the walk carries one switch over a
// column-invariant arm, and a 1:1 type lands straight in the output vector with
// no intermediate buffer at all.
//
// WHY THE WALK NEEDS NO BOUNDS CHECKS
// -----------------------------------
// The parser hands rows up in raw-row mode, which returns a row only once
// SkipRow has established its exact length — so every byte of it is present.
// The walk therefore needs no "do I have enough data" test per value, and no
// rollback for a row that straddles a receive boundary. That is the single
// biggest reason this is a separate mode rather than a flag inside RowReader.
//===----------------------------------------------------------------------===//

#pragma once

#include "codec/staging/column_ops.hpp"
#include "codec/staging/column_staging.hpp"
#include "duckdb/common/types/vector.hpp"
#include "tds/tds_column_metadata.hpp"
#include "tds/tds_row_reader.hpp"

#include <vector>

namespace duckdb {
namespace mssql {
namespace codec {
namespace staging {

//! Walks TDS row bytes into DuckDB vectors, column-major where it pays.
//!
//! Owned by MSSQLResultStream, one per stream, reused across chunks and result
//! sets.
class RowStager {
public:
	RowStager() = default;

	//! Resolve per-column dispatch for a result set. `targets[i] == nullptr`
	//! marks a column that is parsed for its length but discarded.
	//!
	//! `metadata` must outlive this stager: the walk reads column metadata on the
	//! Convert arm, and the legacy reader holds a reference to it.
	void Configure(const std::vector<tds::ColumnMetadata> &metadata, const std::vector<Vector *> &targets);

	//! True once Configure has run for the current result set.
	bool IsConfigured() const {
		return configured_;
	}
	//! Drop the resolution so the next chunk re-resolves (a new result set, or an
	//! output layout that changed under us).
	void Invalidate() {
		configured_ = false;
	}

	//! Start a chunk: re-point Direct columns at this chunk's output storage.
	//! `targets` must be the same vectors Configure saw, re-resolved for this
	//! chunk — DataChunk::Reset restores cached data pointers, it does not
	//! guarantee the same address forever.
	void BeginChunk(const std::vector<Vector *> &targets);

	//! Walk one row. `row`/`row_length` are the token's payload exactly as it sits
	//! on the wire, NULL bitmap included for the NBC form.
	void StageRow(const uint8_t *row, size_t row_length, idx_t row_idx);
	void StageNBCRow(const uint8_t *row, size_t row_length, idx_t row_idx);

	//! Publish staged state into the output vectors and close the chunk.
	//!
	//! `collect_nulls` is the D4 counters' only cost on this path: it turns on a
	//! per-column popcount of the staged validity, once per chunk. Nothing is
	//! counted per value — a branch there would cost more than the append itself.
	void FinalizeChunk(idx_t row_count, bool collect_nulls);

	//! NULLs in column `c` during the chunk just finalized. Meaningful only when
	//! FinalizeChunk was asked to collect them.
	idx_t ChunkNulls(idx_t c) const {
		return chunk_nulls_[c];
	}

	//! True when column `c` never reaches an output vector (Skip arm).
	bool IsSkipped(idx_t c) const {
		return ops_[c].arm == AppendArm::Skip;
	}

private:
	StagingArena arena_;
	std::vector<ColumnOps> ops_;
	//! Output vectors for this chunk, indexed by SQL column. Null for Skip.
	std::vector<Vector *> targets_;
	//! Reused per-value buffer for the Convert arm — the same single copy the
	//! legacy path makes into RowData, not an extra one.
	std::vector<uint8_t> scratch_;
	//! NULLs counted on the Convert arm, per column. Incremented unconditionally:
	//! that arm already pays for a full per-value conversion, so a branchless
	//! add there is invisible, and it keeps a flag off the Direct arms entirely.
	std::vector<idx_t> convert_nulls_;
	//! Per-column NULL count for the chunk just finalized (D4 counters).
	std::vector<idx_t> chunk_nulls_;
	const std::vector<tds::ColumnMetadata> *metadata_ = nullptr;
	unique_ptr<tds::RowReader> reader_;
	bool configured_ = false;
};

}  // namespace staging
}  // namespace codec
}  // namespace mssql
}  // namespace duckdb
