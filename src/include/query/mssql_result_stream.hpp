#pragma once

#include <atomic>
#include <memory>
#include "codec/staging/row_stager.hpp"
#include "codec/type_family.hpp"
#include "duckdb.hpp"
#include "duckdb/common/string_util.hpp"
#include "tds/encoding/type_converter.hpp"
#include "tds/tds_connection.hpp"
#include "tds/tds_row_reader.hpp"
#include "tds/tds_token_parser.hpp"

namespace duckdb {

class ClientContext;
class MSSQLCatalog;

namespace tds {
class ConnectionPool;
}  // namespace tds

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
	// Create result stream with shared connection.
	//
	// Issue #178 review: the destructor must NOT touch any ClientContext — it can
	// run on a worker thread while the client thread commits the query's
	// transaction (TSan: MetaTransaction::Get vs TransactionContext::Commit on
	// the same context). Instead the release targets are captured HERE, on the
	// client thread:
	//   pool_handle        — weak_ptr to the catalog's pool. lock() failure
	//                        (catalog torn down) degrades to dropping the
	//                        connection — a safe failure mode, never a dangling
	//                        dereference (PR #179 review).
	//   transaction_pinned — true when the connection is pinned to an explicit
	//                        DuckDB transaction; the destructor then just drops
	//                        its reference (the MSSQLTransaction owns the pin).
	// query_timeout_seconds: query execution timeout (0 = no timeout, default: 30)
	MSSQLResultStream(std::shared_ptr<tds::TdsConnection> connection, const string &sql, const string &context_name,
					  weak_ptr<tds::ConnectionPool> pool_handle = weak_ptr<tds::ConnectionPool>(),
					  bool transaction_pinned = false, int query_timeout_seconds = 30);
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

	// Staged read path (spec 055 T5). Resolve the output vector for each SQL
	// column exactly once per chunk, applying the same target_vectors_ /
	// output_column_mapping_ / columns_to_fill_ rules ProcessRow applies per row.
	// A column the query does not fill gets a null entry and is walked for its
	// length only.
	void ResolveStagedTargets(DataChunk &chunk);

	// Handle cancellation draining
	void DrainAfterCancel();

	// Drain remaining TDS tokens after detecting an error (e.g., multiple result sets)
	// Similar to DrainAfterCancel but without sending ATTENTION signal
	void DrainRemainingTokens();

	// Connection (shared with pool)
	std::shared_ptr<tds::TdsConnection> connection_;

	// Context name for pool release
	string context_name_;

	// Release targets captured at construction on the client thread so the
	// destructor never dereferences a ClientContext (issue #178 review: TSan
	// race vs TransactionContext::Commit) nor a raw catalog pointer.
	weak_ptr<tds::ConnectionPool> pool_handle_;
	bool transaction_pinned_;

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

	// Staged read path (spec 055 T5): one walk of each row, column-major where
	// the wire form is already DuckDB's. Not used when target_vectors_ is set —
	// those vectors are STRUCT children the caller owns and may have written
	// validity into already, and the staged path publishes a whole column's
	// validity mask at once.
	mssql::codec::staging::RowStager stager_;
	std::vector<Vector *> staged_targets_;

	// D4 (spec 054): per-stream debug counters, active only at MSSQL_DEBUG>=2
	// (counters_enabled_ latched at construction). Plain integers — a stream
	// is filled from one thread at a time. Printed once on destruction.
	struct StreamDebugCounters {
		uint64_t chunks = 0;
		uint64_t nulls = 0;
		uint64_t values_per_family[mssql::codec::TYPE_FAMILY_COUNT] = {};  // indexed by codec::TypeFamily
		uint64_t unknown_family_values = 0;
		uint64_t wire_bytes_in = 0;		// TDS value bytes across all columns
		uint64_t string_bytes_out = 0;	// UTF-8 bytes written to string vectors
		uint64_t plp_values = 0;		// non-NULL values in PLP (MAX-typed) columns
		uint64_t utf16_fallbacks = 0;	// legacy-converter dispatches (invalid UTF-16)
		// Phase timing, accumulated in NANOSECONDS. Microseconds were wrong here:
		// these intervals are per row, a row is processed in ~100 ns, and
		// duration_cast<microseconds> truncated every one of them to zero — so
		// fill_process reported ~0 no matter how much work it did. Only the rare,
		// long socket waits ever crossed a microsecond boundary.
		uint64_t fill_total_ns = 0;	   // wall time inside FillChunk, accumulated
		uint64_t fill_parse_ns = 0;	   // TDS token/row framing
		uint64_t fill_read_ns = 0;	   // socket wait (server + network term)
		uint64_t fill_process_ns = 0;  // per-value decode + chunk fill
	};

	// Count one processed row into counters_ (called from ProcessRow when
	// counters_enabled_; keeps the conversion hot loop untouched).
	void CountRowForDebug(DataChunk &chunk, idx_t row_idx, idx_t cols_to_fill);

	// Staged-path equivalent, once per chunk instead of once per row. Everything
	// it needs is derivable from the finished chunk plus the stager's per-column
	// NULL counts, so the staged walk itself carries no counter code at all.
	void CountChunkForDebug(DataChunk &chunk, idx_t row_count);

	// Print the close summary to stderr (destructor, counters_enabled_ only).
	void PrintDebugCounters();
	// The staged path's own block (spec 055 D10): per-kernel time and staged
	// bytes, the direct-write bypass, string boundary strategies, and the
	// arena's sizing decisions. Silent for a stream that never staged.
	void PrintStagingCounters();

	bool counters_enabled_ = false;
	// Phase timing is opt-in (MSSQL_DEBUG>=1), separately from the counters
	// (>=2), because the FillChunk summary log lives at level 1. It has to be
	// gated at all because steady_clock::now() costs ~15 ns per call on ARM64
	// and the fill loop took FOUR of them per row — ~59 ns/row measured, on a
	// path whose entire per-row budget is of that order. It was unconditional
	// in release builds, so every user paid for instrumentation nobody read.
	bool timing_enabled_ = false;
	StreamDebugCounters counters_;
	// Per-column decode family (value = codec::TypeFamily, 0xFF = unknown)
	// and PLP-ness, precomputed once after COLMETADATA.
	std::vector<uint8_t> counter_col_family_;
	std::vector<bool> counter_col_plp_;

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
		return StringUtil::Format(
			"MSSQL query timed out after %d seconds. "
			"Use SET mssql_query_timeout to increase the timeout.",
			timeout_seconds);
	}
};

}  // namespace duckdb
