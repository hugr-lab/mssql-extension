//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// tds_tls_impl.cpp
//
// TLS implementation using OpenSSL. This file is compiled into a static
// library (mssql_tls) which is linked into the loadable extension with
// all OpenSSL symbols hidden to avoid conflicts.
//===----------------------------------------------------------------------===//

#include "tds/tls/tds_tls_impl.hpp"

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/ssl.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
// MSG_NOSIGNAL prevents SIGPIPE on Linux; macOS uses SO_NOSIGPIPE instead
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
#endif

namespace duckdb {
namespace tds {

// Debug logging controlled by MSSQL_DEBUG environment variable
static int GetMssqlDebugLevel() {
	static int level = -1;
	if (level == -1) {
		const char *env = std::getenv("MSSQL_DEBUG");
		level = env ? std::atoi(env) : 0;
	}
	return level;
}

#define MSSQL_TLS_DEBUG_LOG(lvl, fmt, ...)                           \
	do {                                                             \
		if (GetMssqlDebugLevel() >= lvl)                             \
			fprintf(stderr, "[MSSQL TLS] " fmt "\n", ##__VA_ARGS__); \
	} while (0)

// =============================================================================
// Internal implementation structure - must be defined before BIO callbacks
// =============================================================================

struct TlsImplContext {
	SSL_CTX *ssl_ctx;
	SSL *ssl;
	BIO *bio;

	bool initialized;
	bool handshake_complete;
	int socket_fd;
	std::string last_error;
	int last_error_code;

	// Custom I/O callbacks for TDS-wrapped TLS
	TlsSendCallback send_callback;
	TlsRecvCallback recv_callback;
	int current_timeout_ms;	 // Timeout for current operation

	TlsImplContext()
		: ssl_ctx(nullptr),
		  ssl(nullptr),
		  bio(nullptr),
		  initialized(false),
		  handshake_complete(false),
		  socket_fd(-1),
		  last_error_code(0),
		  current_timeout_ms(30000) {}

	~TlsImplContext() {
		if (ssl) {
			SSL_free(ssl);	// This also frees the BIO attached to SSL
			ssl = nullptr;
			bio = nullptr;	// BIO is freed by SSL_free
		}
		if (ssl_ctx) {
			SSL_CTX_free(ssl_ctx);
			ssl_ctx = nullptr;
		}
	}
};

// =============================================================================
// Custom BIO for TDS-wrapped TLS handshake
// =============================================================================
// OpenSSL uses BIO (Basic I/O) abstraction. For TDS-wrapped TLS we need
// custom BIO methods that route through our callbacks.

// Custom BIO write callback
static int CustomBioWrite(BIO *bio, const char *data, int len) {
	auto *impl = static_cast<TlsImplContext *>(BIO_get_data(bio));
	if (!impl) {
		return -1;
	}

	// Check if custom callback is set (for TDS-wrapped TLS handshake)
	if (impl->send_callback) {
		MSSQL_TLS_DEBUG_LOG(3, "CustomBioWrite: using custom callback, len=%d", len);
		int ret = impl->send_callback(reinterpret_cast<const uint8_t *>(data), static_cast<size_t>(len));
		if (ret < 0) {
			return -1;
		}
		if (ret == 0) {
			BIO_set_retry_write(bio);
			return -1;
		}
		return ret;
	}

	// Direct socket I/O
	int fd = impl->socket_fd;
	MSSQL_TLS_DEBUG_LOG(3, "CustomBioWrite: direct socket fd=%d, len=%d", fd, len);

#ifdef _WIN32
	int ret = send(fd, data, len, 0);
	if (ret < 0) {
		int err = WSAGetLastError();
		if (err == WSAEWOULDBLOCK) {
			BIO_set_retry_write(bio);
		}
		return -1;
	}
#else
	ssize_t ret = send(fd, data, static_cast<size_t>(len), MSG_NOSIGNAL);
	if (ret < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
			BIO_set_retry_write(bio);
		}
		return -1;
	}
#endif

	return static_cast<int>(ret);
}

// Custom BIO read callback
static int CustomBioRead(BIO *bio, char *buf, int len) {
	auto *impl = static_cast<TlsImplContext *>(BIO_get_data(bio));
	if (!impl) {
		return -1;
	}

	// Check if custom callback is set (for TDS-wrapped TLS handshake)
	if (impl->recv_callback) {
		MSSQL_TLS_DEBUG_LOG(3, "CustomBioRead: using custom callback, len=%d, timeout=%d", len,
							impl->current_timeout_ms);
		int ret =
			impl->recv_callback(reinterpret_cast<uint8_t *>(buf), static_cast<size_t>(len), impl->current_timeout_ms);
		if (ret < 0) {
			return -1;
		}
		if (ret == 0) {
			BIO_set_retry_read(bio);
			return -1;
		}
		return ret;
	}

	// Direct socket I/O
	int fd = impl->socket_fd;
	MSSQL_TLS_DEBUG_LOG(3, "CustomBioRead: direct socket fd=%d, len=%d", fd, len);

#ifdef _WIN32
	int ret = recv(fd, buf, len, 0);
	if (ret < 0) {
		int err = WSAGetLastError();
		if (err == WSAEWOULDBLOCK) {
			BIO_set_retry_read(bio);
		}
		return -1;
	}
#else
	ssize_t ret = recv(fd, buf, static_cast<size_t>(len), 0);
	if (ret < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
			BIO_set_retry_read(bio);
		}
		return -1;
	}
#endif

