// test/cpp/bench_materialize.cpp
//
// Spec 054 D1: micro-benchmark harness for the materialization series
// (TDS → DuckDB chunk fill and DuckDB → BCP encode).
//
// T1 scope: harness skeleton + the string-decode group, CURRENT path only
// (codec::string::DecodeFromTds — Utf16LEDecode → std::string → AddString
// into a DataChunk, exactly the scan hot path). Strategy variants
// (A-lite / A) and the fixed-decode / bcp-encode groups land in later
// tasks; the fixture matrix already covers the gating cases for the
// deferred strategies B (value ending in a lone high surrogate) and C
// (embedded U+0000).
//
// Follows the bench_utf16.cpp pattern (spec 044): -O3 -DMSSQL_BENCH_BUILD,
// warm-up + median-of-N steady_clock timing, per-cell correctness
// assertion, manual target only (`make bench-materialize`) — NOT part of
// `make test` or any CI workflow.
//
// Metrics per cell (TSV): ns/value median, p10, p90, utf16 input bytes and
// utf8 output bytes per chunk. Allocation counting (design §7.1) needs
// malloc interposition and is deferred to the Linux baseline run.
//
// Cell design: the full cross of the design-§7.1 matrix is ~3.5k cells;
// the harness instead sweeps ONE dimension at a time around a base cell
// (len=16 code units, ascii, nulls 0%, cardinality 2048 = all-unique):
//   - length  {0,4,8,16,32,64,256,4096}   (12-byte string_t inline
//     threshold makes 4/8/16 mandatory)
//   - script  {ascii, cyrillic, cjk, surrogate}
//   - nulls   {0,10,50,100}%
//   - cardinality {1,2,10,100,101,512,2048}  (101 = one past the
//     mssql_scan_dictionary_max default planned in phase 2)
//   - flags   {embedded U+0000, trailing lone high surrogate}

#include "codec/datetime_codec.hpp"
#include "codec/decimal_codec.hpp"
#include "codec/float_codec.hpp"
#include "codec/integer_codec.hpp"
#include "codec/string_codec.hpp"
#include "codec/uuid_codec.hpp"
#include "copy/target_resolver.hpp"
#include "tds/encoding/bcp_row_encoder.hpp"
#include "tds/encoding/utf16.hpp"
#include "tds/tds_column_metadata.hpp"
#include "tds/tds_types.hpp"

#include "duckdb/common/allocator.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/types/selection_vector.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/types/vector.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace duckdb {
namespace mssql {

// The bench links bcp_row_encoder.cpp but NOT target_resolver.cpp (that TU
// drags in the catalog / ClientContext). EncodeRow's NULL path needs this one
// method; the body mirrors target_resolver.cpp verbatim.
bool BCPColumnMetadata::IsVariableLengthUSHORT() const {
	// NVARCHARTYPE (0xE7) and BIGVARBINARYTYPE (0xA5) use USHORTLEN
	return tds_type_token == 0xE7 || tds_type_token == 0xA5;
}

}  // namespace mssql
}  // namespace duckdb

using duckdb::DataChunk;
using duckdb::FlatVector;
using duckdb::idx_t;
using duckdb::LogicalType;
using duckdb::string_t;
using duckdb::Vector;

