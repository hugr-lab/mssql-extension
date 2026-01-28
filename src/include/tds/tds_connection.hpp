#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include "tds_platform.hpp"
#include "tds_protocol.hpp"
#include "tds_socket.hpp"
#include "tds_types.hpp"

namespace duckdb {
namespace tds {

// Represents a single TDS connection to SQL Server
// Implements connection state machine per FR-009
class TdsConnection {
public:
	TdsConnection();
	~TdsConnection();

	// Non-copyable
	TdsConnection(const TdsConnection &) = delete;
	TdsConnection &operator=(const TdsConnection &) = delete;

	// Movable
	TdsConnection(TdsConnection &&other) noexcept;
	TdsConnection &operator=(TdsConnection &&other) noexcept;

	// Connection establishment (FR-001, FR-002)
	// Establishes TCP connection to host:port
	bool Connect(const std::string &host, uint16_t port, int timeout_seconds = DEFAULT_CONNECTION_TIMEOUT);

	// Authentication (FR-006, FR-007)
	// Performs PRELOGIN and LOGIN7 handshake with SQL Server authentication
	// Parameters:
	//   use_encrypt - if true, enables TLS encryption after PRELOGIN (requires server support)
	bool Authenticate(const std::string &username, const std::string &password, const std::string &database,
					  bool use_encrypt = false);

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

	// Execute SQL batch and start receiving response (FR for User Story 1)
	// Sends SQL_BATCH packet(s) and prepares connection for streaming response
	// Returns true if batch was sent successfully
	// After this, use ReceiveData() to read response packets
	bool ExecuteBatch(const std::string &sql);

	// Receive more response data into provided buffer
	// Returns bytes received, 0 on connection close, -1 on error
	// timeout_ms: 0 = non-blocking, >0 = wait up to timeout_ms
	ssize_t ReceiveData(uint8_t *buffer, size_t buffer_size, int timeout_ms = DEFAULT_QUERY_TIMEOUT * 1000);

	// State management (FR-009, FR-010)
	ConnectionState GetState() const {
		return state_.load(std::memory_order_acquire);
	}

	// Attempt state transition (thread-safe)
	bool TransitionState(ConnectionState from, ConnectionState to);

	// Getters
	uint16_t GetSpid() const {
		return spid_;
	}
	const std::string &GetHost() const {
		return host_;
	}
	uint16_t GetPort() const {
		return port_;
	}
	const std::string &GetDatabase() const {
		return database_;
	}
	const std::string &GetLastError() const {
		return last_error_;
	}
	bool IsTlsEnabled() const {
		return tls_enabled_;
	}

	// Transaction descriptor management (for SQL_BATCH ALL_HEADERS)
	// Set the transaction descriptor (8 bytes) from ENVCHANGE BEGIN_TRANS response
	void SetTransactionDescriptor(const uint8_t *descriptor);

	// Get the current transaction descriptor (returns pointer to 8 bytes, or nullptr if not set)
	const uint8_t *GetTransactionDescriptor() const;

	// Clear the transaction descriptor (e.g., after COMMIT/ROLLBACK)
	void ClearTransactionDescriptor();

	// Check if a transaction descriptor is currently set
	bool HasTransactionDescriptor() const {
		return has_transaction_descriptor_;
	}

	// Connection reset — flag the next SQL_BATCH to include RESET_CONNECTION in TDS header
	void SetNeedsReset(bool reset) {
		needs_reset_ = reset;
	}
	bool NeedsReset() const {
		return needs_reset_;
	}

	// Timestamps for pool management
	std::chrono::steady_clock::time_point GetCreatedAt() const {
		return created_at_;
	}
	std::chrono::steady_clock::time_point GetLastUsedAt() const {
		return last_used_at_;
	}
	void UpdateLastUsed() {
		last_used_at_ = std::chrono::steady_clock::now();
	}

	// Check if connection has been idle longer than threshold
	bool IsLongIdle() const;

	// Get underlying socket for advanced operations
	TdsSocket *GetSocket() {
		return socket_.get();
	}

private:
	std::unique_ptr<TdsSocket> socket_;
	std::atomic<ConnectionState> state_;

	// Connection info
	std::string host_;
	uint16_t port_;
	std::string database_;
	uint16_t spid_;	 // Server Process ID

	// Timing
	std::chrono::steady_clock::time_point created_at_;
	std::chrono::steady_clock::time_point last_used_at_;

	// Error tracking
	std::string last_error_;

	// Packet sequencing
	uint8_t next_packet_id_;

	// TLS state
	bool tls_enabled_;

	// Negotiated packet size from server (from ENVCHANGE during login)
	uint32_t negotiated_packet_size_;

	// Transaction descriptor (8 bytes) for SQL_BATCH ALL_HEADERS
	// Set via SetTransactionDescriptor() after BEGIN TRANSACTION response
	uint8_t transaction_descriptor_[8] = {0};
	bool has_transaction_descriptor_ = false;

	// Connection reset flag — when true, next SQL_BATCH sets RESET_CONNECTION in TDS header
	bool needs_reset_ = false;

	// Internal helpers
	bool DoPrelogin(bool use_encrypt);
	bool DoLogin7(const std::string &username, const std::string &password, const std::string &database);
};

}  // namespace tds
}  // namespace duckdb
