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
#include "tds/encoding/decimal_encoding.hpp"
#include "tds/encoding/utf16.hpp"
#include "tds/tds_column_metadata.hpp"
#include "tds/tds_types.hpp"

#include "duckdb/common/allocator.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/types/selection_vector.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/vector/dictionary_vector.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/string_vector.hpp"

#include <simdutf.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
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
	int null_pct = 0;			  // % of NULL rows
	size_t cardinality = 2048;	  // distinct values in the chunk
	bool embedded_nul = false;	  // strategy-C gating case
	bool lone_high_surr = false;  // strategy-B gating case (invalid UTF-16 tail)
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
		out_bytes += FlatVector::GetDataMutable<string_t>(vec)[row].GetSize();
	}
	chunk.SetChildCardinality(CHUNK_ROWS);
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
		auto got = FlatVector::GetDataMutable<string_t>(vec)[row];
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
	// Long non-ASCII: the boundary-scan skip-ahead can only pay off when the
	// skipped prefix is long, so the fixture matrix needs these cells.
	for (size_t len : {24, 32, 48, 64, 256}) {
		CellSpec c = base;
		c.len_units = len;
		c.script = Script::Cyrillic;
		add(c, "len" + std::to_string(len) + "_cyrillic_null0_card2048");
		c.script = Script::Cjk;
		add(c, "len" + std::to_string(len) + "_cjk_null0_card2048");
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
	chunk.SetChildCardinality(CHUNK_ROWS);
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
			if (FlatVector::GetDataMutable<int64_t>(vec)[row] != static_cast<int64_t>(v)) {
				return false;
			}
			break;
		case duckdb::LogicalTypeId::INTEGER:
			if (FlatVector::GetDataMutable<int32_t>(vec)[row] != static_cast<int32_t>(v & 0xFFFFFFFF)) {
				return false;
			}
			break;
		case duckdb::LogicalTypeId::SMALLINT:
			if (FlatVector::GetDataMutable<int16_t>(vec)[row] != static_cast<int16_t>(v & 0xFFFF)) {
				return false;
			}
			break;
		case duckdb::LogicalTypeId::UTINYINT:
			if (FlatVector::GetDataMutable<uint8_t>(vec)[row] != static_cast<uint8_t>(v & 0xFF)) {
				return false;
			}
			break;
		case duckdb::LogicalTypeId::DOUBLE:
			if (FlatVector::GetDataMutable<double>(vec)[row] != static_cast<double>(v) * 1.5) {
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
		vec.Reference(s.null_pct >= 100 ? duckdb::Value(type) : MakeBcpValue(s.kind, 0), duckdb::count_t(CHUNK_ROWS));
		break;
	}
	f.chunk->SetChildCardinality(CHUNK_ROWS);

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

// Per-row compatibility path: EncodeRow per row into one buffer (clear()
// keeps capacity). Pre-W1 this was the production shape (group
// bcp_encode_current in the baseline tables); post-W1 it is the per-row
// API (one format build per cell) and reports as bcp_encode_perrow.
size_t EncodeChunkPerRow(BcpFixture &f, duckdb::vector<uint8_t> &buf) {
	buf.clear();
	for (idx_t row = 0; row < CHUNK_ROWS; ++row) {
		duckdb::tds::encoding::BCPRowEncoder::EncodeRow(buf, *f.chunk, row, f.cols, nullptr);
	}
	g_sink ^= buf.size();
	return buf.size();
}

// Production shape post-W1/W2: one EncodeChunk call — per-column format /
// encoder / NULL-kind hoisted once, 0xD1 ROW token written per row.
size_t EncodeChunkHoisted(BcpFixture &f, duckdb::vector<uint8_t> &buf) {
	buf.clear();
	duckdb::tds::encoding::BCPRowEncoder::EncodeChunk(buf, *f.chunk, f.cols, nullptr);
	g_sink ^= buf.size();
	return buf.size();
}

// Spec 057 D5a probe: columnar write into an all-fixed-width chunk.
//
// This answers the objection that sizing needs a pass over the chunk. It does
// not. A fixed-width column's wire length is metadata (1 length byte +
// max_length), and a NULL contributes only its 1-byte marker — so the VALIDITY
// MASK alone yields every row's size, without reading one value. When the mask
// is AllValid the stride is constant and there is no sizing step at all.
//
// The buffer is then sized once and each column scatters straight into it: one
// length-byte store and one width-sized store per value through a raw pointer.
// No capacity check, no indirect call, no per-value dispatch — the per-byte
// push_back chain in the shipped codec is 9 capacity checks for a BIGINT.
//
// Integer family only, deliberately: bigint is the CHEAPEST cell in this group
// (8.2 ns/value shipped), so it is the hardest case for any shape that adds
// bookkeeping. If the shape wins here it wins everywhere.
size_t EncodeChunkColumnarFixed(BcpFixture &f, duckdb::vector<uint8_t> &buf) {
	const idx_t rows = CHUNK_ROWS;
	const idx_t ncols = f.cols.size();

	static duckdb::vector<duckdb::UnifiedVectorFormat> fmts;
	static duckdb::vector<size_t> cursor;
	fmts.resize(ncols);

	size_t stride = 1;	// 0xD1 ROW token
	bool all_valid = true;
	for (idx_t c = 0; c < ncols; ++c) {
		f.chunk->data[c].ToUnifiedFormat(fmts[c]);
		stride += 1 + f.cols[c].max_length;
		all_valid = all_valid && fmts[c].validity.AllValid();
	}

	size_t total;
	if (all_valid) {
		total = rows * stride;
	} else {
		// Sizing from the masks: a NULL drops its payload, keeping its marker.
		cursor.resize(rows);
		size_t acc = 0;
		for (idx_t r = 0; r < rows; ++r) {
			cursor[r] = acc;
			size_t sz = 1;
			for (idx_t c = 0; c < ncols; ++c) {
				const auto &fmt = fmts[c];
				sz += 1 + (fmt.validity.RowIsValid(fmt.sel->get_index(r)) ? f.cols[c].max_length : 0);
			}
			acc += sz;
		}
		total = acc;
	}

	buf.resize(total);
	uint8_t *const dst = buf.data();

	if (all_valid) {
		for (idx_t r = 0; r < rows; ++r) {
			dst[r * stride] = 0xD1;
		}
		size_t col_off = 1;
		for (idx_t c = 0; c < ncols; ++c) {
			const auto &fmt = fmts[c];
			const uint8_t w = static_cast<uint8_t>(f.cols[c].max_length);
			const uint8_t *src = reinterpret_cast<const uint8_t *>(fmt.data);
			for (idx_t r = 0; r < rows; ++r) {
				uint8_t *p = dst + r * stride + col_off;
				*p = w;
				memcpy(p + 1, src + static_cast<size_t>(fmt.sel->get_index(r)) * w, w);
			}
			col_off += 1 + w;
		}
	} else {
		// MEASURED: keep the branch. A branchless variant — length byte as
		// `w * valid`, cursor advanced by `1 + w * valid`, payload memcpy'd
		// unconditionally into the slot the next column overwrites, ROW tokens
		// written last from row_start so a trailing spill cannot clobber them —
		// was WORSE on every NULL cell: null50 5.5 -> 7.6, all-NULL const
		// 3.9 -> 7.3. It buys removing one predictable branch and pays with an
		// unconditional 8-byte write per NULL row, a multiply that stops the
		// loop vectorizing, and a third pass for the tokens.
		for (idx_t r = 0; r < rows; ++r) {
			dst[cursor[r]] = 0xD1;
			cursor[r] += 1;
		}
		for (idx_t c = 0; c < ncols; ++c) {
			const auto &fmt = fmts[c];
			const uint8_t w = static_cast<uint8_t>(f.cols[c].max_length);
			const uint8_t *src = reinterpret_cast<const uint8_t *>(fmt.data);
			for (idx_t r = 0; r < rows; ++r) {
				const idx_t idx = fmt.sel->get_index(r);
				uint8_t *p = dst + cursor[r];
				if (fmt.validity.RowIsValid(idx)) {
					*p = w;
					memcpy(p + 1, src + static_cast<size_t>(idx) * w, w);
					cursor[r] += 1 + w;
				} else {
					*p = 0;
					cursor[r] += 1;
				}
			}
		}
	}

	g_sink ^= buf.size();
	return buf.size();
}

//===----------------------------------------------------------------------===//
// Spec 057: does the columnar scatter survive a WIDE row?
//
// The single-column cells above measure the columnar shape at its most
// favourable: with one 8-byte column the row stride is 10 bytes, so six rows
// share a cache line and a per-column pass is very nearly sequential. A real
// target — the 44-column table the FastTransfer article loads — has a stride of
// hundreds of bytes, so each write in a per-column pass lands on its own line,
// and the NEXT column walks the same 2048 lines again. The chunk is then far
// larger than L2 and every column pass reloads all of it.
//
// Four shapes, all producing the identical wire bytes:
//   shipped   — BCPRowEncoder::EncodeChunk (per-value dispatch, push_back)
//   colfull   — one pass per column over all rows (what was measured above)
//   colblk<K> — rows in blocks of K; within a block, all columns. Keeps the
//               block's slice of the output resident while the columns are
//               walked, which is the point of contention this answers.
//   rowmajor  — sequential rows, metadata hoisted per column: perfectly
//               sequential stores, no cursor, no blocking.
//===----------------------------------------------------------------------===//

struct WideFixture {
	std::unique_ptr<duckdb::DataChunk> chunk;
	duckdb::vector<duckdb::mssql::BCPColumnMetadata> cols;
	size_t stride = 0;
};

WideFixture BuildWideFixture(idx_t ncols) {
	WideFixture w;
	duckdb::vector<duckdb::LogicalType> types;
	for (idx_t c = 0; c < ncols; ++c) {
		types.push_back(duckdb::LogicalType::BIGINT);
	}
	w.chunk.reset(new duckdb::DataChunk());
	w.chunk->Initialize(duckdb::Allocator::DefaultAllocator(), types);
	for (idx_t c = 0; c < ncols; ++c) {
		auto *data = duckdb::FlatVector::GetDataMutable<int64_t>(w.chunk->data[c]);
		for (idx_t r = 0; r < CHUNK_ROWS; ++r) {
			data[r] = static_cast<int64_t>(r * 31 + c);
		}
		duckdb::mssql::BCPColumnMetadata col("c", duckdb::LogicalType::BIGINT, true);
		col.tds_type_token = 0x26;	// INTNTYPE
		col.max_length = 8;
		w.cols.push_back(col);
	}
	w.chunk->SetChildCardinality(CHUNK_ROWS);
	w.stride = 1 + ncols * (1 + 8);
	return w;
}

// Per-column full passes. Stride equals the whole row, so with a wide row each
// store touches a distinct cache line.
size_t WideColFull(WideFixture &w, duckdb::vector<uint8_t> &buf) {
	const idx_t rows = CHUNK_ROWS;
	const idx_t ncols = w.cols.size();
	buf.resize(rows * w.stride);
	uint8_t *const dst = buf.data();
	for (idx_t r = 0; r < rows; ++r) {
		dst[r * w.stride] = 0xD1;
	}
	size_t col_off = 1;
	for (idx_t c = 0; c < ncols; ++c) {
		const int64_t *src = duckdb::FlatVector::GetDataMutable<int64_t>(w.chunk->data[c]);
		for (idx_t r = 0; r < rows; ++r) {
			uint8_t *p = dst + r * w.stride + col_off;
			*p = 8;
			memcpy(p + 1, &src[r], 8);
		}
		col_off += 9;
	}
	g_sink ^= buf.size();
	return buf.size();
}

// TEMPLATE FRAMING (spec 057 step 3 design, 2026-08-03).
//
// For an all-valid fixed-width chunk every row's non-payload bytes are
// IDENTICAL: the 0xD1 token and one length byte per column, all at fixed offsets
// within the stride. So they are a template, not per-row work — build one row's
// worth once, replicate it across the whole buffer by doubling, and the payload
// pass then never touches a length byte.
//
// Two things this is meant to buy: the width stops being a per-value store, and
// the payload store becomes a clean strided write of a known width with nothing
// interleaved.
size_t WideColTemplate(WideFixture &w, duckdb::vector<uint8_t> &buf) {
	const idx_t rows = CHUNK_ROWS;
	const idx_t ncols = w.cols.size();
	const size_t stride = w.stride;
	buf.resize(rows * stride);
	uint8_t *const dst = buf.data();

	// One row of framing, payload positions left undefined.
	dst[0] = 0xD1;
	{
		size_t off = 1;
		for (idx_t c = 0; c < ncols; ++c) {
			dst[off] = 8;
			off += 9;
		}
	}
	// Replicate by doubling: log2(rows) memcpys instead of rows * (1 + ncols)
	// single-byte stores.
	size_t done = 1;
	while (done < rows) {
		const size_t take = duckdb::MinValue<size_t>(done, rows - done);
		memcpy(dst + done * stride, dst, take * stride);
		done += take;
	}

	size_t col_off = 1;
	for (idx_t c = 0; c < ncols; ++c) {
		const int64_t *src = duckdb::FlatVector::GetDataMutable<int64_t>(w.chunk->data[c]);
		uint8_t *p = dst + col_off + 1;	 // straight to the payload
		for (idx_t r = 0; r < rows; ++r) {
			memcpy(p + r * stride, &src[r], 8);
		}
		col_off += 9;
	}
	g_sink ^= buf.size();
	return buf.size();
}

// ROW-MAJOR *INSIDE A BLOCK*.
//
// Row-major assembly was measured worse (1.14-1.29 vs 0.39 ns/value at width) and
// dismissed — but that was over a whole 2048-row chunk, where walking every
// column per row touches 44 * 2048 * 8 = 720 KB of sources and none of it stays
// resident. Inside a 128-row block it is 45 KB: every column's slice is in L1 at
// once, so the objection is about volume and blocking removes it.
//
// Why this matters far beyond a few percent: row-major needs NO CURSOR. Going
// column-major over rows of variable length forces a per-row position array,
// because column c's offset depends on which of columns 0..c-1 were NULL in that
// row. Going row-major there is one moving pointer — write, advance by what was
// written — so a NULL stops being a separate path and becomes a branch that
// writes less. That is the entire 4.4x cliff between 0% and 1% NULLs.
size_t WideRowBlocked(WideFixture &w, duckdb::vector<uint8_t> &buf, idx_t block) {
	const idx_t rows = CHUNK_ROWS;
	const idx_t ncols = w.cols.size();
	buf.resize(rows * w.stride);
	uint8_t *ptr = buf.data();
	static duckdb::vector<const int64_t *> srcs;
	srcs.resize(ncols);
	for (idx_t c = 0; c < ncols; ++c) {
		srcs[c] = duckdb::FlatVector::GetDataMutable<int64_t>(w.chunk->data[c]);
	}
	for (idx_t r0 = 0; r0 < rows; r0 += block) {
		const idx_t rend = duckdb::MinValue<idx_t>(r0 + block, rows);
		for (idx_t r = r0; r < rend; ++r) {
			*ptr++ = 0xD1;
			for (idx_t c = 0; c < ncols; ++c) {
				*ptr++ = 8;
				memcpy(ptr, &srcs[c][r], 8);
				ptr += 8;
			}
		}
	}
	g_sink ^= buf.size();
	return buf.size();
}

// THE ZERO-FILL TAX.
//
// The other variants reuse `buf`, so after the first iteration `resize` adds no
// elements and costs nothing. Production does NOT: the accumulator grows chunk
// by chunk, so every chunk's `resize(base + total)` VALUE-INITIALISES the new
// tail — a full memset of the chunk's wire, immediately overwritten.
//
// This variant clears first, forcing the same zero-fill, so the delta against
// colblk is the tax itself. Hypothesis: it is most of the 0.72 ns/value gap
// between the blocked microbenchmark (0.39) and production (1.11).
size_t WideColBlockedZeroFill(WideFixture &w, duckdb::vector<uint8_t> &buf, idx_t block) {
	buf.clear();
	buf.resize(CHUNK_ROWS * w.stride);
	const idx_t rows = CHUNK_ROWS;
	const idx_t ncols = w.cols.size();
	uint8_t *const dst = buf.data();
	for (idx_t r0 = 0; r0 < rows; r0 += block) {
		const idx_t rend = duckdb::MinValue<idx_t>(r0 + block, rows);
		for (idx_t r = r0; r < rend; ++r) {
			dst[r * w.stride] = 0xD1;
		}
		size_t col_off = 1;
		for (idx_t c = 0; c < ncols; ++c) {
			const int64_t *src = duckdb::FlatVector::GetDataMutable<int64_t>(w.chunk->data[c]);
			for (idx_t r = r0; r < rend; ++r) {
				uint8_t *p = dst + r * w.stride + col_off;
				*p = 8;
				memcpy(p + 1, &src[r], 8);
			}
			col_off += 9;
		}
	}
	g_sink ^= buf.size();
	return buf.size();
}

// TEMPLATE FRAMING *INSIDE* A BLOCK.
//
// Framing-as-template was measured a loss at width (0.88 -> 0.95 at 44 columns)
// and that measurement was taken WITHOUT blocking: the replication wrote 813 KB
// that the payload pass immediately overwrote, i.e. it lost on memory traffic.
// Per block the replication is 128 * stride and stays in L1, which removes
// exactly the reason it lost. The two were never measured together.
size_t WideColBlockedTemplate(WideFixture &w, duckdb::vector<uint8_t> &buf, idx_t block) {
	const idx_t rows = CHUNK_ROWS;
	const idx_t ncols = w.cols.size();
	const size_t stride = w.stride;
	buf.resize(rows * stride);
	uint8_t *const dst = buf.data();

	// One row of framing, built once for the whole chunk.
	static duckdb::vector<uint8_t> tmpl;
	tmpl.resize(stride);
	tmpl[0] = 0xD1;
	for (idx_t c = 0, off = 1; c < ncols; ++c, off += 9) {
		tmpl[off] = 8;
	}

	for (idx_t r0 = 0; r0 < rows; r0 += block) {
		const idx_t rend = duckdb::MinValue<idx_t>(r0 + block, rows);
		const idx_t brows = rend - r0;
		uint8_t *const bdst = dst + r0 * stride;
		// Replicate the template across this block only — L1-resident.
		memcpy(bdst, tmpl.data(), stride);
		size_t done = 1;
		while (done < brows) {
			const size_t take = duckdb::MinValue<size_t>(done, brows - done);
			memcpy(bdst + done * stride, bdst, take * stride);
			done += take;
		}
		// Payload only: no length byte, no ROW token.
		size_t col_off = 1;
		for (idx_t c = 0; c < ncols; ++c) {
			const int64_t *src = duckdb::FlatVector::GetDataMutable<int64_t>(w.chunk->data[c]);
			uint8_t *p = bdst + col_off + 1;
			for (idx_t r = 0; r < brows; ++r) {
				memcpy(p + r * stride, &src[r0 + r], 8);
			}
			col_off += 9;
		}
	}
	g_sink ^= buf.size();
	return buf.size();
}

// WIDTH-SPECIALISED SCATTER.
//
// The production path holds the width in a runtime `uint8_t w` and calls
// memcpy(dst, src, w), which no compiler can fold into a single store. The width
// is a COLUMN constant and the arms already exist per width, so it belongs in
// the type, not in a variable. This is the same loop as WideColFull with W as a
// template parameter — the difference is only what the compiler can see.
template <int W>
inline void ScatterWidth(uint8_t *dst, size_t stride, size_t col_off, idx_t rows, const uint8_t *src) {
	for (idx_t r = 0; r < rows; ++r) {
		uint8_t *p = dst + r * stride + col_off;
		*p = W;
		memcpy(p + 1, src + r * W, W);
	}
}

size_t WideColTypedWidth(WideFixture &w, duckdb::vector<uint8_t> &buf) {
	const idx_t rows = CHUNK_ROWS;
	const idx_t ncols = w.cols.size();
	buf.resize(rows * w.stride);
	uint8_t *const dst = buf.data();
	for (idx_t r = 0; r < rows; ++r) {
		dst[r * w.stride] = 0xD1;
	}
	size_t col_off = 1;
	for (idx_t c = 0; c < ncols; ++c) {
		const uint8_t *src =
			reinterpret_cast<const uint8_t *>(duckdb::FlatVector::GetDataMutable<int64_t>(w.chunk->data[c]));
		ScatterWidth<8>(dst, w.stride, col_off, rows, src);
		col_off += 9;
	}
	g_sink ^= buf.size();
	return buf.size();
}

// Both together: template framing AND a width-specialised payload store.
template <int W>
inline void ScatterPayloadOnly(uint8_t *dst, size_t stride, idx_t rows, const uint8_t *src) {
	for (idx_t r = 0; r < rows; ++r) {
		memcpy(dst + r * stride, src + r * W, W);
	}
}

size_t WideColTemplateTyped(WideFixture &w, duckdb::vector<uint8_t> &buf) {
	const idx_t rows = CHUNK_ROWS;
	const idx_t ncols = w.cols.size();
	const size_t stride = w.stride;
	buf.resize(rows * stride);
	uint8_t *const dst = buf.data();
	dst[0] = 0xD1;
	for (idx_t c = 0, off = 1; c < ncols; ++c, off += 9) {
		dst[off] = 8;
	}
	size_t done = 1;
	while (done < rows) {
		const size_t take = duckdb::MinValue<size_t>(done, rows - done);
		memcpy(dst + done * stride, dst, take * stride);
		done += take;
	}
	size_t col_off = 1;
	for (idx_t c = 0; c < ncols; ++c) {
		const uint8_t *src =
			reinterpret_cast<const uint8_t *>(duckdb::FlatVector::GetDataMutable<int64_t>(w.chunk->data[c]));
		ScatterPayloadOnly<8>(dst + col_off + 1, stride, rows, src);
		col_off += 9;
	}
	g_sink ^= buf.size();
	return buf.size();
}

// Rows in blocks; within a block, all columns. The block's output slice stays
// resident across the column walk.
size_t WideColBlocked(WideFixture &w, duckdb::vector<uint8_t> &buf, idx_t block) {
	const idx_t rows = CHUNK_ROWS;
	const idx_t ncols = w.cols.size();
	buf.resize(rows * w.stride);
	uint8_t *const dst = buf.data();
	for (idx_t r0 = 0; r0 < rows; r0 += block) {
		const idx_t rend = duckdb::MinValue<idx_t>(r0 + block, rows);
		for (idx_t r = r0; r < rend; ++r) {
			dst[r * w.stride] = 0xD1;
		}
		size_t col_off = 1;
		for (idx_t c = 0; c < ncols; ++c) {
			const int64_t *src = duckdb::FlatVector::GetDataMutable<int64_t>(w.chunk->data[c]);
			for (idx_t r = r0; r < rend; ++r) {
				uint8_t *p = dst + r * w.stride + col_off;
				*p = 8;
				memcpy(p + 1, &src[r], 8);
			}
			col_off += 9;
		}
	}
	g_sink ^= buf.size();
	return buf.size();
}

// Sequential rows, per-column source pointers hoisted once. Writes advance
// monotonically through the buffer — the ideal access pattern — at the cost of
// touching every column's source array on every row.
size_t WideRowMajor(WideFixture &w, duckdb::vector<uint8_t> &buf) {
	const idx_t rows = CHUNK_ROWS;
	const idx_t ncols = w.cols.size();
	static duckdb::vector<const int64_t *> srcs;
	srcs.resize(ncols);
	for (idx_t c = 0; c < ncols; ++c) {
		srcs[c] = duckdb::FlatVector::GetDataMutable<int64_t>(w.chunk->data[c]);
	}
	buf.resize(rows * w.stride);
	uint8_t *p = buf.data();
	for (idx_t r = 0; r < rows; ++r) {
		*p++ = 0xD1;
		for (idx_t c = 0; c < ncols; ++c) {
			*p++ = 8;
			memcpy(p, &srcs[c][r], 8);
			p += 8;
		}
	}
	g_sink ^= buf.size();
	return buf.size();
}

// Spec 057 probe: representation-aware encode. The shipped path resolves
// DICTIONARY / CONSTANT transparently through `format.sel` and therefore
// re-encodes every row; here the representation is inspected BEFORE
// ToUnifiedFormat (it is erased afterwards) and each distinct value is encoded
// exactly once, with the per-row work reduced to appending a cached span.
//
//   CONSTANT   -> one encode, then memcpy the same span per row
//   DICTIONARY -> one encode per USED child, then memcpy the cached span
//   FLAT       -> the shipped hoisted path, unchanged
size_t EncodeChunkReprAware(BcpFixture &f, duckdb::vector<uint8_t> &buf) {
	using duckdb::tds::encoding::BCPRowEncoder;
	buf.clear();
	auto &vec = f.chunk->data[0];
	const auto vtype = vec.GetVectorType();

	if (vtype == duckdb::VectorType::CONSTANT_VECTOR) {
		duckdb::vector<uint8_t> one;
		BCPRowEncoder::EncodeValue(one, vec.GetValue(0), f.cols[0]);
		for (idx_t row = 0; row < CHUNK_ROWS; ++row) {
			buf.push_back(0xD1);
			buf.insert(buf.end(), one.begin(), one.end());
		}
		g_sink ^= buf.size();
		return buf.size();
	}

	if (vtype == duckdb::VectorType::DICTIONARY_VECTOR) {
		const auto &sel = duckdb::DictionaryVector::SelVector(vec);
		auto &child = duckdb::DictionaryVector::Child(vec);
		const auto dict_size = duckdb::DictionaryVector::DictionarySize(vec);
		const idx_t child_count = dict_size.IsValid() ? dict_size.GetIndex() : CHUNK_ROWS;

		// Encode-once cache: one contiguous buffer + (offset,len) per child.
		duckdb::vector<uint8_t> cache;
		std::vector<uint32_t> coff(child_count, 0), clen(child_count, 0);
		std::vector<bool> done(child_count, false);
		for (idx_t row = 0; row < CHUNK_ROWS; ++row) {
			const idx_t idx = sel.get_index(row);
			if (idx >= child_count || done[idx]) {
				continue;
			}
			const uint32_t start = static_cast<uint32_t>(cache.size());
			BCPRowEncoder::EncodeValue(cache, child.GetValue(idx), f.cols[0]);
			coff[idx] = start;
			clen[idx] = static_cast<uint32_t>(cache.size() - start);
			done[idx] = true;
		}
		for (idx_t row = 0; row < CHUNK_ROWS; ++row) {
			const idx_t idx = sel.get_index(row);
			buf.push_back(0xD1);
			buf.insert(buf.end(), cache.begin() + coff[idx], cache.begin() + coff[idx] + clen[idx]);
		}
		g_sink ^= buf.size();
		return buf.size();
	}

	BCPRowEncoder::EncodeChunk(buf, *f.chunk, f.cols, nullptr);
	g_sink ^= buf.size();
	return buf.size();
}

// Arena-backed variant of the above: the span cache and its index arrays are
// reused across chunks (as the production writer arena would be), and children
// are encoded through the VECTOR overload with a format built once over the
// child — not through Vector::GetValue -> duckdb::Value, which allocates per
// child and, for DECIMAL, routes into the legacy-divergent Value encoder.
size_t EncodeChunkReprAwareArena(BcpFixture &f, duckdb::vector<uint8_t> &buf) {
	using duckdb::tds::encoding::BCPRowEncoder;
	static duckdb::vector<uint8_t> cache;
	static std::vector<uint32_t> coff, clen;
	static std::vector<uint8_t> done;

	buf.clear();
	auto &vec = f.chunk->data[0];
	const auto vtype = vec.GetVectorType();

	// Encode one value of `src` (row `idx`) into `out` via the family's Vector
	// overload — the same call the resolved per-column encoder would make.
	auto encode_one = [&](Vector &src, const duckdb::UnifiedVectorFormat &fmt, idx_t idx,
						  duckdb::vector<uint8_t> &out) {
		switch (f.spec.kind) {
		case BcpCellSpec::Kind::Bigint:
			duckdb::mssql::codec::integer::EncodeToBcp(src, fmt, idx, f.cols[0], out);
			break;
		case BcpCellSpec::Kind::Varchar16:
			duckdb::mssql::codec::string::EncodeToBcp(src, fmt, idx, f.cols[0], out);
			break;
		case BcpCellSpec::Kind::Decimal18s6:
			duckdb::mssql::codec::decimal::EncodeToBcp(src, fmt, idx, f.cols[0], out);
			break;
		}
	};

	if (vtype == duckdb::VectorType::CONSTANT_VECTOR) {
		cache.clear();
		if (duckdb::ConstantVector::IsNull(vec)) {
			// All-NULL constant: the shipped path already emits just the NULL
			// marker per row — keep that, do not build a cache.
			BCPRowEncoder::EncodeChunk(buf, *f.chunk, f.cols, nullptr);
			g_sink ^= buf.size();
			return buf.size();
		}
		duckdb::UnifiedVectorFormat fmt;
		vec.ToUnifiedFormat(fmt);
		encode_one(vec, fmt, 0, cache);
		for (idx_t row = 0; row < CHUNK_ROWS; ++row) {
			buf.push_back(0xD1);
			buf.insert(buf.end(), cache.begin(), cache.end());
		}
		g_sink ^= buf.size();
		return buf.size();
	}

	if (vtype == duckdb::VectorType::DICTIONARY_VECTOR) {
		const auto &sel = duckdb::DictionaryVector::SelVector(vec);
		auto &child = duckdb::DictionaryVector::Child(vec);
		const auto dict_size = duckdb::DictionaryVector::DictionarySize(vec);
		const idx_t child_count = dict_size.IsValid() ? dict_size.GetIndex() : CHUNK_ROWS;

		cache.clear();
		if (coff.size() < child_count) {
			coff.resize(child_count);
			clen.resize(child_count);
			done.resize(child_count);
		}
		std::fill(done.begin(), done.begin() + child_count, 0);

		duckdb::UnifiedVectorFormat child_fmt;
		child.ToUnifiedFormat(child_fmt);
		for (idx_t row = 0; row < CHUNK_ROWS; ++row) {
			const idx_t idx = sel.get_index(row);
			if (idx >= child_count || done[idx]) {
				continue;
			}
			const uint32_t start = static_cast<uint32_t>(cache.size());
			if (!child_fmt.validity.RowIsValid(child_fmt.sel->get_index(idx))) {
				BCPRowEncoder::EncodeValue(cache, duckdb::Value(f.cols[0].duckdb_type), f.cols[0]);
			} else {
				encode_one(child, child_fmt, idx, cache);
			}
			coff[idx] = start;
			clen[idx] = static_cast<uint32_t>(cache.size() - start);
			done[idx] = 1;
		}
		for (idx_t row = 0; row < CHUNK_ROWS; ++row) {
			const idx_t idx = sel.get_index(row);
			buf.push_back(0xD1);
			const uint8_t *src = cache.data() + coff[idx];
			buf.insert(buf.end(), src, src + clen[idx]);
		}
		g_sink ^= buf.size();
		return buf.size();
	}

	BCPRowEncoder::EncodeChunk(buf, *f.chunk, f.cols, nullptr);
	g_sink ^= buf.size();
	return buf.size();
}

// Bulk UTF-8 -> UTF-16LE for a whole FLAT VARCHAR vector (write-path mirror of
// the read-path prealloc scheme):
//   1. gather every value into one contiguous UTF-8 buffer (they live scattered
//      in the vector's string heap — this copy has no read-path analogue);
//   2. ONE convert_valid_utf8_to_utf16le into a scratch of 2x the input (an
//      ASCII byte is the worst case: 1 UTF-8 byte -> 2 UTF-16 bytes);
//   3. per row append the 2-byte length prefix + memcpy the value's span — BCP
//      framing is row-major, so this copy is structural, not overhead.
// Boundaries: written == 2 * utf8_bytes means the column was all ASCII, so each
// value's UTF-16 length is exactly 2x its UTF-8 length; otherwise fall back to
// per-value lengths.
size_t EncodeChunkBulkUtf16(BcpFixture &f, duckdb::vector<uint8_t> &buf) {
	static std::vector<char> gathered;
	static std::vector<char16_t> converted;
	static std::vector<uint32_t> in_off, in_len;

	buf.clear();
	auto &vec = f.chunk->data[0];
	duckdb::UnifiedVectorFormat fmt;
	vec.ToUnifiedFormat(fmt);
	const auto *strs = duckdb::UnifiedVectorFormat::GetData<string_t>(fmt);

	// Gather WITH a U+0000 delimiter after every value (one 0x00 byte in UTF-8,
	// one 0x0000 unit in UTF-16) so per-value output lengths can be recovered
	// by scanning the converted buffer instead of calling simdutf per value —
	// the write-path mirror of the read-path delimiter scheme. Legal because no
	// UTF-8 encoding of a non-zero code point contains a 0x00 byte.
	//
	// Everything below is pre-sized: one cheap size pass (reading string_t
	// lengths only — no byte inspection), then memcpy at a running offset. In
	// particular there is NO per-byte embedded-NUL pre-scan: embedded NULs only
	// matter on the delimiter-scan path, and there they are detected for free by
	// counting the zero units the scan finds.
	if (in_off.size() < CHUNK_ROWS) {
		in_off.resize(CHUNK_ROWS);
		in_len.resize(CHUNK_ROWS);
	}
	size_t total_utf8 = CHUNK_ROWS;	 // one delimiter byte per row
	for (idx_t row = 0; row < CHUNK_ROWS; ++row) {
		const idx_t sidx = fmt.sel->get_index(row);
		if (fmt.validity.RowIsValid(sidx)) {
			total_utf8 += strs[sidx].GetSize();
		}
	}
	if (gathered.size() < total_utf8) {
		gathered.resize(total_utf8);
	}
	size_t g = 0;
	for (idx_t row = 0; row < CHUNK_ROWS; ++row) {
		const idx_t sidx = fmt.sel->get_index(row);
		in_off[row] = static_cast<uint32_t>(g);
		if (fmt.validity.RowIsValid(sidx)) {
			const auto &s = strs[sidx];
			const uint32_t len = static_cast<uint32_t>(s.GetSize());
			std::memcpy(gathered.data() + g, s.GetData(), len);
			g += len;
			in_len[row] = len;
		} else {
			in_len[row] = 0;
		}
		gathered[g++] = '\0';
	}

	if (converted.size() < g) {
		converted.resize(g);  // 1 char16_t per UTF-8 byte is the worst case (ASCII)
	}
	const size_t units = g == 0 ? 0 : simdutf::convert_valid_utf8_to_utf16le(gathered.data(), g, converted.data());
	const bool all_ascii = units == g;
	const uint8_t *base = reinterpret_cast<const uint8_t *>(converted.data());
	// Worst case out: 3 framing bytes per row + 2 bytes per UTF-8 byte.
	buf.reserve(units * 2 + CHUNK_ROWS * 3);

	// Non-ASCII boundaries: one word-wise sweep for zero 16-bit lanes (4 units
	// per 64-bit word). An embedded U+0000 in the data shows up as an extra
	// zero unit — the sweep would then fill all CHUNK_ROWS slots early, which
	// the caller could detect; the fixtures here never contain one.
	static std::vector<uint32_t> unit_end;
	if (!all_ascii) {
		if (unit_end.size() < CHUNK_ROWS) {
			unit_end.resize(CHUNK_ROWS);
		}
		idx_t row = 0;
		size_t i = 0;
		const uint64_t *w64 = reinterpret_cast<const uint64_t *>(converted.data());
		while (i + 4 <= units && row < CHUNK_ROWS) {
			uint64_t w = w64[i / 4];
			uint64_t z = (w - 0x0001000100010001ULL) & ~w & 0x8000800080008000ULL;
			while (z != 0 && row < CHUNK_ROWS) {
				const unsigned lane = static_cast<unsigned>(__builtin_ctzll(z)) / 16;
				unit_end[row++] = static_cast<uint32_t>(i + lane);
				z &= z - 1;
			}
			i += 4;
		}
		while (row < CHUNK_ROWS && i < units) {
			if (converted[i] == 0) {
				unit_end[row++] = static_cast<uint32_t>(i);
			}
			++i;
		}
	}

	// Every output length is known before a byte is written, so size the
	// accumulator ONCE and fill it through a raw pointer — no per-byte
	// push_back capacity checks, no per-row insert.
	static std::vector<uint32_t> out_off_v, out_len_v;
	if (out_off_v.size() < CHUNK_ROWS) {
		out_off_v.resize(CHUNK_ROWS);
		out_len_v.resize(CHUNK_ROWS);
	}
	size_t total_out = 0;
	uint32_t seg_start = 0;	 // in UTF-16 units, for the scanned path
	for (idx_t row = 0; row < CHUNK_ROWS; ++row) {
		if (all_ascii) {
			out_off_v[row] = in_off[row] * 2;  // delimiters are 1 unit, so input offsets map 1:2
			out_len_v[row] = in_len[row] * 2;
		} else {
			out_off_v[row] = seg_start * 2;
			out_len_v[row] = (unit_end[row] - seg_start) * 2;
			seg_start = unit_end[row] + 1;
		}
		total_out += 3;	 // row token + 2-byte length (or the 0xFFFF NULL marker)
		if (fmt.validity.RowIsValid(fmt.sel->get_index(row))) {
			total_out += out_len_v[row];
		}
	}

	buf.resize(total_out);
	uint8_t *out = buf.data();
	for (idx_t row = 0; row < CHUNK_ROWS; ++row) {
		*out++ = 0xD1;
		if (!fmt.validity.RowIsValid(fmt.sel->get_index(row))) {
			*out++ = 0xFF;
			*out++ = 0xFF;
			continue;
		}
		const uint32_t len = out_len_v[row];
		*out++ = static_cast<uint8_t>(len & 0xFF);
		*out++ = static_cast<uint8_t>(len >> 8);
		std::memcpy(out, base + out_off_v[row], len);
		out += len;
	}
	g_sink ^= buf.size();
	return buf.size();
}

// Blocked variant of the above: instead of converting the whole column and
// then copying it into the accumulator (two passes over data that no longer
// fits cache), process `block_rows` at a time so the gathered UTF-8, the
// converted UTF-16 and the accumulator slice stay resident while they are used.
// Also takes the validity mask straight from the vector: `AllValid()` removes
// the per-row NULL branch entirely, and NULL rows are skipped before the gather
// (they contribute nothing to convert — only a fixed-size wire marker).
size_t EncodeChunkBulkUtf16Blocked(BcpFixture &f, duckdb::vector<uint8_t> &buf, idx_t block_rows) {
	static std::vector<char> gathered;
	static std::vector<char16_t> converted;
	static std::vector<uint32_t> in_off, in_len, unit_end;

	auto &vec = f.chunk->data[0];
	duckdb::UnifiedVectorFormat fmt;
	vec.ToUnifiedFormat(CHUNK_ROWS, fmt);
	const auto *strs = duckdb::UnifiedVectorFormat::GetData<string_t>(fmt);
	const bool all_valid = fmt.validity.AllValid();

	if (in_off.size() < CHUNK_ROWS) {
		in_off.resize(CHUNK_ROWS);
		in_len.resize(CHUNK_ROWS);
		unit_end.resize(CHUNK_ROWS);
	}

	// Worst case: 2 output bytes per UTF-8 byte + 3 framing bytes per row.
	size_t total_utf8 = 0;
	for (idx_t row = 0; row < CHUNK_ROWS; ++row) {
		const idx_t sidx = fmt.sel->get_index(row);
		if (all_valid || fmt.validity.RowIsValid(sidx)) {
			total_utf8 += strs[sidx].GetSize();
		}
	}
	buf.resize(total_utf8 * 2 + CHUNK_ROWS * 3);
	uint8_t *out = buf.data();

	if (gathered.size() < total_utf8 + CHUNK_ROWS) {
		gathered.resize(total_utf8 + CHUNK_ROWS);
	}
	if (converted.size() < gathered.size()) {
		converted.resize(gathered.size());
	}

	for (idx_t start = 0; start < CHUNK_ROWS; start += block_rows) {
		const idx_t end = duckdb::MinValue<idx_t>(start + block_rows, CHUNK_ROWS);

		// Gather this block only; NULL rows are skipped outright.
		size_t g = 0;
		for (idx_t row = start; row < end; ++row) {
			const idx_t sidx = fmt.sel->get_index(row);
			in_off[row] = static_cast<uint32_t>(g);
			if (!all_valid && !fmt.validity.RowIsValid(sidx)) {
				in_len[row] = 0;
				continue;  // no payload, no delimiter — nothing to convert
			}
			const auto &s = strs[sidx];
			const uint32_t len = static_cast<uint32_t>(s.GetSize());
			std::memcpy(gathered.data() + g, s.GetData(), len);
			g += len;
			in_len[row] = len;
			gathered[g++] = '\0';
		}

		const size_t units = g == 0 ? 0 : simdutf::convert_valid_utf8_to_utf16le(gathered.data(), g, converted.data());
		const bool ascii = units == g;
		const uint8_t *base = reinterpret_cast<const uint8_t *>(converted.data());

		if (!ascii) {
			idx_t row = start;
			size_t i = 0;
			const uint64_t *w64 = reinterpret_cast<const uint64_t *>(converted.data());
			while (i + 4 <= units && row < end) {
				uint64_t w = w64[i / 4];
				uint64_t z = (w - 0x0001000100010001ULL) & ~w & 0x8000800080008000ULL;
				while (z != 0 && row < end) {
					const unsigned lane = static_cast<unsigned>(__builtin_ctzll(z)) / 16;
					unit_end[row++] = static_cast<uint32_t>(i + lane);
					z &= z - 1;
				}
				i += 4;
			}
			while (row < end && i < units) {
				if (converted[i] == 0) {
					unit_end[row++] = static_cast<uint32_t>(i);
				}
				++i;
			}
		}

		uint32_t seg_start = 0;
		for (idx_t row = start; row < end; ++row) {
			*out++ = 0xD1;
			if (!all_valid && !fmt.validity.RowIsValid(fmt.sel->get_index(row))) {
				*out++ = 0xFF;
				*out++ = 0xFF;
				continue;
			}
			uint32_t off, len;
			if (ascii) {
				off = in_off[row] * 2;
				len = in_len[row] * 2;
			} else {
				off = seg_start * 2;
				len = (unit_end[row] - seg_start) * 2;
				seg_start = unit_end[row] + 1;
			}
			*out++ = static_cast<uint8_t>(len & 0xFF);
			*out++ = static_cast<uint8_t>(len >> 8);
			std::memcpy(out, base + off, len);
			out += len;
		}
	}

	buf.resize(static_cast<size_t>(out - buf.data()));
	g_sink ^= buf.size();
	return buf.size();
}

// Hoisted output must equal the per-row path's output with one 0xD1 token
// injected per row — old-vs-new byte equivalence inside one binary.
bool VerifyHoisted(BcpFixture &f, const duckdb::vector<uint8_t> &got) {
	duckdb::vector<uint8_t> expect;
	for (idx_t row = 0; row < CHUNK_ROWS; ++row) {
		expect.push_back(0xD1);
		duckdb::tds::encoding::BCPRowEncoder::EncodeRow(expect, *f.chunk, row, f.cols, nullptr);
	}
	return expect.size() == got.size() && std::equal(expect.begin(), expect.end(), got.begin());
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
	for (const KindRow &kr : {KindRow{K::Bigint, "bigint"}, KindRow{K::Varchar16, "nvarchar16"}}) {
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

//===----------------------------------------------------------------------===//
// Spec-055 evaluation: column staging + BATCH materialization prototypes.
//
// All four string strategies are fed from the SAME staged column (contiguous
// UTF-16LE payload + per-row offset/length) — what the phase-1 RowReader pivot
// would produce. Staging is built once OUTSIDE timing: today's path pays an
// equivalent per-value copy into RowData::values, so timing it here would
// charge the same scatter to one side only.
//
//   A     : N x (validate+length) + N x convert, one chunk-owned out buffer
//   B     : N x length            + 1 x convert   (gate: valid && !boundary_risky)
//   C     : 1 x length + 1 x convert + memchr split (gate: no embedded U+0000)
//   ASCII : 1 x length + 1 x convert, boundaries by arithmetic (gate: all-ASCII)
//
// Invalid UTF-16 uses STANDARD U+FFFD substitution (maintainer decision
// 2026-07-29), NOT the legacy converter's "replace and swallow the next unit".
//===----------------------------------------------------------------------===//

// Standard-conformant lenient decode built from simdutf only: convert the
// valid prefix, emit U+FFFD for the offending code unit, resume after it.
// Candidate replacement for LegacyUtf16LEDecode.
std::string DecodeReplacingStandard(const uint8_t *data, size_t byte_length) {
	std::string out;
	const size_t units = byte_length / 2;
	if (units == 0) {
		return out;
	}
	std::vector<char16_t> aligned;
	const char16_t *src;
	if ((reinterpret_cast<uintptr_t>(data) & 0x1u) == 0u) {
		src = reinterpret_cast<const char16_t *>(data);
	} else {
		aligned.resize(units);
		std::memcpy(aligned.data(), data, units * 2);
		src = aligned.data();
	}
	std::vector<char> scratch(units * 3 + 4);
	size_t pos = 0;
	while (pos < units) {
		auto r = simdutf::convert_utf16le_to_utf8_with_errors(src + pos, units - pos, scratch.data());
		if (r.error == simdutf::SUCCESS) {
			out.append(scratch.data(), r.count);
			break;
		}
		if (r.count > 0) {
			const size_t written = simdutf::convert_valid_utf16le_to_utf8(src + pos, r.count, scratch.data());
			out.append(scratch.data(), written);
		}
		out.append("\xEF\xBF\xBD", 3);	// U+FFFD
		pos += r.count + 1;				// skip the offending code unit
	}
	return out;
}

struct StagedStrings {
	std::vector<uint8_t> payload;  // contiguous UTF-16LE, non-NULL values only
	std::vector<uint32_t> off;	   // byte offset into payload, per row
	std::vector<uint32_t> len;	   // byte length, per row (0 for NULL)
	std::vector<uint8_t> delim;	   // strategy C: every row's value + U+0000
	std::vector<uint32_t> doff;	   // byte offset into `delim`, per row
	size_t code_units = 0;
	size_t delim_units = 0;
	bool all_valid = true;
	bool boundary_risky = false;  // some value ends in an unpaired high surrogate
	bool saw_embedded_nul = false;
	bool all_ascii = false;
};

StagedStrings StageColumn(const Fixture &f) {
	StagedStrings s;
	s.off.resize(CHUNK_ROWS);
	s.len.resize(CHUNK_ROWS);
	s.doff.resize(CHUNK_ROWS);
	size_t total = 0;
	for (idx_t row = 0; row < CHUNK_ROWS; ++row) {
		total += f.rows[row].size();
	}
	s.payload.reserve(total);
	s.delim.reserve(total + CHUNK_ROWS * 2);
	for (idx_t row = 0; row < CHUNK_ROWS; ++row) {
		const auto &v = f.rows[row];
		s.off[row] = static_cast<uint32_t>(s.payload.size());
		s.doff[row] = static_cast<uint32_t>(s.delim.size());
		s.len[row] = f.null_mask[row] ? 0u : static_cast<uint32_t>(v.size());
		if (!f.null_mask[row] && !v.empty()) {
			s.payload.insert(s.payload.end(), v.begin(), v.end());
			const uint16_t last = static_cast<uint16_t>(v[v.size() - 2] | (v[v.size() - 1] << 8));
			if (last >= 0xD800 && last <= 0xDBFF) {
				s.boundary_risky = true;
			}
			for (size_t i = 0; i + 1 < v.size(); i += 2) {
				if (v[i] == 0 && v[i + 1] == 0) {
					s.saw_embedded_nul = true;
				}
			}
			s.delim.insert(s.delim.end(), v.begin(), v.end());
		}
		AppendUnit(s.delim, 0x0000);
	}
	s.code_units = s.payload.size() / 2;
	s.delim_units = s.delim.size() / 2;
	s.all_valid = s.code_units == 0 ||
				  simdutf::validate_utf16le(reinterpret_cast<const char16_t *>(s.payload.data()), s.code_units);
	if (s.all_valid && s.code_units > 0) {
		s.all_ascii = simdutf::utf8_length_from_utf16le(reinterpret_cast<const char16_t *>(s.payload.data()),
														s.code_units) == s.code_units;
	}
	return s;
}

// Per-chunk scratch (the real implementation keeps these in the staging arena).
struct BatchScratch {
	std::vector<uint32_t> out_off, out_len;
	std::vector<std::string> fb;	 // decoded fallback text for invalid values
	std::vector<uint32_t> fb_epoch;	 // epoch stamp — avoids clearing per chunk
	uint32_t epoch = 0;
};
BatchScratch g_bs;

void EnsureScratch() {
	if (g_bs.out_off.size() == CHUNK_ROWS) {
		return;
	}
	g_bs.out_off.resize(CHUNK_ROWS);
	g_bs.out_len.resize(CHUNK_ROWS);
	g_bs.fb.resize(CHUNK_ROWS);
	g_bs.fb_epoch.assign(CHUNK_ROWS, 0);
}

// One chunk-owned output buffer: allocate a single big slot from the vector's
// own string heap and point per-row string_t at it (≤12 B values are copied
// inline by the string_t constructor, as DuckDB requires).
char *AllocOutput(Vector &vec, size_t total) {
	static char dummy[1] = {0};
	if (total == 0) {
		return dummy;
	}
	auto big = duckdb::StringVector::EmptyString(vec, total);
	return big.GetDataWriteable();
}

size_t FillChunkBatchA(const Fixture &f, const StagedStrings &s, DataChunk &chunk) {
	chunk.Reset();
	auto &vec = chunk.data[0];
	EnsureScratch();
	++g_bs.epoch;
	size_t total = 0;
	for (idx_t row = 0; row < CHUNK_ROWS; ++row) {
		if (f.null_mask[row]) {
			g_bs.out_off[row] = static_cast<uint32_t>(total);
			g_bs.out_len[row] = 0;
			continue;
		}
		const uint8_t *p = s.payload.data() + s.off[row];
		size_t l = duckdb::tds::encoding::Utf8LengthFromUtf16LEView(p, s.len[row]);
		if (l == SIZE_MAX) {
			g_bs.fb[row] = DecodeReplacingStandard(p, s.len[row]);
			g_bs.fb_epoch[row] = g_bs.epoch;
			l = g_bs.fb[row].size();
		}
		g_bs.out_off[row] = static_cast<uint32_t>(total);
		g_bs.out_len[row] = static_cast<uint32_t>(l);
		total += l;
	}
	char *base = AllocOutput(vec, total);
	auto *slots = FlatVector::GetDataMutable<string_t>(vec);
	for (idx_t row = 0; row < CHUNK_ROWS; ++row) {
		if (f.null_mask[row]) {
			FlatVector::SetNull(vec, row, true);
			continue;
		}
		char *dst = base + g_bs.out_off[row];
		if (g_bs.fb_epoch[row] == g_bs.epoch) {
			std::memcpy(dst, g_bs.fb[row].data(), g_bs.out_len[row]);
		} else {
			duckdb::tds::encoding::Utf16LEDecodeValidInto(s.payload.data() + s.off[row], s.len[row], dst);
		}
		slots[row] = string_t(dst, g_bs.out_len[row]);
	}
	chunk.SetChildCardinality(CHUNK_ROWS);
	g_sink ^= total;
	return total;
}

// B: per-value length (no re-validation — the whole payload was validated by
// the gate) + prefix sum + ONE conversion of the concatenated payload.
size_t FillChunkBatchB(const Fixture &f, const StagedStrings &s, DataChunk &chunk) {
	chunk.Reset();
	auto &vec = chunk.data[0];
	EnsureScratch();
	size_t total = 0;
	for (idx_t row = 0; row < CHUNK_ROWS; ++row) {
		g_bs.out_off[row] = static_cast<uint32_t>(total);
		if (f.null_mask[row] || s.len[row] == 0) {
			g_bs.out_len[row] = 0;
			continue;
		}
		const size_t l = simdutf::utf8_length_from_utf16le(
			reinterpret_cast<const char16_t *>(s.payload.data() + s.off[row]), s.len[row] / 2);
		g_bs.out_len[row] = static_cast<uint32_t>(l);
		total += l;
	}
	char *base = AllocOutput(vec, total);
	if (s.code_units > 0) {
		simdutf::convert_valid_utf16le_to_utf8(reinterpret_cast<const char16_t *>(s.payload.data()), s.code_units,
											   base);
	}
	auto *slots = FlatVector::GetDataMutable<string_t>(vec);
	for (idx_t row = 0; row < CHUNK_ROWS; ++row) {
		if (f.null_mask[row]) {
			FlatVector::SetNull(vec, row, true);
			continue;
		}
		slots[row] = string_t(base + g_bs.out_off[row], g_bs.out_len[row]);
	}
	chunk.SetChildCardinality(CHUNK_ROWS);
	g_sink ^= total;
	return total;
}

// ASCII fast path: one bulk length call doubles as the all-ASCII probe
// (utf8_len == code_units), then ONE conversion; boundaries are arithmetic.
size_t FillChunkBatchAscii(const Fixture &f, const StagedStrings &s, DataChunk &chunk) {
	chunk.Reset();
	auto &vec = chunk.data[0];
	const size_t total = s.code_units == 0 ? 0
										   : simdutf::utf8_length_from_utf16le(
												 reinterpret_cast<const char16_t *>(s.payload.data()), s.code_units);
	char *base = AllocOutput(vec, total);
	if (s.code_units > 0) {
		simdutf::convert_valid_utf16le_to_utf8(reinterpret_cast<const char16_t *>(s.payload.data()), s.code_units,
											   base);
	}
	auto *slots = FlatVector::GetDataMutable<string_t>(vec);
	for (idx_t row = 0; row < CHUNK_ROWS; ++row) {
		if (f.null_mask[row]) {
			FlatVector::SetNull(vec, row, true);
			continue;
		}
		slots[row] = string_t(base + s.off[row] / 2, s.len[row] / 2);
	}
	chunk.SetChildCardinality(CHUNK_ROWS);
	g_sink ^= total;
	return total;
}

// C: NUL-delimited single conversion + memchr split. Zero per-value simdutf
// calls even for non-ASCII input — the delimiter carries the boundaries.
size_t FillChunkBatchC(const Fixture &f, const StagedStrings &s, DataChunk &chunk) {
	chunk.Reset();
	auto &vec = chunk.data[0];
	const size_t total =
		simdutf::utf8_length_from_utf16le(reinterpret_cast<const char16_t *>(s.delim.data()), s.delim_units);
	char *base = AllocOutput(vec, total);
	simdutf::convert_valid_utf16le_to_utf8(reinterpret_cast<const char16_t *>(s.delim.data()), s.delim_units, base);
	auto *slots = FlatVector::GetDataMutable<string_t>(vec);
	const char *p = base;
	const char *end = base + total;
	for (idx_t row = 0; row < CHUNK_ROWS; ++row) {
		const char *nul = static_cast<const char *>(std::memchr(p, '\0', static_cast<size_t>(end - p)));
		if (!nul) {
			break;
		}
		if (f.null_mask[row]) {
			FlatVector::SetNull(vec, row, true);
		} else {
			slots[row] = string_t(p, static_cast<uint32_t>(nul - p));
		}
		p = nul + 1;
	}
	chunk.SetChildCardinality(CHUNK_ROWS);
	g_sink ^= total;
	return total;
}

// C2: same SINGLE conversion of the delimited payload as C, but the value
// boundaries are recovered with ONE word-wise pass over the output instead of
// 2048 memchr calls — per-value call overhead was all C was paying.
size_t FillChunkBatchC2(const Fixture &f, const StagedStrings &s, DataChunk &chunk) {
	chunk.Reset();
	auto &vec = chunk.data[0];
	const size_t total =
		simdutf::utf8_length_from_utf16le(reinterpret_cast<const char16_t *>(s.delim.data()), s.delim_units);
	char *base = AllocOutput(vec, total);
	simdutf::convert_valid_utf16le_to_utf8(reinterpret_cast<const char16_t *>(s.delim.data()), s.delim_units, base);
	auto *slots = FlatVector::GetDataMutable<string_t>(vec);

	idx_t row = 0;
	const char *seg = base;
	size_t i = 0;
	// SWAR zero-byte detection: 8 bytes per iteration, no per-value call.
	while (i + 8 <= total && row < CHUNK_ROWS) {
		uint64_t w;
		std::memcpy(&w, base + i, 8);
		uint64_t z = (w - 0x0101010101010101ULL) & ~w & 0x8080808080808080ULL;
		while (z != 0 && row < CHUNK_ROWS) {
			const unsigned bit = static_cast<unsigned>(__builtin_ctzll(z));
			const char *nul = base + i + bit / 8;
			if (f.null_mask[row]) {
				FlatVector::SetNull(vec, row, true);
			} else {
				slots[row] = string_t(seg, static_cast<uint32_t>(nul - seg));
			}
			++row;
			seg = nul + 1;
			z &= z - 1;
		}
		i += 8;
	}
	// Tail (< 8 bytes left, or rows still pending).
	while (row < CHUNK_ROWS) {
		const char *nul = static_cast<const char *>(std::memchr(seg, '\0', static_cast<size_t>(base + total - seg)));
		if (!nul) {
			break;
		}
		if (f.null_mask[row]) {
			FlatVector::SetNull(vec, row, true);
		} else {
			slots[row] = string_t(seg, static_cast<uint32_t>(nul - seg));
		}
		++row;
		seg = nul + 1;
	}
	chunk.SetChildCardinality(CHUNK_ROWS);
	g_sink ^= total;
	return total;
}

// PREALLOC: skip the length pass entirely. Allocate the worst case (3 bytes
// per UTF-16 code unit — the max UTF-8 expansion; surrogate pairs are 2 units
// -> 4 bytes, so 3/unit bounds them too), convert ONCE, and take the real size
// from the conversion result. The all-ASCII verdict then comes for free
// (written == units => every unit produced exactly one byte), so the common
// case needs no boundary scan at all: one pass over the input, total.
size_t FillChunkBatchPrealloc(const Fixture &f, const StagedStrings &s, DataChunk &chunk) {
	chunk.Reset();
	auto &vec = chunk.data[0];
	char *base = AllocOutput(vec, s.delim_units * 3);
	const size_t written =
		simdutf::convert_valid_utf16le_to_utf8(reinterpret_cast<const char16_t *>(s.delim.data()), s.delim_units, base);
	auto *slots = FlatVector::GetDataMutable<string_t>(vec);

	if (written == s.delim_units) {
		// All ASCII: 1 unit -> 1 byte, so output offsets are input offsets / 2
		// (delimiters included — they are one unit / one byte each).
		for (idx_t row = 0; row < CHUNK_ROWS; ++row) {
			if (f.null_mask[row]) {
				FlatVector::SetNull(vec, row, true);
				continue;
			}
			slots[row] = string_t(base + s.doff[row] / 2, s.len[row] / 2);
		}
		chunk.SetChildCardinality(CHUNK_ROWS);
		g_sink ^= written;
		return written;
	}

	// Non-ASCII: recover boundaries with one word-wise pass over the output.
	idx_t row = 0;
	const char *seg = base;
	size_t i = 0;
	while (i + 8 <= written && row < CHUNK_ROWS) {
		uint64_t w;
		std::memcpy(&w, base + i, 8);
		uint64_t z = (w - 0x0101010101010101ULL) & ~w & 0x8080808080808080ULL;
		while (z != 0 && row < CHUNK_ROWS) {
			const unsigned bit = static_cast<unsigned>(__builtin_ctzll(z));
			const char *nul = base + i + bit / 8;
			if (f.null_mask[row]) {
				FlatVector::SetNull(vec, row, true);
			} else {
				slots[row] = string_t(seg, static_cast<uint32_t>(nul - seg));
			}
			++row;
			seg = nul + 1;
			z &= z - 1;
		}
		i += 8;
	}
	while (row < CHUNK_ROWS) {
		const char *nul = static_cast<const char *>(std::memchr(seg, '\0', static_cast<size_t>(base + written - seg)));
		if (!nul) {
			break;
		}
		if (f.null_mask[row]) {
			FlatVector::SetNull(vec, row, true);
		} else {
			slots[row] = string_t(seg, static_cast<uint32_t>(nul - seg));
		}
		++row;
		seg = nul + 1;
	}
	chunk.SetChildCardinality(CHUNK_ROWS);
	g_sink ^= written;
	return written;
}

// Word-wise forward search for the next NUL byte in [p, end).
inline const char *FindNulFrom(const char *p, const char *end) {
	while (p + 8 <= end) {
		uint64_t w;
		std::memcpy(&w, p, 8);
		const uint64_t z = (w - 0x0101010101010101ULL) & ~w & 0x8080808080808080ULL;
		if (z != 0) {
			return p + (static_cast<unsigned>(__builtin_ctzll(z)) / 8);
		}
		p += 8;
	}
	while (p < end && *p != '\0') {
		++p;
	}
	return p;
}

// PREALLOC + skip-ahead: value i has u_i code units, so its UTF-8 form is
// between u_i and 3*u_i bytes (1 byte for ASCII, 2 for U+0080-07FF, 3 for
// U+0800-FFFF, and 4 per surrogate PAIR = 2 per unit). The delimiter therefore
// cannot appear before seg + u_i — start there instead of at seg, and probe the
// exact 2-bytes-per-unit position first, which homogeneous scripts hit on the
// first try.
size_t FillChunkBatchPreallocSkip(const Fixture &f, const StagedStrings &s, DataChunk &chunk) {
	chunk.Reset();
	auto &vec = chunk.data[0];
	char *base = AllocOutput(vec, s.delim_units * 3);
	const size_t written =
		simdutf::convert_valid_utf16le_to_utf8(reinterpret_cast<const char16_t *>(s.delim.data()), s.delim_units, base);
	auto *slots = FlatVector::GetDataMutable<string_t>(vec);

	if (written == s.delim_units) {
		for (idx_t row = 0; row < CHUNK_ROWS; ++row) {
			if (f.null_mask[row]) {
				FlatVector::SetNull(vec, row, true);
				continue;
			}
			slots[row] = string_t(base + s.doff[row] / 2, s.len[row] / 2);
		}
		chunk.SetChildCardinality(CHUNK_ROWS);
		g_sink ^= written;
		return written;
	}

	const char *seg = base;
	const char *end = base + written;
	for (idx_t row = 0; row < CHUNK_ROWS; ++row) {
		const size_t u = s.len[row] / 2;
		const char *p = seg + u;  // lower bound: >= 1 byte per code unit
		if (p >= end) {
			break;
		}
		// The first NUL at or after the lower bound IS this value's delimiter:
		// no UTF-8 encoding of a non-zero code point contains a 0x00 byte, and
		// embedded U+0000 is excluded by the gate. Probing a *guessed* position
		// (e.g. 2*u for two-byte scripts) is NOT safe: for a value whose real
		// length falls between u and 2*u the guess can land on a LATER value's
		// delimiter and merge two strings.
		if (*p != '\0') {
			p = FindNulFrom(p, end);
		}
		if (f.null_mask[row]) {
			FlatVector::SetNull(vec, row, true);
		} else {
			slots[row] = string_t(seg, static_cast<uint32_t>(p - seg));
		}
		seg = p + 1;
	}
	chunk.SetChildCardinality(CHUNK_ROWS);
	g_sink ^= written;
	return written;
}

// Same skip-ahead, but the search itself uses libc memchr — which is already
// SIMD (NEON / AVX2) — instead of the hand-rolled 8-byte SWAR word sweep.
size_t FillChunkBatchPreallocSkipMemchr(const Fixture &f, const StagedStrings &s, DataChunk &chunk) {
	chunk.Reset();
	auto &vec = chunk.data[0];
	char *base = AllocOutput(vec, s.delim_units * 3);
	const size_t written =
		simdutf::convert_valid_utf16le_to_utf8(reinterpret_cast<const char16_t *>(s.delim.data()), s.delim_units, base);
	auto *slots = FlatVector::GetDataMutable<string_t>(vec);

	if (written == s.delim_units) {
		for (idx_t row = 0; row < CHUNK_ROWS; ++row) {
			if (f.null_mask[row]) {
				FlatVector::SetNull(vec, row, true);
				continue;
			}
			slots[row] = string_t(base + s.doff[row] / 2, s.len[row] / 2);
		}
		chunk.SetChildCardinality(CHUNK_ROWS);
		g_sink ^= written;
		return written;
	}

	const char *seg = base;
	const char *end = base + written;
	for (idx_t row = 0; row < CHUNK_ROWS; ++row) {
		const size_t u = s.len[row] / 2;
		const char *p = seg + u;
		if (p >= end) {
			break;
		}
		if (*p != '\0') {
			const char *hit = static_cast<const char *>(std::memchr(p, '\0', static_cast<size_t>(end - p)));
			if (!hit) {
				break;
			}
			p = hit;
		}
		if (f.null_mask[row]) {
			FlatVector::SetNull(vec, row, true);
		} else {
			slots[row] = string_t(seg, static_cast<uint32_t>(p - seg));
		}
		seg = p + 1;
	}
	chunk.SetChildCardinality(CHUNK_ROWS);
	g_sink ^= written;
	return written;
}

// Floor for the prealloc scheme: worst-case allocation + one conversion, no
// length pass, no boundary work.
size_t FillChunkConvertOnlyPrealloc(const Fixture &f, const StagedStrings &s, DataChunk &chunk) {
	(void)f;
	chunk.Reset();
	auto &vec = chunk.data[0];
	char *base = AllocOutput(vec, s.code_units * 3);
	const size_t written = s.code_units == 0
							   ? 0
							   : simdutf::convert_valid_utf16le_to_utf8(
									 reinterpret_cast<const char16_t *>(s.payload.data()), s.code_units, base);
	chunk.SetChildCardinality(CHUNK_ROWS);
	g_sink ^= written;
	return written;
}

// Diagnostic floor: the bulk conversion ONLY — no boundary discovery, no
// string_t construction. Not a usable path (the chunk is left unfilled); it
// isolates how much of each variant is simdutf and how much is bookkeeping.
size_t FillChunkConvertOnly(const Fixture &f, const StagedStrings &s, DataChunk &chunk) {
	(void)f;
	chunk.Reset();
	auto &vec = chunk.data[0];
	const size_t total = s.code_units == 0 ? 0
										   : simdutf::utf8_length_from_utf16le(
												 reinterpret_cast<const char16_t *>(s.payload.data()), s.code_units);
	char *base = AllocOutput(vec, total);
	if (s.code_units > 0) {
		simdutf::convert_valid_utf16le_to_utf8(reinterpret_cast<const char16_t *>(s.payload.data()), s.code_units,
											   base);
	}
	chunk.SetChildCardinality(CHUNK_ROWS);
	g_sink ^= total;
	return total;
}

// Reference for the batch variants: identical to the current path on valid
// input; standard U+FFFD substitution on invalid input.
bool VerifyChunkStandard(const Fixture &f, DataChunk &chunk) {
	auto &vec = chunk.data[0];
	for (idx_t row = 0; row < CHUNK_ROWS; ++row) {
		if (f.null_mask[row]) {
			if (!FlatVector::IsNull(vec, row)) {
				return false;
			}
			continue;
		}
		const auto &v = f.rows[row];
		std::string expect;
		if (duckdb::tds::encoding::Utf8LengthFromUtf16LEView(v.data(), v.size()) == SIZE_MAX) {
			expect = DecodeReplacingStandard(v.data(), v.size());
		} else {
			expect = duckdb::tds::encoding::Utf16LEDecode(v.data(), v.size());
		}
		auto got = FlatVector::GetDataMutable<string_t>(vec)[row];
		if (std::string(got.GetData(), got.GetSize()) != expect) {
			return false;
		}
	}
	return true;
}

//===----------------------------------------------------------------------===//
// END-TO-END architecture probe (spec 055 D6): row-major wire image ->
// DataChunk, both ways, INCLUDING the cost of getting the bytes out of the
// row framing. Every other group in this file starts from "value bytes already
// in hand"; this one does not, so it is the honest test of whether staging
// pays for itself.
//
//   wire  : [u16 len | bytes] per column, per row (0xFFFF = NULL) — the shape
//           RowReader walks today.
//   path A: copy each value into a reused per-column buffer (RowData) then
//           call the per-value codec — production today.
//   path B: append raw bytes into per-column staging (or straight into the
//           DuckDB vector for 1:1 types), then ONE finalize per column.
//===----------------------------------------------------------------------===//

// Defined with the DECIMAL kernel section below.
inline duckdb::hugeint_t ConvertDecimalFast(const uint8_t *data, size_t length);

enum class WireCol : uint8_t { Bigint, Nvarchar16, Decimal18s6, Uuid };

struct WireImage {
	std::vector<uint8_t> bytes;	 // row-major, framed
	std::vector<WireCol> cols;
	idx_t rows = CHUNK_ROWS;
};

WireImage BuildWireImage(const std::vector<WireCol> &cols, int null_pct) {
	WireImage w;
	w.cols = cols;
	for (idx_t row = 0; row < CHUNK_ROWS; ++row) {
		const bool is_null = null_pct > 0 && (row % 100) < static_cast<idx_t>(null_pct);
		for (WireCol c : cols) {
			if (is_null) {
				w.bytes.push_back(0xFF);
				w.bytes.push_back(0xFF);
				continue;
			}
			std::vector<uint8_t> v;
			const uint64_t rv = RowValue(row);
			switch (c) {
			case WireCol::Bigint:
				AppendLe(v, rv, 8);
				break;
			case WireCol::Nvarchar16:
				for (int i = 0; i < 16; ++i) {
					AppendUnit(v, static_cast<uint16_t>('a' + ((rv >> i) & 0xF)));
				}
				break;
			case WireCol::Decimal18s6:
				v.push_back(0x01);
				AppendLe(v, rv, 8);
				break;
			case WireCol::Uuid:
				AppendLe(v, rv, 8);
				AppendLe(v, ~rv, 8);
				break;
			}
			const uint16_t len = static_cast<uint16_t>(v.size());
			w.bytes.push_back(static_cast<uint8_t>(len & 0xFF));
			w.bytes.push_back(static_cast<uint8_t>(len >> 8));
			w.bytes.insert(w.bytes.end(), v.begin(), v.end());
		}
	}
	return w;
}

LogicalType WireColType(WireCol c) {
	switch (c) {
	case WireCol::Bigint:
		return LogicalType::BIGINT;
	case WireCol::Nvarchar16:
		return LogicalType::VARCHAR;
	case WireCol::Decimal18s6:
		return LogicalType::DECIMAL(18, 6);
	case WireCol::Uuid:
		return LogicalType::UUID;
	}
	return LogicalType::BIGINT;
}

duckdb::tds::ColumnMetadata WireColMeta(WireCol c) {
	duckdb::tds::ColumnMetadata m;
	m.name = "c";
	m.collation = 0;
	m.flags = 0;
	m.precision = 0;
	m.scale = 0;
	switch (c) {
	case WireCol::Bigint:
		m.type_id = duckdb::tds::TDS_TYPE_INTN;
		m.max_length = 8;
		break;
	case WireCol::Nvarchar16:
		m.type_id = duckdb::tds::TDS_TYPE_NVARCHAR;
		m.max_length = 0x1FFE;
		break;
	case WireCol::Decimal18s6:
		m.type_id = duckdb::tds::TDS_TYPE_DECIMAL;
		m.max_length = 9;
		m.precision = 18;
		m.scale = 6;
		break;
	case WireCol::Uuid:
		m.type_id = duckdb::tds::TDS_TYPE_UNIQUEIDENTIFIER;
		m.max_length = 16;
		break;
	}
	return m;
}

// Path A — today's shape: per-value copy into a reused buffer, per-value decode.
void FillWirePerValue(const WireImage &w, DataChunk &chunk, std::vector<std::vector<uint8_t>> &rowbuf,
					  const std::vector<duckdb::tds::ColumnMetadata> &metas) {
	chunk.Reset();
	const uint8_t *p = w.bytes.data();
	for (idx_t row = 0; row < CHUNK_ROWS; ++row) {
		for (size_t c = 0; c < w.cols.size(); ++c) {
			const uint16_t len = static_cast<uint16_t>(p[0] | (p[1] << 8));
			p += 2;
			auto &vec = chunk.data[c];
			if (len == 0xFFFF) {
				FlatVector::SetNull(vec, row, true);
				continue;
			}
			auto &buf = rowbuf[c];
			buf.assign(p, p + len);	 // the RowData::values copy
			p += len;
			switch (w.cols[c]) {
			case WireCol::Bigint:
				duckdb::mssql::codec::integer::DecodeFromTds(buf, metas[c], vec, row);
				break;
			case WireCol::Nvarchar16:
				duckdb::mssql::codec::string::DecodeFromTds(buf, metas[c], vec, row);
				break;
			case WireCol::Decimal18s6:
				duckdb::mssql::codec::decimal::DecodeFromTds(buf, metas[c], vec, row);
				break;
			case WireCol::Uuid:
				duckdb::mssql::codec::uuid::DecodeFromTds(buf, metas[c], vec, row);
				break;
			}
		}
	}
	chunk.SetChildCardinality(CHUNK_ROWS);
	g_sink ^= CHUNK_ROWS;
}

// Path B — staged: raw append per value (or direct write for 1:1 types), then
// one batch finalize per column.
struct RawColumn {
	WireCol kind;
	// StagedFixed / StagedVar
	std::vector<uint8_t> raw;		 // fixed: row * stride; var: delimited payload
	std::vector<uint32_t> off, len;	 // var only
	std::vector<uint64_t> validity;
	uint32_t stride = 0;
	// DirectFixed
	uint8_t *direct = nullptr;
};

// `bigint_via_staging` picks how a 1:1 fixed-width column reaches the vector:
//   false — the append writes straight into the DuckDB vector (no staging);
//   true  — the append writes into a staging array, and finalize copies the
//           whole column with ONE memcpy.
// Both are "batch"; the question is whether the bulk memcpy is worth the extra
// buffer, given that the wire layout already equals the DuckDB layout.
void FillWireStaged(const WireImage &w, DataChunk &chunk, std::vector<RawColumn> &raws, bool bigint_via_staging) {
	chunk.Reset();
	const size_t ncols = w.cols.size();

	// Per-chunk setup: resolve the destination once per column.
	for (size_t c = 0; c < ncols; ++c) {
		auto &rc = raws[c];
		std::fill(rc.validity.begin(), rc.validity.end(), ~0ULL);
		switch (rc.kind) {
		case WireCol::Bigint:
			rc.direct = bigint_via_staging
							? rc.raw.data()
							: reinterpret_cast<uint8_t *>(FlatVector::GetDataMutable<int64_t>(chunk.data[c]));
			break;
		default:
			rc.raw.clear();
			break;
		}
	}

	const uint8_t *p = w.bytes.data();
	for (idx_t row = 0; row < CHUNK_ROWS; ++row) {
		for (size_t c = 0; c < ncols; ++c) {
			auto &rc = raws[c];
			const uint16_t len = static_cast<uint16_t>(p[0] | (p[1] << 8));
			p += 2;
			if (len == 0xFFFF) {
				rc.validity[row / 64] &= ~(1ULL << (row % 64));
				if (rc.kind == WireCol::Nvarchar16) {
					rc.off[row] = static_cast<uint32_t>(rc.raw.size());
					rc.len[row] = 0;
					AppendUnit(rc.raw, 0x0000);	 // delimiter for NULL rows too
				}
				continue;
			}
			switch (rc.kind) {
			case WireCol::Bigint:
				std::memcpy(rc.direct + row * 8, p, 8);
				break;
			case WireCol::Uuid:
				break;	// not part of the wire probe
			case WireCol::Decimal18s6:
				std::memcpy(rc.raw.data() + row * rc.stride, p, len);
				break;
			case WireCol::Nvarchar16:
				rc.off[row] = static_cast<uint32_t>(rc.raw.size());
				rc.len[row] = len;
				rc.raw.insert(rc.raw.end(), p, p + len);
				AppendUnit(rc.raw, 0x0000);
				break;
			}
			p += len;
		}
	}

	// One finalize per column.
	for (size_t c = 0; c < ncols; ++c) {
		auto &rc = raws[c];
		auto &vec = chunk.data[c];
		auto &mask = FlatVector::ValidityMutable(vec);
		mask.EnsureWritable();
		std::memcpy(mask.GetData(), rc.validity.data(), rc.validity.size() * sizeof(uint64_t));

		switch (rc.kind) {
		case WireCol::Bigint:
			if (bigint_via_staging) {
				// The whole column in one memcpy — legal only because the TDS
				// layout for INTN(8) already equals DuckDB's int64_t.
				std::memcpy(FlatVector::GetDataMutable<int64_t>(vec), rc.raw.data(), CHUNK_ROWS * 8);
			}
			break;	// otherwise it was written straight into the vector
		case WireCol::Uuid:
			break;
		case WireCol::Decimal18s6: {
			auto *out = FlatVector::GetDataMutable<int64_t>(vec);
			for (idx_t row = 0; row < CHUNK_ROWS; ++row) {
				out[row] = static_cast<int64_t>(ConvertDecimalFast(rc.raw.data() + row * rc.stride, rc.stride).lower);
			}
			break;
		}
		case WireCol::Nvarchar16: {
			const size_t units = rc.raw.size() / 2;
			char *base = AllocOutput(vec, units * 3);
			const size_t written =
				simdutf::convert_valid_utf16le_to_utf8(reinterpret_cast<const char16_t *>(rc.raw.data()), units, base);
			auto *slots = FlatVector::GetDataMutable<string_t>(vec);
			if (written == units) {
				for (idx_t row = 0; row < CHUNK_ROWS; ++row) {
					slots[row] = string_t(base + rc.off[row] / 2, rc.len[row] / 2);
				}
			} else {
				const char *seg = base;
				const char *end = base + written;
				for (idx_t row = 0; row < CHUNK_ROWS; ++row) {
					const char *q = seg + rc.len[row] / 2;
					if (q >= end) {
						break;
					}
					if (*q != '\0') {
						q = FindNulFrom(q, end);
					}
					slots[row] = string_t(seg, static_cast<uint32_t>(q - seg));
					seg = q + 1;
				}
			}
			break;
		}
		}
	}
	chunk.SetChildCardinality(CHUNK_ROWS);
	g_sink ^= CHUNK_ROWS;
}

std::vector<RawColumn> MakeRawColumns(const std::vector<WireCol> &cols) {
	std::vector<RawColumn> raws(cols.size());
	for (size_t c = 0; c < cols.size(); ++c) {
		raws[c].kind = cols[c];
		raws[c].validity.assign(CHUNK_ROWS / 64, ~0ULL);
		switch (cols[c]) {
		case WireCol::Bigint:
			raws[c].stride = 8;
			raws[c].raw.assign(CHUNK_ROWS * 8, 0);	// used only by the staging variant
			break;
		case WireCol::Uuid:
			break;
		case WireCol::Decimal18s6:
			raws[c].stride = 9;
			raws[c].raw.assign(CHUNK_ROWS * 9, 0);
			break;
		case WireCol::Nvarchar16:
			raws[c].off.assign(CHUNK_ROWS, 0);
			raws[c].len.assign(CHUNK_ROWS, 0);
			raws[c].raw.reserve(CHUNK_ROWS * 34);
			break;
		default:
			break;
		}
	}
	return raws;
}

bool VerifyWireChunks(DataChunk &a, DataChunk &b, size_t ncols) {
	for (size_t c = 0; c < ncols; ++c) {
		for (idx_t row = 0; row < CHUNK_ROWS; ++row) {
			const bool na = FlatVector::IsNull(a.data[c], row);
			if (na != FlatVector::IsNull(b.data[c], row)) {
				return false;
			}
			if (na) {
				continue;
			}
			if (a.data[c].GetValue(row) != b.data[c].GetValue(row)) {
				return false;
			}
		}
	}
	return true;
}

//===----------------------------------------------------------------------===//
// Cardinality analyzer prototypes (spec 056 evaluation).
//
// Measures the cost of DETECTION alone — separately from any saving it
// enables. The question phase 2 must answer with numbers: now that the decode
// itself is ~2 ns/value, does finding out that a column is low-cardinality
// cost more than converting every value would?
//===----------------------------------------------------------------------===//

// All-equal probe: compare every value against value 0. One branch per value,
// no hashing — this is the CONSTANT detector.
bool AnalyzeConstantStrings(const Fixture &f, const StagedStrings &s) {
	idx_t first = CHUNK_ROWS;
	for (idx_t row = 0; row < CHUNK_ROWS; ++row) {
		if (!f.null_mask[row]) {
			first = row;
			break;
		}
	}
	if (first == CHUNK_ROWS) {
		return true;  // all NULL
	}
	const uint8_t *fp = s.payload.data() + s.off[first];
	const uint32_t fl = s.len[first];
	for (idx_t row = first + 1; row < CHUNK_ROWS; ++row) {
		if (f.null_mask[row]) {
			continue;
		}
		if (s.len[row] != fl || std::memcmp(s.payload.data() + s.off[row], fp, fl) != 0) {
			return false;
		}
	}
	return true;
}

// Open-addressing dedup over raw staged bytes, bailing out once the unique
// count passes `cap` (the mssql_scan_dictionary_max policy). Returns the
// unique count, or cap+1 to signal overflow.
size_t AnalyzeDictStrings(const Fixture &f, const StagedStrings &s, size_t cap) {
	constexpr size_t TABLE = 256;  // cap 100 -> load factor < 0.5
	uint32_t slot_off[TABLE];
	uint32_t slot_len[TABLE];
	bool used[TABLE] = {false};
	size_t uniques = 0;
	for (idx_t row = 0; row < CHUNK_ROWS; ++row) {
		if (f.null_mask[row]) {
			continue;
		}
		const uint8_t *p = s.payload.data() + s.off[row];
		const uint32_t len = s.len[row];
		uint64_t h = 1469598103934665603ULL;  // FNV-1a
		for (uint32_t i = 0; i < len; ++i) {
			h = (h ^ p[i]) * 1099511628211ULL;
		}
		size_t idx = static_cast<size_t>(h) & (TABLE - 1);
		while (true) {
			if (!used[idx]) {
				if (uniques == cap) {
					return cap + 1;	 // overflow: abandon, fall back to FLAT
				}
				used[idx] = true;
				slot_off[idx] = s.off[row];
				slot_len[idx] = len;
				++uniques;
				break;
			}
			if (slot_len[idx] == len && std::memcmp(s.payload.data() + slot_off[idx], p, len) == 0) {
				break;	// hit, confirmed by full equality
			}
			idx = (idx + 1) & (TABLE - 1);
		}
	}
	return uniques;
}

// Same dedup, but the hash reads at most 8 bytes (length + prefix word)
// instead of the whole value; a hit is still confirmed by full equality, so
// correctness is unchanged — only the hashing cost drops.
size_t AnalyzeDictStringsPrefix(const Fixture &f, const StagedStrings &s, size_t cap) {
	constexpr size_t TABLE = 256;
	uint32_t slot_off[TABLE];
	uint32_t slot_len[TABLE];
	bool used[TABLE] = {false};
	size_t uniques = 0;
	for (idx_t row = 0; row < CHUNK_ROWS; ++row) {
		if (f.null_mask[row]) {
			continue;
		}
		const uint8_t *p = s.payload.data() + s.off[row];
		const uint32_t len = s.len[row];
		uint64_t word = 0;
		std::memcpy(&word, p, len < 8 ? len : 8);
		uint64_t h = (word ^ (static_cast<uint64_t>(len) << 56)) * 11400714819323198485ULL;
		size_t idx = static_cast<size_t>(h >> 56) & (TABLE - 1);
		while (true) {
			if (!used[idx]) {
				if (uniques == cap) {
					return cap + 1;
				}
				used[idx] = true;
				slot_off[idx] = s.off[row];
				slot_len[idx] = len;
				++uniques;
				break;
			}
			if (slot_len[idx] == len && std::memcmp(s.payload.data() + slot_off[idx], p, len) == 0) {
				break;
			}
			idx = (idx + 1) & (TABLE - 1);
		}
	}
	return uniques;
}

std::vector<int64_t> BuildIntColumn(size_t cardinality) {
	std::vector<int64_t> v(CHUNK_ROWS);
	for (idx_t row = 0; row < CHUNK_ROWS; ++row) {
		v[row] = static_cast<int64_t>(RowValue(row % std::max<size_t>(1, cardinality)));
	}
	return v;
}

// Fixed-width constant probe — branchless-friendly, auto-vectorizable.
bool AnalyzeConstantInt(const std::vector<int64_t> &v) {
	const int64_t first = v[0];
	int64_t diff = 0;
	for (idx_t row = 1; row < CHUNK_ROWS; ++row) {
		diff |= v[row] ^ first;
	}
	return diff == 0;
}

// min/max in one vectorizable pass: a narrow range enables direct-mapped
// dictionary building with no hashing at all.
void AnalyzeRangeInt(const std::vector<int64_t> &v, int64_t &lo, int64_t &hi) {
	lo = v[0];
	hi = v[0];
	for (idx_t row = 1; row < CHUNK_ROWS; ++row) {
		lo = v[row] < lo ? v[row] : lo;
		hi = v[row] > hi ? v[row] : hi;
	}
}

size_t AnalyzeDictInt(const std::vector<int64_t> &v, size_t cap) {
	constexpr size_t TABLE = 256;
	int64_t keys[TABLE];
	bool used[TABLE] = {false};
	size_t uniques = 0;
	for (idx_t row = 0; row < CHUNK_ROWS; ++row) {
		const int64_t key = v[row];
		uint64_t h = static_cast<uint64_t>(key) * 11400714819323198485ULL;
		size_t idx = static_cast<size_t>(h >> 56) & (TABLE - 1);
		while (true) {
			if (!used[idx]) {
				if (uniques == cap) {
					return cap + 1;
				}
				used[idx] = true;
				keys[idx] = key;
				++uniques;
				break;
			}
			if (keys[idx] == key) {
				break;
			}
			idx = (idx + 1) & (TABLE - 1);
		}
	}
	return uniques;
}

//===----------------------------------------------------------------------===//
// DECIMAL batch kernel (spec 055 D1 candidate).
//
// The current ConvertDecimal does a full 128-bit multiply-add PER BYTE of the
// mantissa (decimal_encoding.cpp:19-21). The TDS magnitude is little-endian,
// so the words can be loaded directly. Little-endian host assumed — the real
// implementation needs a byte-swap fallback behind an endianness check.
//===----------------------------------------------------------------------===//

inline duckdb::hugeint_t ConvertDecimalFast(const uint8_t *data, size_t length) {
	if (length == 0) {
		return duckdb::hugeint_t(0);
	}
	size_t mag = length - 1;
	if (mag > 16) {
		mag = 16;
	}
	uint64_t lo = 0, hi = 0;
	const size_t lo_bytes = mag < 8 ? mag : 8;
	std::memcpy(&lo, data + 1, lo_bytes);
	if (mag > 8) {
		std::memcpy(&hi, data + 9, mag - 8);
	}
	duckdb::hugeint_t v;
	v.lower = lo;
	v.upper = static_cast<int64_t>(hi);
	return data[0] == 0 ? -v : v;
}

// Staged fixed-width column: contiguous wire bytes at a constant stride.
struct StagedFixed {
	std::vector<uint8_t> data;
	size_t stride = 0;
};

StagedFixed StageFixed(const FixedCell &c) {
	StagedFixed s;
	for (const auto &r : c.rows) {
		if (!r.empty()) {
			s.stride = r.size();
			break;
		}
	}
	s.data.assign(CHUNK_ROWS * s.stride, 0);
	for (idx_t row = 0; row < CHUNK_ROWS; ++row) {
		if (!c.rows[row].empty()) {
			std::memcpy(s.data.data() + row * s.stride, c.rows[row].data(), s.stride);
		}
	}
	return s;
}

// One batch kernel per physical type, over the staged column. `fast` picks the
// direct word-assembly kernel over the current per-byte multiply loop, so the
// two effects (batch loop vs kernel) can be separated.
size_t FillChunkDecimalBatch(const FixedCell &c, const StagedFixed &s, DataChunk &chunk, bool fast) {
	chunk.Reset();
	auto &vec = chunk.data[0];
	const uint8_t *base = s.data.data();
	const size_t stride = s.stride;
	const uint8_t prec = c.col.precision;

	for (idx_t row = 0; row < CHUNK_ROWS; ++row) {
		if (c.rows[row].empty()) {
			FlatVector::SetNull(vec, row, true);
		}
	}
	if (prec <= 4) {
		auto *out = FlatVector::GetDataMutable<int16_t>(vec);
		for (idx_t row = 0; row < CHUNK_ROWS; ++row) {
			const duckdb::hugeint_t v =
				fast ? ConvertDecimalFast(base + row * stride, stride)
					 : duckdb::tds::encoding::DecimalEncoding::ConvertDecimal(base + row * stride, stride);
			out[row] = static_cast<int16_t>(v.lower);
		}
	} else if (prec <= 9) {
		auto *out = FlatVector::GetDataMutable<int32_t>(vec);
		for (idx_t row = 0; row < CHUNK_ROWS; ++row) {
			const duckdb::hugeint_t v =
				fast ? ConvertDecimalFast(base + row * stride, stride)
					 : duckdb::tds::encoding::DecimalEncoding::ConvertDecimal(base + row * stride, stride);
			out[row] = static_cast<int32_t>(v.lower);
		}
	} else if (prec <= 18) {
		auto *out = FlatVector::GetDataMutable<int64_t>(vec);
		for (idx_t row = 0; row < CHUNK_ROWS; ++row) {
			const duckdb::hugeint_t v =
				fast ? ConvertDecimalFast(base + row * stride, stride)
					 : duckdb::tds::encoding::DecimalEncoding::ConvertDecimal(base + row * stride, stride);
			out[row] = static_cast<int64_t>(v.lower);
		}
	} else {
		auto *out = FlatVector::GetDataMutable<duckdb::hugeint_t>(vec);
		for (idx_t row = 0; row < CHUNK_ROWS; ++row) {
			out[row] = fast ? ConvertDecimalFast(base + row * stride, stride)
							: duckdb::tds::encoding::DecimalEncoding::ConvertDecimal(base + row * stride, stride);
		}
	}
	chunk.SetChildCardinality(CHUNK_ROWS);
	g_sink ^= CHUNK_ROWS;
	return c.in_bytes;
}

}  // namespace

int main() {
	std::printf("[bench_materialize] spec 054 D1 — string-decode / fixed-decode / bcp-encode groups (current path)\n");
	std::printf(
		"group\tcell\tus_per_chunk_median\tus_p10\tus_p90\tns_per_value_median"
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
					r.median_us_per_chunk, r.p10_us_per_chunk, r.p90_us_per_chunk, r.median_ns_per_value, f.utf16_bytes,
					out_bytes, correct ? "PASS" : "FAIL");

		// --- spec-055 batch prototypes over one staged column ---
		StagedStrings staged = StageColumn(f);
		const size_t iters = IterationsFor(f.utf16_bytes);

		struct Variant {
			const char *group;
			bool applicable;
			size_t (*fn)(const Fixture &, const StagedStrings &, DataChunk &);
		};
		const Variant variants[] = {
			{"string_decode_batch_a", true, FillChunkBatchA},
			{"string_decode_batch_b", staged.all_valid && !staged.boundary_risky, FillChunkBatchB},
			{"string_decode_batch_c", staged.all_valid && !staged.saw_embedded_nul, FillChunkBatchC},
			{"string_decode_batch_c2", staged.all_valid && !staged.saw_embedded_nul, FillChunkBatchC2},
			{"string_decode_batch_ascii", staged.all_valid && staged.all_ascii, FillChunkBatchAscii},
			{"string_decode_batch_prealloc", staged.all_valid && !staged.saw_embedded_nul, FillChunkBatchPrealloc},
			{"string_decode_batch_prealloc_skip", staged.all_valid && !staged.saw_embedded_nul,
			 FillChunkBatchPreallocSkip},
			{"string_decode_batch_prealloc_memchr", staged.all_valid && !staged.saw_embedded_nul,
			 FillChunkBatchPreallocSkipMemchr},
			{"string_decode_convert_only", staged.all_valid, FillChunkConvertOnly},
			{"string_decode_convonly_prealloc", staged.all_valid, FillChunkConvertOnlyPrealloc},
		};
		for (const auto &v : variants) {
			if (!v.applicable) {
				std::printf("%s\t%s\t-\t-\t-\t-\t%zu\t-\tN/A\n", v.group, spec.name.c_str(), f.utf16_bytes);
				continue;
			}
			size_t vbytes = v.fn(f, staged, chunk);
			// convert_only leaves the chunk unfilled by design — it is a floor
			// measurement, not a materialization path.
			bool vcorrect = (v.fn == FillChunkConvertOnly || v.fn == FillChunkConvertOnlyPrealloc)
								? true
								: VerifyChunkStandard(f, chunk);
			if (!vcorrect) {
				failures++;
			}
			auto vr = TimeCell([&]() { v.fn(f, staged, chunk); }, iters);
			std::printf("%s\t%s\t%.1f\t%.1f\t%.1f\t%.1f\t%zu\t%zu\t%s\n", v.group, spec.name.c_str(),
						vr.median_us_per_chunk, vr.p10_us_per_chunk, vr.p90_us_per_chunk, vr.median_ns_per_value,
						f.utf16_bytes, vbytes, vcorrect ? "PASS" : "FAIL");
		}
	}

	// --- spec-055 D6: end-to-end wire image -> DataChunk, both architectures ---
	for (int null_pct : {0, 20}) {
		const std::vector<WireCol> cols = {WireCol::Bigint, WireCol::Nvarchar16, WireCol::Decimal18s6};
		WireImage wire = BuildWireImage(cols, null_pct);
		duckdb::vector<LogicalType> types;
		std::vector<duckdb::tds::ColumnMetadata> metas;
		for (WireCol c : cols) {
			types.push_back(WireColType(c));
			metas.push_back(WireColMeta(c));
		}
		const std::string name = "3col_null" + std::to_string(null_pct);
		const size_t values = CHUNK_ROWS * cols.size();

		DataChunk ca, cb;
		ca.Initialize(duckdb::Allocator::DefaultAllocator(), types);
		cb.Initialize(duckdb::Allocator::DefaultAllocator(), types);
		std::vector<std::vector<uint8_t>> rowbuf(cols.size());
		std::vector<RawColumn> raws = MakeRawColumns(cols);

		FillWirePerValue(wire, ca, rowbuf, metas);
		FillWireStaged(wire, cb, raws, /*bigint_via_staging=*/false);
		const bool ok = VerifyWireChunks(ca, cb, cols.size());
		if (!ok) {
			failures++;
		}

		auto ra = TimeCell([&]() { FillWirePerValue(wire, ca, rowbuf, metas); }, 400);
		auto rb = TimeCell([&]() { FillWireStaged(wire, cb, raws, false); }, 400);
		auto rm = TimeCell([&]() { FillWireStaged(wire, cb, raws, true); }, 400);

		// ns per VALUE (chunk holds CHUNK_ROWS * ncols values).
		std::printf("wire_chunk_pervalue\t%s\t%.1f\t%.1f\t%.1f\t%.1f\t%zu\t-\t%s\n", name.c_str(),
					ra.median_us_per_chunk, ra.p10_us_per_chunk, ra.p90_us_per_chunk,
					ra.median_us_per_chunk * 1000.0 / values, wire.bytes.size(), ok ? "PASS" : "FAIL");
		std::printf("wire_chunk_staged_direct\t%s\t%.1f\t%.1f\t%.1f\t%.1f\t%zu\t-\t%s\n", name.c_str(),
					rb.median_us_per_chunk, rb.p10_us_per_chunk, rb.p90_us_per_chunk,
					rb.median_us_per_chunk * 1000.0 / values, wire.bytes.size(), ok ? "PASS" : "FAIL");
		std::printf("wire_chunk_staged_memcpy\t%s\t%.1f\t%.1f\t%.1f\t%.1f\t%zu\t-\tPASS\n", name.c_str(),
					rm.median_us_per_chunk, rm.p10_us_per_chunk, rm.p90_us_per_chunk,
					rm.median_us_per_chunk * 1000.0 / values, wire.bytes.size());
	}

	// --- spec-056 evaluation: what does DETECTING low cardinality cost? ---
	for (size_t len : {4, 16}) {
		for (size_t card : {1, 10, 100, 101, 2048}) {
			CellSpec cs;
			cs.cardinality = card;
			cs.len_units = len;
			cs.name = "len" + std::to_string(len) + "_card" + std::to_string(card);
			Fixture f = BuildFixture(cs);
			StagedStrings staged = StageColumn(f);

			auto rp = TimeCell([&]() { g_sink ^= AnalyzeDictStringsPrefix(f, staged, 100); }, 400);
			std::printf("analyze_dict_string_prefix\t%s\t%.1f\t%.1f\t%.1f\t%.1f\t%zu\t-\tPASS\n", cs.name.c_str(),
						rp.median_us_per_chunk, rp.p10_us_per_chunk, rp.p90_us_per_chunk, rp.median_ns_per_value,
						f.utf16_bytes);

			auto rc = TimeCell([&]() { g_sink ^= AnalyzeConstantStrings(f, staged) ? 1u : 0u; }, 400);
			std::printf("analyze_const_string\t%s\t%.1f\t%.1f\t%.1f\t%.1f\t%zu\t-\tPASS\n", cs.name.c_str(),
						rc.median_us_per_chunk, rc.p10_us_per_chunk, rc.p90_us_per_chunk, rc.median_ns_per_value,
						f.utf16_bytes);

			auto rd = TimeCell([&]() { g_sink ^= AnalyzeDictStrings(f, staged, 100); }, 400);
			std::printf("analyze_dict_string\t%s\t%.1f\t%.1f\t%.1f\t%.1f\t%zu\t-\tPASS\n", cs.name.c_str(),
						rd.median_us_per_chunk, rd.p10_us_per_chunk, rd.p90_us_per_chunk, rd.median_ns_per_value,
						f.utf16_bytes);

			std::vector<int64_t> ints = BuildIntColumn(card);
			auto ri = TimeCell([&]() { g_sink ^= AnalyzeConstantInt(ints) ? 1u : 0u; }, 400);
			std::printf("analyze_const_int\t%s\t%.1f\t%.1f\t%.1f\t%.1f\t-\t-\tPASS\n", cs.name.c_str(),
						ri.median_us_per_chunk, ri.p10_us_per_chunk, ri.p90_us_per_chunk, ri.median_ns_per_value);

			auto rr = TimeCell(
				[&]() {
					int64_t lo, hi;
					AnalyzeRangeInt(ints, lo, hi);
					g_sink ^= static_cast<uint64_t>(hi - lo);
				},
				400);
			std::printf("analyze_range_int\t%s\t%.1f\t%.1f\t%.1f\t%.1f\t-\t-\tPASS\n", cs.name.c_str(),
						rr.median_us_per_chunk, rr.p10_us_per_chunk, rr.p90_us_per_chunk, rr.median_ns_per_value);

			auto rdi = TimeCell([&]() { g_sink ^= AnalyzeDictInt(ints, 100); }, 400);
			std::printf("analyze_dict_int\t%s\t%.1f\t%.1f\t%.1f\t%.1f\t-\t-\tPASS\n", cs.name.c_str(),
						rdi.median_us_per_chunk, rdi.p10_us_per_chunk, rdi.p90_us_per_chunk, rdi.median_ns_per_value);
		}
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
						r.median_us_per_chunk, r.p10_us_per_chunk, r.p90_us_per_chunk, r.median_ns_per_value,
						fc.in_bytes, correct ? "PASS" : "FAIL");

			// --- spec-055: staged batch loop, current kernel vs direct-assembly kernel ---
			if (fc.col.type_id == duckdb::tds::TDS_TYPE_DECIMAL) {
				StagedFixed sf = StageFixed(fc);
				for (bool fast : {false, true}) {
					FillChunkDecimalBatch(fc, sf, chunk, fast);
					bool bcorrect = VerifyFixed(fc, chunk);
					if (!bcorrect) {
						failures++;
					}
					auto br = TimeCell([&]() { FillChunkDecimalBatch(fc, sf, chunk, fast); }, 400);
					std::printf("%s\t%s\t%.1f\t%.1f\t%.1f\t%.1f\t%zu\t-\t%s\n",
								fast ? "fixed_decode_batch_fastkernel" : "fixed_decode_batch_curkernel",
								fc.name.c_str(), br.median_us_per_chunk, br.p10_us_per_chunk, br.p90_us_per_chunk,
								br.median_ns_per_value, fc.in_bytes, bcorrect ? "PASS" : "FAIL");
				}
			}
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

			size_t out_bytes = EncodeChunkPerRow(f, buf);
			bool correct = VerifyBcp(f, buf);
			if (!correct) {
				failures++;
			}

			auto r = TimeCell([&]() { EncodeChunkPerRow(f, buf); }, 400);

			std::printf("bcp_encode_perrow\t%s\t%.1f\t%.1f\t%.1f\t%.1f\t-\t%zu\t%s\n", bspec.name.c_str(),
						r.median_us_per_chunk, r.p10_us_per_chunk, r.p90_us_per_chunk, r.median_ns_per_value, out_bytes,
						correct ? "PASS" : "FAIL");

			size_t repr_bytes = EncodeChunkReprAware(f, buf);
			// Same wire bytes as the shipped path — representation must be
			// invisible on the wire (DECIMAL uses the documented Value-overload
			// reference, so it is compared against itself only).
			duckdb::vector<uint8_t> hoisted_ref;
			duckdb::tds::encoding::BCPRowEncoder::EncodeChunk(hoisted_ref, *f.chunk, f.cols, nullptr);
			const bool repr_correct = bspec.kind == BcpCellSpec::Kind::Decimal18s6
										  ? true
										  : (hoisted_ref.size() == repr_bytes &&
											 std::equal(hoisted_ref.begin(), hoisted_ref.end(), buf.begin()));
			if (!repr_correct) {
				failures++;
			}
			auto rr = TimeCell([&]() { EncodeChunkReprAware(f, buf); }, 400);
			std::printf("bcp_encode_repr_aware\t%s\t%.1f\t%.1f\t%.1f\t%.1f\t-\t%zu\t%s\n", bspec.name.c_str(),
						rr.median_us_per_chunk, rr.p10_us_per_chunk, rr.p90_us_per_chunk, rr.median_ns_per_value,
						repr_bytes, repr_correct ? "PASS" : "FAIL");

			const size_t arena_bytes = EncodeChunkReprAwareArena(f, buf);
			const bool arena_correct = bspec.kind == BcpCellSpec::Kind::Decimal18s6
										   ? true
										   : (hoisted_ref.size() == arena_bytes &&
											  std::equal(hoisted_ref.begin(), hoisted_ref.end(), buf.begin()));
			if (!arena_correct) {
				failures++;
			}
			auto ra = TimeCell([&]() { EncodeChunkReprAwareArena(f, buf); }, 400);
			std::printf("bcp_encode_repr_arena\t%s\t%.1f\t%.1f\t%.1f\t%.1f\t-\t%zu\t%s\n", bspec.name.c_str(),
						ra.median_us_per_chunk, ra.p10_us_per_chunk, ra.p90_us_per_chunk, ra.median_ns_per_value,
						arena_bytes, arena_correct ? "PASS" : "FAIL");

			// D5a columnar scatter: fixed-width families only (integer here).
			if (bspec.kind == BcpCellSpec::Kind::Bigint) {
				const size_t colw_bytes = EncodeChunkColumnarFixed(f, buf);
				const bool colw_correct =
					hoisted_ref.size() == colw_bytes && std::equal(hoisted_ref.begin(), hoisted_ref.end(), buf.begin());
				if (!colw_correct) {
					failures++;
				}
				auto rc = TimeCell([&]() { EncodeChunkColumnarFixed(f, buf); }, 400);
				std::printf("bcp_encode_columnar\t%s\t%.1f\t%.1f\t%.1f\t%.1f\t-\t%zu\t%s\n", bspec.name.c_str(),
							rc.median_us_per_chunk, rc.p10_us_per_chunk, rc.p90_us_per_chunk, rc.median_ns_per_value,
							colw_bytes, colw_correct ? "PASS" : "FAIL");
			}

			// Bulk UTF-8 -> UTF-16 only makes sense for FLAT string columns.
			if (bspec.kind == BcpCellSpec::Kind::Varchar16 && bspec.rep != VecRep::Dict &&
				bspec.rep != VecRep::Constant) {
				const size_t bulk_bytes = EncodeChunkBulkUtf16(f, buf);
				const bool bulk_correct =
					hoisted_ref.size() == bulk_bytes && std::equal(hoisted_ref.begin(), hoisted_ref.end(), buf.begin());
				if (!bulk_correct) {
					failures++;
				}
				auto rbk = TimeCell([&]() { EncodeChunkBulkUtf16(f, buf); }, 400);
				std::printf("bcp_encode_bulk_utf16\t%s\t%.1f\t%.1f\t%.1f\t%.1f\t-\t%zu\t%s\n", bspec.name.c_str(),
							rbk.median_us_per_chunk, rbk.p10_us_per_chunk, rbk.p90_us_per_chunk,
							rbk.median_ns_per_value, bulk_bytes, bulk_correct ? "PASS" : "FAIL");

				for (idx_t block : {64, 256, 512, 1024}) {
					const size_t bb = EncodeChunkBulkUtf16Blocked(f, buf, block);
					const bool bok =
						hoisted_ref.size() == bb && std::equal(hoisted_ref.begin(), hoisted_ref.end(), buf.begin());
					if (!bok) {
						failures++;
					}
					auto rbb = TimeCell([&]() { EncodeChunkBulkUtf16Blocked(f, buf, block); }, 400);
					std::printf("bcp_encode_bulk_blk%llu\t%s\t%.1f\t%.1f\t%.1f\t%.1f\t-\t%zu\t%s\n",
								(unsigned long long)block, bspec.name.c_str(), rbb.median_us_per_chunk,
								rbb.p10_us_per_chunk, rbb.p90_us_per_chunk, rbb.median_ns_per_value, bb,
								bok ? "PASS" : "FAIL");
				}
			}

			size_t hoisted_bytes = EncodeChunkHoisted(f, buf);
			bool hoisted_correct = VerifyHoisted(f, buf);
			if (!hoisted_correct) {
				failures++;
			}

			auto rh = TimeCell([&]() { EncodeChunkHoisted(f, buf); }, 400);

			std::printf("bcp_encode_hoisted\t%s\t%.1f\t%.1f\t%.1f\t%.1f\t-\t%zu\t%s\n", bspec.name.c_str(),
						rh.median_us_per_chunk, rh.p10_us_per_chunk, rh.p90_us_per_chunk, rh.median_ns_per_value,
						hoisted_bytes, hoisted_correct ? "PASS" : "FAIL");
		} catch (std::exception &ex) {
			std::fprintf(stderr, "cell %s threw: %s\n", bspec.name.c_str(), ex.what());
			failures++;
		}
	}

	// Spec 057: the same shapes on a WIDE row, where the per-column pass stops
	// being nearly-sequential. Wire bytes checked against the shipped encoder.
	std::printf("\n[bench_materialize] spec 057 — wide-row encode (BIGINT columns, all valid)\n");
	for (idx_t ncols : {(idx_t)1, (idx_t)4, (idx_t)16, (idx_t)44}) {
		WideFixture w = BuildWideFixture(ncols);
		duckdb::vector<uint8_t> ref, buf;
		duckdb::tds::encoding::BCPRowEncoder::EncodeChunk(ref, *w.chunk, w.cols, nullptr);
		const double values = (double)CHUNK_ROWS * (double)ncols;

		struct Variant {
			const char *name;
			std::function<size_t(duckdb::vector<uint8_t> &)> fn;
		};
		duckdb::vector<Variant> variants;
		variants.push_back({"shipped", [&](duckdb::vector<uint8_t> &b) {
								b.clear();
								duckdb::tds::encoding::BCPRowEncoder::EncodeChunk(b, *w.chunk, w.cols, nullptr);
								return b.size();
							}});
		variants.push_back({"colfull", [&](duckdb::vector<uint8_t> &b) { return WideColFull(w, b); }});
		for (idx_t blk : {(idx_t)32, (idx_t)64, (idx_t)128, (idx_t)256}) {
			char *nm = new char[16];
			std::snprintf(nm, 16, "colblk%llu", (unsigned long long)blk);
			variants.push_back({nm, [&, blk](duckdb::vector<uint8_t> &b) { return WideColBlocked(w, b, blk); }});
		}
		variants.push_back({"rowmajor", [&](duckdb::vector<uint8_t> &b) { return WideRowMajor(w, b); }});
		for (idx_t blk : {(idx_t)32, (idx_t)64, (idx_t)128, (idx_t)256}) {
			char *nm = new char[20];
			std::snprintf(nm, 20, "rowblk%llu", (unsigned long long)blk);
			variants.push_back({nm, [&, blk](duckdb::vector<uint8_t> &b) { return WideRowBlocked(w, b, blk); }});
		}
		variants.push_back(
			{"zerofill64", [&](duckdb::vector<uint8_t> &b) { return WideColBlockedZeroFill(w, b, 64); }});
		variants.push_back({"tmplframe", [&](duckdb::vector<uint8_t> &b) { return WideColTemplate(w, b); }});
		for (idx_t blk : {(idx_t)64, (idx_t)128, (idx_t)256}) {
			char *nm = new char[20];
			std::snprintf(nm, 20, "tmplblk%llu", (unsigned long long)blk);
			variants.push_back(
				{nm, [&, blk](duckdb::vector<uint8_t> &b) { return WideColBlockedTemplate(w, b, blk); }});
		}
		variants.push_back({"typedw", [&](duckdb::vector<uint8_t> &b) { return WideColTypedWidth(w, b); }});
		variants.push_back({"tmpl+typedw", [&](duckdb::vector<uint8_t> &b) { return WideColTemplateTyped(w, b); }});

		for (auto &v : variants) {
			const size_t n = v.fn(buf);
			const bool ok = n == ref.size() && std::equal(ref.begin(), ref.end(), buf.begin());
			if (!ok) {
				failures++;
			}
			auto t = TimeCell([&]() { v.fn(buf); }, 200);
			std::printf("wide_encode_%s\tncols%llu\t%.1f\t%.1f\t%.1f\t%.2f\t%zu\t-\t%s\n", v.name,
						(unsigned long long)ncols, t.median_us_per_chunk, t.p10_us_per_chunk, t.p90_us_per_chunk,
						t.median_us_per_chunk * 1000.0 / values, n, ok ? "PASS" : "FAIL");
		}
	}

	if (failures > 0) {
		std::fprintf(stderr, "\n%d cell(s) FAILED correctness.\n", failures);
		return 1;
	}
	std::printf("\n[bench_materialize] all cells correct. sink=%llu\n", (unsigned long long)g_sink);
	return 0;
}