namespace {

constexpr idx_t CHUNK_ROWS = 2048;	// STANDARD_VECTOR_SIZE

//===----------------------------------------------------------------------===//
// Fixture generation — UTF-16LE payloads, one per row of a 2048-row chunk.
//===----------------------------------------------------------------------===//

enum class Script { Ascii, Cyrillic, Cjk, Surrogate };

struct CellSpec {
	std::string name;
	size_t len_units = 16;	// UTF-16 code units per value
	Script script = Script::Ascii;
	int null_pct = 0;			   // % of NULL rows
	size_t cardinality = 2048;	   // distinct values in the chunk
	bool embedded_nul = false;	   // strategy-C gating case
	bool lone_high_surr = false;   // strategy-B gating case (invalid UTF-16 tail)
};

void AppendUnit(std::vector<uint8_t> &buf, uint16_t unit) {
	buf.push_back(static_cast<uint8_t>(unit & 0xFF));
	buf.push_back(static_cast<uint8_t>(unit >> 8));
}

// Build one UTF-16LE payload. `distinct_id` is mixed into the tail as hex
// digits so cells with cardinality > 1 get genuinely distinct byte content.
std::vector<uint8_t> BuildPayload(const CellSpec &c, size_t distinct_id) {
	std::vector<uint8_t> buf;
	buf.reserve(c.len_units * 2);

	size_t units_left = c.len_units;

	// Reserve up to 4 tail units for the distinct-id hex digits (fewer when
	// the value is shorter; len 0 stays empty → cardinality collapses to 1).
	size_t id_units = std::min<size_t>(4, units_left);

	size_t body_units = units_left - id_units;
	size_t i = 0;
	while (i < body_units) {
		switch (c.script) {
		case Script::Ascii:
			AppendUnit(buf, static_cast<uint16_t>('a' + (i % 26)));
			i += 1;
			break;
		case Script::Cyrillic:
			AppendUnit(buf, static_cast<uint16_t>(0x0410 + (i % 32)));	// А..Я
			i += 1;
			break;
		case Script::Cjk:
			AppendUnit(buf, static_cast<uint16_t>(0x4E00 + (i % 256)));
			i += 1;
			break;
		case Script::Surrogate:
			if (i + 2 <= body_units) {
				// U+1F600.. as a surrogate pair.
				AppendUnit(buf, 0xD83D);
				AppendUnit(buf, static_cast<uint16_t>(0xDE00 + (i % 64)));
				i += 2;
			} else {
				AppendUnit(buf, static_cast<uint16_t>('a' + (i % 26)));
				i += 1;
			}
			break;
		}
	}

	for (size_t d = 0; d < id_units; ++d) {
		AppendUnit(buf, static_cast<uint16_t>("0123456789abcdef"[(distinct_id >> (4 * d)) & 0xF]));
	}

	if (c.embedded_nul && buf.size() >= 4) {
		// Overwrite the first unit with U+0000 (valid UTF-16, embedded NUL).
		buf[0] = 0x00;
		buf[1] = 0x00;
	}
	if (c.lone_high_surr && buf.size() >= 2) {
		// Overwrite the LAST unit with an unpaired high surrogate — invalid
		// UTF-16; the current path must route through the legacy fallback.
		buf[buf.size() - 2] = 0x00;
		buf[buf.size() - 1] = 0xD8;
	}
	return buf;
}

struct Fixture {
	CellSpec spec;
	// Per-row payloads (empty vector at index i + null_mask[i] => NULL row).
	std::vector<std::vector<uint8_t>> rows;
	std::vector<bool> null_mask;
	size_t utf16_bytes = 0;	 // total non-NULL input bytes per chunk
};

Fixture BuildFixture(const CellSpec &c) {
	Fixture f;
	f.spec = c;
	f.rows.reserve(CHUNK_ROWS);
	f.null_mask.reserve(CHUNK_ROWS);

	// Distinct payload pool.
	size_t card = std::max<size_t>(1, std::min<size_t>(c.cardinality, CHUNK_ROWS));
	std::vector<std::vector<uint8_t>> pool;
	pool.reserve(card);
	for (size_t v = 0; v < card; ++v) {
		pool.push_back(BuildPayload(c, v));
	}

	for (idx_t row = 0; row < CHUNK_ROWS; ++row) {
		// Deterministic NULL spread: null_pct rows out of every 100, striped.
		bool is_null = c.null_pct >= 100 || (c.null_pct > 0 && (row % 100) < static_cast<idx_t>(c.null_pct));
		f.null_mask.push_back(is_null);
		if (is_null) {
			f.rows.emplace_back();
		} else {
			f.rows.push_back(pool[row % card]);
			f.utf16_bytes += f.rows.back().size();
		}
	}
	return f;
}

//===----------------------------------------------------------------------===//
// Timed body — the CURRENT scan path, per value:
// codec::string::DecodeFromTds (Utf16LEDecode → std::string → AddString).
//===----------------------------------------------------------------------===//

volatile uint64_t g_sink = 0;

// One chunk fill. Returns total utf8 output bytes (fed to g_sink so the
// optimizer cannot drop the work).
size_t FillChunkCurrent(const Fixture &f, DataChunk &chunk, const duckdb::tds::ColumnMetadata &col) {
	chunk.Reset();
	auto &vec = chunk.data[0];
	size_t out_bytes = 0;
	for (idx_t row = 0; row < CHUNK_ROWS; ++row) {
		if (f.null_mask[row]) {
			FlatVector::SetNull(vec, row, true);
			continue;
		}
		duckdb::mssql::codec::string::DecodeFromTds(f.rows[row], col, vec, row);
		out_bytes += FlatVector::GetData<string_t>(vec)[row].GetSize();
	}
	chunk.SetCardinality(CHUNK_ROWS);
	g_sink ^= out_bytes;
	return out_bytes;
}

//===----------------------------------------------------------------------===//
// Correctness: the vector contents must equal a direct per-value reference
// decode (later strategy variants are asserted against the same reference).
//===----------------------------------------------------------------------===//

bool VerifyChunk(const Fixture &f, DataChunk &chunk, const duckdb::tds::ColumnMetadata &col) {
	auto &vec = chunk.data[0];
	for (idx_t row = 0; row < CHUNK_ROWS; ++row) {
		if (f.null_mask[row]) {
			if (!FlatVector::IsNull(vec, row)) {
				return false;
			}
			continue;
		}
		std::string expect = duckdb::tds::encoding::Utf16LEDecode(f.rows[row].data(), f.rows[row].size());
		auto got = FlatVector::GetData<string_t>(vec)[row];
		if (std::string(got.GetData(), got.GetSize()) != expect) {
			return false;
		}
	}
	return true;
}

//===----------------------------------------------------------------------===//
// Timing: warm-up + median / p10 / p90 of N iterations.
//===----------------------------------------------------------------------===//

// One sample = one full 2048-row chunk fill. Reported at BOTH granularities:
// per-chunk (µs — what FillChunk actually costs per iteration) and per-value
// (ns — comparable across cells with different NULL ratios only via the
// chunk number, since NULL rows still occupy a slot).
struct CellResult {
	double median_us_per_chunk;
	double p10_us_per_chunk;
	double p90_us_per_chunk;
	double median_ns_per_value;	 // median_us_per_chunk * 1000 / CHUNK_ROWS
};

template <typename Fn>
CellResult TimeCell(Fn fn, size_t iterations) {
	using clock = std::chrono::steady_clock;
	for (size_t w = 0; w < 10; ++w) {
		fn();
	}
	std::vector<double> samples;  // ns per chunk
	samples.reserve(iterations);
	for (size_t i = 0; i < iterations; ++i) {
		const auto t0 = clock::now();
		fn();
		const auto t1 = clock::now();
		samples.push_back(static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()));
	}
	std::sort(samples.begin(), samples.end());
	CellResult r;
	r.median_us_per_chunk = samples[samples.size() / 2] / 1e3;
	r.p10_us_per_chunk = samples[samples.size() / 10] / 1e3;
	r.p90_us_per_chunk = samples[(samples.size() * 9) / 10] / 1e3;
	r.median_ns_per_value = samples[samples.size() / 2] / static_cast<double>(CHUNK_ROWS);
	return r;
}

