#pragma once

#include "tds_types.hpp"
#include <vector>
#include <cstdint>
#include <memory>

namespace duckdb {
namespace tds {

// TDS Packet with 8-byte header and variable payload
// Header format (all multi-byte values big-endian):
//   Offset 0: Type (1 byte)
//   Offset 1: Status (1 byte)
//   Offset 2-3: Length (2 bytes, includes header)
//   Offset 4-5: SPID (2 bytes)
//   Offset 6: Packet ID (1 byte, sequence 1-255)
//   Offset 7: Window (1 byte, reserved, always 0)
class TdsPacket {
public:
	TdsPacket();
	TdsPacket(PacketType type, PacketStatus status = PacketStatus::END_OF_MESSAGE);
	~TdsPacket() = default;

	// Getters
	PacketType GetType() const { return type_; }
	PacketStatus GetStatus() const { return status_; }
	uint16_t GetLength() const;
	uint16_t GetSpid() const { return spid_; }
	uint8_t GetPacketId() const { return packet_id_; }
	const std::vector<uint8_t>& GetPayload() const { return payload_; }
	std::vector<uint8_t>& GetPayload() { return payload_; }

	// Setters
	void SetType(PacketType type) { type_ = type; }
	void SetStatus(PacketStatus status) { status_ = status; }
	void SetSpid(uint16_t spid) { spid_ = spid; }
	void SetPacketId(uint8_t id) { packet_id_ = id; }

	// Payload manipulation
	void AppendPayload(const uint8_t* data, size_t length);
	void AppendPayload(const std::vector<uint8_t>& data);
	void AppendByte(uint8_t byte);
	void AppendUInt16BE(uint16_t value);
	void AppendUInt32BE(uint32_t value);
	void AppendUInt16LE(uint16_t value);
	void AppendUInt32LE(uint32_t value);
	void AppendString(const std::string& str);  // ASCII/UTF-8
	void AppendUTF16LE(const std::string& str); // UTF-16LE for TDS strings
	void ClearPayload();

	// Serialize packet to bytes (header + payload)
	std::vector<uint8_t> Serialize() const;

	// Parse packet from bytes (returns bytes consumed, 0 on incomplete)
	static size_t Parse(const uint8_t* data, size_t length, TdsPacket& packet);

	// Check if we have a complete packet header
	static bool HasCompleteHeader(const uint8_t* data, size_t length);

	// Get the expected packet length from header
	static uint16_t GetPacketLength(const uint8_t* data);

	// Helper to check EOM flag
	bool IsEndOfMessage() const {
		return (static_cast<uint8_t>(status_) & static_cast<uint8_t>(PacketStatus::END_OF_MESSAGE)) != 0;
	}

private:
	PacketType type_;
	PacketStatus status_;
	uint16_t spid_;
	uint8_t packet_id_;
	uint8_t window_;  // Reserved, always 0
	std::vector<uint8_t> payload_;
};

}  // namespace tds
}  // namespace duckdb
