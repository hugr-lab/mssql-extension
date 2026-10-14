// test/cpp/test_batch_builder.cpp
// Unit tests for MSSQLBatchBuilder
//
// These tests do NOT require a running SQL Server instance.
// They test the batching logic in isolation.
//
// Tests cover:
// - Row accumulation and batch flushing
// - Byte size limits
// - Row count limits
// - Progress tracking (row offset, batch count)
// - SQL generation
//
// Compile:
//   See Makefile or CMakeLists.txt
//
// Run:
//   ./test_batch_builder

#include <cassert>
#include <cstring>
#include <iostream>

#include "dml/insert/mssql_batch_builder.hpp"
#include "dml/insert/mssql_insert_config.hpp"
#include "dml/insert/mssql_insert_statement.hpp"
#include "dml/insert/mssql_insert_target.hpp"
#include "duckdb/common/allocator.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/data_chunk.hpp"

using namespace duckdb;

//==============================================================================
// Helper macros for assertions with messages
//==============================================================================
#define ASSERT_EQ(actual, expected)                                                          \
	do {                                                                                     \
		if ((actual) != (expected)) {                                                        \
			std::cerr << "ASSERTION FAILED at " << __FILE__ << ":" << __LINE__ << std::endl; \
			std::cerr << "  Expected: " << (expected) << std::endl;                          \
			std::cerr << "  Actual:   " << (actual) << std::endl;                            \
			assert(false);                                                                   \
		}                                                                                    \
	} while (0)

#define ASSERT_TRUE(expr)                                                                    \
	do {                                                                                     \
		if (!(expr)) {                                                                       \
			std::cerr << "ASSERTION FAILED at " << __FILE__ << ":" << __LINE__ << std::endl; \
			std::cerr << "  Expression was false: " << #expr << std::endl;                   \
			assert(false);                                                                   \
		}                                                                                    \
	} while (0)

#define ASSERT_FALSE(expr)                                                                   \
	do {                                                                                     \
		if ((expr)) {                                                                        \
			std::cerr << "ASSERTION FAILED at " << __FILE__ << ":" << __LINE__ << std::endl; \
			std::cerr << "  Expression was true: " << #expr << std::endl;                    \
			assert(false);                                                                   \
		}                                                                                    \
	} while (0)

#define ASSERT_CONTAINS(str, substr)                                                         \
	do {                                                                                     \
		if ((str).find(substr) == std::string::npos) {                                       \
			std::cerr << "ASSERTION FAILED at " << __FILE__ << ":" << __LINE__ << std::endl; \
			std::cerr << "  String does not contain: " << (substr) << std::endl;             \
			std::cerr << "  Actual string: " << (str) << std::endl;                          \
			assert(false);                                                                   \
		}                                                                                    \
	} while (0)

#define ASSERT_GE(actual, expected)                                                          \
	do {                                                                                     \
		if ((actual) < (expected)) {                                                         \
			std::cerr << "ASSERTION FAILED at " << __FILE__ << ":" << __LINE__ << std::endl; \
			std::cerr << "  Expected >= " << (expected) << std::endl;                        \
			std::cerr << "  Actual:     " << (actual) << std::endl;                          \
			assert(false);                                                                   \
		}                                                                                    \
	} while (0)

#define ASSERT_LE(actual, expected)                                                          \
	do {                                                                                     \
		if ((actual) > (expected)) {                                                         \
			std::cerr << "ASSERTION FAILED at " << __FILE__ << ":" << __LINE__ << std::endl; \
			std::cerr << "  Expected <= " << (expected) << std::endl;                        \
			std::cerr << "  Actual:     " << (actual) << std::endl;                          \
			assert(false);                                                                   \
		}                                                                                    \
	} while (0)