size_t IterationsFor(size_t utf16_bytes) {
	// Keep each cell around a comparable wall budget: big payloads run fewer
	// iterations. ~1 MB per chunk → 50; small cells → 400.
	return (utf16_bytes >= 1 << 20) ? 50 : 400;
}

//===----------------------------------------------------------------------===//
// Cell list — base + one-dimension sweeps (see header comment).
//===----------------------------------------------------------------------===//

std::vector<CellSpec> BuildCells() {
	std::vector<CellSpec> cells;
	auto add = [&](CellSpec c, const std::string &name) {
		c.name = name;
		cells.push_back(c);
	};

	CellSpec base;	// len=16 ascii null0 card2048

	for (size_t len : {0, 4, 8, 16, 32, 64, 256, 4096}) {
		CellSpec c = base;
		c.len_units = len;
		add(c, "len" + std::to_string(len) + "_ascii_null0_card2048");
	}
	{
		CellSpec c = base;
		c.script = Script::Cyrillic;
		add(c, "len16_cyrillic_null0_card2048");
		c.script = Script::Cjk;
		add(c, "len16_cjk_null0_card2048");
		c.script = Script::Surrogate;
		add(c, "len16_surrogate_null0_card2048");
	}
	for (int nulls : {10, 50, 100}) {
		CellSpec c = base;
		c.null_pct = nulls;
		add(c, "len16_ascii_null" + std::to_string(nulls) + "_card2048");
	}
	for (size_t card : {1, 2, 10, 100, 101, 512}) {
		CellSpec c = base;
		c.cardinality = card;
		add(c, "len16_ascii_null0_card" + std::to_string(card));
	}
	{
		CellSpec c = base;
		c.embedded_nul = true;
		add(c, "len16_ascii_embedded_nul");
	}
	{
		CellSpec c = base;
		c.lone_high_surr = true;
		add(c, "len16_ascii_lone_high_surrogate");
	}
	return cells;
}

