// test/cpp/test_vector_encodings.cpp
//
// Dictionary and constant vectors through the BCP encoder, asserted directly
// (PR #245 review). The SQL twin — test/sql/copy/vector_encodings_bcp.test —
// covers the end-to-end round trip but depends on the parquet reader and the
// storage layer CHOOSING dictionary encoding for its columns; if a future
// DuckDB stopped, that file would pass while covering nothing. This test
// cannot go vacuous: it constructs the vector classes explicitly.
//
// Why it matters: v0.2.2's row encoder indexed vectors as if flat, and the
// first dictionary vector at scale made EncodeBinaryPLP dereference string
// CONTENT bytes as a string_t pointer — a segfault on any parquet-sourced
// COPY past ~1M rows (report finding F0, bench_results_v023_report.md). The
// spec-057 encoder normalizes through ToUnifiedFormat; this pins that.
//
// The assertion is byte equality: a chunk whose columns are DICTIONARY (or
// CONSTANT) vectors must encode to exactly the bytes of a flat chunk with the
// same logical content, through both EncodeChunk (columnar path) and
// EncodeRow (row/compat path).
//
// Needs libduckdb (Vector/DataChunk), so it runs in `make test-cpp` and the
// build job's unit-test step — not in the lightweight cpp-unit-tests PR job.

#include <cassert>
#include <cstring>
#include <iostream>
#include <string>

#include "copy/target_resolver.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/types/selection_vector.hpp"
#include "duckdb/common/types/vector.hpp"
#include "tds/encoding/bcp_row_encoder.hpp"

using namespace duckdb;
using duckdb::tds::encoding::BCPRowEncoder;

static int g_failures = 0;

#define CHECK(cond, msg)                                                                         \
	do {                                                                                         \
		if (!(cond)) {                                                                           \
			std::cerr << "FAIL at " << __FILE__ << ":" << __LINE__ << " — " << msg << std::endl; \
			g_failures++;                                                                        \
		}                                                                                        \
	} while (0)

static mssql::BCPColumnMetadata MakeCol(const std::string &name, LogicalType type, uint8_t token, uint16_t max_length,
										uint8_t precision = 0, uint8_t scale = 0) {
	mssql::BCPColumnMetadata col;
	col.name = name;
	col.duckdb_type = type;
	col.tds_type_token = token;
	col.max_length = max_length;
	col.precision = precision;
	col.scale = scale;
	col.nullable = true;
	return col;
}

// The shared column set: fixed-width, inline-length nvarchar, and the two PLP
// (MAX) families — nvarchar(max) and varbinary(max), the F0 crash family.
static duckdb::vector<mssql::BCPColumnMetadata> TestColumns() {
	duckdb::vector<mssql::BCPColumnMetadata> cols;
	cols.push_back(MakeCol("c_int", LogicalType::INTEGER, 0x26, 4));
	cols.push_back(MakeCol("c_big", LogicalType::BIGINT, 0x26, 8));
	cols.push_back(MakeCol("c_str", LogicalType::VARCHAR, 0xE7, 80));		  // nvarchar(40), USHORT length
	cols.push_back(MakeCol("c_strmax", LogicalType::VARCHAR, 0xE7, 0xFFFF));  // nvarchar(max), PLP
	cols.push_back(MakeCol("c_bin", LogicalType::BLOB, 0xA5, 0xFFFF));		  // varbinary(max), PLP
	cols.push_back(MakeCol("c_null", LogicalType::BIGINT, 0x26, 8));
	return cols;
}

static duckdb::vector<LogicalType> TestTypes() {
	return {LogicalType::INTEGER, LogicalType::BIGINT, LogicalType::VARCHAR,
			LogicalType::VARCHAR, LogicalType::BLOB,   LogicalType::BIGINT};
}

// 5 distinct logical values per column; index 4 is NULL. Long strings on
// purpose: >12 bytes puts string_t in pointer (non-inlined) form — the exact
// representation the v0.2.2 encoder mis-dereferenced.
static Value BaseValue(idx_t col, idx_t k) {
	if (k == 4) {
		return Value(TestTypes()[col]);
	}
	switch (col) {
	case 0:
		return Value::INTEGER(static_cast<int32_t>(100 + k));
	case 1:
		return Value::BIGINT(static_cast<int64_t>(-5000000000LL + static_cast<int64_t>(k) * 1234567));
	case 2:
		return Value("short_" + std::to_string(k));
	case 3:
		return Value("plp_payload_long_enough_to_not_inline_" + std::to_string(k) + std::string(20, 'x'));
	case 4:
		return Value::BLOB_RAW("binary_blob_payload_number_" + std::to_string(k) + std::string(15, 'b'));
	default:
		return k % 2 ? Value::BIGINT(static_cast<int64_t>(k)) : Value(LogicalType::BIGINT);
	}
}

