// Spec 093 T3: the bulk wire of a vector(n) column on the columnar scatter (BCPRowEncoder::EncodeChunk).
// The oracle is the row path (EncodeRow, one value at a time through sql_vector::EncodeToBcp): for every
// chunk shape the columnar bytes must equal the 0xD1 token + EncodeRow of each row. And the chunk must
// stay columnar -- MSSQL_COUNTERS' chunks_row_major unchanged -- with an int and an nvarchar beside the
// vectors: the strided path when nothing is NULL, the cursor when a row is; a dictionary and a constant
// parent vector; float16 narrowed; a NULL element and a float16 overflow refused naming the row.

#include <cmath>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <string>

#include "copy/target_resolver.hpp"
#include "duckdb.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "mssql_counters.hpp"
#include "tds/encoding/bcp_row_encoder.hpp"

using namespace duckdb;
using duckdb::tds::encoding::BCPRowEncoder;

namespace {

int failures = 0;

void Check(bool ok, const std::string &what) {
	if (!ok) {
		std::cout << "  FAIL " << what << std::endl;
		failures++;
	}
}

LogicalType Vec(idx_t n) {
	return LogicalType::ARRAY(LogicalType::FLOAT, n);
}

Value VecValue(std::vector<float> elements) {
	vector<Value> children;
	for (float e : elements) {
		children.push_back(Value::FLOAT(e));
	}
	return Value::ARRAY(LogicalType::FLOAT, std::move(children));
}

vector<mssql::BCPColumnMetadata> Columns() {
	return {mssql::BCPColumnMetadata::FromServerColumn("id", "int", 4, 10, 0, true, ""),
			mssql::BCPColumnMetadata::FromServerColumn("s", "nvarchar", 100, 0, 0, true, ""),
			mssql::BCPColumnMetadata::FromServerColumn("v", "vector", 20, 0, 0, true, ""),
			mssql::BCPColumnMetadata::FromServerColumn("h", "vector", 14, 0, 1, true, "")};
}

//! A chunk of `rows` rows: id, s, v FLOAT[3], h FLOAT[3]; `null_v` makes v NULL on every third row.
void Fill(DataChunk &chunk, idx_t rows, bool null_v) {
	chunk.Initialize(Allocator::DefaultAllocator(), {LogicalType::INTEGER, LogicalType::VARCHAR, Vec(3), Vec(3)});
	for (idx_t r = 0; r < rows; r++) {
		chunk.SetValue(0, r, Value::INTEGER(static_cast<int32_t>(r)));
		chunk.SetValue(1, r, Value("row " + std::to_string(r)));
		const float f = static_cast<float>(r);
		chunk.SetValue(2, r, null_v && r % 3 == 1 ? Value(Vec(3)) : VecValue({f, f + 0.1f, -f * 3.5f}));
		chunk.SetValue(3, r, VecValue({f * 0.25f, -2.0f, 0.1f}));
	}
	chunk.SetChildCardinality(rows);
}

struct Counts {
	uint64_t strided, cursor, row_major;
};

Counts Now() {
	auto &pc = mssql::GetEncodePathCounters();
	return {pc.chunks_strided.load(), pc.chunks_cursor.load(), pc.chunks_row_major.load()};
}

//! The columnar bytes equal the row path's; the chunk was not row-major; it took `expect_strided`'s path.
void Compare(DataChunk &chunk, const std::string &label, bool expect_strided) {
	const auto columns = Columns();
	const auto before = Now();
	duckdb::vector<uint8_t> columnar;
	BCPRowEncoder::EncodeChunk(columnar, chunk, columns);
	const auto after = Now();
	duckdb::vector<uint8_t> rows;
	for (idx_t r = 0; r < chunk.size(); r++) {
		rows.push_back(0xD1);
		BCPRowEncoder::EncodeRow(rows, chunk, r, columns);
	}
	Check(columnar == rows, label + ": the columnar bytes are the row path's (" + std::to_string(columnar.size()) +
								" vs " + std::to_string(rows.size()) + " bytes)");
	Check(after.row_major == before.row_major, label + ": not row-major");
	if (expect_strided) {
		Check(after.strided == before.strided + 1, label + ": the strided path");
	} else {
		Check(after.cursor == before.cursor + 1, label + ": the cursor path");
	}
}

std::string Throws(const std::function<void()> &fn) {
	try {
		fn();
	} catch (std::exception &e) {
		return ErrorData(e).RawMessage();
	}
	return std::string();
}

void TestShapes() {
	{
		DataChunk chunk;
		Fill(chunk, 2048, false);
		Compare(chunk, "2048 rows, no NULL", false);  // the nvarchar column makes the rows vary: cursor
	}
	{
		DataChunk chunk;
		Fill(chunk, 1000, true);
		Compare(chunk, "a NULL vector every third row", false);
	}
	{
		// A dictionary parent: the chunk sliced by a selection that repeats and reverses rows.
		DataChunk chunk;
		Fill(chunk, 300, true);
		SelectionVector sel(200);
		for (idx_t i = 0; i < 200; i++) {
			sel.set_index(i, (299 - i * 3) % 300);
		}
		chunk.Slice(sel, 200);
		Compare(chunk, "a dictionary parent", false);
	}
	{
		// A constant parent (and a constant NULL one).
		DataChunk chunk;
		Fill(chunk, 100, false);
		Vector constant(VecValue({7.0f, 8.0f, 9.0f}), count_t(100));
		chunk.data[2].Reference(constant);
		Vector null_constant(Value(Vec(3)), count_t(100));
		chunk.data[3].Reference(null_constant);
		chunk.SetChildCardinality(100);
		Compare(chunk, "a constant parent and a constant NULL one", false);
	}
	{
		// Only fixed-width columns and vectors, nothing NULL: the strided path.
		DataChunk chunk;
		chunk.Initialize(Allocator::DefaultAllocator(), {LogicalType::INTEGER, Vec(3), Vec(3)});
		for (idx_t r = 0; r < 500; r++) {
			chunk.SetValue(0, r, Value::INTEGER(static_cast<int32_t>(r)));
			chunk.SetValue(1, r, VecValue({1.5f, static_cast<float>(r), 3.4028235e38f}));
			chunk.SetValue(2, r, VecValue({65504.0f, -0.5f, 65510.0f}));
		}
		chunk.SetChildCardinality(500);
		const vector<mssql::BCPColumnMetadata> columns = {
			mssql::BCPColumnMetadata::FromServerColumn("id", "int", 4, 10, 0, true, ""),
			mssql::BCPColumnMetadata::FromServerColumn("v", "vector", 20, 0, 0, true, ""),
			mssql::BCPColumnMetadata::FromServerColumn("h", "vector", 14, 0, 1, true, "")};
		const auto before = Now();
		duckdb::vector<uint8_t> columnar;
		BCPRowEncoder::EncodeChunk(columnar, chunk, columns);
		const auto after = Now();
		duckdb::vector<uint8_t> rows;
		for (idx_t r = 0; r < 500; r++) {
			rows.push_back(0xD1);
			BCPRowEncoder::EncodeRow(rows, chunk, r, columns);
		}
		Check(columnar == rows, "int + float32 + float16 vectors: the row path's bytes");
		Check(after.strided == before.strided + 1 && after.row_major == before.row_major,
			  "int + vectors, no NULL: the strided path, not row-major");
		// Row 0's float16 bytes: 65504 -> 7BFF, -0.5 -> B800, 65510 -> 7BFF.
		const size_t h_at = 1 + (1 + 4) + (2 + 20) + 2 + 8;
		Check(columnar.size() > h_at + 6 && columnar[h_at] == 0xFF && columnar[h_at + 1] == 0x7B &&
				  columnar[h_at + 2] == 0x00 && columnar[h_at + 3] == 0xB8 && columnar[h_at + 4] == 0xFF &&
				  columnar[h_at + 5] == 0x7B,
			  "float16 narrowed on the columnar path: 65504, -0.5, 65510 -> 65504");
	}
}

void TestRefusals() {
	const auto columns = Columns();
	{
		// A NULL element in row 3.
		DataChunk chunk;
		Fill(chunk, 10, false);
		chunk.SetValue(2, 2,
					   Value::ARRAY(LogicalType::FLOAT, {Value::FLOAT(1), Value(LogicalType::FLOAT), Value::FLOAT(3)}));
		const auto e = Throws([&] {
			duckdb::vector<uint8_t> out;
			BCPRowEncoder::EncodeChunk(out, chunk, columns);
		});
		Check(e.find("column \"v\"") != std::string::npos && e.find("row 3 of the chunk") != std::string::npos &&
				  e.find("element 2 is NULL") != std::string::npos,
			  "a NULL element refused naming the row, got '" + e + "'");
	}
	{
		// A float16 overflow in row 5.
		DataChunk chunk;
		Fill(chunk, 10, false);
		chunk.SetValue(3, 4, VecValue({1, 70000, 1}));
		const auto e = Throws([&] {
			duckdb::vector<uint8_t> out;
			BCPRowEncoder::EncodeChunk(out, chunk, columns);
		});
		Check(e.find("column \"h\"") != std::string::npos && e.find("row 5 of the chunk") != std::string::npos &&
				  e.find("65504") != std::string::npos,
			  "a float16 overflow refused naming the row, got '" + e + "'");
	}
	{
		// A NaN in a float32 vector, row 1.
		DataChunk chunk;
		Fill(chunk, 4, false);
		chunk.SetValue(2, 0, VecValue({1, NAN, 3}));
		const auto e = Throws([&] {
			duckdb::vector<uint8_t> out;
			BCPRowEncoder::EncodeChunk(out, chunk, columns);
		});
		Check(e.find("row 1 of the chunk, element 2") != std::string::npos && e.find("NaN") != std::string::npos,
			  "a NaN refused naming the row, got '" + e + "'");
	}
}

}  // namespace

int main() {
	// The counters are what says a chunk stayed columnar; they read the environment once.
	setenv("MSSQL_COUNTERS", "1", 1);
	TestShapes();
	TestRefusals();
	if (failures) {
		std::cerr << failures << " check(s) failed\n";
		return 1;
	}
	std::cout << "test_vector_scatter: all checks passed\n";
	return 0;
}