//==============================================================================
// Test Helpers - Create target and config
//==============================================================================
MSSQLInsertTarget CreateTestTarget() {
	MSSQLInsertTarget target;
	target.catalog_name = "test_catalog";
	target.schema_name = "dbo";
	target.table_name = "test_table";

	// Add columns
	MSSQLInsertColumn col1;
	col1.name = "id";
	col1.duckdb_type = LogicalType::INTEGER;
	col1.mssql_type = "INT";
	col1.is_identity = false;
	col1.is_nullable = false;
	target.columns.push_back(col1);

	MSSQLInsertColumn col2;
	col2.name = "name";
	col2.duckdb_type = LogicalType::VARCHAR;
	col2.mssql_type = "NVARCHAR(100)";
	col2.is_identity = false;
	col2.is_nullable = true;
	target.columns.push_back(col2);

	// Set insert column indices (non-identity columns)
	target.insert_column_indices = {0, 1};

	return target;
}

MSSQLInsertConfig CreateTestConfig(idx_t batch_size = 10, idx_t max_sql_bytes = 8192) {
	MSSQLInsertConfig config;
	config.batch_size = batch_size;
	config.max_rows_per_statement = batch_size;
	config.max_sql_bytes = max_sql_bytes;
	return config;
}

// unique_ptr, not by value: DataChunk is neither copyable nor movable.
unique_ptr<DataChunk> CreateTestChunk(idx_t row_count) {
	auto chunk_ptr = make_uniq<DataChunk>();
	auto &chunk = *chunk_ptr;
	vector<LogicalType> types = {LogicalType::INTEGER, LogicalType::VARCHAR};
	chunk.Initialize(Allocator::DefaultAllocator(), types);

	auto &id_vec = chunk.data[0];
	auto &name_vec = chunk.data[1];

	for (idx_t i = 0; i < row_count; i++) {
		id_vec.SetValue(i, Value::INTEGER(static_cast<int32_t>(i + 1)));
		name_vec.SetValue(i, Value("name_" + std::to_string(i + 1)));
	}

	chunk.SetChildCardinality(row_count);
	return chunk_ptr;
}

