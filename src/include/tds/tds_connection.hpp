#pragma once

#include "tds_types.hpp"
#include "tds_socket.hpp"
#include "tds_protocol.hpp"
#include <atomic>
#include <chrono>
#include <memory>
#include <string>

namespace duckdb {
namespace tds {

// Represents a single TDS connection to SQL Server
// Implements connection state machine per FR-009
class TdsConnection {
public:
	TdsConnection();
	~TdsConnection();

	// Non-copyable
	TdsConnection(const TdsConnection&) = delete;
	TdsConnection& operator=(const TdsConnection&) = delete;

	// Movable
	TdsConnection(TdsConnection&& other) noexcept;
	TdsConnection& operator=(TdsConnection&& other) noexcept;

	// Connection establishment (FR-001, FR-002)
	// Establishes TCP connection to host:port
	bool Connect(const std::string& host, uint16_t port, int timeout_seconds = DEFAULT_CONNECTION_TIMEOUT);

	// Authentication (FR-006, FR-007)
	// Performs PRELOGIN and LOGIN7 handshake with SQL Server authentication
	bool Authenticate(const std::string& username, const std::string& password, const std::string& database);

	// Connection health check (FR-015)
	// Quick state check - no I/O, just checks internal state
	bool IsAlive() const;

	// TDS-level ping - sends empty SQLBATCH and waits for DONE token
	bool Ping(int timeout_ms = 5000);

	// Full validation with ping for long-idle connections
	bool ValidateWithPing();

	// Close connection (FR-003)
	void Close();

	// Send ATTENTION signal for query cancellation
	bool SendAttention();

	// Wait for ATTENTION acknowledgment
	bool WaitForAttentionAck(int timeout_ms = CANCELLATION_TIMEOUT * 1000);

	// State management (FR-009, FR-010)
	ConnectionState GetState() const { return state_.load(std::memory_order_acquire); }

	// Attempt state transition (thread-safe)
	bool TransitionState(ConnectionState from, ConnectionState to);

	// Getters
	uint16_t GetSpid() const { return spid_; }
	const std::string& GetHost() const { return host_; }
	uint16_t GetPort() const { return port_; }
	const std::string& GetDatabase() const { return database_; }
	const std::string& GetLastError() const { return last_error_; }

	// Timestamps for pool management
	std::chrono::steady_clock::time_point GetCreatedAt() const { return created_at_; }
	std::chrono::steady_clock::time_point GetLastUsedAt() const { return last_used_at_; }
	void UpdateLastUsed() { last_used_at_ = std::chrono::steady_clock::now(); }

	// Check if connection has been idle longer than threshold
	bool IsLongIdle() const;

	// Get underlying socket for advanced operations
	TdsSocket* GetSocket() { return socket_.get(); }

private:
	std::unique_ptr<TdsSocket> socket_;
	std::atomic<ConnectionState> state_;

	// Connection info
	std::string host_;
	uint16_t port_;
	std::string database_;
	uint16_t spid_;  // Server Process ID

	// Timing
	std::chrono::steady_clock::time_point created_at_;
	std::chrono::steady_clock::time_point last_used_at_;

	// Error tracking
	std::string last_error_;

	// Packet sequencing
	uint8_t next_packet_id_;

	// Internal helpers
	bool DoPrelogin();
	bool DoLogin7(const std::string& username, const std::string& password, const std::string& database);
};

}  // namespace tds
}  // namespace duckdb