//===----------------------------------------------------------------------===//
// Fixed-decode group (T6): per-family CURRENT per-value decode path.
// Wire payloads are generated from a deterministic per-row value so the
// integer/float/decimal cells can be verified against independently
// reconstructed expectations; datetime/uuid cells assert determinism
// (two decodes produce identical chunks).
//===----------------------------------------------------------------------===//

using DecodeFn = void (*)(const std::vector<uint8_t> &, const duckdb::tds::ColumnMetadata &, Vector &, idx_t);

struct FixedCell {
	std::string name;
	LogicalType type;
	duckdb::tds::ColumnMetadata col;
	DecodeFn decode;
	std::vector<std::vector<uint8_t>> rows;	 // per-row wire payloads
	size_t in_bytes = 0;
	int null_pct = 0;
};

void AppendLe(std::vector<uint8_t> &b, uint64_t v, int n) {
	for (int i = 0; i < n; ++i) {
		b.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
	}
}

uint64_t RowValue(idx_t row) {
	return row * 2654435761ULL + 17;  // deterministic, mixes bits
}

FixedCell MakeFixedCell(const std::string &name, LogicalType type, uint8_t tds_type, uint8_t precision, uint8_t scale,
						DecodeFn decode, size_t wire_bytes_per_value, int null_pct = 0) {
	FixedCell c;
	c.name = name;
	c.type = type;
	c.col.name = "c";
	c.col.type_id = tds_type;
	c.col.max_length = static_cast<uint16_t>(wire_bytes_per_value);
	c.col.precision = precision;
	c.col.scale = scale;
	c.col.collation = 0;
	c.col.flags = 0;
	c.decode = decode;
	c.null_pct = null_pct;
	c.rows.reserve(CHUNK_ROWS);
	for (idx_t row = 0; row < CHUNK_ROWS; ++row) {
		if (null_pct >= 100 || (null_pct > 0 && (row % 100) < static_cast<idx_t>(null_pct))) {
			c.rows.emplace_back();	// NULL
			continue;
		}
		std::vector<uint8_t> b;
		uint64_t v = RowValue(row);
		switch (tds_type) {
		case duckdb::tds::TDS_TYPE_INTN:
			AppendLe(b, v, static_cast<int>(wire_bytes_per_value));
			break;
		case duckdb::tds::TDS_TYPE_FLOATN: {
			double d = static_cast<double>(v) * 1.5;
			uint64_t bits;
			std::memcpy(&bits, &d, 8);
			AppendLe(b, bits, 8);
			break;
		}
		case duckdb::tds::TDS_TYPE_DATETIME2: {
			// scale 6: 5-byte time (µs units) + 3-byte date (days since 0001-01-01)
			AppendLe(b, v % 86400000000ULL, 5);
			AppendLe(b, 738000 + (v % 1000), 3);
			break;
		}
		case duckdb::tds::TDS_TYPE_DECIMAL: {
			// sign + LE mantissa (bucket size = wire_bytes_per_value - 1)
			b.push_back(0x01);
			uint64_t mantissa = (precision <= 4) ? (v % 9999) : v;
			AppendLe(b, mantissa, static_cast<int>(wire_bytes_per_value) - 1);
			break;
		}
		case duckdb::tds::TDS_TYPE_UNIQUEIDENTIFIER:
			AppendLe(b, v, 8);
			AppendLe(b, ~v, 8);
			break;
		default:
			break;
		}
		c.in_bytes += b.size();
		c.rows.push_back(std::move(b));
	}
	return c;
}

size_t FillChunkFixed(const FixedCell &c, DataChunk &chunk) {
	chunk.Reset();
	auto &vec = chunk.data[0];
	for (idx_t row = 0; row < CHUNK_ROWS; ++row) {
		if (c.rows[row].empty()) {
			FlatVector::SetNull(vec, row, true);
			continue;
		}
		c.decode(c.rows[row], c.col, vec, row);
	}
	chunk.SetCardinality(CHUNK_ROWS);
	g_sink ^= CHUNK_ROWS;
	return c.in_bytes;
}