// The dictionary selection: 12 logical rows over the 5 base slots, with
// repeats and the NULL slot referenced twice.
static const idx_t SEL[] = {0, 3, 1, 4, 3, 2, 0, 4, 1, 3, 2, 0};
static constexpr idx_t ROWS = 12;

static void FillFlat(DataChunk &chunk) {
	chunk.Initialize(Allocator::DefaultAllocator(), TestTypes());
	for (idx_t c = 0; c < TestTypes().size(); c++) {
		for (idx_t r = 0; r < ROWS; r++) {
			chunk.SetValue(c, r, BaseValue(c, SEL[r]));
		}
	}
	chunk.SetCardinality(ROWS);
}

static void FillDictionary(DataChunk &base, DataChunk &dict) {
	// Base holds the 5 distinct values (incl. the NULL slot) per column…
	base.Initialize(Allocator::DefaultAllocator(), TestTypes());
	for (idx_t c = 0; c < TestTypes().size(); c++) {
		for (idx_t k = 0; k < 5; k++) {
			base.SetValue(c, k, BaseValue(c, k));
		}
	}
	base.SetCardinality(5);
	// …and every column of `dict` is a DICTIONARY vector over it.
	SelectionVector sel(ROWS);
	for (idx_t r = 0; r < ROWS; r++) {
		sel.set_index(r, SEL[r]);
	}
	dict.Initialize(Allocator::DefaultAllocator(), TestTypes());
	for (idx_t c = 0; c < TestTypes().size(); c++) {
		dict.data[c].Reference(base.data[c]);
		dict.data[c].Slice(sel, ROWS);
		CHECK(dict.data[c].GetVectorType() == VectorType::DICTIONARY_VECTOR,
			  "column "
				  << "must be a dictionary vector after Slice");
	}
	dict.SetCardinality(ROWS);
}

static void test_dictionary_encodes_like_flat() {
	std::cout << "\n=== dictionary chunk encodes byte-identically to flat ===" << std::endl;
	auto cols = TestColumns();

	DataChunk flat;
	FillFlat(flat);
	DataChunk base, dict;
	FillDictionary(base, dict);

	duckdb::vector<uint8_t> flat_bytes, dict_bytes;
	BCPRowEncoder::EncodeChunk(flat_bytes, flat, cols, nullptr);
	BCPRowEncoder::EncodeChunk(dict_bytes, dict, cols, nullptr);
	CHECK(!flat_bytes.empty(), "flat encode produced no bytes");
	CHECK(flat_bytes == dict_bytes, "EncodeChunk: dictionary bytes differ from flat");

	// The row/compat path too — one row at a time.
	duckdb::vector<uint8_t> flat_rows, dict_rows;
	for (idx_t r = 0; r < ROWS; r++) {
		BCPRowEncoder::EncodeRow(flat_rows, flat, r, cols, nullptr);
		BCPRowEncoder::EncodeRow(dict_rows, dict, r, cols, nullptr);
	}
	CHECK(flat_rows == dict_rows, "EncodeRow: dictionary bytes differ from flat");
	if (g_failures == 0) {
		std::cout << "PASSED (" << flat_bytes.size() << " bytes compared)" << std::endl;
	}
}

