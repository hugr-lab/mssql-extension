#pragma once

#include "tds/tds_connection.hpp"
#include "tds/tds_token_parser.hpp"
#include "tds/tds_row_reader.hpp"
#include "tds/encoding/type_converter.hpp"
#include "duckdb.hpp"
#include <atomic>
#include <memory>

namespace duckdb {

class ClientContext;

//===----------------------------------------------------------------------===//
// MSSQLResultStream - Streaming result iterator that yields DataChunks
//===----------------------------------------------------------------------===//

enum class MSSQLResultStreamState : uint8_t {
	Initializing,   // Waiting for COLMETADATA
	Streaming,      // Yielding ROW tokens
	Draining,       // Cancellation in progress
	Complete,       // Final DONE received
	Error           // Fatal error occurred
};

class MSSQLResultStream {
public:
	// Create result stream with shared connection
	// context_name is needed for returning connection to pool
	MSSQLResultStream(std::shared_ptr<tds::TdsConnection> connection, const string& sql,
	                  const string& context_name);
	~MSSQLResultStream();

	// Non-copyable, non-movable (manages connection)
	MSSQLResultStream(const MSSQLResultStream&) = delete;
	MSSQLResultStream& operator=(const MSSQLResultStream&) = delete;

	// Initialize the stream (send query, wait for COLMETADATA)
	// Returns true if initialization succeeded
	// Throws on connection or protocol error
	bool Initialize();

	// Get column information (valid after Initialize)
	const vector<LogicalType>& GetColumnTypes() const { return column_types_; }
	const vector<string>& GetColumnNames() const { return column_names_; }
	idx_t GetColumnCount() const { return column_types_.size(); }

	// Fill a DataChunk with rows (streaming interface)
	// Returns number of rows written (0 when complete)
	// Throws on error
	idx_t FillChunk(DataChunk& chunk);

	// Request cancellation of the query
	void Cancel();

	// Check if query is complete
	bool IsComplete() const { return state_ == MSSQLResultStreamState::Complete; }

	// Check if cancelled
	bool IsCancelled() const { return is_cancelled_.load(std::memory_order_acquire); }

	// Get accumulated errors
	const std::vector<tds::TdsError>& GetErrors() const { return errors_; }

	// Get accumulated info messages
	const std::vector<tds::TdsInfo>& GetInfoMessages() const { return info_messages_; }

	// Get total rows read
	uint64_t GetRowsRead() const { return rows_read_; }

	// Get the connection (for pool return)
	std::shared_ptr<tds::TdsConnection> GetConnection() const { return connection_; }

	// Surface warnings to DuckDB context
	void SurfaceWarnings(ClientContext& context);

private:
	// Read more data from connection into parser
	// timeout_ms: socket receive timeout in milliseconds
	bool ReadMoreData(int timeout_ms);

	// Process parsed row into DataChunk
	void ProcessRow(DataChunk& chunk, idx_t row_idx);

	// Handle cancellation draining
	void DrainAfterCancel();

	// Connection (shared with pool)
	std::shared_ptr<tds::TdsConnection> connection_;

	// Context name for pool release
	string context_name_;

	// Query
	string sql_;

	// State
	MSSQLResultStreamState state_;
	std::atomic<bool> is_cancelled_;

	// Parser and reader
	tds::TokenParser parser_;
	unique_ptr<tds::RowReader> row_reader_;

	// Column info (set after COLMETADATA)
	vector<LogicalType> column_types_;
	vector<string> column_names_;
	std::vector<tds::ColumnMetadata> column_metadata_;

	// Accumulated messages
	std::vector<tds::TdsError> errors_;
	std::vector<tds::TdsInfo> info_messages_;

	// Statistics
	uint64_t rows_read_;

	// Timeouts
	int read_timeout_ms_ = 30000;         // Normal read timeout (30 seconds)
	// Cancel drain: read data as fast as possible, timeout runs in parallel
	// Very short per-read timeout (10ms) to quickly consume buffered data
	// Overall timeout determines when we give up and close connection
	int cancel_timeout_ms_ = 5000;        // Overall cancel timeout - if no DONE+ATTN in 5s, close connection
	int cancel_read_timeout_ms_ = 10;     // Per-read timeout during cancel (10ms - just poll for data)
};

}  // namespace duckdb