// Integer/float/decimal: verify against independently reconstructed values.
// Datetime/uuid: verify two decodes agree (determinism).
bool VerifyFixed(const FixedCell &c, DataChunk &chunk) {
	auto &vec = chunk.data[0];
	for (idx_t row = 0; row < CHUNK_ROWS; ++row) {
		bool expect_null = c.rows[row].empty();
		if (FlatVector::IsNull(vec, row) != expect_null) {
			return false;
		}
		if (expect_null) {
			continue;
		}
		uint64_t v = RowValue(row);
		switch (c.type.id()) {
		case duckdb::LogicalTypeId::BIGINT:
			if (FlatVector::GetData<int64_t>(vec)[row] != static_cast<int64_t>(v)) {
				return false;
			}
			break;
		case duckdb::LogicalTypeId::INTEGER:
			if (FlatVector::GetData<int32_t>(vec)[row] != static_cast<int32_t>(v & 0xFFFFFFFF)) {
				return false;
			}
			break;
		case duckdb::LogicalTypeId::SMALLINT:
			if (FlatVector::GetData<int16_t>(vec)[row] != static_cast<int16_t>(v & 0xFFFF)) {
				return false;
			}
			break;
		case duckdb::LogicalTypeId::UTINYINT:
			if (FlatVector::GetData<uint8_t>(vec)[row] != static_cast<uint8_t>(v & 0xFF)) {
				return false;
			}
			break;
		case duckdb::LogicalTypeId::DOUBLE:
			if (FlatVector::GetData<double>(vec)[row] != static_cast<double>(v) * 1.5) {
				return false;
			}
			break;
		default:
			break;	// datetime/uuid/decimal buckets: determinism check below
		}
	}
	// Determinism: a second decode into a fresh chunk must be value-identical.
	DataChunk chunk2;
	chunk2.Initialize(duckdb::Allocator::DefaultAllocator(), {c.type});
	FillChunkFixed(c, chunk2);
	for (idx_t row = 0; row < CHUNK_ROWS; ++row) {
		if (c.rows[row].empty()) {
			continue;  // NULL rows verified via the mask above; Value comparison on NULLs throws
		}
		if (chunk.data[0].GetValue(row) != chunk2.data[0].GetValue(row)) {
			return false;
		}
	}
	return true;
}

std::vector<FixedCell> BuildFixedCells() {
	namespace t = duckdb::tds;
	std::vector<FixedCell> cells;
	cells.push_back(MakeFixedCell("int1_utinyint", LogicalType::UTINYINT, t::TDS_TYPE_INTN, 0, 0,
								  duckdb::mssql::codec::integer::DecodeFromTds, 1));
	cells.push_back(MakeFixedCell("int2_smallint", LogicalType::SMALLINT, t::TDS_TYPE_INTN, 0, 0,
								  duckdb::mssql::codec::integer::DecodeFromTds, 2));
	cells.push_back(MakeFixedCell("int4_integer", LogicalType::INTEGER, t::TDS_TYPE_INTN, 0, 0,
								  duckdb::mssql::codec::integer::DecodeFromTds, 4));
	cells.push_back(MakeFixedCell("int8_bigint", LogicalType::BIGINT, t::TDS_TYPE_INTN, 0, 0,
								  duckdb::mssql::codec::integer::DecodeFromTds, 8));
	cells.push_back(MakeFixedCell("int8_bigint_null50", LogicalType::BIGINT, t::TDS_TYPE_INTN, 0, 0,
								  duckdb::mssql::codec::integer::DecodeFromTds, 8, 50));
	cells.push_back(MakeFixedCell("float8_double", LogicalType::DOUBLE, t::TDS_TYPE_FLOATN, 0, 0,
								  duckdb::mssql::codec::float_family::DecodeFromTds, 8));
	cells.push_back(MakeFixedCell("datetime2_s6_timestamp", LogicalType::TIMESTAMP, t::TDS_TYPE_DATETIME2, 0, 6,
								  duckdb::mssql::codec::datetime::DecodeFromTds, 8));
	cells.push_back(MakeFixedCell("decimal_p4s2_int16", LogicalType::DECIMAL(4, 2), t::TDS_TYPE_DECIMAL, 4, 2,
								  duckdb::mssql::codec::decimal::DecodeFromTds, 5));
	cells.push_back(MakeFixedCell("decimal_p18s6_int64", LogicalType::DECIMAL(18, 6), t::TDS_TYPE_DECIMAL, 18, 6,
								  duckdb::mssql::codec::decimal::DecodeFromTds, 9));
	cells.push_back(MakeFixedCell("decimal_p38s0_int128", LogicalType::DECIMAL(38, 0), t::TDS_TYPE_DECIMAL, 38, 0,
								  duckdb::mssql::codec::decimal::DecodeFromTds, 17));
	cells.push_back(MakeFixedCell("uuid", LogicalType::UUID, t::TDS_TYPE_UNIQUEIDENTIFIER, 0, 0,
								  duckdb::mssql::codec::uuid::DecodeFromTds, 16));
	return cells;
}

