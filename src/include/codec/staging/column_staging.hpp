//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// codec/staging/column_staging.hpp
//
// Column-major staging buffers for the read path (spec 055 D3/D6a).
//
// WHY THIS EXISTS
// ---------------
// The read path already copies every wire value once — RowReader::ReadRow
// memcpy's each value into RowData::values[col], a vector<vector<uint8_t>> —
// and then MSSQLResultStream::ProcessRow reads that back and dispatches per
// value into the codec. Two passes over the same bytes, a type switch in each,
// and N separate heap buffers per row to chase.
//
// Staging does not add a copy. It redirects the copy that already happens into
// COLUMN-major buffers, so the conversion afterwards is one batch call per
// column instead of one call per value, and the second pass disappears.
//
// THE COST CONSTRAINT (D6a)
// -------------------------
// The whole phase budget is ~2 ns/value and a virtual or function-pointer call
// costs 1-2 ns, so **no abstraction may sit on the per-value path**. All
// polymorphism resolves at the column level, which is entered ~4 times per
// chunk rather than ~8192 times:
//
//   append    per value, hot   -> a switch on `kind` inlined at the call site.
//                                 The arm is invariant for a whole column, so
//                                 the predictor is right every time.
//   finalize  per column       -> one indirect call into the family kernel.
//
// This header is therefore DATA plus trivially inlinable accessors. It performs
// no conversion and knows nothing about SQL Server types.
//
// LIFETIME RULE (the dangerous one, stated once)
// ----------------------------------------------
// Staging buffers are reused by the next chunk. **No string_t handed to DuckDB
// may point into staging.** Strings must be materialized into DuckDB-owned
// storage during finalize. StagingArena owns every buffer so that this rule has
// exactly one place to be looked up.
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/exception.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/common/vector_size.hpp"

#include <cstdint>
#include <cstring>

namespace duckdb {
namespace mssql {
namespace codec {
namespace staging {

//===----------------------------------------------------------------------===//
// Constants
//===----------------------------------------------------------------------===//

//! Validity is kept as 64-bit words, matching DuckDB's ValidityMask layout so
//! finalize can memcpy them straight into the output vector.
static const idx_t VALIDITY_WORDS_PER_CHUNK = (STANDARD_VECTOR_SIZE + 63) / 64;

//! Offsets into a column's per-chunk payload are uint32_t. This cap keeps every
//! offset representable with a wide margin and turns a corrupt length prefix
//! into a clear error rather than a wrapped offset. A single chunk of 2048
//! values would have to average 128 KB each to reach it.
static const idx_t MAX_STAGING_PAYLOAD_BYTES = 256ULL * 1024ULL * 1024ULL;

//===----------------------------------------------------------------------===//
// StagingKind
//===----------------------------------------------------------------------===//

//! How a column's raw wire bytes are staged. Resolved once per column; the
//! append switch has exactly these three arms.
enum class StagingKind : uint8_t {
	//! 1:1 fixed width, wire representation identical to DuckDB's — the value is
	//! stored straight into the output vector and finalize is a no-op beyond
	//! validity. No staging buffer is used at all.
	Direct = 0,
	//! Fixed width, but the wire form needs a conversion pass (DECIMAL mantissa,
	//! datetime, GUID byte order). Staged contiguously at `stride` bytes.
	Fixed = 1,
	//! Variable length (NVARCHAR, VARBINARY, PLP). Payload is contiguous with
	//! per-value offset and length.
	Var = 2
};

//===----------------------------------------------------------------------===//
// ColumnStaging
//===----------------------------------------------------------------------===//

//! Per-column staging state for one chunk. Data + inlinable accessors only.
struct ColumnStaging {
	//===--------------------------------------------------------------------===//
	// Column-invariant configuration (set once, after COLMETADATA)
	//===--------------------------------------------------------------------===//
	StagingKind kind = StagingKind::Var;
	//! Bytes per value for Direct / Fixed. Unused for Var.
	uint32_t stride = 0;

	//===--------------------------------------------------------------------===//
	// Per-chunk state
	//===--------------------------------------------------------------------===//
	//! Direct only: where the next value is written, inside the DuckDB vector.
	//! Re-pointed at the start of every chunk; never retained past it.
	uint8_t *direct_dst = nullptr;

	//! Fixed: `count * stride` raw bytes. Var: the contiguous payload.
	duckdb::vector<uint8_t> buffer;
	//! Var only: byte offset and byte length of each value inside `buffer`.
	//! Length is always in BYTES, never code units — a fixed contract, because
	//! the two differ for UTF-16 and mixing them is a silent corruption.
	duckdb::vector<uint32_t> offsets;
	duckdb::vector<uint32_t> lengths;

	//! One bit per row, 1 = valid. Laid out exactly like duckdb::ValidityMask.
	duckdb::vector<uint64_t> validity_words;

	//! Rows staged so far in this chunk.
	idx_t count = 0;

