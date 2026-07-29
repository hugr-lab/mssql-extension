// test/cpp/codec/test_column_staging.cpp
// Unit tests for codec::staging (spec 055 D3 — staging structures and ownership).
//
// Does NOT require a running SQL Server instance. No conversion is exercised:
// this layer stages raw wire bytes column-major and knows nothing about types.
//
// Covers:
//   - append semantics for every StagingKind (Direct1/2/4/8 / Fixed / Var),
//   - validity: all-valid by default, NULLs clear their bit and nothing else —
//     Direct and Fixed address positionally, Var's offset/length are undefined
//     on a NULL row,
//   - the checked-arithmetic cap on payload growth (a corrupt length prefix
//     must raise, not wrap a uint32_t offset),
//   - arena ownership: ColumnStaging addresses stay stable across chunks
//     (the row reader caches them for a whole chunk),
//   - the watermark policy: capacity is retained across chunks, and an outlier
//     chunk is released after SHRINK_INTERVAL_CHUNKS instead of pinning memory
//     for the life of the stream.
//
// Build & run:
//   make test-column-staging

#include "codec/staging/column_ops.hpp"
#include "codec/staging/column_staging.hpp"

#include "duckdb/common/exception.hpp"
#include "tds/encoding/type_converter.hpp"
#include "tds/tds_types.hpp"

#include <cstring>
#include <iostream>
#include <string>
#include <vector>

using duckdb::idx_t;
using duckdb::mssql::codec::staging::ColumnOps;
using duckdb::mssql::codec::staging::ColumnStaging;
using duckdb::mssql::codec::staging::DirectKindForWidth;
using duckdb::mssql::codec::staging::IsDirectKind;
using duckdb::mssql::codec::staging::ResolveColumnOps;
using duckdb::mssql::codec::staging::StagingArena;
using duckdb::mssql::codec::staging::StagingKind;

