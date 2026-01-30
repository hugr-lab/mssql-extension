#include "copy/bcp_writer.hpp"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>

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
	static int level = -1;
	if (level == -1) {
		const char *env = std::getenv("MSSQL_DEBUG");
		level = env ? std::atoi(env) : 0;
	}
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

static double ElapsedMs(TimePoint start) {
	auto end = Clock::now();
	return std::chrono::duration<double, std::milli>(end - start).count();
}

//===----------------------------------------------------------------------===//
// BCPWriter Construction
//===----------------------------------------------------------------------===//

BCPWriter::BCPWriter(tds::TdsConnection &conn, const BCPCopyTarget &target, vector<BCPColumnMetadata> columns,
					 vector<int32_t> column_mapping)
	: conn_(conn), target_(target), columns_(std::move(columns)), column_mapping_(std::move(column_mapping)) {
	// Pre-allocate buffer to reduce reallocation overhead
	// Estimate: 100 bytes per column per row, reserve for 10K rows
	// This will grow as needed but reduces initial reallocations
	size_t estimated_row_size = columns_.size() * 100;
	accumulator_buffer_.reserve(estimated_row_size * 10000);  // ~1MB initial
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

	auto start_encode = Clock::now();
	// Accumulate rows into the accumulator buffer
	for (idx_t row_idx = 0; row_idx < row_count; row_idx++) {
		// Build ROW token for this row
		BuildRowToken(accumulator_buffer_, chunk, row_idx);
		rows_written++;
	}
	double encode_ms = ElapsedMs(start_encode);

	size_t bytes_added = accumulator_buffer_.size() - buffer_start;
	rows_sent_.fetch_add(rows_written);
	rows_in_batch_.fetch_add(rows_written);

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
	SendBulkLoadPacket(accumulator_buffer_, true);
}

