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

//! Offsets into a column's per-chunk payload are uint32_t, so this is the point
//! past which one would wrap. It is set just under 2 GB, which is also SQL
//! Server's own limit for a MAX-typed value — so a single legal value always
//! fits, and anything past it is a corrupt length rather than data.
//!
//! This is NOT the memory policy. A chunk stops accepting rows long before
//! here, at STAGING_CHUNK_PAYLOAD_BUDGET_BYTES; the cap only exists to make the
//! arithmetic safe.
static const idx_t MAX_STAGING_PAYLOAD_BYTES = 2047ULL * 1024ULL * 1024ULL;

//! Total staged bytes across a chunk's columns after which the chunk is closed
//! early, even though it holds fewer than STANDARD_VECTOR_SIZE rows.
//!
//! Only MAX-typed columns can reach it: a bounded column's whole chunk is at
//! most max_length * 2048. Without it, 2048 rows of megabyte blobs would stage
//! gigabytes before anything was handed to DuckDB. A DataChunk is allowed to be
//! short, so ending early costs a slightly smaller chunk and nothing else.
static const idx_t STAGING_CHUNK_PAYLOAD_BUDGET_BYTES = 32ULL * 1024ULL * 1024ULL;

//! Per-column budget for preallocating a column's PROVABLE worst case.
//!
//! For a non-MAX variable column the declared type bounds every value exactly
//! (nvarchar(16) is at most 32 bytes on the wire), so max_length *
//! STANDARD_VECTOR_SIZE is the largest payload a chunk can possibly produce.
//! Reserving that up front means the column can never resize again — not
//! "rarely", never.
//!
//! It is only worth doing while the bound is cheap. nvarchar(16) needs 64 KB and
//! nvarchar(200) 800 KB, but nvarchar(4000) would demand 16 MB per column for a
//! width real data almost never reaches. Past this budget the high-water mark
//! finds the actual size within a chunk or two and the column is resize-free
//! from then on anyway.
static const idx_t STAGING_PREALLOC_BUDGET_BYTES = 2ULL * 1024ULL * 1024ULL;

//! Starting payload capacity for columns with no usable bound (PLP / MAX types).
static const idx_t STAGING_UNBOUNDED_INITIAL_BYTES = 64ULL * 1024ULL;

//===----------------------------------------------------------------------===//
// StagingKind
//===----------------------------------------------------------------------===//

//! How a column's raw wire bytes are staged. Resolved once per column and used
//! as the append switch's selector.
//!
//! The Direct family is split by width ON PURPOSE. A `memcpy` whose size is a
//! runtime value compiles to a call into libc; the same copy with the size in
//! the type is one unaligned `mov`, and `count * STRIDE` folds into the
//! addressing mode instead of an `imul`. Measured on the arena's real shape
//! (columns on the heap, 4 columns x 2048 rows): **1.63 -> 0.62 ns/value**.
//! Four arms in a switch that is invariant for a whole column costs nothing —
//! the predictor is right every time.
enum class StagingKind : uint8_t {
	//! 1:1 fixed width, wire representation identical to DuckDB's — the value is
	//! stored straight into the output vector and finalize is a no-op beyond
	//! validity. No staging buffer is used at all. Suffix = bytes per value;
	//! these are the only widths TDS produces for a 1:1 type (TINYINT/BIT,
	//! SMALLINT, INT/REAL, BIGINT/FLOAT).
	Direct1 = 0,
	Direct2 = 1,
	Direct4 = 2,
	Direct8 = 3,
	//! Fixed width, but the wire form needs a conversion pass (DECIMAL mantissa,
	//! datetime, GUID byte order). Staged contiguously at `stride` bytes. Not
	//! split by width: the widths are many, and the conversion kernel that
	//! follows dwarfs the copy.
	Fixed = 4,
	//! Variable length (NVARCHAR, VARBINARY, PLP). Payload is contiguous with
	//! per-value offset and length.
	Var = 5
};

//! Direct arms are ordered first, so this is one comparison.
inline bool IsDirectKind(StagingKind kind) {
	return kind <= StagingKind::Direct8;
}

