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

//! What the staged path did, for `MSSQL_DEBUG>=2` (spec 055 D10).
//!
//! Every field is accumulated PER COLUMN PER CHUNK or per event — nothing here
//! is reachable from the per-value path, which is why the whole block can be
//! filled without a branch anywhere near an append.
struct StagingCounters {
	//! Wire bytes staged, and time spent publishing them, by kernel. `staged` is
	//! 0 for the direct kernels by construction: those values never enter a
	//! staging buffer at all.
	uint64_t staged_bytes[FINALIZE_KERNEL_COUNT] = {};
	uint64_t kernel_ns[FINALIZE_KERNEL_COUNT] = {};
	uint64_t kernel_values[FINALIZE_KERNEL_COUNT] = {};
	//! Values that bypassed staging entirely — wire form was DuckDB's, so they
	//! were stored straight into the output vector.
	uint64_t direct_values = 0;
	//! String column-chunks by how their boundaries were found.
	uint64_t boundary[BOUNDARY_STRATEGY_COUNT] = {};
	//! Code units replaced with U+FFFD (invalid UTF-16, legal in UCS-2).
	uint64_t replaced_units = 0;
	//! Columns by how their payload was sized, counted once per result set.
	//! `capped` is the interesting one: a bound existed but was too large to
	//! preallocate, so the column finds its size by growing.
	uint64_t prealloc_bounded_columns = 0;
	uint64_t prealloc_capped_columns = 0;
	uint64_t unbounded_columns = 0;
};

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

	//! Turn on the D10 counters. Latched once, at MSSQL_DEBUG>=2, by the stream
	//! that owns this stager. Off, finalize takes no clock readings at all —
	//! steady_clock::now() costs ~15 ns and would otherwise be paid twice per
	//! column per chunk for a line nobody reads.
	void EnableCounters() {
		counters_enabled_ = true;
	}
	const StagingCounters &Counters() const {
		return counters_;
	}
	const StagingArena &Arena() const {
		return arena_;
	}

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
	void FinalizeChunk(idx_t row_count);

	//! Has this chunk staged more than `budget` bytes in its MAX-typed columns?
	//!
	//! Checked once per ROW, so it must cost nothing when there is nothing to
	//! check. A bounded column's whole chunk is at most max_length * 2048, so only
	//! MAX-typed columns can reach a budget at all — and a result set that has
	//! none exits on a member test, with no call and no loop. (It first shipped as
	//! an out-of-line call summing every column, which cost a measured +5.8% on an
	//! all-bigint scan that could never trip it.)
	inline bool StagedBytesExceed(idx_t budget) const {
		if (!has_unbounded_column_) {
			return false;
		}
		idx_t total = 0;
		for (idx_t i = 0; i < unbounded_columns_.size(); i++) {
			total += arena_.Column(unbounded_columns_[i]).PayloadSize();
		}
		return total >= budget;
	}

	//! NULLs in column `c` during the chunk just finalized. Counted by the append
	//! arm, so this is free and always available (D4/D10 counters).
	idx_t ChunkNulls(idx_t c) const {
		return chunk_nulls_[c];
	}

	//! True when column `c` never reaches an output vector (Skip arm).
	bool IsSkipped(idx_t c) const {
		return ops_[c].arm == AppendArm::Skip;
	}

private:
	//! Fold one finished column into the counters. Out of line and called only
	//! when they are on.
	void CountColumn(idx_t c, const ColumnStaging &st, idx_t row_count, uint64_t elapsed_ns);

	StagingArena arena_;
	std::vector<ColumnOps> ops_;
	//! Output vectors for this chunk, indexed by SQL column. Null for Skip.
	std::vector<Vector *> targets_;
	//! Per-column NULL count for the chunk just finalized (D4 counters).
	std::vector<idx_t> chunk_nulls_;
	//! Columns whose staged size is not bounded by their declared width (PLP).
	std::vector<idx_t> unbounded_columns_;
	bool has_unbounded_column_ = false;
	const std::vector<tds::ColumnMetadata> *metadata_ = nullptr;
	unique_ptr<tds::RowReader> reader_;
	bool configured_ = false;
	bool counters_enabled_ = false;
	StagingCounters counters_;
};

}  // namespace staging
}  // namespace codec
}  // namespace mssql
}  // namespace duckdb