	//===--------------------------------------------------------------------===//
	// Flags computed during append (D5 consumes them, D3 only records them)
	//===--------------------------------------------------------------------===//
	//! A staged value contained a U+0000 code unit, so it cannot be told apart
	//! from the delimiter and the delimited fast path must not be used.
	bool saw_embedded_nul = false;
	//! A staged value ends in an unpaired high surrogate, so a bulk conversion
	//! spanning the delimiter could pair it with the delimiter itself.
	bool boundary_risky = false;

	//===--------------------------------------------------------------------===//
	// Setup
	//===--------------------------------------------------------------------===//

	//! Configure the column once, after COLMETADATA. Sizes the fixed-capacity
	//! arrays; the payload grows on demand and keeps its high-water capacity.
	void Configure(StagingKind kind_p, uint32_t stride_p) {
		kind = kind_p;
		stride = stride_p;
		validity_words.resize(VALIDITY_WORDS_PER_CHUNK);
		if (kind == StagingKind::Var) {
			offsets.resize(STANDARD_VECTOR_SIZE);
			lengths.resize(STANDARD_VECTOR_SIZE);
		} else {
			offsets.clear();
			lengths.clear();
		}
		if (kind == StagingKind::Fixed) {
			buffer.resize(static_cast<idx_t>(stride) * STANDARD_VECTOR_SIZE);
		}
		BeginChunk(nullptr);
	}

	//! Start a chunk. Capacity is retained; only sizes and flags reset.
	//! `direct_dst_p` is the output vector's data pointer for Direct columns and
	//! is ignored otherwise.
	void BeginChunk(uint8_t *direct_dst_p) {
		count = 0;
		direct_dst = direct_dst_p;
		saw_embedded_nul = false;
		boundary_risky = false;
		if (kind == StagingKind::Var) {
			buffer.clear();
		}
		// All-valid by default: NULLs clear their bit, which is the rarer case
		// and keeps the common path free of validity work.
		for (idx_t i = 0; i < validity_words.size(); i++) {
			validity_words[i] = ~static_cast<uint64_t>(0);
		}
	}

	//===--------------------------------------------------------------------===//
	// Append (hot path — trivially inlinable, no dispatch of its own)
	//===--------------------------------------------------------------------===//

	//! Mark the row at `count` NULL and advance. Valid for every kind.
	inline void AppendNull() {
		validity_words[count / 64] &= ~(static_cast<uint64_t>(1) << (count % 64));
		if (kind == StagingKind::Var) {
			// A NULL still needs an offset/length slot so finalize can walk rows
			// positionally. Zero length, offset at the current payload end.
			offsets[count] = static_cast<uint32_t>(buffer.size());
			lengths[count] = 0;
		} else if (kind == StagingKind::Fixed) {
			// Zero the slot rather than leaving whatever the previous chunk put
			// there. The whole point of batch decode is a branch-free kernel over
			// all `count` rows, so a NULL slot IS read — leaving it stale would
			// feed a previous chunk's bytes into the decoder and make the output
			// for NULL rows nondeterministic (validity hides it from SQL, but not
			// from sanitizers, tests, or anything that hashes the vector).
			std::memset(buffer.data() + count * stride, 0, stride);
		} else {
			// Direct writes into the DuckDB vector, where NULL slots are
			// conventionally left undefined and the validity mask governs — this
			// matches what DuckDB's own scanners do. Only the cursor advances.
			direct_dst += stride;
		}
		count++;
	}

	//! Stage `stride` bytes for a Fixed column.
	//!
	//! The layout is POSITIONAL, not packed: row N always sits at N * stride, so
	//! a NULL leaves a (zeroed) hole. Finalize therefore indexes straight by row
	//! number and needs no separate cursor, which is what makes a branch-free
	//! kernel possible.
	inline void AppendFixed(const uint8_t *src) {
		std::memcpy(buffer.data() + count * stride, src, stride);
		count++;
	}

	//! Copy `stride` bytes straight into the output vector for a Direct column.
	inline void AppendDirect(const uint8_t *src) {
		std::memcpy(direct_dst, src, stride);
		direct_dst += stride;
		count++;
	}

	//! Stage a variable-length value. Returns the destination so the caller can
	//! write the payload itself when it would otherwise copy twice.
	inline uint8_t *AppendVarSlot(uint32_t length) {
		const idx_t offset = buffer.size();
		// Checked: a corrupt length prefix must not wrap a uint32_t offset.
		if (offset + length > MAX_STAGING_PAYLOAD_BYTES) {
			ThrowPayloadOverflow(offset, length);
		}
		buffer.resize(offset + length);
		offsets[count] = static_cast<uint32_t>(offset);
		lengths[count] = length;
		count++;
		return buffer.data() + offset;
	}

	//! Stage a variable-length value by copying it.
	inline void AppendVar(const uint8_t *src, uint32_t length) {
		uint8_t *dst = AppendVarSlot(length);
		if (length > 0) {
			std::memcpy(dst, src, length);
		}
	}