//! Map a 1:1 width to its arm. Only 1, 2, 4 and 8 reach here — the caller
//! (ResolveColumnOps) proves the width is a DuckDB physical type's size before
//! classifying a column as direct.
inline StagingKind DirectKindForWidth(uint32_t width) {
	switch (width) {
	case 1:
		return StagingKind::Direct1;
	case 2:
		return StagingKind::Direct2;
	case 4:
		return StagingKind::Direct4;
	default:
		return StagingKind::Direct8;
	}
}

//===----------------------------------------------------------------------===//
// BoundaryStrategy
//===----------------------------------------------------------------------===//

//! How a string column's batch decode located its value boundaries (D10).
//!
//! Which one a column takes is the single most useful thing to know when a
//! string-heavy scan is slower than expected: the arithmetic path does no
//! searching at all, and the two walks differ by an order of magnitude in what
//! they cost per byte scanned.
enum class BoundaryStrategy : uint8_t {
	//! No boundaries to find: the column staged nothing, or is not a string.
	None = 0,
	//! Output bytes equalled input units, so every offset is an input offset
	//! halved. No search ran.
	AsciiOffsets,
	//! Delimiter walk from each value's lower bound, swept a word at a time.
	SkipSweep,
	//! Delimiter walk from each value's lower bound, via memchr.
	SkipMemchr,
	//! A value contained a U+0000 of its own, so the walk was redone against the
	//! input's own zero units.
	EmbeddedNul
};

static const uint8_t BOUNDARY_STRATEGY_COUNT = 5;