idx_t BCPWriter::Finalize() {
	// Read the server response after DONE token
	// We expect a DONE token with the row count
	auto start_total = Clock::now();

	std::vector<uint8_t> response;
	auto socket = conn_.GetSocket();
	if (!socket) {
		throw IOException("MSSQL: Connection socket is null");
	}

	// Receive the complete response message
	auto start_recv = Clock::now();
	BCPDebugLog(1, "Finalize: waiting for server response (timeout: 30s)...");
	if (!socket->ReceiveMessage(response, 30000)) {
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
		throw InvalidInputException("MSSQL: BCP failed: %s", error_message);
	}

	if (!found_done) {
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
	SendBulkLoadPacket(accumulator_buffer_, true);
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

		case tds::TDS_TYPE_NVARCHAR:  // 0xE7 - Unicode string
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

void BCPWriter::BuildRowToken(vector<uint8_t> &buffer, DataChunk &chunk, idx_t row_idx) {
	// ROW token format:
	// Token (1 byte): 0xD1
	// Column values (variable): Type-specific encoding for each column

	// Token
	WriteUInt8(buffer, TOKEN_ROW);

	// Encode all column values using BCPRowEncoder
	// Pass column mapping if we have one (for name-based source-to-target mapping)
	const vector<int32_t> *mapping_ptr = column_mapping_.empty() ? nullptr : &column_mapping_;
	tds::encoding::BCPRowEncoder::EncodeRow(buffer, chunk, row_idx, columns_, mapping_ptr);
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

void BCPWriter::SendBulkLoadPacket(const vector<uint8_t> &buffer, bool is_last) {
	auto start_total = Clock::now();
	BCPDebugLog(2, "SendBulkLoadPacket: buffer_size=%zu, is_last=%d, packet_id=%u", buffer.size(), is_last ? 1 : 0,
				packet_id_);

	// Get the socket
	auto socket = conn_.GetSocket();
	if (!socket) {
		throw IOException("MSSQL: Connection socket is null");
	}

	// Debug: dump first 64 bytes of payload
	if (GetBCPDebugLevel() >= 3 && !buffer.empty()) {
		std::string hex;
		size_t dump_len = std::min(buffer.size(), (size_t)64);
		for (size_t i = 0; i < dump_len; i++) {
			char buf[4];
			snprintf(buf, sizeof(buf), "%02X ", buffer[i]);
			hex += buf;
		}
		BCPDebugLog(3, "SendBulkLoadPacket: first %zu bytes: %s", dump_len, hex.c_str());
	}

	// Use TDS protocol layer to build properly fragmented packets
	auto start_build = Clock::now();
	uint32_t packet_size = conn_.GetNegotiatedPacketSize();
	std::vector<tds::TdsPacket> packets = tds::TdsProtocol::BuildBulkLoadMultiPacket(buffer, packet_size);
	double build_ms = ElapsedMs(start_build);

	BCPDebugLog(1, "SendBulkLoadPacket: built %zu packets in %.2f ms (packet_size=%u)", packets.size(), build_ms,
				packet_size);

	// Send all packets with incrementing packet IDs
	auto start_send = Clock::now();
	size_t bytes_sent_so_far = 0;
	double slowest_packet_ms = 0;
	size_t slowest_packet_idx = 0;

	for (size_t i = 0; i < packets.size(); i++) {
		auto &packet = packets[i];
		packet.SetPacketId(packet_id_++);

		// Debug: dump the TDS packet header
		if (GetBCPDebugLevel() >= 2) {
			BCPDebugLog(2,
						"SendBulkLoadPacket: sending packet %zu/%zu, type=0x%02X, status=0x%02X, length=%u, "
						"payload=%zu, eom=%d, pkt_id=%u",
						i + 1, packets.size(), static_cast<unsigned>(packet.GetType()),
						static_cast<unsigned>(packet.GetStatus()), packet.GetLength(), packet.GetPayload().size(),
						packet.IsEndOfMessage(), packet.GetPacketId());
		}

		if (GetBCPDebugLevel() >= 3) {
			auto serialized = packet.Serialize();
			BCPDebugLog(3, "SendBulkLoadPacket: TDS header (8 bytes): %02X %02X %02X %02X %02X %02X %02X %02X",
						serialized[0], serialized[1], serialized[2], serialized[3], serialized[4], serialized[5],
						serialized[6], serialized[7]);
		}

		// Send the packet with timing
		auto start_pkt = Clock::now();
		if (!socket->SendPacket(packet)) {
			throw IOException("MSSQL: Failed to send BULK_LOAD packet %zu/%zu: %s", i + 1, packets.size(),
							  socket->GetLastError().c_str());
		}
		double pkt_ms = ElapsedMs(start_pkt);
		if (pkt_ms > slowest_packet_ms) {
			slowest_packet_ms = pkt_ms;
			slowest_packet_idx = i;
		}

		// Update bytes sent counter
		bytes_sent_.fetch_add(packet.GetPayload().size() + tds::TDS_HEADER_SIZE);
		bytes_sent_so_far += packet.GetPayload().size() + tds::TDS_HEADER_SIZE;

		// Progress every 1000 packets or at the end
		if ((i + 1) % 1000 == 0 || i + 1 == packets.size()) {
			double elapsed = ElapsedMs(start_send);
			double mb_per_sec = (elapsed > 0) ? (bytes_sent_so_far / 1024.0 / 1024.0 * 1000.0 / elapsed) : 0;
			BCPDebugLog(1, "SendBulkLoadPacket: sent %zu/%zu packets (%zu KB) in %.2f ms | %.1f MB/s", i + 1,
						packets.size(), bytes_sent_so_far / 1024, elapsed, mb_per_sec);
		}
	}

	double send_ms = ElapsedMs(start_send);
	double total_ms = ElapsedMs(start_total);
	double mb_per_sec = (send_ms > 0) ? (bytes_sent_so_far / 1024.0 / 1024.0 * 1000.0 / send_ms) : 0;

	BCPDebugLog(1,
				"SendBulkLoadPacket: DONE - %zu packets, %zu KB in %.2f ms (build: %.2f, send: %.2f) | %.1f MB/s | "
				"slowest pkt[%zu]: %.2f ms",
				packets.size(), bytes_sent_so_far / 1024, total_ms, build_ms, send_ms, mb_per_sec, slowest_packet_idx,
				slowest_packet_ms);
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
