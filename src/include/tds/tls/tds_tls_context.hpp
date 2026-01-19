//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// tds_tls_context.hpp
//
// TLS wrapper using mbedTLS for encrypted TDS connections
//===----------------------------------------------------------------------===//

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace duckdb {
namespace tds {

// I/O callback types for custom send/receive (used for TDS-wrapped TLS)
// Return: bytes sent/received on success, -1 on error, 0 on would-block/timeout
using TlsSendCallback = std::function<int(const uint8_t *data, size_t len)>;
using TlsRecvCallback = std::function<int(uint8_t *buf, size_t len, int timeout_ms)>;

// TLS error codes for distinct error handling
enum class TlsErrorCode {
	NONE = 0,
	INIT_FAILED,		// mbedTLS initialization error
	HANDSHAKE_FAILED,	// TLS handshake error
	HANDSHAKE_TIMEOUT,	// TLS handshake timed out
	SEND_FAILED,		// TLS write error
	RECV_FAILED,		// TLS read error
	NOT_INITIALIZED,	// TLS context not initialized
	PEER_CLOSED,		// Peer closed connection gracefully
	SERVER_NO_ENCRYPT,	// Server does not support encryption
	TLS_NOT_AVAILABLE	// TLS support not compiled in (loadable extension)
};

// Convert TLS error code to string
const char *TlsErrorCodeToString(TlsErrorCode code);

// Forward declaration for PIMPL
struct TlsTdsContextImpl;

// TLS context wrapper for mbedTLS
// Manages TLS state for a single encrypted connection
// Uses PIMPL to hide mbedTLS headers from DuckDB's conflicting bundled mbedTLS
class TlsTdsContext {
public:
	TlsTdsContext();
	~TlsTdsContext();

	// Non-copyable
	TlsTdsContext(const TlsTdsContext &) = delete;
	TlsTdsContext &operator=(const TlsTdsContext &) = delete;

	// Movable
	TlsTdsContext(TlsTdsContext &&other) noexcept;
	TlsTdsContext &operator=(TlsTdsContext &&other) noexcept;

	// Initialize TLS context (entropy, RNG, config)
	// Must be called before WrapSocket/Handshake
	bool Initialize();

	// Wrap an existing socket file descriptor
	// The socket must already be connected via TCP
	// hostname is optional, used for SNI (Server Name Indication)
	bool WrapSocket(int socket_fd, const std::string &hostname = "");

	// Set custom I/O callbacks for send/receive
	// This is used for TDS-wrapped TLS where handshake data must be wrapped
	// in TDS PRELOGIN packets. If not set, uses direct socket I/O.
	void SetBioCallbacks(TlsSendCallback send_cb, TlsRecvCallback recv_cb);

	// Clear custom I/O callbacks (reverts to direct socket I/O)
	void ClearBioCallbacks();

	// Perform TLS handshake
	// timeout_ms: maximum time to wait for handshake completion
	// Returns true on success, false on failure (check GetLastError)
	bool Handshake(int timeout_ms = 30000);

	// Send data over TLS
	// Returns number of bytes sent, or -1 on error
	ssize_t Send(const uint8_t *data, size_t length);

	// Receive data over TLS
	// Returns number of bytes received, 0 on peer close, -1 on error
	ssize_t Receive(uint8_t *buffer, size_t max_length, int timeout_ms = 0);

	// Close TLS connection gracefully
	// Sends close_notify and frees resources
	void Close();

	// Check if TLS is initialized and handshake completed
	bool IsInitialized() const;

	// Get last error message (includes mbedTLS details)
	const std::string &GetLastError() const;

	// Get last error code
	TlsErrorCode GetLastErrorCode() const;

	// Get negotiated cipher suite name (for logging)
	std::string GetCipherSuite() const;

	// Get TLS version string (for logging)
	std::string GetTlsVersion() const;

private:
	// PIMPL - hides mbedTLS types from header
	std::unique_ptr<TlsTdsContextImpl> impl_;
};

}  // namespace tds
}  // namespace duckdb