//! Short name, for the debug counters.
const char *BoundaryStrategyName(BoundaryStrategy strategy);

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
	//! Direct only: base of the output vector's data. Row N is written at
	//! `direct_dst + N * stride`. Re-pointed at the start of every chunk; never
	//! retained past it.
	uint8_t *direct_dst = nullptr;

	//! These are `unsafe_vector`, i.e. duckdb::vector<T, false>, DELIBERATELY.
	//! The default duckdb::vector bounds-checks every operator[] **in release
	//! builds too** (MemorySafety<true>::ENABLED is unconditionally true), which
	//! would put a branch and a size() load on every AppendNull and every Var
	//! append. The bound is structural, not hopeful: each array is sized for a
	//! whole chunk in Configure and `count` cannot exceed STANDARD_VECTOR_SIZE,
	//! because the chunk loop closes the chunk at that point. Debug builds still
	//! check — MemorySafety forces ENABLED on under DEBUG regardless of SAFE.
	//!
	//! Fixed: `count * stride` raw bytes. Var: the contiguous payload.
	duckdb::unsafe_vector<uint8_t> buffer;
	//! Var only: byte offset and byte length of each value inside `buffer`.
	//! Length is always in BYTES, never code units — a fixed contract, because
	//! the two differ for UTF-16 and mixing them is a silent corruption.
	duckdb::unsafe_vector<uint32_t> offsets;
	duckdb::unsafe_vector<uint32_t> lengths;

	//! One bit per row, 1 = valid. Laid out exactly like duckdb::ValidityMask.
	duckdb::unsafe_vector<uint64_t> validity_words;

	//! Rows staged so far in this chunk.
	idx_t count = 0;
	//! NULLs among them. One increment on the NULL arm — which is the only arm
	//! that can produce one — in exchange for two loops per column per chunk:
	//! finalize no longer scans the validity words to find out whether the mask
	//! is worth publishing, and the debug counters no longer popcount it.
	idx_t null_count = 0;
	//! Var only: payload bytes used. `buffer` is sized to CAPACITY, not to use —
	//! vector::resize() value-initialises the new bytes, and we memcpy over them
	//! immediately, so resizing per value would write every payload byte twice.
	idx_t payload_used = 0;

	//! The payload was preallocated to this column's provable worst case, so it
	//! can never grow and must never be shrunk.
	bool payload_bounded = false;
	//! Times this column's payload had to grow. Written by GrowPayload, which is
	//! out of line and cold; a bounded column can never touch it.
	idx_t grow_events = 0;

	//===--------------------------------------------------------------------===//
	// Finalize outcome (D10 counters)
	//===--------------------------------------------------------------------===//
	//
	// What the batch kernel DID with the column, as opposed to what was staged
	// into it. The kernels take the staging by const reference because they do
	// not touch the staged bytes; a report about their own pass is not staged
	// data, hence `mutable`. One store per column per chunk, so it is
	// unconditional — a test for whether the counters are on would cost more
	// than writing it.

	//! Set by the string kernel; None for every other family.
	mutable BoundaryStrategy boundary = BoundaryStrategy::None;
	//! Code units the string kernel replaced with U+FFFD because the payload was
	//! not valid UTF-16 (legal in a UCS-2 collation, so this is data, not
	//! corruption). The error path's only per-value work in the whole scan.
	mutable idx_t replaced_units = 0;

	//===--------------------------------------------------------------------===//
	// Setup
	//===--------------------------------------------------------------------===//

	//! Configure the column once, after COLMETADATA.
	//!
	//! `max_value_bytes` is the declared upper bound on ONE value's wire size,
	//! from the column metadata; 0 means unbounded (PLP / MAX types). Every
	//! allocation this column will ever make happens here whenever that bound is
	//! usable — see STAGING_PREALLOC_BUDGET_BYTES.
	void Configure(StagingKind kind_p, uint32_t stride_p, uint32_t max_value_bytes = 0) {
		kind = kind_p;
		stride = stride_p;
		payload_bounded = false;
		validity_words.resize(VALIDITY_WORDS_PER_CHUNK);
		if (kind == StagingKind::Var) {
			offsets.resize(STANDARD_VECTOR_SIZE);
			lengths.resize(STANDARD_VECTOR_SIZE);
			// The provable worst case for a whole chunk. uint64 arithmetic: a
			// declared max_length near 0xFFFF times 2048 overflows uint32.
			const idx_t bound = static_cast<idx_t>(max_value_bytes) * STANDARD_VECTOR_SIZE;
			if (max_value_bytes > 0 && bound <= STAGING_PREALLOC_BUDGET_BYTES) {
				buffer.resize(bound);
				// Nothing can exceed this, so growth is now unreachable and the
				// arena must not shrink it back into reach.
				payload_bounded = true;
			} else if (buffer.size() < STAGING_UNBOUNDED_INITIAL_BYTES) {
				buffer.resize(STAGING_UNBOUNDED_INITIAL_BYTES);
			}
		} else {
			offsets.clear();
			lengths.clear();
		}
		if (kind == StagingKind::Fixed) {
			// Exact and final: a fixed-width column's chunk payload is known.
			buffer.resize(static_cast<idx_t>(stride) * STANDARD_VECTOR_SIZE);
		}
		BeginChunk(nullptr);
	}

	//! Start a chunk. Capacity is retained; only sizes and flags reset.
	//! `direct_dst_p` is the output vector's data pointer for Direct columns and
	//! is ignored otherwise.
	void BeginChunk(uint8_t *direct_dst_p) {
		count = 0;
		null_count = 0;
		direct_dst = direct_dst_p;
		boundary = BoundaryStrategy::None;
		replaced_units = 0;
		payload_used = 0;
		// All-valid by default: NULLs clear their bit, which is the rarer case
		// and keeps the common path free of validity work.
		for (idx_t i = 0; i < validity_words.size(); i++) {
			validity_words[i] = ~static_cast<uint64_t>(0);
		}
	}

	//===--------------------------------------------------------------------===//
	// Append (hot path — trivially inlinable, no dispatch of its own)
	//===--------------------------------------------------------------------===//

	//! Mark the row at `count` NULL and advance.
	//!
	//! A NULL is a BIT AND NOTHING ELSE — same model as DuckDB's own vectors, and
	//! the reason this has no branch and does not consult `kind`:
	//!
	//! - Direct and Fixed address positionally (row * stride), so a NULL row's
	//!   slot simply keeps whatever was there. Every Fixed kernel is total over
	//!   any byte pattern (decimal is a load, GUID a byte swap, money a load), so
	//!   a branch-free pass across it produces a value the mask discards.
	//! - Var cannot be addressed positionally, so its finalize must consult the
	//!   mask no matter what — which makes writing an offset/length for a NULL
	//!   row work that the mask already covers. Those slots are therefore
	//!   UNDEFINED on NULL rows, by contract.
	inline void AppendNull() {
		validity_words[count >> 6] &= ~(static_cast<uint64_t>(1) << (count & 63));
		count++;
		null_count++;
	}

	//! Stage `stride` bytes for a Fixed column.
	//!
	//! The layout is POSITIONAL, not packed: row N always sits at N * stride, so
	//! a NULL leaves a (zeroed) hole. Finalize therefore indexes straight by row
	//! number and needs no separate cursor, which is what makes a branch-free
	//! kernel possible.
	//! STRIDE is a template parameter for the same reason AppendDirect's is: a
	//! memcpy whose size is a runtime value compiles to a libc call, and
	//! count * STRIDE folds into the addressing mode instead of a multiply.
	template <uint32_t STRIDE>
	inline void AppendFixed(const uint8_t *src) {
		std::memcpy(buffer.data() + count * STRIDE, src, STRIDE);
		count++;
	}

	//! Stage a value SHORTER than the slot, zero-filling the rest.
	//!
	//! Only DECIMAL uses this. Its mantissa is little-endian, so widening a short
	//! encoding with high zero bytes is exactly value-preserving — which is what
	//! lets a server that sends fewer bytes than the precision declares stay on
	//! the batch path instead of falling back to a per-value decode.
	template <uint32_t STRIDE>
	inline void AppendFixedPadded(const uint8_t *src, uint32_t length) {
		uint8_t *dst = buffer.data() + count * STRIDE;
		std::memcpy(dst, src, length);
		std::memset(dst + length, 0, STRIDE - length);
		count++;
	}

	//! Copy STRIDE bytes straight into the output vector for a Direct column.
	//! Positional, like Fixed: no cursor to keep in step with NULLs.
	//!
	//! STRIDE is a template parameter rather than the `stride` member so the copy
	//! is a single sized store and the offset is an addressing-mode scale. The
	//! caller picks the instantiation from `kind`, which is column-invariant.
	template <uint32_t STRIDE>
	inline void AppendDirect(const uint8_t *src) {
		std::memcpy(direct_dst + count * STRIDE, src, STRIDE);
		count++;
	}

	//! Stage a variable-length value. Returns the destination so the caller can
	//! write the payload itself when it would otherwise copy twice.
	inline uint8_t *AppendVarSlot(uint32_t length) {
		const idx_t offset = payload_used;
		const idx_t needed = offset + length;
		if (needed > buffer.size()) {
			// The one place the payload can grow. `length` came off the wire, so
			// this is also where a corrupt length prefix is caught — before it
			// can wrap the uint32_t offsets or ask for an absurd allocation.
			GrowPayload(needed);
		}
		payload_used = needed;
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
	// Incremental variable-length append (PLP)
	//===--------------------------------------------------------------------===//
	//
	// A PLP value arrives as a chunk list and may declare its total length
	// UNKNOWN, so the slot cannot be sized up front the way AppendVarSlot needs.
	// Begin/Extend/Finish record the offset first, grow as chunks arrive, and
	// only then write the length.

	inline void BeginVar() {
		offsets[count] = static_cast<uint32_t>(payload_used);
	}

	//! Reserve `length` more bytes for the value being built and return where to
	//! write them.
	inline uint8_t *ExtendVar(uint32_t length) {
		const idx_t offset = payload_used;
		const idx_t needed = offset + length;
		if (needed > buffer.size()) {
			GrowPayload(needed);
		}
		payload_used = needed;
		return buffer.data() + offset;
	}

	//! Close the value: everything appended since BeginVar is its content.
	inline void FinishVar() {
		lengths[count] = static_cast<uint32_t>(payload_used - offsets[count]);
		count++;
	}

	//! Close a value that carries a trailing delimiter, which is NOT part of it.
	inline void FinishVarDelimited() {
		uint8_t *dst = ExtendVar(2);
		dst[0] = 0;
		dst[1] = 0;
		lengths[count] = static_cast<uint32_t>(payload_used - offsets[count] - 2);
		count++;
	}

	//! Stage a UTF-16LE value followed by a U+0000 code unit.
	//!
	//! The delimiter is what lets the whole column be converted in ONE simdutf
	//! call and still be split back into values: a code unit of zero converts to
	//! a single 0x00 byte, so the values are separated in the output too. Without
	//! it the output offsets of a column containing any multi-byte character
	//! cannot be derived from the input offsets at all, and the only alternative
	//! is a conversion call per value.
	//!
	//! `lengths[]` records the value WITHOUT its delimiter, so the ASCII fast
	//! path's arithmetic is unchanged and finalize never has to subtract.
	inline void AppendVarDelimited(const uint8_t *src, uint32_t length) {
		uint8_t *dst = AppendVarSlot(length + 2);
		std::memcpy(dst, src, length);
		dst[length] = 0;
		dst[length + 1] = 0;
		lengths[count - 1] = length;
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

	//! Var: undefined on NULL rows (see AppendNull) — check IsValid first.
	inline uint32_t LengthAt(idx_t row) const {
		return kind == StagingKind::Var ? lengths[row] : stride;
	}

	//! Payload bytes used this chunk (Var). `buffer.size()` is capacity.
	inline idx_t PayloadSize() const {
		return payload_used;
	}

	//! Bytes currently retained by this column, for the arena's watermark policy.
	idx_t RetainedBytes() const {
		return static_cast<idx_t>(buffer.size()) + offsets.capacity() * sizeof(uint32_t) +
			   lengths.capacity() * sizeof(uint32_t) + validity_words.capacity() * sizeof(uint64_t);
	}

private:
	//! Out of line: growth and the wire-corruption error stay off the inlined
	//! append body. Growth doubles, so a steady stream allocates once and then
	//! never again — the capacity is retained across chunks.
	void GrowPayload(idx_t needed);
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
			const idx_t used = columns_[i]->payload_used;
			if (used > peak_bytes_[i]) {
				peak_bytes_[i] = used;
			}
			if (used > peak_payload_bytes_) {
				// All-time, across every column and interval — the number that
				// says how much a MAX-typed column really asked for (D10).
				peak_payload_bytes_ = used;
			}
		}
		if (++chunks_since_evaluation_ < SHRINK_INTERVAL_CHUNKS) {
			return;
		}
		chunks_since_evaluation_ = 0;
		for (idx_t i = 0; i < columns_.size(); i++) {
			ColumnStaging &col = *columns_[i];
			const idx_t capacity = col.buffer.size();
			const idx_t peak = peak_bytes_[i];
			// A bounded column already holds exactly its worst case and is small
			// by construction; releasing it would only make growth reachable
			// again.
			if (!col.payload_bounded && capacity > MIN_RETAINED_BYTES && capacity > peak * SHRINK_SLACK_FACTOR) {
				const idx_t target = peak * 2 > MIN_RETAINED_BYTES ? peak * 2 : MIN_RETAINED_BYTES;
				duckdb::unsafe_vector<uint8_t> shrunk(target);
				col.buffer.swap(shrunk);
				col.payload_used = 0;
				shrink_events_++;
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

	//! Largest payload any one column held in any one chunk, for the life of the
	//! arena. With the grow and shrink counts, this is the memory profile the
	//! spec's criterion 5.7 asks to see: capacity returning to the watermark
	//! rather than growing monotonically (D10).
	idx_t PeakPayloadBytes() const {
		return peak_payload_bytes_;
	}
	idx_t ShrinkEvents() const {
		return shrink_events_;
	}
	//! Growth is counted per column, where it happens.
	idx_t GrowEvents() const {
		idx_t total = 0;
		for (idx_t i = 0; i < columns_.size(); i++) {
			total += columns_[i]->grow_events;
		}
		return total;
	}

private:
	duckdb::vector<duckdb::unique_ptr<ColumnStaging>> columns_;
	duckdb::vector<idx_t> peak_bytes_;
	idx_t chunks_since_evaluation_;
	idx_t peak_payload_bytes_ = 0;
	idx_t shrink_events_ = 0;
};

}  // namespace staging
}  // namespace codec
}  // namespace mssql
}  // namespace duckdb