	//===--------------------------------------------------------------------===//
	// Read-back helpers (finalize side)
	//===--------------------------------------------------------------------===//

	inline bool IsValid(idx_t row) const {
		return (validity_words[row / 64] & (static_cast<uint64_t>(1) << (row % 64))) != 0;
	}

	inline const uint8_t *ValueAt(idx_t row) const {
		return kind == StagingKind::Var ? buffer.data() + offsets[row] : buffer.data() + row * stride;
	}

	inline uint32_t LengthAt(idx_t row) const {
		return kind == StagingKind::Var ? lengths[row] : stride;
	}

	//! Bytes currently retained by this column, for the arena's watermark policy.
	idx_t RetainedBytes() const {
		return static_cast<idx_t>(buffer.capacity()) + offsets.capacity() * sizeof(uint32_t) +
			   lengths.capacity() * sizeof(uint32_t) + validity_words.capacity() * sizeof(uint64_t);
	}

private:
	//! Out of line: keeps the throw path off the inlined append.
	static void ThrowPayloadOverflow(idx_t offset, uint32_t length);
};

//===----------------------------------------------------------------------===//
// StagingArena
//===----------------------------------------------------------------------===//

//! Owns every staging buffer for one result stream, across chunks and across
//! result sets. The single place staging lifetime and capacity policy live.
//!
//! Capacity is retained at its high-water mark so a steady workload allocates
//! once; a stream that saw one huge chunk and then many small ones releases the
//! outlier after SHRINK_INTERVAL_CHUNKS (otherwise a single 200 MB row would be
//! pinned for the life of the connection).
class StagingArena {
public:
	//! Chunks between shrink evaluations.
	static const idx_t SHRINK_INTERVAL_CHUNKS = 64;
	//! Shrink only when retained capacity exceeds the recent peak by this factor
	//! — a hysteresis band, so ordinary chunk-to-chunk variation never reallocs.
	static const idx_t SHRINK_SLACK_FACTOR = 4;
	//! Never shrink below this; churning small allocations costs more than it
	//! saves.
	static const idx_t MIN_RETAINED_BYTES = 64ULL * 1024ULL;

public:
	StagingArena() : chunks_since_evaluation_(0) {}

	//! (Re)configure for a result set. Existing buffers are reused when the
	//! column count is unchanged, which is the multi-statement scan case.
	void Configure(idx_t column_count) {
		if (columns_.size() != column_count) {
			columns_.clear();
			for (idx_t i = 0; i < column_count; i++) {
				// unique_ptr elements: the row reader caches ColumnStaging
				// pointers for a whole chunk, so addresses must not move.
				columns_.push_back(make_uniq<ColumnStaging>());
			}
		}
		peak_bytes_.assign(column_count, 0);
	}

	idx_t ColumnCount() const {
		return columns_.size();
	}

	ColumnStaging &Column(idx_t idx) {
		return *columns_[idx];
	}
	const ColumnStaging &Column(idx_t idx) const {
		return *columns_[idx];
	}

	//! Record this chunk's usage and apply the watermark policy.
	//!
	//! `peak_bytes_` is the maximum over the interval since the last evaluation,
	//! so an outlier chunk is released at the first evaluation whose ENTIRE
	//! interval stayed small — up to two intervals after the outlier itself.
	//! That hysteresis is deliberate: releasing on the first quiet chunk would
	//! realloc against any workload that alternates chunk sizes.
	void EndChunk() {
		for (idx_t i = 0; i < columns_.size(); i++) {
			const idx_t used = columns_[i]->buffer.size();
			if (used > peak_bytes_[i]) {
				peak_bytes_[i] = used;
			}
		}
		if (++chunks_since_evaluation_ < SHRINK_INTERVAL_CHUNKS) {
			return;
		}
		chunks_since_evaluation_ = 0;
		for (idx_t i = 0; i < columns_.size(); i++) {
			ColumnStaging &col = *columns_[i];
			const idx_t capacity = col.buffer.capacity();
			const idx_t peak = peak_bytes_[i];
			if (capacity > MIN_RETAINED_BYTES && capacity > peak * SHRINK_SLACK_FACTOR) {
				duckdb::vector<uint8_t> shrunk;
				shrunk.reserve(peak * 2 > MIN_RETAINED_BYTES ? peak * 2 : MIN_RETAINED_BYTES);
				col.buffer.swap(shrunk);
				col.buffer.clear();
			}
			peak_bytes_[i] = 0;
		}
	}

	//! Total bytes retained across all columns (tests and the D10 counters).
	idx_t RetainedBytes() const {
		idx_t total = 0;
		for (idx_t i = 0; i < columns_.size(); i++) {
			total += columns_[i]->RetainedBytes();
		}
		return total;
	}

private:
	duckdb::vector<duckdb::unique_ptr<ColumnStaging>> columns_;
	duckdb::vector<idx_t> peak_bytes_;
	idx_t chunks_since_evaluation_;
};

}  // namespace staging
}  // namespace codec
}  // namespace mssql
}  // namespace duckdb
