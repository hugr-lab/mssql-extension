#pragma once

#include <chrono>
#include <memory>
#include <string>
#include "tds/tds_platform.hpp"
#include "tds/tls/tds_tls_context.hpp"
#include "tds_packet.hpp"
#include "tds_types.hpp"

namespace duckdb {
namespace tds {

// Low-level TCP socket wrapper for TDS connections
// Supports both plaintext and TLS-encrypted communication
// Pure C++ with no DuckDB dependencies
class TdsSocket {
public:
	TdsSocket();
	// noexcept (spec 047 T046k): destructor body wraps Close().
	~TdsSocket() noexcept;

	// Non-copyable
	TdsSocket(const TdsSocket &) = delete;
	TdsSocket &operator=(const TdsSocket &) = delete;

	// Movable
	TdsSocket(TdsSocket &&other) noexcept;
	TdsSocket &operator=(TdsSocket &&other) noexcept;

	// Connection management
	bool Connect(const std::string &host, uint16_t port, int timeout_seconds);
	void Close();
	bool IsConnected() const;

	// TLS support
	// Enable TLS encryption on an existing connected socket
	// Must be called after Connect() and before sending any encrypted data
	// The packet_id parameter is used to continue the TDS packet sequence during TLS handshake
	// (TLS handshake is wrapped in TDS PRELOGIN packets which need sequential packet IDs)
	// Optional sni_hostname overrides the default (host_) for TLS SNI - useful for Azure routing
	// Returns true on success, false on failure (check GetLastError())
	bool EnableTls(uint8_t &packet_id, int timeout_ms = 30000, const std::string &sni_hostname = "");

	// Check if TLS is currently enabled
	bool IsTlsEnabled() const;

	// Get TLS information (only valid when TLS is enabled)
	std::string GetTlsCipherSuite() const;
	std::string GetTlsVersion() const;

	// Data transfer
	bool Send(const uint8_t *data, size_t length);
	bool Send(const std::vector<uint8_t> &data);
	bool SendPacket(const TdsPacket &packet);

	// Receive with timeout
	// Returns number of bytes received, 0 on timeout, -1 on error
	ssize_t Receive(uint8_t *buffer, size_t max_length, int timeout_ms);

	// Receive a complete TDS packet with timeout
	// Returns true if packet received, false on timeout/error
	bool ReceivePacket(TdsPacket &packet, int timeout_ms);

	//! View of the next packet's PAYLOAD, valid until the next receive call on
	//! this socket. Lets the streaming read path skip the copy into
	//! TdsPacket::payload_, which it immediately copied again.
	bool ReceivePayloadView(const uint8_t *&payload, size_t &payload_length, int timeout_ms);

	// Receive all packets until EOM (End Of Message)
	// Returns accumulated payload from all packets
	bool ReceiveMessage(std::vector<uint8_t> &message, int timeout_ms);

	// Connection info
	const std::string &GetHost() const {
		return host_;
	}
	uint16_t GetPort() const {
		return port_;
	}
	int GetSocketFd() const {
		return fd_;
	}

	// Error handling
	const std::string &GetLastError() const {
		return last_error_;
	}

	// Clear receive buffer (useful before starting a new query)
	void ClearReceiveBuffer() {
		receive_buffer_.clear();
		receive_pos_ = 0;
	}

	// Spec 055: size the receive staging buffer from the negotiated frame size.
	// Called once after login, when the frame size is actually known.
	//
	// Two things depended on the old fixed 4096 scratch: a 32 KB frame was
	// reassembled from eight recv() calls (the syscall count the larger frame was
	// meant to remove), and every completed packet triggered an erase() from the
	// front of receive_buffer_ — an O(n) memmove of everything still buffered.
	// Reading whole frames at a time and consuming through a cursor removes both.
	void SetReceiveFraming(uint32_t packet_size, uint32_t frames);

private:
	int fd_;				  // Socket file descriptor (-1 if closed)
	std::string host_;		  // Remote hostname
	uint16_t port_;			  // Remote port
	bool connected_;		  // Connection status
	std::string last_error_;  // Last error message

	// TLS context for encrypted connections (null when TLS is not enabled)
	std::unique_ptr<TlsTdsContext> tls_context_;

	// Internal receive buffer for partial packet handling.
	// Consumed through receive_pos_ rather than erase(): a completed packet only
	// advances the cursor, and the tail is compacted to the front once, when the
	// cursor has caught up with the end (spec 055).
	std::vector<uint8_t> receive_buffer_;
	size_t receive_pos_ = 0;

	// Staging buffer for recv(), sized to hold several whole TDS frames
	// (SetReceiveFraming). Empty until sized; ReceivePacket then falls back to
	// the pre-negotiation default.
	//! Bytes per recv(). Not a buffer: reads go straight into the tail of
	//! receive_buffer_.
	size_t recv_read_size_ = TDS_DEFAULT_PACKET_SIZE;

	//! Frame assembly. NextPacket is the single place TDS framing happens.
	const uint8_t *NextPacket(size_t &packet_length, int timeout_ms);
	//! recv() one read's worth into the tail of the assembly buffer.
	bool FillReceiveBuffer(int timeout_ms);

	// Helper to set non-blocking mode
	bool SetNonBlocking(bool enable);

	// Helper to wait for socket ready (select/poll)
	bool WaitForReady(bool for_write, int timeout_ms);
};

}  // namespace tds
}  // namespace duckdb