//===----------------------------------------------------------------------===//
// BCP-encode group (T6 part 2): the CURRENT write path — one
// BCPRowEncoder::EncodeRow call per row (family dispatch; per-cell
// ToUnifiedFormat in BOTH the NULL check and the codec value read), on
// flat-unique / flat-low-cardinality / dictionary {1,10,100} / constant
// inputs × NULL ratios. Dictionary inputs are built with
// Vector::Dictionary(dict, dict_size, sel, count) — the same construction
// the phase-2 scan will emit and the phase-3 representation-aware encoder
// will detect via DictionaryVector::DictionarySize; the current path is
// expected to be representation-blind (dict ≈ flat at equal value bytes).
//===----------------------------------------------------------------------===//

enum class VecRep { FlatUnique, FlatLowCard, Dict, Constant };

struct BcpCellSpec {
	enum class Kind { Bigint, Varchar16, Decimal18s6 };
	std::string name;
	Kind kind = Kind::Bigint;
	VecRep rep = VecRep::FlatUnique;
	size_t card = 2048;	 // distinct values (FlatLowCard / Dict)
	int null_pct = 0;
};

bool RowIsNull(int null_pct, idx_t row) {
	return null_pct >= 100 || (null_pct > 0 && (row % 100) < static_cast<idx_t>(null_pct));
}

LogicalType BcpTypeFor(BcpCellSpec::Kind k) {
	switch (k) {
	case BcpCellSpec::Kind::Bigint:
		return LogicalType::BIGINT;
	case BcpCellSpec::Kind::Varchar16:
		return LogicalType::VARCHAR;
	case BcpCellSpec::Kind::Decimal18s6:
		return LogicalType::DECIMAL(18, 6);
	}
	return LogicalType::BIGINT;
}

duckdb::Value MakeBcpValue(BcpCellSpec::Kind k, size_t id) {
	using duckdb::Value;
	switch (k) {
	case BcpCellSpec::Kind::Bigint:
		return Value::BIGINT(static_cast<int64_t>(RowValue(id)));
	case BcpCellSpec::Kind::Varchar16: {
		// 16 ASCII chars: 8-char body + 8 hex digits of the distinct id.
		char s[17];
		std::snprintf(s, sizeof(s), "abcdefgh%08lx", static_cast<unsigned long>(id) & 0xFFFFFFFFUL);
		return Value(std::string(s, 16));
	}
	case BcpCellSpec::Kind::Decimal18s6:
		// Mantissa < 10^18 so the precision-18 range guard never fires.
		return Value::DECIMAL(static_cast<int64_t>(RowValue(id) % 1000000000000000000ULL), 18, 6);
	}
	return Value();
}

struct BcpFixture {
	BcpCellSpec spec;
	std::unique_ptr<Vector> dict_base;	// keep the dictionary child alive
	std::unique_ptr<DataChunk> chunk;
	duckdb::vector<duckdb::mssql::BCPColumnMetadata> cols;
};

