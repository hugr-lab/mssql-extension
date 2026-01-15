#include "tds/tds_packet.hpp"
#include <cstring>
#include <stdexcept>

namespace duckdb {
namespace tds {

TdsPacket::TdsPacket()
    : type_(PacketType::SQL_BATCH),
      status_(PacketStatus::END_OF_MESSAGE),
      spid_(0),
      packet_id_(1),
      window_(0) {
}

TdsPacket::TdsPacket(PacketType type, PacketStatus status)
    : type_(type),
      status_(status),
      spid_(0),
      packet_id_(1),
      window_(0) {
}

uint16_t TdsPacket::GetLength() const {
	return static_cast<uint16_t>(TDS_HEADER_SIZE + payload_.size());
}

void TdsPacket::AppendPayload(const uint8_t* data, size_t length) {
	payload_.insert(payload_.end(), data, data + length);
}

void TdsPacket::AppendPayload(const std::vector<uint8_t>& data) {
	payload_.insert(payload_.end(), data.begin(), data.end());
}

void TdsPacket::AppendByte(uint8_t byte) {
	payload_.push_back(byte);
}

void TdsPacket::AppendUInt16BE(uint16_t value) {
	payload_.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
	payload_.push_back(static_cast<uint8_t>(value & 0xFF));
}

void TdsPacket::AppendUInt32BE(uint32_t value) {
	payload_.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
	payload_.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
	payload_.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
	payload_.push_back(static_cast<uint8_t>(value & 0xFF));
}

void TdsPacket::AppendUInt16LE(uint16_t value) {
	payload_.push_back(static_cast<uint8_t>(value & 0xFF));
	payload_.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
}

void TdsPacket::AppendUInt32LE(uint32_t value) {
	payload_.push_back(static_cast<uint8_t>(value & 0xFF));
	payload_.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
	payload_.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
	payload_.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
}

void TdsPacket::AppendString(const std::string& str) {
	payload_.insert(payload_.end(), str.begin(), str.end());
}

void TdsPacket::AppendUTF16LE(const std::string& str) {
	// Simple ASCII to UTF-16LE conversion
	// For full Unicode support, would need proper conversion
	for (char c : str) {
		payload_.push_back(static_cast<uint8_t>(c));
		payload_.push_back(0);  // High byte for ASCII chars
	}
}

void TdsPacket::ClearPayload() {
	payload_.clear();
}

std::vector<uint8_t> TdsPacket::Serialize() const {
	std::vector<uint8_t> result;
	uint16_t length = GetLength();

	// Header (8 bytes)
	result.push_back(static_cast<uint8_t>(type_));
	result.push_back(static_cast<uint8_t>(status_));
	// Length (big-endian)
	result.push_back(static_cast<uint8_t>((length >> 8) & 0xFF));
	result.push_back(static_cast<uint8_t>(length & 0xFF));
	// SPID (big-endian)
	result.push_back(static_cast<uint8_t>((spid_ >> 8) & 0xFF));
	result.push_back(static_cast<uint8_t>(spid_ & 0xFF));
	// Packet ID
	result.push_back(packet_id_);
	// Window (reserved)
	result.push_back(window_);

	// Payload
	result.insert(result.end(), payload_.begin(), payload_.end());

	return result;
}

bool TdsPacket::HasCompleteHeader(const uint8_t* data, size_t length) {
	return length >= TDS_HEADER_SIZE;
}

uint16_t TdsPacket::GetPacketLength(const uint8_t* data) {
	// Length is at offset 2-3, big-endian
	return (static_cast<uint16_t>(data[2]) << 8) | static_cast<uint16_t>(data[3]);
}

size_t TdsPacket::Parse(const uint8_t* data, size_t length, TdsPacket& packet) {
	if (!HasCompleteHeader(data, length)) {
		return 0;  // Need more data
	}

	uint16_t packet_length = GetPacketLength(data);

	if (packet_length < TDS_HEADER_SIZE || packet_length > TDS_MAX_PACKET_SIZE) {
		throw std::runtime_error("Invalid TDS packet length");
	}

	if (length < packet_length) {
		return 0;  // Need more data
	}

	// Parse header
	packet.type_ = static_cast<PacketType>(data[0]);
	packet.status_ = static_cast<PacketStatus>(data[1]);
	packet.spid_ = (static_cast<uint16_t>(data[4]) << 8) | static_cast<uint16_t>(data[5]);
	packet.packet_id_ = data[6];
	packet.window_ = data[7];

	// Copy payload
	packet.payload_.clear();
	if (packet_length > TDS_HEADER_SIZE) {
		packet.payload_.insert(packet.payload_.end(),
		                       data + TDS_HEADER_SIZE,
		                       data + packet_length);
	}

	return packet_length;
}

}  // namespace tds
}  // namespace duckdb
