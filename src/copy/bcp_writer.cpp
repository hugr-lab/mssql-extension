#include "copy/bcp_writer.hpp"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "codec/type_family.hpp"
#include "duckdb/common/exception.hpp"
#include "tds/encoding/bcp_row_encoder.hpp"
#include "tds/encoding/utf16.hpp"
#include "tds/tds_connection.hpp"
#include "tds/tds_packet.hpp"
#include "tds/tds_protocol.hpp"
#include "tds/tds_types.hpp"

namespace duckdb {
namespace mssql {

// TDS Token types for BCP
constexpr uint8_t TOKEN_COLMETADATA = 0x81;
constexpr uint8_t TOKEN_ROW = 0xD1;
constexpr uint8_t TOKEN_DONE = 0xFD;

// DONE status flags
constexpr uint16_t DONE_FINAL = 0x0000;
constexpr uint16_t DONE_COUNT = 0x0010;

// DONE command for INSERT
constexpr uint16_t CURCMD_INSERT = 0x00C3;

//===----------------------------------------------------------------------===//
// Debug Logging
//===----------------------------------------------------------------------===//

static int GetBCPDebugLevel() {
	static const int level = []() {
		const char *env = std::getenv("MSSQL_DEBUG");
		return env ? std::atoi(env) : 0;
	}();
	return level;
}

static void BCPDebugLog(int level, const char *format, ...) {
	if (GetBCPDebugLevel() < level) {
		return;
	}
	va_list args;
	va_start(args, format);
	fprintf(stderr, "[MSSQL BCP] ");
	vfprintf(stderr, format, args);
	fprintf(stderr, "\n");
	fflush(stderr);
	va_end(args);
}

// High-resolution timer for performance analysis
using Clock = std::chrono::high_resolution_clock;
using TimePoint = std::chrono::time_point<Clock>;

// Accumulate a phase into an atomic on scope exit, so an early return or a throw
// cannot leave the counter short. Nanoseconds: spec 055 D0 caught microsecond
// accumulation truncating short intervals to zero. One instance per batch, not
// per value, so it is not on any hot path.
namespace {
struct ScopedNs {
	std::atomic<uint64_t> &sink;
	TimePoint start;
	explicit ScopedNs(std::atomic<uint64_t> &sink_p) : sink(sink_p), start(Clock::now()) {}
	~ScopedNs() {
		sink.fetch_add(
			static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count()),
			std::memory_order_relaxed);
	}
};
}  // namespace

static double ElapsedMs(TimePoint start) {
	auto end = Clock::now();
	return std::chrono::duration<double, std::milli>(end - start).count();
}

//===----------------------------------------------------------------------===//
// BCPWriter Construction
//===----------------------------------------------------------------------===//

BCPWriter::BCPWriter(tds::TdsConnection &conn, const BCPCopyTarget &target, vector<BCPColumnMetadata> columns,
					 vector<int32_t> column_mapping)
	: conn_(conn),
	  target_(target),
	  columns_(std::move(columns)),
	  column_mapping_(std::move(column_mapping)),
	  counters_enabled_(GetBCPDebugLevel() >= 2) {
	// Pre-allocate enough to hold what the buffer ACTUALLY holds, which since the
	// streaming change is STREAM_BLOCK_CHUNKS chunks and not a batch: WriteRows
	// drains whole frames once that many have accumulated.
	//
	// It used to reserve `columns * 100 * 10000` and call it "~1MB initial",
	// which is only true at one column. At 44 it is 44 MB — committed before a
	// single row is encoded, and now multiplied by every parallel writer.
	//
	// 32 bytes per column is an average rather than a bound, so the vector may
	// still grow; that is deliberate. Growth is amortised, capacity is retained
	// across batches by ResetForNextBatch, and reaching the true size by doubling
	// costs a handful of copies once per load — against pre-committing memory
	// per writer for a width the data may never have.
	const size_t est_row_size = columns_.size() * 32;
	const size_t est_buffer = est_row_size * STANDARD_VECTOR_SIZE * STREAM_BLOCK_CHUNKS;
	// The cap is what stops a very wide table from re-creating the old problem
	// across N writers. Past it, growth finds the real size.
	static constexpr size_t MAX_INITIAL_RESERVE = 8u * 1024 * 1024;
	accumulator_buffer_.reserve(MinValue<size_t>(est_buffer, MAX_INITIAL_RESERVE));

	// D4 (spec 054): every row encodes every target column, so per-family
	// value counts are rows × per-family column counts — precompute the
	// latter once (same FamilyFromLogicalType dispatch EncodeRow uses).
	if (counters_enabled_) {
		for (const auto &col : columns_) {
			try {
				family_col_count_[static_cast<uint8_t>(codec::FamilyFromLogicalType(col.duckdb_type))]++;
			} catch (...) {
				unknown_family_col_count_++;
			}
			if (col.IsPLPType()) {
				plp_col_count_++;
			}
		}
	}
}

BCPWriter::~BCPWriter() {
	if (!counters_enabled_) {
		return;
	}
	fprintf(stderr,
			"[MSSQL COUNTERS] bcp writer close: rows=%llu chunks=%llu bytes_sent=%llu plp_values=%llu "
			"utf16_fallback=%llu write_rows=%lluus\n",
			(unsigned long long)rows_sent_.load(), (unsigned long long)counter_chunks_,
			(unsigned long long)bytes_sent_.load(), (unsigned long long)counter_plp_values_,
			(unsigned long long)counter_utf16_fallbacks_, (unsigned long long)counter_write_rows_us_);
	std::string families;
	for (uint8_t f = 0; f < codec::TYPE_FAMILY_COUNT; f++) {
		if (counter_values_per_family_[f] > 0) {
			families += " ";
			families += codec::FamilyName(static_cast<codec::TypeFamily>(f));
			families += "=" + std::to_string(counter_values_per_family_[f]);
		}
	}
	if (counter_unknown_family_values_ > 0) {
		families += " unknown=" + std::to_string(counter_unknown_family_values_);
	}
	if (!families.empty()) {
		fprintf(stderr, "[MSSQL COUNTERS]   values/family (incl. NULLs):%s\n", families.c_str());
	}
}

//===----------------------------------------------------------------------===//
// BCP Protocol Operations
//===----------------------------------------------------------------------===//

void BCPWriter::WriteColmetadata() {
	if (colmetadata_sent_) {
		throw InvalidInputException("MSSQL: COLMETADATA already sent");
	}

	// Build the COLMETADATA token into the accumulator buffer
	// We accumulate all data (COLMETADATA + ROWs + DONE) before sending
	accumulator_buffer_.clear();
	BuildColmetadataToken(accumulator_buffer_);

	colmetadata_sent_ = true;
}

idx_t BCPWriter::WriteRows(DataChunk &chunk) {
	if (!colmetadata_sent_) {
		throw InvalidInputException("MSSQL: COLMETADATA must be sent before rows");
	}

	auto start_lock = Clock::now();
	std::lock_guard<std::mutex> lock(write_mutex_);
	double lock_ms = ElapsedMs(start_lock);

	idx_t rows_written = 0;
	idx_t row_count = chunk.size();
	size_t buffer_start = accumulator_buffer_.size();
	// D4: thread-local snapshot — the delta after the encode loop is this
	// call's fallbacks (the loop runs on the calling thread, under the mutex).
	const uint64_t utf16_fallbacks_at_entry = counters_enabled_ ? tds::encoding::Utf16FallbackCount() : 0;

	auto start_encode = Clock::now();
	// Accumulate all rows into the accumulator buffer. EncodeChunk writes the
	// 0xD1 ROW token per row and hoists per-column state (UnifiedVectorFormat,
	// family encoder, NULL wire kind) once per chunk (spec 054 W1+W2).
	const vector<int32_t> *mapping_ptr = column_mapping_.empty() ? nullptr : &column_mapping_;
	tds::encoding::BCPRowEncoder::EncodeChunk(accumulator_buffer_, chunk, columns_, mapping_ptr);
	rows_written = row_count;
	double encode_ms = ElapsedMs(start_encode);

	size_t bytes_added = accumulator_buffer_.size() - buffer_start;
	// Push what is already framable rather than holding the whole batch. Still
	// under write_mutex_, so frames leave in the order they were encoded.
	if (++chunks_since_drain_ >= STREAM_BLOCK_CHUNKS) {
		chunks_since_drain_ = 0;
		DrainWholeFrames();
	}
	rows_sent_.fetch_add(rows_written);
	rows_in_batch_.fetch_add(rows_written);

	if (counters_enabled_ && rows_written > 0) {
		counter_chunks_++;
		for (int f = 0; f < 9; f++) {
			counter_values_per_family_[f] += rows_written * family_col_count_[f];
		}
		counter_unknown_family_values_ += rows_written * unknown_family_col_count_;
		counter_plp_values_ += rows_written * plp_col_count_;
		counter_utf16_fallbacks_ += tds::encoding::Utf16FallbackCount() - utf16_fallbacks_at_entry;
		counter_write_rows_us_ += static_cast<uint64_t>(ElapsedMs(start_lock) * 1000.0);
	}

	// Log timing if debug enabled
	if (GetBCPDebugLevel() >= 1) {
		double rows_per_sec = (encode_ms > 0) ? (rows_written * 1000.0 / encode_ms) : 0;
		double mb_per_sec = (encode_ms > 0) ? (bytes_added / 1024.0 / 1024.0 * 1000.0 / encode_ms) : 0;
		BCPDebugLog(
			1, "WriteRows: %llu rows, %zu bytes in %.2f ms (lock: %.2f ms) | %.0f rows/s, %.1f MB/s | buffer: %zu MB",
			(unsigned long long)rows_written, bytes_added, encode_ms, lock_ms, rows_per_sec, mb_per_sec,
			accumulator_buffer_.size() / (1024 * 1024));
	}

	return rows_written;
}

void BCPWriter::WriteDone(idx_t row_count) {
	if (!colmetadata_sent_) {
		throw InvalidInputException("MSSQL: COLMETADATA must be sent before DONE");
	}

	// Build the DONE token into the accumulator buffer
	BuildDoneToken(accumulator_buffer_, row_count);

	// Send the complete accumulated buffer (COLMETADATA + ROWs + DONE) as a single BULK_LOAD message
	// This sends all data in one packet with EOM flag
	WriteFrames(accumulator_buffer_.data(), accumulator_buffer_.size(), true);
}

idx_t BCPWriter::Finalize() {
	// Blocking on SQL Server's confirmation — the other half of `flush`, and the
	// one no amount of client work can shrink.
	ScopedNs phase(counter_server_wait_ns_);
	// Read the server response after DONE token
	// We expect a DONE token with the row count
	auto start_total = Clock::now();

	std::vector<uint8_t> response;
	auto socket = conn_.GetSocket();
	if (!socket) {
		// T009: Close connection to ensure clean pool state
		conn_.Close();
		throw IOException("MSSQL: Connection socket is null");
	}

	// Receive the complete response message
	auto start_recv = Clock::now();
	BCPDebugLog(1, "Finalize: waiting for server response (timeout: 30s)...");
	if (!socket->ReceiveMessage(response, 30000)) {
		// T009: Close connection before throwing to prevent corrupted pool state
		conn_.Close();
		throw IOException("MSSQL: Failed to receive BCP response: " + socket->GetLastError());
	}
	double recv_ms = ElapsedMs(start_recv);
	BCPDebugLog(1, "Finalize: received %zu bytes in %.2f ms", response.size(), recv_ms);

	// Parse the response to find DONE token and extract row count
	idx_t server_row_count = 0;
	bool found_done = false;
	bool has_error = false;
	string error_message;

	size_t pos = 0;
	while (pos < response.size()) {
		uint8_t token = response[pos++];

		if (token == 0xAA) {  // ERROR token
			has_error = true;
			if (pos + 2 > response.size())
				break;
			uint16_t length = response[pos] | (response[pos + 1] << 8);
			pos += 2;
			if (pos + 4 > response.size())
				break;
			// Error number (4 bytes)
			pos += 4;
			if (pos + 1 > response.size())
				break;
			// State (1 byte)
			pos += 1;
			if (pos + 1 > response.size())
				break;
			// Class (1 byte)
			pos += 1;
			// Message length (USHORT) and message
			if (pos + 2 > response.size())
				break;
			uint16_t msg_len = response[pos] | (response[pos + 1] << 8);
			pos += 2;
			if (pos + msg_len * 2 > response.size())
				break;
			error_message = tds::encoding::Utf16LEDecode(&response[pos], msg_len * 2);
			pos += msg_len * 2;
			// Skip server name and proc name
			if (pos + 1 > response.size())
				break;
			uint8_t server_len = response[pos++];
			pos += server_len * 2;
			if (pos + 1 > response.size())
				break;
			uint8_t proc_len = response[pos++];
			pos += proc_len * 2;
			// Line number (4 bytes for TDS 7.2+)
			pos += 4;
		} else if (token == TOKEN_DONE || token == 0xFE || token == 0xFF) {
			// DONE, DONEPROC, or DONEINPROC token
			if (pos + 8 > response.size())
				break;
			uint16_t status = response[pos] | (response[pos + 1] << 8);
			pos += 2;
			// CurCmd (2 bytes) - skip
			pos += 2;
			// RowCount (8 bytes for TDS 7.2+)
			server_row_count = 0;
			for (int i = 0; i < 8; i++) {
				server_row_count |= static_cast<uint64_t>(response[pos + i]) << (i * 8);
			}
			pos += 8;
			found_done = true;

			// Check for error in DONE status
			if (status & 0x0002) {	// DONE_ERROR
				has_error = true;
			}
		} else if (token == 0xAB) {	 // INFO token - skip
			if (pos + 2 > response.size())
				break;
			uint16_t length = response[pos] | (response[pos + 1] << 8);
			pos += 2 + length;
		} else if (token == 0xE3) {	 // ENVCHANGE token - skip
			if (pos + 2 > response.size())
				break;
			uint16_t length = response[pos] | (response[pos + 1] << 8);
			pos += 2 + length;
		} else {
			// Unknown token - try to skip based on typical format
			// Most tokens have USHORT length
			if (pos + 2 > response.size())
				break;
			uint16_t length = response[pos] | (response[pos + 1] << 8);
			pos += 2 + length;
		}
	}

	if (has_error) {
		if (error_message.empty()) {
			error_message = "Unknown SQL Server error during bulk load";
		}
		// T009 (FR-001): Close connection before throwing to prevent corrupted state in pool
		// Without this, connection remains in Executing state and corrupts pool on Release()
		conn_.Close();
		throw InvalidInputException("MSSQL: BCP failed: %s", error_message);
	}

	if (!found_done) {
		// T009: Close connection before throwing to prevent corrupted pool state
		conn_.Close();
		throw IOException("MSSQL: Did not receive DONE token in BCP response");
	}

	// Transition connection back to Idle state
	conn_.TransitionState(tds::ConnectionState::Executing, tds::ConnectionState::Idle);

	double total_ms = ElapsedMs(start_total);
	BCPDebugLog(1, "Finalize: DONE - server confirmed %llu rows in %.2f ms (recv: %.2f ms)",
				(unsigned long long)server_row_count, total_ms, recv_ms);

	return server_row_count;
}

idx_t BCPWriter::FlushBatch(idx_t row_count) {
	auto start_total = Clock::now();
	BCPDebugLog(1, "FlushBatch: flushing batch with %llu rows, buffer_size=%zu MB", (unsigned long long)row_count,
				accumulator_buffer_.size() / (1024 * 1024));

	if (!colmetadata_sent_) {
		throw InvalidInputException("MSSQL: COLMETADATA must be sent before flush");
	}

	// Build DONE token and append to accumulator
	auto start_done = Clock::now();
	BuildDoneToken(accumulator_buffer_, row_count);
	double done_ms = ElapsedMs(start_done);
	BCPDebugLog(1, "FlushBatch: DONE token built in %.2f ms", done_ms);

	// Send the complete accumulated buffer
	auto start_send = Clock::now();
	BCPDebugLog(1, "FlushBatch: sending %zu bytes to server...", accumulator_buffer_.size());
	WriteFrames(accumulator_buffer_.data(), accumulator_buffer_.size(), true);
	double send_ms = ElapsedMs(start_send);
	BCPDebugLog(1, "FlushBatch: send complete in %.2f ms (%.1f MB/s)", send_ms,
				(send_ms > 0) ? (accumulator_buffer_.size() / 1024.0 / 1024.0 * 1000.0 / send_ms) : 0);

	// Read server response
	auto start_recv = Clock::now();
	BCPDebugLog(1, "FlushBatch: waiting for server response...");
	idx_t confirmed_rows = Finalize();
	double recv_ms = ElapsedMs(start_recv);
	BCPDebugLog(1, "FlushBatch: server response in %.2f ms", recv_ms);

	double total_ms = ElapsedMs(start_total);
	double rows_per_sec = (total_ms > 0) ? (row_count * 1000.0 / total_ms) : 0;
	BCPDebugLog(1, "FlushBatch: DONE - %llu rows in %.2f ms (send: %.2f, recv: %.2f) | %.0f rows/s",
				(unsigned long long)confirmed_rows, total_ms, send_ms, recv_ms, rows_per_sec);

	return confirmed_rows;
}

void BCPWriter::ResetForNextBatch() {
	BCPDebugLog(2, "ResetForNextBatch: clearing state for next batch, buffer_capacity=%zu",
				accumulator_buffer_.capacity());

	// Clear buffer but KEEP capacity for reuse (reduces memory fragmentation)
	// The buffer will be reused for the next batch without reallocation
	accumulator_buffer_.clear();
	chunks_since_drain_ = 0;

	// Reset COLMETADATA state so it can be sent again
	colmetadata_sent_ = false;

	// Reset batch row counter (total rows counter is kept)
	rows_in_batch_.store(0);

	// Note: packet_id_ continues incrementing across batches
	// Note: rows_sent_ and bytes_sent_ are cumulative totals
	BCPDebugLog(2, "ResetForNextBatch: buffer cleared, capacity retained=%zu", accumulator_buffer_.capacity());
}

//===----------------------------------------------------------------------===//
// Token Builders
//===----------------------------------------------------------------------===//

void BCPWriter::BuildColmetadataToken(vector<uint8_t> &buffer) {
	// COLMETADATA token format:
	// Token (1 byte): 0x81
	// Count (2 bytes): Number of columns (USHORT)
	// For each column:
	//   UserType (4 bytes): 0x00000000
	//   Flags (2 bytes): Column flags
	//   TYPE_INFO: Type-specific metadata
	//   ColName (B_VARCHAR): Column name

	// Token
	WriteUInt8(buffer, TOKEN_COLMETADATA);

	// Column count
	WriteUInt16LE(buffer, static_cast<uint16_t>(columns_.size()));

	// Column definitions
	for (const auto &col : columns_) {
		// UserType (always 0)
		WriteUInt32LE(buffer, 0x00000000);

		// Flags
		WriteUInt16LE(buffer, col.GetFlags());

		// TYPE_INFO varies by type
		WriteUInt8(buffer, col.tds_type_token);

		switch (col.tds_type_token) {
		case tds::TDS_TYPE_INTN:  // 0x26 - Nullable int
			WriteUInt8(buffer, static_cast<uint8_t>(col.max_length));
			break;

		case tds::TDS_TYPE_BITN:  // 0x68 - Nullable bit
			WriteUInt8(buffer, 1);
			break;

		case tds::TDS_TYPE_FLOATN:	// 0x6D - Nullable float
			WriteUInt8(buffer, static_cast<uint8_t>(col.max_length));
			break;

		case tds::TDS_TYPE_DECIMAL:	 // 0x6A - Decimal
		case tds::TDS_TYPE_NUMERIC:	 // 0x6C - Numeric
			WriteUInt8(buffer, static_cast<uint8_t>(col.max_length));
			WriteUInt8(buffer, col.precision);
			WriteUInt8(buffer, col.scale);
			break;

		case tds::TDS_TYPE_NVARCHAR:	// 0xE7 - Unicode string
		case tds::TDS_TYPE_BIGVARCHAR:	// 0xA7 - single-byte string; UTF-8 when the collation says so
			WriteUInt16LE(buffer, col.max_length);
			// Collation (5 bytes)
			for (int i = 0; i < 5; i++) {
				WriteUInt8(buffer, col.collation[i]);
			}
			break;

		case tds::TDS_TYPE_BIGVARBINARY:  // 0xA5 - Binary
			WriteUInt16LE(buffer, col.max_length);
			break;

		case tds::TDS_TYPE_UNIQUEIDENTIFIER:  // 0x24 - GUID
			WriteUInt8(buffer, 16);
			break;

		case tds::TDS_TYPE_XML:	 // 0xF1
			// SQL Server rejects XML type (0xF1) in BCP COLMETADATA.
			// Rewrite as NVARCHAR(MAX) — SQL Server auto-converts to XML on the target column.
			// No length limitation: nvarchar(max) supports up to 2 GB, same as XML.
			buffer.back() = tds::TDS_TYPE_NVARCHAR;
			WriteUInt16LE(buffer, 0xFFFF);	// MAX indicator
			// Collation (5 bytes)
			for (int i = 0; i < 5; i++) {
				WriteUInt8(buffer, col.collation[i]);
			}
			break;

		case tds::TDS_TYPE_DATE:  // 0x28
			// No additional metadata
			break;

		case tds::TDS_TYPE_TIME:  // 0x29
			WriteUInt8(buffer, col.scale);
			break;

		case tds::TDS_TYPE_DATETIME2:  // 0x2A
			WriteUInt8(buffer, col.scale);
			break;

		case tds::TDS_TYPE_DATETIMEOFFSET:	// 0x2B
			WriteUInt8(buffer, col.scale);
			break;

		default:
			throw NotImplementedException("MSSQL: Unsupported TDS type 0x%02X in COLMETADATA", col.tds_type_token);
		}

		// Column name (B_VARCHAR format: length byte + UTF-16LE)
		WriteUTF16LEString(buffer, col.name);
	}
}

void BCPWriter::BuildDoneToken(vector<uint8_t> &buffer, idx_t row_count) {
	// DONE token format:
	// Token (1 byte): 0xFD
	// Status (2 bytes): DONE_COUNT (0x0010)
	// CurCmd (2 bytes): INSERT (0x00C3)
	// RowCount (8 bytes for TDS 7.2+): Number of rows

	// Token
	WriteUInt8(buffer, TOKEN_DONE);

	// Status: DONE_COUNT to indicate row count is valid
	WriteUInt16LE(buffer, DONE_COUNT);

	// CurCmd: INSERT
	WriteUInt16LE(buffer, CURCMD_INSERT);

	// RowCount (8 bytes little-endian)
	WriteUInt64LE(buffer, static_cast<uint64_t>(row_count));
}

//===----------------------------------------------------------------------===//
// Wire Helpers
//===----------------------------------------------------------------------===//

void BCPWriter::DrainWholeFrames() {
	const size_t max_payload = conn_.GetNegotiatedPacketSize() - tds::TDS_HEADER_SIZE;
	// Whole frames only. The tail that does not fill one stays behind rather than
	// going out short: the batch is one TDS message either way, and a short frame
	// per block would multiply the send count for nothing.
	const size_t sendable = (accumulator_buffer_.size() / max_payload) * max_payload;
	if (sendable == 0) {
		return;
	}
	WriteFrames(accumulator_buffer_.data(), sendable, false);
	// The remainder is shorter than one frame, so this moves at most max_payload
	// bytes.
	accumulator_buffer_.erase(accumulator_buffer_.begin(),
							  accumulator_buffer_.begin() + static_cast<std::ptrdiff_t>(sendable));
}

void BCPWriter::WriteFrames(const uint8_t *data, size_t length, bool eom) {
	ScopedNs phase(counter_build_send_ns_);
	counter_send_calls_.fetch_add(1, std::memory_order_relaxed);

	auto socket = conn_.GetSocket();
	if (!socket) {
		throw IOException("MSSQL: Connection socket is null");
	}

	const uint32_t packet_size = conn_.GetNegotiatedPacketSize();
	const size_t max_payload = packet_size - tds::TDS_HEADER_SIZE;
	if (frame_buffer_.size() != packet_size) {
		frame_buffer_.resize(packet_size);
	}

	size_t offset = 0;
	// An empty payload still has to produce the end-of-message frame, or the
	// server keeps waiting for a message that was already complete.
	while (offset < length || (length == 0 && eom)) {
		const size_t chunk = std::min(max_payload, length - offset);
		const bool last = eom && (offset + chunk >= length);
		const uint16_t total = static_cast<uint16_t>(tds::TDS_HEADER_SIZE + chunk);

		// The 8-byte TDS header, written in place ahead of the payload: type,
		// status, big-endian length, SPID, packet id, window.
		uint8_t *f = frame_buffer_.data();
		f[0] = static_cast<uint8_t>(tds::PacketType::BULK_LOAD);
		f[1] = last ? 0x01 : 0x00;
		f[2] = static_cast<uint8_t>((total >> 8) & 0xFF);
		f[3] = static_cast<uint8_t>(total & 0xFF);
		f[4] = 0;
		f[5] = 0;
		f[6] = packet_id_++;
		f[7] = 0;
		std::memcpy(f + tds::TDS_HEADER_SIZE, data + offset, chunk);

		if (!socket->Send(f, total)) {
			// Close before throwing: a half-written message leaves the connection
			// unusable, and returning it to the pool would hand the corruption to
			// the next caller (T009).
			conn_.Close();
			throw IOException("MSSQL: Failed to send BULK_LOAD frame at offset %zu/%zu: %s", offset, length,
							  socket->GetLastError().c_str());
		}
		bytes_sent_.fetch_add(total);
		offset += chunk;
		if (chunk == 0) {
			break;
		}
	}
}

void BCPWriter::WriteUInt8(vector<uint8_t> &buffer, uint8_t value) {
	buffer.push_back(value);
}

void BCPWriter::WriteUInt16LE(vector<uint8_t> &buffer, uint16_t value) {
	buffer.push_back(static_cast<uint8_t>(value & 0xFF));
	buffer.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
}

void BCPWriter::WriteUInt32LE(vector<uint8_t> &buffer, uint32_t value) {
	buffer.push_back(static_cast<uint8_t>(value & 0xFF));
	buffer.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
	buffer.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
	buffer.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
}

void BCPWriter::WriteUInt64LE(vector<uint8_t> &buffer, uint64_t value) {
	for (int i = 0; i < 8; i++) {
		buffer.push_back(static_cast<uint8_t>((value >> (i * 8)) & 0xFF));
	}
}

void BCPWriter::WriteInt16LE(vector<uint8_t> &buffer, int16_t value) {
	WriteUInt16LE(buffer, static_cast<uint16_t>(value));
}

void BCPWriter::WriteUTF16LEString(vector<uint8_t> &buffer, const string &str) {
	// B_VARCHAR format for column names in COLMETADATA:
	// Length (1 byte): Number of characters (not bytes)
	// Data: UTF-16LE encoded string

	// Convert to UTF-16LE
	auto utf16_bytes = tds::encoding::Utf16LEEncode(str);

	// Write length in characters (UTF-16LE is 2 bytes per character for BMP)
	uint8_t char_count = static_cast<uint8_t>(utf16_bytes.size() / 2);
	buffer.push_back(char_count);

	// Write the UTF-16LE bytes
	buffer.insert(buffer.end(), utf16_bytes.begin(), utf16_bytes.end());
}

}  // namespace mssql
}  // namespace duckdb