BcpFixture BuildBcpFixture(const BcpCellSpec &s) {
	BcpFixture f;
	f.spec = s;
	LogicalType type = BcpTypeFor(s.kind);
	f.chunk.reset(new DataChunk());
	f.chunk->Initialize(duckdb::Allocator::DefaultAllocator(), {type});
	auto &vec = f.chunk->data[0];

	switch (s.rep) {
	case VecRep::FlatUnique:
	case VecRep::FlatLowCard: {
		const size_t card = (s.rep == VecRep::FlatUnique) ? CHUNK_ROWS : s.card;
		for (idx_t row = 0; row < CHUNK_ROWS; ++row) {
			vec.SetValue(row, RowIsNull(s.null_pct, row) ? duckdb::Value(type) : MakeBcpValue(s.kind, row % card));
		}
		break;
	}
	case VecRep::Dict: {
		// NULL rows point at a trailing NULL child slot — the emission shape
		// planned for the phase-2 scan.
		const bool has_null = s.null_pct > 0;
		const idx_t dict_size = s.card + (has_null ? 1 : 0);
		f.dict_base.reset(new Vector(type, dict_size));
		for (idx_t i = 0; i < s.card; ++i) {
			f.dict_base->SetValue(i, MakeBcpValue(s.kind, i));
		}
		if (has_null) {
			f.dict_base->SetValue(s.card, duckdb::Value(type));
		}
		duckdb::SelectionVector sel(CHUNK_ROWS);
		for (idx_t row = 0; row < CHUNK_ROWS; ++row) {
			sel.set_index(row, RowIsNull(s.null_pct, row) ? s.card : (row % s.card));
		}
		vec.Dictionary(*f.dict_base, dict_size, sel, CHUNK_ROWS);
		break;
	}
	case VecRep::Constant:
		vec.Reference(s.null_pct >= 100 ? duckdb::Value(type) : MakeBcpValue(s.kind, 0));
		break;
	}
	f.chunk->SetCardinality(CHUNK_ROWS);

	duckdb::mssql::BCPColumnMetadata col("c", type, true);
	switch (s.kind) {
	case BcpCellSpec::Kind::Bigint:
		col.tds_type_token = 0x26;	// INTNTYPE
		col.max_length = 8;
		break;
	case BcpCellSpec::Kind::Varchar16:
		col.tds_type_token = 0xE7;	// NVARCHARTYPE, nvarchar(16) — non-PLP USHORT path
		col.max_length = 32;
		break;
	case BcpCellSpec::Kind::Decimal18s6:
		col.tds_type_token = 0x6A;	// DECIMALNTYPE
		col.max_length = 9;
		col.precision = 18;
		col.scale = 6;
		break;
	}
	f.cols.push_back(col);
	return f;
}

// One chunk encode — production shape: EncodeRow per row into one buffer
// (clear() keeps capacity, matching the reused per-batch packet buffer).
size_t EncodeChunkCurrent(BcpFixture &f, duckdb::vector<uint8_t> &buf) {
	buf.clear();
	for (idx_t row = 0; row < CHUNK_ROWS; ++row) {
		duckdb::tds::encoding::BCPRowEncoder::EncodeRow(buf, *f.chunk, row, f.cols, nullptr);
	}
	g_sink ^= buf.size();
	return buf.size();
}

// Reference: the Value-based encoder over GetValue(row) (resolves dict /
// constant indirection independently). Byte-identical output required.
// DECIMAL is the one arm where the Value overload is documented as divergent
// from the Vector overload (legacy parity: Value::GetValue<hugeint_t>()
// rescales the mantissa — see codec::decimal::EncodeToBcp(Value...)), so the
// reference encodes the raw mantissa directly for that kind.
bool VerifyBcp(BcpFixture &f, const duckdb::vector<uint8_t> &got) {
	using duckdb::tds::encoding::BCPRowEncoder;
	duckdb::vector<uint8_t> ref;
	for (idx_t row = 0; row < CHUNK_ROWS; ++row) {
		duckdb::Value v = f.chunk->data[0].GetValue(row);
		if (f.spec.kind == BcpCellSpec::Kind::Decimal18s6 && !v.IsNull()) {
			BCPRowEncoder::EncodeDecimal(ref, duckdb::hugeint_t(v.GetValueUnsafe<int64_t>()), f.cols[0].precision,
										 f.cols[0].scale);
		} else {
			BCPRowEncoder::EncodeValue(ref, v, f.cols[0]);
		}
	}
	return ref.size() == got.size() && std::equal(ref.begin(), ref.end(), got.begin());
}

std::vector<BcpCellSpec> BuildBcpCells() {
	using K = BcpCellSpec::Kind;
	std::vector<BcpCellSpec> cells;
	auto add = [&](K k, VecRep rep, size_t card, int nulls, const std::string &name) {
		BcpCellSpec s;
		s.kind = k;
		s.rep = rep;
		s.card = card;
		s.null_pct = nulls;
		s.name = name;
		cells.push_back(s);
	};

	struct KindRow {
		K kind;
		const char *prefix;
	};
	for (const KindRow &kr : {KindRow {K::Bigint, "bigint"}, KindRow {K::Varchar16, "nvarchar16"}}) {
		add(kr.kind, VecRep::FlatUnique, 2048, 0, std::string(kr.prefix) + "_flat_unique");
		add(kr.kind, VecRep::FlatLowCard, 10, 0, std::string(kr.prefix) + "_flat_card10");
		for (size_t card : {1, 10, 100}) {
			add(kr.kind, VecRep::Dict, card, 0, std::string(kr.prefix) + "_dict" + std::to_string(card));
		}
		add(kr.kind, VecRep::Constant, 1, 0, std::string(kr.prefix) + "_const");
		add(kr.kind, VecRep::FlatUnique, 2048, 50, std::string(kr.prefix) + "_flat_unique_null50");
	}
	add(K::Bigint, VecRep::Dict, 100, 50, "bigint_dict100_null50");
	add(K::Bigint, VecRep::Constant, 1, 100, "bigint_const_null");
	add(K::Decimal18s6, VecRep::FlatUnique, 2048, 0, "decimal18s6_flat_unique");
	add(K::Decimal18s6, VecRep::Dict, 100, 0, "decimal18s6_dict100");
	add(K::Decimal18s6, VecRep::Constant, 1, 0, "decimal18s6_const");
	return cells;
}

}  // namespace

