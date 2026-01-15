#pragma once

#include "tds_types.hpp"
#include "tds_packet.hpp"
#include <string>
#include <memory>
#include <chrono>

namespace duckdb {
namespace tds {

// Low-level TCP socket wrapper for TDS connections
// Pure C++ with no DuckDB dependencies
class TdsSocket {
public:
	TdsSocket();
	~TdsSocket();

	// Non-copyable
	TdsSocket(const TdsSocket&) = delete;
	TdsSocket& operator=(const TdsSocket&) = delete;

	// Movable
	TdsSocket(TdsSocket&& other) noexcept;
	TdsSocket& operator=(TdsSocket&& other) noexcept;

	// Connection management
	bool Connect(const std::string& host, uint16_t port, int timeout_seconds);
	void Close();
	bool IsConnected() const;

	// Data transfer
	bool Send(const uint8_t* data, size_t length);
	bool Send(const std::vector<uint8_t>& data);
	bool SendPacket(const TdsPacket& packet);

	// Receive with timeout
	// Returns number of bytes received, 0 on timeout, -1 on error
	ssize_t Receive(uint8_t* buffer, size_t max_length, int timeout_ms);

	// Receive a complete TDS packet with timeout
	// Returns true if packet received, false on timeout/error
	bool ReceivePacket(TdsPacket& packet, int timeout_ms);

	// Receive all packets until EOM (End Of Message)
	// Returns accumulated payload from all packets
	bool ReceiveMessage(std::vector<uint8_t>& message, int timeout_ms);

	// Connection info
	const std::string& GetHost() const { return host_; }
	uint16_t GetPort() const { return port_; }
	int GetSocketFd() const { return fd_; }

	// Error handling
	const std::string& GetLastError() const { return last_error_; }

private:
	int fd_;                  // Socket file descriptor (-1 if closed)
	std::string host_;        // Remote hostname
	uint16_t port_;           // Remote port
	bool connected_;          // Connection status
	std::string last_error_;  // Last error message

	// Internal receive buffer for partial packet handling
	std::vector<uint8_t> receive_buffer_;

	// Helper to set non-blocking mode
	bool SetNonBlocking(bool enable);

	// Helper to wait for socket ready (select/poll)
	bool WaitForReady(bool for_write, int timeout_ms);
};

}  // namespace tds
}  // namespace duckdb