// All-fixed-width chunk: this is the shape EncodeChunk routes to the columnar
// scatter kernels (spec 057), whose selection handling is a separate template
// arm (HAS_SEL) from the row loop — so it gets its own pin.
static void test_dictionary_on_scatter_path() {
	std::cout << "\n=== dictionary through the all-fixed-width scatter path ===" << std::endl;
	duckdb::vector<mssql::BCPColumnMetadata> cols;
	cols.push_back(MakeCol("a", LogicalType::INTEGER, 0x26, 4));
	cols.push_back(MakeCol("b", LogicalType::BIGINT, 0x26, 8));
	duckdb::vector<LogicalType> types = {LogicalType::INTEGER, LogicalType::BIGINT};

	DataChunk flat;
	flat.Initialize(Allocator::DefaultAllocator(), types);
	DataChunk base;
	base.Initialize(Allocator::DefaultAllocator(), types);
	for (idx_t k = 0; k < 5; k++) {
		base.SetValue(0, k, k == 4 ? Value(LogicalType::INTEGER) : Value::INTEGER(static_cast<int32_t>(7 * k)));
		base.SetValue(1, k, k == 4 ? Value(LogicalType::BIGINT) : Value::BIGINT(static_cast<int64_t>(1) << (2 * k)));
	}
	base.SetCardinality(5);

	SelectionVector sel(ROWS);
	for (idx_t r = 0; r < ROWS; r++) {
		sel.set_index(r, SEL[r]);
		flat.SetValue(0, r,
					  SEL[r] == 4 ? Value(LogicalType::INTEGER) : Value::INTEGER(static_cast<int32_t>(7 * SEL[r])));
		flat.SetValue(
			1, r, SEL[r] == 4 ? Value(LogicalType::BIGINT) : Value::BIGINT(static_cast<int64_t>(1) << (2 * SEL[r])));
	}
	flat.SetCardinality(ROWS);

	DataChunk dict;
	dict.Initialize(Allocator::DefaultAllocator(), types);
	for (idx_t c = 0; c < 2; c++) {
		dict.data[c].Reference(base.data[c]);
		dict.data[c].Slice(sel, ROWS);
	}
	dict.SetCardinality(ROWS);

	duckdb::vector<uint8_t> flat_bytes, dict_bytes;
	BCPRowEncoder::EncodeChunk(flat_bytes, flat, cols, nullptr);
	BCPRowEncoder::EncodeChunk(dict_bytes, dict, cols, nullptr);
	CHECK(!flat_bytes.empty(), "flat encode produced no bytes");
	CHECK(flat_bytes == dict_bytes, "scatter path: dictionary bytes differ from flat");
	if (g_failures == 0) {
		std::cout << "PASSED (" << flat_bytes.size() << " bytes compared)" << std::endl;
	}
}

static void test_constant_encodes_like_flat() {
	std::cout << "\n=== constant chunk encodes byte-identically to flat ===" << std::endl;
	auto cols = TestColumns();
	auto types = TestTypes();

	// Constant per column: value columns hold slot 1, c_null holds constant NULL.
	DataChunk cchunk;
	cchunk.Initialize(Allocator::DefaultAllocator(), types);
	for (idx_t c = 0; c < types.size(); c++) {
		Value v = (c == 5) ? Value(LogicalType::BIGINT) : BaseValue(c, 1);
		cchunk.data[c].Reference(v, duckdb::count_t(ROWS));
		CHECK(cchunk.data[c].GetVectorType() == VectorType::CONSTANT_VECTOR, "column must be a constant vector");
	}
	cchunk.SetCardinality(ROWS);

	DataChunk flat;
	flat.Initialize(Allocator::DefaultAllocator(), types);
	for (idx_t c = 0; c < types.size(); c++) {
		Value v = (c == 5) ? Value(LogicalType::BIGINT) : BaseValue(c, 1);
		for (idx_t r = 0; r < ROWS; r++) {
			flat.SetValue(c, r, v);
		}
	}
	flat.SetCardinality(ROWS);

	duckdb::vector<uint8_t> flat_bytes, const_bytes;
	BCPRowEncoder::EncodeChunk(flat_bytes, flat, cols, nullptr);
	BCPRowEncoder::EncodeChunk(const_bytes, cchunk, cols, nullptr);
	CHECK(!flat_bytes.empty(), "flat encode produced no bytes");
	CHECK(flat_bytes == const_bytes, "EncodeChunk: constant bytes differ from flat");
	if (g_failures == 0) {
		std::cout << "PASSED (" << flat_bytes.size() << " bytes compared)" << std::endl;
	}
}

int main() {
	std::cout << "BCP encoder vector-class tests (dictionary / constant vs flat)" << std::endl;
	test_dictionary_encodes_like_flat();
	test_dictionary_on_scatter_path();
	test_constant_encodes_like_flat();
	if (g_failures) {
		std::cerr << "\n" << g_failures << " FAILURE(S)" << std::endl;
		return 1;
	}
	std::cout << "\nAll vector-encoding tests passed." << std::endl;
	return 0;
}