int main() {
	std::printf("[bench_materialize] spec 054 D1 — string-decode / fixed-decode / bcp-encode groups (current path)\n");
	std::printf("group\tcell\tus_per_chunk_median\tus_p10\tus_p90\tns_per_value_median"
				"\tutf16_in_bytes\tutf8_out_bytes\tcorrect\n");

	duckdb::tds::ColumnMetadata col;
	col.name = "c";
	col.type_id = duckdb::tds::TDS_TYPE_NVARCHAR;
	col.max_length = 0x1FFE;
	col.precision = 0;
	col.scale = 0;
	col.collation = 0;
	col.flags = 0;

	int failures = 0;
	auto cells = BuildCells();
	for (const auto &spec : cells) {
		Fixture f = BuildFixture(spec);

		DataChunk chunk;
		chunk.Initialize(duckdb::Allocator::DefaultAllocator(), {LogicalType::VARCHAR});

		// Correctness once, outside timing.
		size_t out_bytes = FillChunkCurrent(f, chunk, col);
		bool correct = VerifyChunk(f, chunk, col);
		if (!correct) {
			failures++;
		}

		auto r = TimeCell([&]() { FillChunkCurrent(f, chunk, col); }, IterationsFor(f.utf16_bytes));

		std::printf("string_decode_current\t%s\t%.1f\t%.1f\t%.1f\t%.1f\t%zu\t%zu\t%s\n", spec.name.c_str(),
					r.median_us_per_chunk, r.p10_us_per_chunk, r.p90_us_per_chunk, r.median_ns_per_value,
					f.utf16_bytes, out_bytes, correct ? "PASS" : "FAIL");
	}

	for (const auto &fc : BuildFixedCells()) {
		try {
		DataChunk chunk;
		chunk.Initialize(duckdb::Allocator::DefaultAllocator(), {fc.type});

		FillChunkFixed(fc, chunk);
		bool correct = VerifyFixed(fc, chunk);
		if (!correct) {
			failures++;
		}

		auto r = TimeCell([&]() { FillChunkFixed(fc, chunk); }, 400);

		std::printf("fixed_decode_current\t%s\t%.1f\t%.1f\t%.1f\t%.1f\t%zu\t-\t%s\n", fc.name.c_str(),
					r.median_us_per_chunk, r.p10_us_per_chunk, r.p90_us_per_chunk, r.median_ns_per_value, fc.in_bytes,
					correct ? "PASS" : "FAIL");
		} catch (std::exception &ex) {
			std::fprintf(stderr, "cell %s threw: %s\n", fc.name.c_str(), ex.what());
			failures++;
		}
	}

	for (const auto &bspec : BuildBcpCells()) {
		try {
			BcpFixture f = BuildBcpFixture(bspec);
			duckdb::vector<uint8_t> buf;
			buf.reserve(1 << 20);

			size_t out_bytes = EncodeChunkCurrent(f, buf);
			bool correct = VerifyBcp(f, buf);
			if (!correct) {
				failures++;
			}

			auto r = TimeCell([&]() { EncodeChunkCurrent(f, buf); }, 400);

			std::printf("bcp_encode_current\t%s\t%.1f\t%.1f\t%.1f\t%.1f\t-\t%zu\t%s\n", bspec.name.c_str(),
						r.median_us_per_chunk, r.p10_us_per_chunk, r.p90_us_per_chunk, r.median_ns_per_value, out_bytes,
						correct ? "PASS" : "FAIL");
		} catch (std::exception &ex) {
			std::fprintf(stderr, "cell %s threw: %s\n", bspec.name.c_str(), ex.what());
			failures++;
		}
	}

	if (failures > 0) {
		std::fprintf(stderr, "\n%d cell(s) FAILED correctness.\n", failures);
		return 1;
	}
	std::printf("\n[bench_materialize] all cells correct. sink=%llu\n", (unsigned long long)g_sink);
	return 0;
}