namespace {

int failures = 0;

#define CHECK_TRUE(expr, label)                                                    \
	do {                                                                           \
		if (!(expr)) {                                                             \
			std::cerr << "FAIL: " << (label) << " (" << #expr << ")" << std::endl; \
			failures++;                                                            \
		}                                                                          \
	} while (0)

#define CHECK_EQ(actual, expected, label)                                                            \
	do {                                                                                             \
		auto a_ = (actual);                                                                          \
		auto e_ = (expected);                                                                        \
		if (!(a_ == e_)) {                                                                           \
			std::cerr << "FAIL: " << (label) << " — got " << a_ << ", expected " << e_ << std::endl; \
			failures++;                                                                              \
		}                                                                                            \
	} while (0)

void TestVarAppendAndReadBack() {
	std::cout << "[1] Var: append, offsets, lengths, read-back..." << std::endl;
	ColumnStaging col;
	col.Configure(StagingKind::Var, 0);
	col.BeginChunk(nullptr);

	const std::string a = "hello";
	const std::string b = "";
	const std::string c = "worldwide";
	col.AppendVar(reinterpret_cast<const uint8_t *>(a.data()), static_cast<uint32_t>(a.size()));
	col.AppendVar(reinterpret_cast<const uint8_t *>(b.data()), 0);
	col.AppendNull();
	col.AppendVar(reinterpret_cast<const uint8_t *>(c.data()), static_cast<uint32_t>(c.size()));

	CHECK_EQ(col.count, static_cast<idx_t>(4), "four rows staged");
	CHECK_EQ(col.LengthAt(0), 5u, "row 0 length");
	CHECK_EQ(col.LengthAt(1), 0u, "row 1 length (empty, not NULL)");
	// Row 2's offset/length are deliberately NOT checked: they are undefined on
	// NULL rows, because a NULL is only a bit in the mask.
	CHECK_EQ(col.LengthAt(3), 9u, "row 3 length");

	CHECK_TRUE(col.IsValid(0), "row 0 valid");
	CHECK_TRUE(col.IsValid(1), "row 1 valid — empty is not NULL");
	CHECK_TRUE(!col.IsValid(2), "row 2 NULL");
	CHECK_TRUE(col.IsValid(3), "row 3 valid");

	CHECK_TRUE(std::memcmp(col.ValueAt(0), a.data(), 5) == 0, "row 0 payload");
	CHECK_TRUE(std::memcmp(col.ValueAt(3), c.data(), 9) == 0, "row 3 payload");
	// Payload is contiguous: total equals the sum of the lengths.
	CHECK_EQ(col.PayloadSize(), static_cast<idx_t>(14), "payload is contiguous and exactly the sum of lengths");
}

void TestFixedAndDirectAppend() {
	std::cout << "[2] Fixed / Direct: strided append..." << std::endl;

	ColumnStaging fixed;
	fixed.Configure(StagingKind::Fixed, 8);
	fixed.BeginChunk(nullptr);
	uint64_t v0 = 0x0102030405060708ULL;
	uint64_t v1 = 0x1112131415161718ULL;
	fixed.AppendFixed(reinterpret_cast<const uint8_t *>(&v0));
	fixed.AppendNull();
	fixed.AppendFixed(reinterpret_cast<const uint8_t *>(&v1));
	CHECK_EQ(fixed.count, static_cast<idx_t>(3), "three rows");
	CHECK_TRUE(!fixed.IsValid(1), "middle row NULL");
	// Layout is POSITIONAL: row N lives at N * stride, so row 2 is at byte 16,
	// not packed at byte 8. Finalize indexes by row number with no cursor.
	CHECK_TRUE(std::memcmp(fixed.buffer.data() + 16, &v1, 8) == 0, "row 2 at its positional offset");
	// The NULL slot is left as-is: every Fixed kernel is total over any byte
	// pattern, so a branch-free pass across it yields a value validity discards.
	CHECK_TRUE(std::memcmp(fixed.ValueAt(2), &v1, 8) == 0, "ValueAt agrees with the positional layout");

	// Direct writes into caller-owned storage (the DuckDB vector), so the
	// destination pointer advances even for NULLs to keep row positions aligned.
	std::vector<uint8_t> out(4 * 4, 0xAA);
	ColumnStaging direct;
	direct.Configure(StagingKind::Direct4, 4);
	direct.BeginChunk(out.data());
	uint32_t d0 = 0x01020304u;
	uint32_t d2 = 0x0A0B0C0Du;
	direct.AppendDirect<4>(reinterpret_cast<const uint8_t *>(&d0));
	direct.AppendNull();
	direct.AppendDirect<4>(reinterpret_cast<const uint8_t *>(&d2));
	CHECK_TRUE(std::memcmp(out.data(), &d0, 4) == 0, "direct row 0 written into the output vector");
	CHECK_TRUE(out[4] == 0xAA, "direct NULL row left untouched — the mask governs");
	CHECK_TRUE(std::memcmp(out.data() + 8, &d2, 4) == 0, "direct row 2 lands at its positional offset");
	CHECK_TRUE(!direct.IsValid(1), "direct NULL recorded in the mask only");
}

void TestValidityDefaultsToValid() {
	std::cout << "[3] validity: all-valid by default, reset per chunk..." << std::endl;
	ColumnStaging col;
	col.Configure(StagingKind::Var, 0);
	col.BeginChunk(nullptr);
	for (idx_t i = 0; i < 130; i++) {
		col.AppendNull();
	}
	for (idx_t i = 0; i < 130; i++) {
		CHECK_TRUE(!col.IsValid(i), "row marked NULL across word boundaries");
	}
	// A new chunk must not inherit the previous chunk's NULL bits.
	col.BeginChunk(nullptr);
	const uint8_t byte = 0x7F;
	for (idx_t i = 0; i < 130; i++) {
		col.AppendVar(&byte, 1);
	}
	for (idx_t i = 0; i < 130; i++) {
		CHECK_TRUE(col.IsValid(i), "validity reset to all-valid at chunk start");
	}
	CHECK_EQ(col.count, static_cast<idx_t>(130), "count reset per chunk");
}

void TestPayloadCapIsChecked() {
	std::cout << "[4] checked arithmetic: payload cap raises instead of wrapping..." << std::endl;
	ColumnStaging col;
	col.Configure(StagingKind::Var, 0);
	col.BeginChunk(nullptr);

	// A corrupt length prefix claiming more than the cap must throw rather than
	// overflow the uint32_t offsets. Nothing is allocated for it.
	bool threw = false;
	try {
		col.AppendVarSlot(0xFFFFFFFFu);
	} catch (const duckdb::InvalidInputException &) {
		threw = true;
	}
	CHECK_TRUE(threw, "oversized value raises InvalidInputException");
}

void TestArenaAddressStability() {
	std::cout << "[5] arena: column addresses stable across chunks..." << std::endl;
	StagingArena arena;
	arena.Configure(3);
	CHECK_EQ(arena.ColumnCount(), static_cast<idx_t>(3), "three columns");

	ColumnStaging *before[3];
	for (idx_t i = 0; i < 3; i++) {
		arena.Column(i).Configure(StagingKind::Var, 0);
		before[i] = &arena.Column(i);
	}
	const uint8_t byte = 1;
	for (int chunk = 0; chunk < 10; chunk++) {
		for (idx_t i = 0; i < 3; i++) {
			arena.Column(i).BeginChunk(nullptr);
			for (int r = 0; r < 100; r++) {
				arena.Column(i).AppendVar(&byte, 1);
			}
		}
		arena.EndChunk();
	}
	for (idx_t i = 0; i < 3; i++) {
		CHECK_TRUE(before[i] == &arena.Column(i), "column address unchanged after ten chunks");
	}

	// Reconfiguring with the SAME column count reuses the buffers — the
	// multi-statement scan case, where a second result set must not realloc.
	arena.Configure(3);
	for (idx_t i = 0; i < 3; i++) {
		CHECK_TRUE(before[i] == &arena.Column(i), "same column count reuses existing staging");
	}
}

void TestWatermarkRetainsThenReleases() {
	std::cout << "[6] watermark: retain capacity, release an outlier..." << std::endl;
	StagingArena arena;
	arena.Configure(1);
	ColumnStaging &col = arena.Column(0);
	col.Configure(StagingKind::Var, 0);

	// One outlier chunk well above the shrink floor.
	const idx_t big = StagingArena::MIN_RETAINED_BYTES * 16;
	std::vector<uint8_t> blob(big, 0x5A);
	col.BeginChunk(nullptr);
	col.AppendVar(blob.data(), static_cast<uint32_t>(blob.size()));
	arena.EndChunk();
	const idx_t after_big = col.buffer.size();
	CHECK_TRUE(after_big >= big, "capacity grew to hold the outlier");

	// A steady run of small chunks. Capacity is retained through the first whole
	// interval: the outlier fell inside it, so that evaluation still sees a large
	// peak and must not shrink. Reallocating here is exactly what this avoids.
	const uint8_t small = 0x11;
	for (idx_t i = 0; i < StagingArena::SHRINK_INTERVAL_CHUNKS; i++) {
		col.BeginChunk(nullptr);
		col.AppendVar(&small, 1);
		arena.EndChunk();
		CHECK_TRUE(col.buffer.size() == after_big, "capacity retained through the interval containing the outlier");
	}

	// The next interval contains only small chunks, so its evaluation releases.
	for (idx_t i = 0; i < StagingArena::SHRINK_INTERVAL_CHUNKS; i++) {
		col.BeginChunk(nullptr);
		col.AppendVar(&small, 1);
		arena.EndChunk();
	}
	CHECK_TRUE(col.buffer.size() < after_big, "outlier released after a full quiet interval");
	CHECK_TRUE(col.buffer.size() >= StagingArena::MIN_RETAINED_BYTES, "never shrinks below the floor");

	// And the column still works after the shrink.
	col.BeginChunk(nullptr);
	col.AppendVar(&small, 1);
	CHECK_EQ(col.LengthAt(0), 1u, "usable after shrink");
}

void TestColumnOpsResolution() {
	std::cout << "[7] ColumnOps: per-column classification (D4)..." << std::endl;
	using duckdb::LogicalType;
	using duckdb::tds::ColumnMetadata;
	using duckdb::tds::encoding::TypeConverter;

	auto meta = [](uint8_t type_id, int32_t max_length) {
		ColumnMetadata c;
		c.type_id = type_id;
		c.max_length = max_length;
		return c;
	};
	// Resolve against the type the wire itself implies — the agreeing case.
	auto resolve = [](const ColumnMetadata &c) { return ResolveColumnOps(c, TypeConverter::GetDuckDBType(c)); };

	// Integers and IEEE floats: wire bytes ARE the DuckDB representation.
	struct DirectCase {
		uint8_t type_id;
		int32_t max_length;
		uint32_t stride;
	};
	const DirectCase direct_cases[] = {
		{duckdb::tds::TDS_TYPE_TINYINT, 1, 1}, {duckdb::tds::TDS_TYPE_SMALLINT, 2, 2},
		{duckdb::tds::TDS_TYPE_INT, 4, 4},	   {duckdb::tds::TDS_TYPE_BIGINT, 8, 8},
		{duckdb::tds::TDS_TYPE_REAL, 4, 4},	   {duckdb::tds::TDS_TYPE_FLOAT, 8, 8},
		{duckdb::tds::TDS_TYPE_INTN, 1, 1},	   {duckdb::tds::TDS_TYPE_INTN, 2, 2},
		{duckdb::tds::TDS_TYPE_INTN, 4, 4},	   {duckdb::tds::TDS_TYPE_INTN, 8, 8},
		{duckdb::tds::TDS_TYPE_FLOATN, 4, 4},  {duckdb::tds::TDS_TYPE_FLOATN, 8, 8},
	};
	for (const auto &c : direct_cases) {
		const ColumnOps ops = resolve(meta(c.type_id, c.max_length));
		CHECK_TRUE(ops.direct_write, "direct-writable type takes the bypass");
		CHECK_TRUE(IsDirectKind(ops.kind), "direct-writable type gets a Direct arm");
		CHECK_TRUE(ops.kind == DirectKindForWidth(c.stride), "the Direct arm carries the width");
		CHECK_EQ(ops.stride, c.stride, "direct stride matches the wire width");
		CHECK_TRUE(!ops.needs_value_fallback, "direct type needs no per-value fallback");
	}

	// BIT is direct: TDS defines the value byte as exactly 0 or 1 (NULL lives in
	// BITN's length prefix), which is DuckDB's one-byte bool. Nothing to convert.
	const ColumnOps bit_ops = resolve(meta(duckdb::tds::TDS_TYPE_BIT, 1));
	CHECK_TRUE(bit_ops.direct_write, "BIT is direct-written — no third value exists to normalise");
	CHECK_EQ(bit_ops.stride, 1u, "BIT stride");

	// Fixed width, conversion required.
	CHECK_TRUE(resolve(meta(duckdb::tds::TDS_TYPE_UNIQUEIDENTIFIER, 16)).kind == StagingKind::Fixed,
			   "GUID is Fixed (byte-order swap)");
	CHECK_EQ(resolve(meta(duckdb::tds::TDS_TYPE_UNIQUEIDENTIFIER, 16)).stride, 16u, "GUID stride");
	CHECK_TRUE(resolve(meta(duckdb::tds::TDS_TYPE_MONEY, 8)).kind == StagingKind::Fixed, "MONEY is Fixed");

	// Variable length.
	CHECK_TRUE(resolve(meta(duckdb::tds::TDS_TYPE_NVARCHAR, 32)).kind == StagingKind::Var, "NVARCHAR is Var");
	CHECK_TRUE(resolve(meta(duckdb::tds::TDS_TYPE_BIGVARBINARY, 16)).kind == StagingKind::Var, "VARBINARY is Var");
	CHECK_TRUE(!resolve(meta(duckdb::tds::TDS_TYPE_NVARCHAR, 32)).direct_write, "Var is never direct-written");
	CHECK_EQ(resolve(meta(duckdb::tds::TDS_TYPE_NVARCHAR, 32)).max_value_bytes, 32u,
			 "declared width is carried through for preallocation");

	// The issue-#89 guard, resolved once per column: when the vector we write
	// into disagrees with what the wire implies, the column must fall back to
	// the per-value converter rather than take a batch path with the wrong
	// physical type.
	const ColumnOps mismatched = ResolveColumnOps(meta(duckdb::tds::TDS_TYPE_INT, 4), LogicalType::VARCHAR);
	CHECK_TRUE(mismatched.needs_value_fallback, "type disagreement forces the per-value fallback");
	CHECK_TRUE(!mismatched.direct_write, "type disagreement never direct-writes");
	CHECK_TRUE(mismatched.kind == StagingKind::Var, "fallback columns stage as Var");

	// An unknown wire type must resolve, not throw — the per-value path owns
	// that error so it can name the column.
	bool threw = false;
	try {
		const ColumnOps unknown = ResolveColumnOps(meta(0x00, 0), LogicalType::VARCHAR);
		CHECK_TRUE(unknown.needs_value_fallback, "unknown wire type falls back");
	} catch (...) {
		threw = true;
	}
	CHECK_TRUE(!threw, "unknown wire type does not throw during resolution");
}

void TestPreallocatedBoundNeverResizes() {
	std::cout << "[8] prealloc: a bounded column never resizes (D3)..." << std::endl;
	// nvarchar(16) is 32 wire bytes per value, so a chunk cannot exceed
	// 32 * STANDARD_VECTOR_SIZE. Preallocating that makes growth unreachable.
	const uint32_t per_value = 32;
	ColumnStaging col;
	col.Configure(StagingKind::Var, 0, per_value);
	CHECK_TRUE(col.payload_bounded, "narrow column reports a provable bound");
	const idx_t reserved = col.buffer.size();
	CHECK_EQ(reserved, static_cast<idx_t>(per_value) * STANDARD_VECTOR_SIZE, "reserved exactly the worst case");

	// Fill a full chunk with worst-case values: the buffer must not move.
	std::vector<uint8_t> value(per_value, 0x41);
	const uint8_t *base_before = col.buffer.data();
	col.BeginChunk(nullptr);
	for (idx_t i = 0; i < STANDARD_VECTOR_SIZE; i++) {
		col.AppendVar(value.data(), per_value);
	}
	CHECK_EQ(col.buffer.size(), reserved, "capacity unchanged after a worst-case chunk");
	CHECK_TRUE(col.buffer.data() == base_before, "payload never reallocated");
	CHECK_EQ(col.PayloadSize(), reserved, "worst-case chunk exactly fills the reservation");

	// The arena must not shrink it back into reach of growth.
	StagingArena arena;
	arena.Configure(1);
	arena.Column(0).Configure(StagingKind::Var, 0, per_value);
	const idx_t bounded_size = arena.Column(0).buffer.size();
	const uint8_t small = 1;
	for (idx_t i = 0; i < StagingArena::SHRINK_INTERVAL_CHUNKS * 2 + 2; i++) {
		arena.Column(0).BeginChunk(nullptr);
		arena.Column(0).AppendVar(&small, 1);
		arena.EndChunk();
	}
	CHECK_EQ(arena.Column(0).buffer.size(), bounded_size, "bounded column is never shrunk");

	// An unbounded column (PLP / MAX) has no such guarantee and starts small.
	ColumnStaging unbounded;
	unbounded.Configure(StagingKind::Var, 0, 0);
	CHECK_TRUE(!unbounded.payload_bounded, "PLP column has no provable bound");
	CHECK_EQ(unbounded.buffer.size(), duckdb::mssql::codec::staging::STAGING_UNBOUNDED_INITIAL_BYTES,
			 "unbounded column starts at the initial size");

	// A bound too large to prepay is not preallocated; growth handles it.
	ColumnStaging wide;
	wide.Configure(StagingKind::Var, 0, 8000);	// nvarchar(4000)
	CHECK_TRUE(!wide.payload_bounded, "over-budget bound is not prepaid");
}

}  // namespace

int main() {
	std::cout << "============================================================" << std::endl;
	std::cout << "codec::staging unit tests (spec 055 D3)" << std::endl;
	std::cout << "============================================================" << std::endl;

	try {
		TestVarAppendAndReadBack();
		TestFixedAndDirectAppend();
		TestValidityDefaultsToValid();
		TestPayloadCapIsChecked();
		TestArenaAddressStability();
		TestWatermarkRetainsThenReleases();
		TestColumnOpsResolution();
		TestPreallocatedBoundNeverResizes();
	} catch (const std::exception &e) {
		std::cerr << "UNCAUGHT EXCEPTION: " << e.what() << std::endl;
		return 2;
	}

	std::cout << "============================================================" << std::endl;
	if (failures > 0) {
		std::cerr << failures << " assertion(s) failed." << std::endl;
		return 1;
	}
	std::cout << "ALL TESTS PASSED" << std::endl;
	return 0;
}
