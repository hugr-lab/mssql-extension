//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// tds_tls_impl.hpp
//
// TLS implementation interface - provides TLS functionality using OpenSSL.
// This is compiled into a static library and linked with symbol hiding.
//
// TDS Protocol TLS Integration:
// In TDS, TLS handshake data must be wrapped in TDS PRELOGIN packets during
// the handshake phase. This is handled by setting custom I/O callbacks via
// SetBioCallbacks() before calling Handshake(). After handshake completes,
// TLS data flows directly over the socket.
//===----------------------------------------------------------------------===//

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include "../tds_platform.hpp"

namespace duckdb {
namespace tds {

// Forward declaration for PIMPL
struct TlsImplContext;

// I/O callback types for custom send/receive (used for TDS-wrapped TLS)
// Return: bytes sent/received on success, -1 on error, 0 on would-block
using TlsSendCallback = std::function<int(const uint8_t *data, size_t len)>;
using TlsRecvCallback = std::function<int(uint8_t *buf, size_t len, int timeout_ms)>;

// TLS implementation class using OpenSSL
// This class is used by the loadable extension with symbols hidden
class TlsImpl {
public:
	TlsImpl();
	~TlsImpl();

	// Non-copyable
	TlsImpl(const TlsImpl &) = delete;
	TlsImpl &operator=(const TlsImpl &) = delete;

	// Initialize TLS context (entropy, RNG, config)
	bool Initialize();

	// Wrap an existing socket file descriptor
	// hostname is optional, used for SNI (Server Name Indication)
	bool WrapSocket(int socket_fd, const std::string &hostname = "");

	// Set custom I/O callbacks for send/receive
	// This is used for TDS-wrapped TLS where handshake data must be wrapped
	// in TDS PRELOGIN packets. If not set, uses direct socket I/O.
	// The callbacks are used during Handshake and can be cleared after.
	void SetBioCallbacks(TlsSendCallback send_cb, TlsRecvCallback recv_cb);

	// Clear custom I/O callbacks (reverts to direct socket I/O)
	void ClearBioCallbacks();

	// Perform TLS handshake
	bool Handshake(int timeout_ms = 30000);

	// Send data over TLS
	ssize_t Send(const uint8_t *data, size_t length);

	// Receive data over TLS
	ssize_t Receive(uint8_t *buffer, size_t max_length, int timeout_ms = 0);

	// Close TLS connection gracefully
	void Close();

	// Check if TLS is initialized and handshake completed
	bool IsInitialized() const;

	// Get last error message
	const std::string &GetLastError() const;

	// Get last error code
	int GetLastErrorCode() const;

	// Get negotiated cipher suite name
	std::string GetCipherSuite() const;

	// Get TLS version string
	std::string GetTlsVersion() const;

private:
	std::unique_ptr<TlsImplContext> ctx_;
};

}  // namespace tds
}  // namespace duckdb
