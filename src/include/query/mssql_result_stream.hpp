#pragma once

#include <atomic>
#include <memory>
#include "duckdb.hpp"
#include "duckdb/common/string_util.hpp"
#include "tds/encoding/type_converter.hpp"
#include "tds/tds_connection.hpp"
#include "tds/tds_row_reader.hpp"
#include "tds/tds_token_parser.hpp"

namespace duckdb {

class ClientContext;

//===----------------------------------------------------------------------===//
// MSSQLResultStream - Streaming result iterator that yields DataChunks
//===----------------------------------------------------------------------===//

enum class MSSQLResultStreamState : uint8_t {
	Initializing,  // Waiting for COLMETADATA
	Streaming,	   // Yielding ROW tokens
	Draining,	   // Cancellation in progress
	Complete,	   // Final DONE received
	Error		   // Fatal error occurred
};

class MSSQLResultStream {
public:
	// Create result stream with shared connection
	// context_name is needed for returning connection to pool
	// client_context is needed for transaction-aware connection release
	// query_timeout_seconds: query execution timeout (0 = no timeout, default: 30)
	MSSQLResultStream(std::shared_ptr<tds::TdsConnection> connection, const string &sql, const string &context_name,
					  ClientContext *client_context = nullptr, int query_timeout_seconds = 30);
	~MSSQLResultStream();

	// Non-copyable, non-movable (manages connection)
	MSSQLResultStream(const MSSQLResultStream &) = delete;
	MSSQLResultStream &operator=(const MSSQLResultStream &) = delete;

	// Initialize the stream (send query, wait for COLMETADATA)
	// Returns true if initialization succeeded
	// Throws on connection or protocol error
	bool Initialize();

	// Get column information (valid after Initialize)
	const vector<LogicalType> &GetColumnTypes() const {
		return column_types_;
	}
	const vector<string> &GetColumnNames() const {
		return column_names_;
	}
	idx_t GetColumnCount() const {
		return column_types_.size();
	}

	// Fill a DataChunk with rows (streaming interface)
	// Returns number of rows written (0 when complete)
	// Throws on error
	idx_t FillChunk(DataChunk &chunk);

	// Request cancellation of the query
	void Cancel();

	// Check if query is complete
	bool IsComplete() const {
		return state_ == MSSQLResultStreamState::Complete;
	}

	// Check if cancelled
	bool IsCancelled() const {
		return is_cancelled_.load(std::memory_order_acquire);
	}

	// Get accumulated errors
	const std::vector<tds::TdsError> &GetErrors() const {
		return errors_;
	}

	// Get accumulated info messages
	const std::vector<tds::TdsInfo> &GetInfoMessages() const {
		return info_messages_;
	}

	// Get total rows read
	uint64_t GetRowsRead() const {
		return rows_read_;
	}

	// Get the connection (for pool return)
	std::shared_ptr<tds::TdsConnection> GetConnection() const {
		return connection_;
	}

	// Set the number of columns to fill in output chunks
	// This may be less than GetColumnCount() when DuckDB only projects virtual columns (e.g., COUNT(*))
	// When set to 0, rows are counted but no column data is filled
	void SetColumnsToFill(idx_t count) {
		columns_to_fill_ = count;
	}

	// Set a mapping from SQL result column indices to output chunk column indices
	// This is needed when virtual columns (like rowid) are interspersed with data columns
	// If not set, SQL column i is written to output position i
	void SetOutputColumnMapping(vector<idx_t> mapping) {
		output_column_mapping_ = std::move(mapping);
	}

	// Set target vectors for writing (bypasses chunk.data)
	// Used for composite PK rowid-only case where we write directly to STRUCT children
	void SetTargetVectors(vector<Vector *> targets) {
		target_vectors_ = std::move(targets);
	}

	// Surface warnings to DuckDB context
	void SurfaceWarnings(ClientContext &context);

private:
	// Read more data from connection into parser
	// timeout_ms: socket receive timeout in milliseconds
	bool ReadMoreData(int timeout_ms);

	// Process parsed row into DataChunk
	void ProcessRow(DataChunk &chunk, idx_t row_idx);

	// Handle cancellation draining
	void DrainAfterCancel();

	// Drain remaining TDS tokens after detecting an error (e.g., multiple result sets)
	// Similar to DrainAfterCancel but without sending ATTENTION signal
	void DrainRemainingTokens();

	// Connection (shared with pool)
	std::shared_ptr<tds::TdsConnection> connection_;

	// Context name for pool release
	string context_name_;

	// Client context for transaction-aware connection release
	// May be nullptr for non-transactional use
	ClientContext *client_context_;

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

	// Number of columns to fill in output chunk
	// May be less than column_metadata_.size() when DuckDB projects virtual columns
	// Default: maximum value means use column_metadata_.size()
	idx_t columns_to_fill_ = static_cast<idx_t>(-1);

	// Mapping from SQL result column index to output chunk column index
	// If empty, SQL column i is written to output position i
	// Example: for output [id, rowid, name] with SQL [id, name], mapping is [0, 2]
	vector<idx_t> output_column_mapping_;

	// Target vectors for writing (alternative to chunk.data)
	// Used for composite PK rowid-only case where we write to STRUCT children
	// If non-empty, these vectors are used instead of chunk.data
	vector<Vector *> target_vectors_;

	// Timeouts
	int read_timeout_ms_ = 30000;  // Normal read timeout (30 seconds)
	// Cancel drain: read data as fast as possible, timeout runs in parallel
	// Very short per-read timeout (10ms) to quickly consume buffered data
	// Overall timeout determines when we give up and close connection
	int cancel_timeout_ms_ = 5000;	   // Overall cancel timeout - if no DONE+ATTN in 5s, close connection
	int cancel_read_timeout_ms_ = 10;  // Per-read timeout during cancel (10ms - just poll for data)

	// Last socket error for better error reporting
	string last_socket_error_;

	// Check if last error was a timeout
	bool IsTimeoutError() const {
		return last_socket_error_.find("timeout") != string::npos;
	}

	// Get formatted timeout error message
	string GetTimeoutErrorMessage() const {
		int timeout_seconds = read_timeout_ms_ / 1000;
		if (timeout_seconds <= 0) {
			return "MSSQL query timed out";
		}
		return StringUtil::Format("MSSQL query timed out after %d seconds. "
								  "Use SET mssql_query_timeout to increase the timeout.",
								  timeout_seconds);
	}
};

}  // namespace duckdb