	return static_cast<int>(ret);
}

// Custom BIO ctrl callback (handles BIO_flush, etc.)
static long CustomBioCtrl(BIO *bio, int cmd, long num, void *ptr) {
	(void)bio;
	(void)num;
	(void)ptr;

	switch (cmd) {
	case BIO_CTRL_FLUSH:
		return 1;
	case BIO_CTRL_PUSH:
	case BIO_CTRL_POP:
		return 0;
	default:
		return 0;
	}
}

// Custom BIO create callback
static int CustomBioCreate(BIO *bio) {
	BIO_set_init(bio, 1);
	return 1;
}

// Custom BIO destroy callback
static int CustomBioDestroy(BIO *bio) {
	if (!bio) {
		return 0;
	}
	BIO_set_data(bio, nullptr);
	BIO_set_init(bio, 0);
	return 1;
}

// Create custom BIO method (thread-safe singleton)
static BIO_METHOD *GetCustomBioMethod() {
	static BIO_METHOD *method = nullptr;
	if (!method) {
		method = BIO_meth_new(BIO_TYPE_SOURCE_SINK | BIO_get_new_index(), "mssql_tds");
		BIO_meth_set_write(method, CustomBioWrite);
		BIO_meth_set_read(method, CustomBioRead);
		BIO_meth_set_ctrl(method, CustomBioCtrl);
		BIO_meth_set_create(method, CustomBioCreate);
		BIO_meth_set_destroy(method, CustomBioDestroy);
	}
	return method;
}

// =============================================================================
// Helper functions
// =============================================================================

// Helper to format OpenSSL error
static std::string FormatOpenSSLError() {
	unsigned long err = ERR_get_error();
	if (err == 0) {
		return "Unknown error";
	}
	char buf[256];
	ERR_error_string_n(err, buf, sizeof(buf));
	return std::string(buf);
}

// Clear OpenSSL error queue
static void ClearOpenSSLErrors() {
	ERR_clear_error();
}

// =============================================================================
// TlsImpl class implementation
// =============================================================================

TlsImpl::TlsImpl() : ctx_(new TlsImplContext()) {}

TlsImpl::~TlsImpl() {
	Close();
}

bool TlsImpl::Initialize() {
	MSSQL_TLS_DEBUG_LOG(1, "Initialize: starting TLS context initialization");

	if (ctx_->initialized) {
		return true;
	}

	ClearOpenSSLErrors();

	// Create SSL context for TLS client
	ctx_->ssl_ctx = SSL_CTX_new(TLS_client_method());
	if (!ctx_->ssl_ctx) {
		ctx_->last_error_code = 1;	// INIT_FAILED
		ctx_->last_error = "SSL_CTX_new failed: " + FormatOpenSSLError();
		MSSQL_TLS_DEBUG_LOG(1, "Initialize: FAILED - %s", ctx_->last_error.c_str());
		return false;
	}

	// Set minimum TLS version (1.2 for SQL Server)
	SSL_CTX_set_min_proto_version(ctx_->ssl_ctx, TLS1_2_VERSION);

	// Disable certificate verification (same as mbedTLS VERIFY_NONE)
	// This is appropriate for development/testing - production should verify
	SSL_CTX_set_verify(ctx_->ssl_ctx, SSL_VERIFY_NONE, nullptr);

	// Create SSL object
	ctx_->ssl = SSL_new(ctx_->ssl_ctx);
	if (!ctx_->ssl) {
		ctx_->last_error_code = 1;	// INIT_FAILED
		ctx_->last_error = "SSL_new failed: " + FormatOpenSSLError();
		MSSQL_TLS_DEBUG_LOG(1, "Initialize: FAILED - %s", ctx_->last_error.c_str());
		return false;
	}

	// Create custom BIO and attach to SSL
	ctx_->bio = BIO_new(GetCustomBioMethod());
	if (!ctx_->bio) {
		ctx_->last_error_code = 1;	// INIT_FAILED
		ctx_->last_error = "BIO_new failed: " + FormatOpenSSLError();
		MSSQL_TLS_DEBUG_LOG(1, "Initialize: FAILED - %s", ctx_->last_error.c_str());
		return false;
	}

	// Store context pointer in BIO for callbacks
	BIO_set_data(ctx_->bio, ctx_.get());

	// Attach BIO to SSL (SSL takes ownership)
	SSL_set_bio(ctx_->ssl, ctx_->bio, ctx_->bio);

	ctx_->initialized = true;
	MSSQL_TLS_DEBUG_LOG(1, "Initialize: success");
	return true;
}

bool TlsImpl::WrapSocket(int socket_fd, const std::string &hostname) {
	MSSQL_TLS_DEBUG_LOG(1, "WrapSocket: fd=%d, hostname=%s", socket_fd, hostname.empty() ? "(none)" : hostname.c_str());

	if (!ctx_->initialized) {
		ctx_->last_error_code = 6;	// NOT_INITIALIZED
		ctx_->last_error = "Call Initialize() first";
		return false;
	}

	ctx_->socket_fd = socket_fd;

	// Set hostname for SNI (Server Name Indication) if provided
	if (!hostname.empty()) {
		if (SSL_set_tlsext_host_name(ctx_->ssl, hostname.c_str()) != 1) {
			ctx_->last_error_code = 1;	// INIT_FAILED
			ctx_->last_error = "Failed to set hostname for SNI: " + FormatOpenSSLError();
			MSSQL_TLS_DEBUG_LOG(1, "WrapSocket: FAILED to set hostname - %s", ctx_->last_error.c_str());
			return false;
		}
		MSSQL_TLS_DEBUG_LOG(2, "WrapSocket: SNI hostname set to '%s'", hostname.c_str());
	}

	return true;
}

bool TlsImpl::Handshake(int timeout_ms) {
	MSSQL_TLS_DEBUG_LOG(1, "Handshake: starting (timeout=%dms)", timeout_ms);

	if (!ctx_->initialized) {
		ctx_->last_error_code = 6;	// NOT_INITIALIZED
		ctx_->last_error = "Not initialized";
		return false;
	}

	if (ctx_->socket_fd < 0) {
		ctx_->last_error_code = 6;	// NOT_INITIALIZED
		ctx_->last_error = "Socket not wrapped";
		return false;
	}

	ClearOpenSSLErrors();
	auto start = std::chrono::steady_clock::now();

	// Store timeout for BIO callbacks to use
	ctx_->current_timeout_ms = timeout_ms;

	// Set SSL to client mode
	SSL_set_connect_state(ctx_->ssl);

	int ret;
	while ((ret = SSL_do_handshake(ctx_->ssl)) != 1) {
		int ssl_error = SSL_get_error(ctx_->ssl, ret);

		if (ssl_error == SSL_ERROR_WANT_READ || ssl_error == SSL_ERROR_WANT_WRITE) {
			// Need to wait for socket readiness
			auto elapsed =
				std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
			if (elapsed >= timeout_ms) {
				ctx_->last_error_code = 3;	// HANDSHAKE_TIMEOUT
				ctx_->last_error = "Timeout after " + std::to_string(elapsed) + "ms";
				MSSQL_TLS_DEBUG_LOG(1, "Handshake: TIMEOUT");
				return false;
			}

			// Wait for socket if using direct I/O (not custom callbacks)
			if (!ctx_->send_callback && !ctx_->recv_callback) {
				int remaining_ms = static_cast<int>(timeout_ms - elapsed);
#ifdef _WIN32
				fd_set fds;
				FD_ZERO(&fds);
				FD_SET(ctx_->socket_fd, &fds);
				struct timeval tv;
				tv.tv_sec = remaining_ms / 1000;
				tv.tv_usec = (remaining_ms % 1000) * 1000;
				if (ssl_error == SSL_ERROR_WANT_READ) {
					select(ctx_->socket_fd + 1, &fds, nullptr, nullptr, &tv);
				} else {
					select(ctx_->socket_fd + 1, nullptr, &fds, nullptr, &tv);
				}
#else
				struct pollfd pfd;
				pfd.fd = ctx_->socket_fd;
				pfd.events = (ssl_error == SSL_ERROR_WANT_READ) ? POLLIN : POLLOUT;
				pfd.revents = 0;
				poll(&pfd, 1, remaining_ms);
#endif
			}
			continue;
		}

		// Other error
		ctx_->last_error_code = 2;	// HANDSHAKE_FAILED
		ctx_->last_error = "Handshake failed: " + FormatOpenSSLError();
		MSSQL_TLS_DEBUG_LOG(1, "Handshake: FAILED - %s (ssl_error=%d)", ctx_->last_error.c_str(), ssl_error);
		return false;
	}

	ctx_->handshake_complete = true;

	const char *cipher = SSL_get_cipher(ctx_->ssl);
	const char *version = SSL_get_version(ctx_->ssl);
	MSSQL_TLS_DEBUG_LOG(1, "Handshake: SUCCESS - %s, %s", version ? version : "unknown", cipher ? cipher : "unknown");

	return true;
}

ssize_t TlsImpl::Send(const uint8_t *data, size_t length) {
	if (!ctx_->handshake_complete) {
		ctx_->last_error_code = 6;	// NOT_INITIALIZED
		ctx_->last_error = "Handshake not complete";
		return -1;
	}

	ClearOpenSSLErrors();
	size_t total_sent = 0;

	while (total_sent < length) {
		int to_send = static_cast<int>(length - total_sent);
		int ret = SSL_write(ctx_->ssl, data + total_sent, to_send);

		if (ret > 0) {
			total_sent += static_cast<size_t>(ret);
		} else {
			int ssl_error = SSL_get_error(ctx_->ssl, ret);
			if (ssl_error == SSL_ERROR_WANT_READ || ssl_error == SSL_ERROR_WANT_WRITE) {
				continue;
			} else if (ssl_error == SSL_ERROR_ZERO_RETURN) {
				ctx_->last_error_code = 7;	// PEER_CLOSED
				ctx_->last_error = "Peer closed connection";
				return -1;
			} else {
				ctx_->last_error_code = 4;	// SEND_FAILED
				ctx_->last_error = "Send failed: " + FormatOpenSSLError();
				return -1;
			}
		}
	}

	return static_cast<ssize_t>(total_sent);
}

ssize_t TlsImpl::Receive(uint8_t *buffer, size_t max_length, int timeout_ms) {
	if (!ctx_->handshake_complete) {
		ctx_->last_error_code = 6;	// NOT_INITIALIZED
		ctx_->last_error = "Handshake not complete";
		return -1;
	}

	// If timeout specified, wait for data first
	if (timeout_ms > 0) {
#ifdef _WIN32
		fd_set read_fds;
		FD_ZERO(&read_fds);
		FD_SET(ctx_->socket_fd, &read_fds);
		struct timeval tv;
		tv.tv_sec = timeout_ms / 1000;
		tv.tv_usec = (timeout_ms % 1000) * 1000;
		int ready = select(ctx_->socket_fd + 1, &read_fds, nullptr, nullptr, &tv);
		if (ready <= 0) {
			return 0;
		}
#else
		struct pollfd pfd;
		pfd.fd = ctx_->socket_fd;
		pfd.events = POLLIN;
		pfd.revents = 0;
		int ready = poll(&pfd, 1, timeout_ms);
		if (ready <= 0) {
			return 0;
		}
#endif
	}

	ClearOpenSSLErrors();
	int ret = SSL_read(ctx_->ssl, buffer, static_cast<int>(max_length));

	if (ret > 0) {
		return ret;
	}

	int ssl_error = SSL_get_error(ctx_->ssl, ret);
	if (ssl_error == SSL_ERROR_ZERO_RETURN) {
		ctx_->last_error_code = 7;	// PEER_CLOSED
		ctx_->last_error = "Connection closed by peer";
		return 0;
	} else if (ssl_error == SSL_ERROR_WANT_READ || ssl_error == SSL_ERROR_WANT_WRITE) {
		return 0;
	} else if (ssl_error == SSL_ERROR_SYSCALL) {
		// System call error - get the actual error
#ifdef _WIN32
		int sys_err = WSAGetLastError();
		if (sys_err == 0 && ret == 0) {
			// EOF - peer closed connection unexpectedly
			ctx_->last_error_code = 7;	// PEER_CLOSED
			ctx_->last_error = "Connection reset by peer (unexpected EOF during TLS read)";
			return 0;
		}
		ctx_->last_error_code = 5;	// RECV_FAILED
		ctx_->last_error = "Receive failed: syscall error " + std::to_string(sys_err);
#else
		int sys_err = errno;
		if (sys_err == 0 && ret == 0) {
			// EOF - peer closed connection unexpectedly
			ctx_->last_error_code = 7;	// PEER_CLOSED
			ctx_->last_error = "Connection reset by peer (unexpected EOF during TLS read)";
			return 0;
		}
		ctx_->last_error_code = 5;	// RECV_FAILED
		ctx_->last_error = "Receive failed (SSL_ERROR_SYSCALL, ret=" + std::to_string(ret) + "): " +
		                   std::string(strerror(sys_err)) + " (errno=" + std::to_string(sys_err) + ")";
#endif
		return -1;
	} else {
		ctx_->last_error_code = 5;	// RECV_FAILED
		ctx_->last_error = "Receive failed (ssl_error=" + std::to_string(ssl_error) + ", ret=" +
		                   std::to_string(ret) + "): " + FormatOpenSSLError();
		return -1;
	}
}

void TlsImpl::Close() {
	MSSQL_TLS_DEBUG_LOG(1, "Close: closing TLS connection");

	if (ctx_->handshake_complete && ctx_->ssl) {
		SSL_shutdown(ctx_->ssl);
	}

	// Clean up SSL objects
	if (ctx_->ssl) {
		SSL_free(ctx_->ssl);  // This also frees the BIO
		ctx_->ssl = nullptr;
		ctx_->bio = nullptr;
	}
	if (ctx_->ssl_ctx) {
		SSL_CTX_free(ctx_->ssl_ctx);
		ctx_->ssl_ctx = nullptr;
	}

	// Reset state for potential reuse
	ctx_->initialized = false;
	ctx_->handshake_complete = false;
	ctx_->socket_fd = -1;
	ctx_->last_error.clear();
	ctx_->last_error_code = 0;
}

void TlsImpl::SetBioCallbacks(TlsSendCallback send_cb, TlsRecvCallback recv_cb) {
	ctx_->send_callback = std::move(send_cb);
	ctx_->recv_callback = std::move(recv_cb);
	MSSQL_TLS_DEBUG_LOG(2, "SetBioCallbacks: custom callbacks set");
}

void TlsImpl::ClearBioCallbacks() {
	ctx_->send_callback = nullptr;
	ctx_->recv_callback = nullptr;
	MSSQL_TLS_DEBUG_LOG(2, "ClearBioCallbacks: reverted to direct socket I/O");
}

bool TlsImpl::IsInitialized() const {
	return ctx_->initialized;
}

const std::string &TlsImpl::GetLastError() const {
	return ctx_->last_error;
}

int TlsImpl::GetLastErrorCode() const {
	return ctx_->last_error_code;
}

std::string TlsImpl::GetCipherSuite() const {
	if (!ctx_->handshake_complete || !ctx_->ssl) {
		return "";
	}
	const char *suite = SSL_get_cipher(ctx_->ssl);
	return suite ? suite : "";
}

std::string TlsImpl::GetTlsVersion() const {
	if (!ctx_->handshake_complete || !ctx_->ssl) {
		return "";
	}
	const char *version = SSL_get_version(ctx_->ssl);
	return version ? version : "";
}

}  // namespace tds
}  // namespace duckdb
