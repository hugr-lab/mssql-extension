#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>
#include "copy/target_resolver.hpp"
#include "duckdb/common/types/data_chunk.hpp"

namespace duckdb {

namespace tds {
class TdsConnection;
}  // namespace tds

namespace mssql {

struct BCPCopyTarget;
struct BCPColumnMetadata;

//===----------------------------------------------------------------------===//
// BCPWriter - Constructs and sends TDS BulkLoadBCP packets
//
// Handles the wire format encoding for:
// - COLMETADATA token (0x81): Column definitions
// - ROW token (0xD1): Row data
// - DONE token (0xFD): Batch completion
//
// Thread-safe for concurrent Sink operations via write_mutex.
//===----------------------------------------------------------------------===//

class BCPWriter {
public:
	//===----------------------------------------------------------------------===//
	// Construction
	//===----------------------------------------------------------------------===//

	// Create a BCPWriter for the given connection and target
	// @param conn TDS connection (must be in BulkLoad mode after INSERT BULK)
	// @param target Resolved copy target
	// @param columns Column metadata for encoding
	// @param column_mapping Optional column mapping for name-based source-to-target mapping
	BCPWriter(tds::TdsConnection &conn, const BCPCopyTarget &target, vector<BCPColumnMetadata> columns,
			  vector<int32_t> column_mapping = {});

	// Non-copyable
	BCPWriter(const BCPWriter &) = delete;
	BCPWriter &operator=(const BCPWriter &) = delete;

	//===----------------------------------------------------------------------===//
	// BCP Protocol Operations
	//===----------------------------------------------------------------------===//

	// Write COLMETADATA token to start the bulk load
	// Must be called once before any WriteRows calls
	// @throws IOException on network error
	void WriteColmetadata();

	// Write ROW tokens for a batch of data
	// Thread-safe: multiple threads can call this concurrently
	// @param chunk DataChunk containing rows to write
	// @return Number of rows written
	// @throws IOException on network error
	idx_t WriteRows(DataChunk &chunk);

	// Write DONE token to complete the bulk load
	// @param row_count Total rows sent (for verification)
	// @throws IOException on network error
	void WriteDone(idx_t row_count);

	// Finalize the bulk load and read server response
	// @return Actual row count from server
	// @throws IOException on network error
	// @throws InvalidInputException on server error
	idx_t Finalize();

	// Flush current batch and reset for next batch
	// This sends DONE token, reads server response, and resets internal state.
	// After calling this, the connection returns to Idle state.
	// Caller must re-execute INSERT BULK and call WriteColmetadata() before
	// continuing with more rows.
	// @param row_count Rows in this batch (for DONE token)
	// @return Actual row count confirmed by server
	// @throws IOException on network error
	// @throws InvalidInputException on server error
	idx_t FlushBatch(idx_t row_count);

	// Reset writer state for a new batch (after FlushBatch)
	// Call this after re-executing INSERT BULK
	void ResetForNextBatch();

	//===----------------------------------------------------------------------===//
	// State Accessors
	//===----------------------------------------------------------------------===//

	// Check if COLMETADATA has been sent
	bool IsColmetadataSent() const {
		return colmetadata_sent_;
	}

	// Get total rows sent so far
	idx_t GetRowsSent() const {
		return rows_sent_.load();
	}

	// Get total bytes sent so far
	idx_t GetBytesSent() const {
		return bytes_sent_.load();
	}

	// Get rows accumulated in current batch (not yet sent to server)
	idx_t GetRowsInCurrentBatch() const {
		return rows_in_batch_.load();
	}

	// Get current accumulator buffer size in bytes
	size_t GetAccumulatorSize() const {
		return accumulator_buffer_.size();
	}

private:
	//===----------------------------------------------------------------------===//
	// Token Builders
	//===----------------------------------------------------------------------===//

	// Build COLMETADATA token into buffer
	void BuildColmetadataToken(vector<uint8_t> &buffer);

	// Build ROW token for a single row into buffer
	void BuildRowToken(vector<uint8_t> &buffer, DataChunk &chunk, idx_t row_idx);

	// Build DONE token into buffer
	void BuildDoneToken(vector<uint8_t> &buffer, idx_t row_count);

	//===----------------------------------------------------------------------===//
	// Type-specific Encoding
	//===----------------------------------------------------------------------===//

	// Encode a column value into the buffer
	void EncodeColumnValue(vector<uint8_t> &buffer, DataChunk &chunk, idx_t col_idx, idx_t row_idx,
						   const BCPColumnMetadata &col);

	//===----------------------------------------------------------------------===//
	// Wire Helpers
	//===----------------------------------------------------------------------===//

	// Send buffer as BULK_LOAD packet
	void SendBulkLoadPacket(const vector<uint8_t> &buffer, bool is_last = false);

	// Write little-endian integers
	static void WriteUInt8(vector<uint8_t> &buffer, uint8_t value);
	static void WriteUInt16LE(vector<uint8_t> &buffer, uint16_t value);
	static void WriteUInt32LE(vector<uint8_t> &buffer, uint32_t value);
	static void WriteUInt64LE(vector<uint8_t> &buffer, uint64_t value);
	static void WriteInt16LE(vector<uint8_t> &buffer, int16_t value);

	// Write string as UTF-16LE with B_VARCHAR format (length prefix + data)
	static void WriteUTF16LEString(vector<uint8_t> &buffer, const string &str);

	//===----------------------------------------------------------------------===//
	// Member Variables
	//===----------------------------------------------------------------------===//

	// TDS connection reference
	tds::TdsConnection &conn_;

	// Target table information
	BCPCopyTarget target_;

	// Column metadata
	vector<BCPColumnMetadata> columns_;

	// Column mapping: mapping[target_idx] = source_idx, or -1 if source doesn't have this column
	// Empty if no mapping needed (1:1 positional match)
	vector<int32_t> column_mapping_;

	// State tracking
	bool colmetadata_sent_ = false;
	std::atomic<idx_t> rows_sent_{0};	   // Total rows sent across all batches
	std::atomic<idx_t> bytes_sent_{0};	   // Total bytes sent across all batches
	std::atomic<idx_t> rows_in_batch_{0};  // Rows in current batch (resets on flush)

	// Packet ID counter for TDS protocol
	uint8_t packet_id_ = 1;

	// Thread safety
	std::mutex write_mutex_;

	// Accumulator buffer for all BCP data (COLMETADATA + ROWs + DONE)
	// Used to send all data in a single message
	// Note: Memory is released via swap trick in ResetForNextBatch()
	vector<uint8_t> accumulator_buffer_;
};

}  // namespace mssql
}  // namespace duckdb