//==============================================================================
// Test: Basic row addition
//==============================================================================
void test_add_row_basic() {
	std::cout << "\n=== Test: AddRow - Basic ===" << std::endl;

	auto target = CreateTestTarget();
	auto config = CreateTestConfig(10);
	MSSQLBatchBuilder builder(target, config, false);

	ASSERT_FALSE(builder.HasPendingRows());
	ASSERT_EQ(builder.GetPendingRowCount(), 0u);
	ASSERT_EQ(builder.GetBatchCount(), 0u);

	auto chunk_ptr = CreateTestChunk(3);
	auto &chunk = *chunk_ptr;

	// Add first row
	ASSERT_TRUE(builder.AddRow(chunk, 0));
	ASSERT_TRUE(builder.HasPendingRows());
	ASSERT_EQ(builder.GetPendingRowCount(), 1u);

	// Add second row
	ASSERT_TRUE(builder.AddRow(chunk, 1));
	ASSERT_EQ(builder.GetPendingRowCount(), 2u);

	// Add third row
	ASSERT_TRUE(builder.AddRow(chunk, 2));
	ASSERT_EQ(builder.GetPendingRowCount(), 3u);

	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Test: Batch flushing
//==============================================================================
void test_flush_batch() {
	std::cout << "\n=== Test: FlushBatch ===" << std::endl;

	auto target = CreateTestTarget();
	auto config = CreateTestConfig(10);
	MSSQLBatchBuilder builder(target, config, false);

	auto chunk_ptr = CreateTestChunk(3);
	auto &chunk = *chunk_ptr;

	// Add 3 rows
	builder.AddRow(chunk, 0);
	builder.AddRow(chunk, 1);
	builder.AddRow(chunk, 2);

	ASSERT_EQ(builder.GetPendingRowCount(), 3u);
	ASSERT_EQ(builder.GetBatchCount(), 0u);

	// Flush
	auto batch = builder.FlushBatch();

	ASSERT_FALSE(builder.HasPendingRows());
	ASSERT_EQ(builder.GetPendingRowCount(), 0u);
	ASSERT_EQ(builder.GetBatchCount(), 1u);

	// Verify batch contents
	ASSERT_EQ(batch.row_count, 3u);
	ASSERT_EQ(batch.row_offset_start, 0u);
	ASSERT_EQ(batch.row_offset_end, 3u);
	ASSERT_EQ(static_cast<int>(batch.state), static_cast<int>(MSSQLInsertBatch::State::READY));

	// Verify SQL contains expected parts
	ASSERT_CONTAINS(batch.sql_statement, "INSERT INTO");
	ASSERT_CONTAINS(batch.sql_statement, "[dbo].[test_table]");
	ASSERT_CONTAINS(batch.sql_statement, "[id]");
	ASSERT_CONTAINS(batch.sql_statement, "[name]");
	ASSERT_CONTAINS(batch.sql_statement, "VALUES");

	std::cout << "  Generated SQL:\n" << batch.sql_statement << std::endl;
	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Test: Row count limit triggers flush
//==============================================================================
void test_row_count_limit() {
	std::cout << "\n=== Test: Row Count Limit ===" << std::endl;

	auto target = CreateTestTarget();
	auto config = CreateTestConfig(3);	// Only 3 rows per batch
	MSSQLBatchBuilder builder(target, config, false);

	auto chunk_ptr = CreateTestChunk(5);
	auto &chunk = *chunk_ptr;

	// Add rows up to limit
	ASSERT_TRUE(builder.AddRow(chunk, 0));
	ASSERT_TRUE(builder.AddRow(chunk, 1));
	ASSERT_TRUE(builder.AddRow(chunk, 2));

	// Next row should fail (batch full)
	ASSERT_FALSE(builder.AddRow(chunk, 3));

	// Flush and add the pending row
	auto batch1 = builder.FlushBatch();
	ASSERT_EQ(batch1.row_count, 3u);

	// Now we can add more rows
	ASSERT_TRUE(builder.AddRow(chunk, 3));
	ASSERT_TRUE(builder.AddRow(chunk, 4));

	auto batch2 = builder.FlushBatch();
	ASSERT_EQ(batch2.row_count, 2u);
	ASSERT_EQ(batch2.row_offset_start, 3u);
	ASSERT_EQ(batch2.row_offset_end, 5u);

	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Test: Progress tracking across batches
//==============================================================================
void test_progress_tracking() {
	std::cout << "\n=== Test: Progress Tracking ===" << std::endl;

	auto target = CreateTestTarget();
	auto config = CreateTestConfig(2);	// 2 rows per batch
	MSSQLBatchBuilder builder(target, config, false);

	auto chunk_ptr = CreateTestChunk(6);
	auto &chunk = *chunk_ptr;

	// Add rows and track offsets
	builder.AddRow(chunk, 0);
	ASSERT_EQ(builder.GetCurrentRowOffset(), 1u);

	builder.AddRow(chunk, 1);
	ASSERT_EQ(builder.GetCurrentRowOffset(), 2u);

	// Flush batch 1
	builder.FlushBatch();
	ASSERT_EQ(builder.GetBatchCount(), 1u);
	ASSERT_EQ(builder.GetCurrentRowOffset(), 2u);  // Offset persists

	// Add more rows
	builder.AddRow(chunk, 2);
	builder.AddRow(chunk, 3);
	ASSERT_EQ(builder.GetCurrentRowOffset(), 4u);

	// Flush batch 2
	builder.FlushBatch();
	ASSERT_EQ(builder.GetBatchCount(), 2u);

	// Add final rows
	builder.AddRow(chunk, 4);
	builder.AddRow(chunk, 5);
	builder.FlushBatch();
	ASSERT_EQ(builder.GetBatchCount(), 3u);
	ASSERT_EQ(builder.GetCurrentRowOffset(), 6u);

	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Test: SQL generation with OUTPUT clause
//==============================================================================
void test_output_clause() {
	std::cout << "\n=== Test: OUTPUT Clause Generation ===" << std::endl;

	auto target = CreateTestTarget();
	target.returning_column_indices = {0, 1};  // Return both columns

	auto config = CreateTestConfig(10);
	MSSQLBatchBuilder builder(target, config, true);  // With OUTPUT

	auto chunk_ptr = CreateTestChunk(2);
	auto &chunk = *chunk_ptr;
	builder.AddRow(chunk, 0);
	builder.AddRow(chunk, 1);

	auto batch = builder.FlushBatch();

	// Verify OUTPUT clause is present
	ASSERT_CONTAINS(batch.sql_statement, "OUTPUT");
	ASSERT_CONTAINS(batch.sql_statement, "INSERTED");

	std::cout << "  Generated SQL:\n" << batch.sql_statement << std::endl;
	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Test: OUTPUT ... INTO a session table (issue #471)
//==============================================================================
void test_output_into_table() {
	std::cout << "\n=== Test: OUTPUT INTO the RETURNING table (#471) ===" << std::endl;

	auto target = CreateTestTarget();
	MSSQLInsertColumn rv;
	rv.name = "rv";
	rv.duckdb_type = LogicalType::BLOB;
	rv.mssql_type = "timestamp";
	rv.is_identity = false;
	rv.is_nullable = false;
	target.columns.push_back(rv);
	MSSQLInsertColumn code;
	code.name = "code";
	code.duckdb_type = LogicalType::VARCHAR;
	code.mssql_type = "char";
	code.max_length = 2100;
	code.is_identity = false;
	code.is_nullable = true;
	target.columns.push_back(code);
	target.returning_column_indices = {0, 1, 2, 3};
	target.output_into_table = true;

	auto config = CreateTestConfig(10);
	MSSQLBatchBuilder builder(target, config, true);
	auto chunk_ptr = CreateTestChunk(2);
	auto &chunk = *chunk_ptr;

	// The bare OUTPUT first, its literals kept for a rebuild.
	builder.AddRow(chunk, 0);
	builder.AddRow(chunk, 1);
	auto first = builder.FlushBatch();
	ASSERT_EQ(first.sql_statement.find("INSERT INTO "), 0u);
	ASSERT_FALSE(first.sql_statement.find("#mssql_returning_") != std::string::npos);
	ASSERT_EQ(first.row_literals.size(), 2u);
	// Rebuilt from them in the INTO form: the same rows.
	const auto rebuilt = MSSQLInsertStatement(target, true, true).Build(first.row_literals);
	ASSERT_CONTAINS(rebuilt, "(1, N'name_1'),\n  (2, N'name_2');");

	// After a 334 the builder builds the INTO form itself, and keeps no literals.
	builder.UseOutputInto();
	ASSERT_TRUE(builder.UsesOutputInto());
	builder.AddRow(chunk, 0);
	builder.AddRow(chunk, 1);
	auto batch = builder.FlushBatch();
	ASSERT_EQ(batch.row_literals.size(), 0u);
	const string &sql = batch.sql_statement;
	std::cout << "  Generated SQL:\n" << sql << std::endl;

	// One request: create, fill, read, drop -- one table name, the batch's own.
	ASSERT_EQ(sql.find("SELECT "), 0u);
	const auto name_at = sql.find(" INTO [#mssql_returning_");
	ASSERT_TRUE(name_at != std::string::npos);
	const auto name = sql.substr(name_at + 6, sql.find(']', name_at) - name_at - 5);
	idx_t uses = 0;
	for (auto at = sql.find(name); at != std::string::npos; at = sql.find(name, at + 1)) {
		uses++;
	}
	ASSERT_EQ(uses, 4u);
	ASSERT_CONTAINS(sql, name + " FROM [dbo].[test_table] AS t WHERE 1 = 0 UNION ALL SELECT ");
	ASSERT_CONTAINS(sql, " INTO " + name + "\nVALUES");
	const string tail = "\nSELECT * FROM " + name + ";\nDROP TABLE " + name + ";";
	ASSERT_EQ(sql.substr(sql.size() - tail.size()), tail);
	// Positional aliases, the same list on both sides.
	ASSERT_CONTAINS(sql, "t.[id] AS [c0]");
	ASSERT_CONTAINS(sql, "INSERTED.[id] AS [c0]");
	// A rowversion goes as binary(8): OUTPUT cannot write into a timestamp
	// column, and SELECT INTO would make one.
	ASSERT_CONTAINS(sql, "CAST(t.[rv] AS binary(8)) AS [c2]");
	ASSERT_CONTAINS(sql, "CAST(INSERTED.[rv] AS binary(8)) AS [c2]");
	// A char(n) as a trimmed NVARCHAR: the read's NCHAR(n) doubles its fixed
	// width, past the 8060-byte row.
	ASSERT_CONTAINS(sql, "RTRIM(CAST(t.[code] AS NVARCHAR(2100))) AS [c3]");
	ASSERT_CONTAINS(sql, "RTRIM(CAST(INSERTED.[code] AS NVARCHAR(2100))) AS [c3]");

	// The next batch gets a table of its own.
	builder.AddRow(chunk, 0);
	auto next = builder.FlushBatch();
	ASSERT_CONTAINS(next.sql_statement, "INTO [#mssql_returning_");
	ASSERT_FALSE(next.sql_statement.find(name) != std::string::npos);

	// Without the capability (Fabric, Synapse): the bare OUTPUT, no literals kept.
	target.output_into_table = false;
	MSSQLBatchBuilder bare(target, config, true);
	bare.AddRow(chunk, 0);
	auto bare_batch = bare.FlushBatch();
	ASSERT_EQ(bare_batch.sql_statement.find("INSERT INTO "), 0u);
	ASSERT_CONTAINS(bare_batch.sql_statement, "OUTPUT INSERTED.");
	ASSERT_EQ(bare_batch.row_literals.size(), 0u);

	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Test: Empty batch handling
//==============================================================================
void test_empty_batch() {
	std::cout << "\n=== Test: Empty Batch Handling ===" << std::endl;

	auto target = CreateTestTarget();
	auto config = CreateTestConfig(10);
	MSSQLBatchBuilder builder(target, config, false);

	ASSERT_FALSE(builder.HasPendingRows());
	ASSERT_EQ(builder.GetPendingRowCount(), 0u);

	// Flushing empty batch should still work
	auto batch = builder.FlushBatch();
	ASSERT_EQ(batch.row_count, 0u);

	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Test: Large batch (many rows)
//==============================================================================
void test_large_batch() {
	std::cout << "\n=== Test: Large Batch ===" << std::endl;

	auto target = CreateTestTarget();
	auto config = CreateTestConfig(100, 1024 * 1024);  // 100 rows, 1MB
	MSSQLBatchBuilder builder(target, config, false);

	auto chunk_ptr = CreateTestChunk(100);
	auto &chunk = *chunk_ptr;

	// Add 100 rows
	for (idx_t i = 0; i < 100; i++) {
		ASSERT_TRUE(builder.AddRow(chunk, i));
	}

	ASSERT_EQ(builder.GetPendingRowCount(), 100u);

	auto batch = builder.FlushBatch();
	ASSERT_EQ(batch.row_count, 100u);
	ASSERT_GE(batch.sql_bytes, 1000u);	// Should be non-trivial size

	std::cout << "  Batch SQL size: " << batch.sql_bytes << " bytes" << std::endl;
	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Main
//==============================================================================
int main() {
	std::cout << "==========================================" << std::endl;
	std::cout << "MSSQLBatchBuilder Unit Tests" << std::endl;
	std::cout << "==========================================" << std::endl;

	try {
		test_add_row_basic();
		test_flush_batch();
		test_row_count_limit();
		test_progress_tracking();
		test_output_clause();
		test_output_into_table();
		test_empty_batch();
		test_large_batch();

		std::cout << "\n==========================================" << std::endl;
		std::cout << "ALL TESTS PASSED!" << std::endl;
		std::cout << "==========================================" << std::endl;
		return 0;

	} catch (const std::exception &e) {
		std::cerr << "\n==========================================" << std::endl;
		std::cerr << "TEST FAILED WITH EXCEPTION: " << e.what() << std::endl;
		std::cerr << "==========================================" << std::endl;
		return 1;
	}
}
