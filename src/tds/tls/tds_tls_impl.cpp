//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// tds_tls_impl.cpp
//
// TLS implementation using mbedTLS. This file is compiled into a static
// library (mssql_tls) which is linked into the loadable extension with
// all mbedTLS symbols hidden to avoid conflicts with DuckDB's bundled mbedTLS.
//===----------------------------------------------------------------------===//

#include "tds/tls/tds_tls_impl.hpp"

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/debug.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>

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

// Internal implementation structure
struct TlsImplContext {
	mbedtls_ssl_context ssl;
	mbedtls_ssl_config conf;
	mbedtls_ctr_drbg_context ctr_drbg;
	mbedtls_entropy_context entropy;
	mbedtls_net_context net_ctx;

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
		: initialized(false), handshake_complete(false), socket_fd(-1), last_error_code(0), current_timeout_ms(30000) {
		mbedtls_ssl_init(&ssl);
		mbedtls_ssl_config_init(&conf);
		mbedtls_ctr_drbg_init(&ctr_drbg);
		mbedtls_entropy_init(&entropy);
		mbedtls_net_init(&net_ctx);
	}

	~TlsImplContext() {
		mbedtls_ssl_free(&ssl);
		mbedtls_ssl_config_free(&conf);
		mbedtls_net_free(&net_ctx);
		mbedtls_ctr_drbg_free(&ctr_drbg);
		mbedtls_entropy_free(&entropy);
	}
};

// Helper to format mbedTLS error
static std::string FormatMbedTlsError(int ret) {
	char buf[256];
	mbedtls_strerror(ret, buf, sizeof(buf));
	return std::string(buf);
}

// mbedTLS debug callback
static void MbedTlsDebugCallback(void *ctx, int level, const char *file, int line, const char *str) {
	(void)ctx;								  // unused
	if (GetMssqlDebugLevel() >= level + 1) {  // level is 0-4 in mbedTLS, map to our 1-5
		// Remove trailing newline from str
		size_t len = strlen(str);
		if (len > 0 && str[len - 1] == '\n') {
			char *buf = new char[len];
			memcpy(buf, str, len - 1);
			buf[len - 1] = '\0';
			MSSQL_TLS_DEBUG_LOG(level + 1, "[mbedTLS %d] %s:%04d: %s", level, file, line, buf);
			delete[] buf;
		} else {
			MSSQL_TLS_DEBUG_LOG(level + 1, "[mbedTLS %d] %s:%04d: %s", level, file, line, str);
		}
	}
}

// Custom I/O callbacks for mbedTLS
// These check if custom callbacks are set (for TDS-wrapped TLS) and use them,
// otherwise fall back to direct socket I/O
static int BioSend(void *ctx, const unsigned char *buf, size_t len) {
	auto *impl = static_cast<TlsImplContext *>(ctx);

	// Use custom callback if set (for TDS-wrapped TLS handshake)
	if (impl->send_callback) {
		MSSQL_TLS_DEBUG_LOG(3, "BioSend: using custom callback, len=%zu", len);
		int ret = impl->send_callback(reinterpret_cast<const uint8_t *>(buf), len);
		if (ret < 0) {
			return MBEDTLS_ERR_NET_SEND_FAILED;
		}
		if (ret == 0) {
			return MBEDTLS_ERR_SSL_WANT_WRITE;
		}
		return ret;
	}

	// Direct socket I/O
	int fd = impl->socket_fd;
	MSSQL_TLS_DEBUG_LOG(3, "BioSend: direct socket fd=%d, len=%zu", fd, len);

#ifdef _WIN32
	int ret = send(fd, reinterpret_cast<const char *>(buf), static_cast<int>(len), 0);
	if (ret < 0) {
		int err = WSAGetLastError();
		if (err == WSAEWOULDBLOCK) {
			return MBEDTLS_ERR_SSL_WANT_WRITE;
		}
		return MBEDTLS_ERR_NET_SEND_FAILED;
	}
#else
	ssize_t ret = send(fd, buf, len, 0);
	if (ret < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
			return MBEDTLS_ERR_SSL_WANT_WRITE;
		}
		return MBEDTLS_ERR_NET_SEND_FAILED;
	}
#endif

	return static_cast<int>(ret);
}

static int BioRecv(void *ctx, unsigned char *buf, size_t len) {
	auto *impl = static_cast<TlsImplContext *>(ctx);

	// Use custom callback if set (for TDS-wrapped TLS handshake)
	if (impl->recv_callback) {
		MSSQL_TLS_DEBUG_LOG(3, "BioRecv: using custom callback, len=%zu", len);
		int ret = impl->recv_callback(reinterpret_cast<uint8_t *>(buf), len, 0);
		if (ret < 0) {
			return MBEDTLS_ERR_NET_RECV_FAILED;
		}
		if (ret == 0) {
			return MBEDTLS_ERR_SSL_WANT_READ;
		}
		return ret;
	}

	// Direct socket I/O
	int fd = impl->socket_fd;
	MSSQL_TLS_DEBUG_LOG(3, "BioRecv: direct socket fd=%d, len=%zu", fd, len);

#ifdef _WIN32
	int ret = recv(fd, reinterpret_cast<char *>(buf), static_cast<int>(len), 0);
	if (ret < 0) {
		int err = WSAGetLastError();
		if (err == WSAEWOULDBLOCK) {
			return MBEDTLS_ERR_SSL_WANT_READ;
		}
		return MBEDTLS_ERR_NET_RECV_FAILED;
	}
#else
	ssize_t ret = recv(fd, buf, len, 0);
	if (ret < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
			return MBEDTLS_ERR_SSL_WANT_READ;
		}
		return MBEDTLS_ERR_NET_RECV_FAILED;
	}
#endif

	return static_cast<int>(ret);
}

static int BioRecvTimeout(void *ctx, unsigned char *buf, size_t len, uint32_t timeout) {
	auto *impl = static_cast<TlsImplContext *>(ctx);

	// Use custom callback if set (for TDS-wrapped TLS handshake)
	if (impl->recv_callback) {
		MSSQL_TLS_DEBUG_LOG(3, "BioRecvTimeout: using custom callback, len=%zu, timeout=%u", len, timeout);
		int ret = impl->recv_callback(reinterpret_cast<uint8_t *>(buf), len, static_cast<int>(timeout));
		if (ret < 0) {
			return MBEDTLS_ERR_NET_RECV_FAILED;
		}
		if (ret == 0) {
			return MBEDTLS_ERR_SSL_TIMEOUT;
		}
		return ret;
	}

	// Direct socket I/O with poll
	int fd = impl->socket_fd;
	MSSQL_TLS_DEBUG_LOG(3, "BioRecvTimeout: direct socket fd=%d, len=%zu, timeout=%u", fd, len, timeout);

#ifdef _WIN32
	fd_set read_fds;
	FD_ZERO(&read_fds);
	FD_SET(fd, &read_fds);
	struct timeval tv;
	tv.tv_sec = timeout / 1000;
	tv.tv_usec = (timeout % 1000) * 1000;
	int ready = select(fd + 1, &read_fds, nullptr, nullptr, &tv);
	if (ready < 0) {
		return MBEDTLS_ERR_NET_RECV_FAILED;
	}
	if (ready == 0) {
		return MBEDTLS_ERR_SSL_TIMEOUT;
	}
#else
	struct pollfd pfd;
	pfd.fd = fd;
	pfd.events = POLLIN;
	pfd.revents = 0;
	int ready = poll(&pfd, 1, static_cast<int>(timeout));
	if (ready < 0) {
		if (errno == EINTR) {
			return MBEDTLS_ERR_SSL_WANT_READ;
		}
		return MBEDTLS_ERR_NET_RECV_FAILED;
	}
	if (ready == 0) {
		return MBEDTLS_ERR_SSL_TIMEOUT;
	}
#endif

	return BioRecv(ctx, buf, len);
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

	int ret;

	// Seed the random number generator
	const char *pers = "duckdb_mssql_tls";
	ret = mbedtls_ctr_drbg_seed(&ctx_->ctr_drbg, mbedtls_entropy_func, &ctx_->entropy,
								reinterpret_cast<const unsigned char *>(pers), strlen(pers));
	if (ret != 0) {
		ctx_->last_error_code = 1;	// INIT_FAILED
		ctx_->last_error = "CTR DRBG seed failed: " + FormatMbedTlsError(ret);
		return false;
	}

	// Set up SSL config for client mode
	ret = mbedtls_ssl_config_defaults(&ctx_->conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
									  MBEDTLS_SSL_PRESET_DEFAULT);
	if (ret != 0) {
		ctx_->last_error_code = 1;	// INIT_FAILED
		ctx_->last_error = "SSL config defaults failed: " + FormatMbedTlsError(ret);
		return false;
	}

	// Configure RNG
	mbedtls_ssl_conf_rng(&ctx_->conf, mbedtls_ctr_drbg_random, &ctx_->ctr_drbg);

	// Enable debug output if MSSQL_DEBUG is set high enough
	if (GetMssqlDebugLevel() >= 3) {
		mbedtls_ssl_conf_dbg(&ctx_->conf, MbedTlsDebugCallback, nullptr);
		mbedtls_debug_set_threshold(4);	 // 0-4, 4 is most verbose
	}

	// Trust server certificate by default (VERIFY_NONE)
	mbedtls_ssl_conf_authmode(&ctx_->conf, MBEDTLS_SSL_VERIFY_NONE);
	// Force TLS 1.2 (SQL Server prefers this)
	mbedtls_ssl_conf_min_tls_version(&ctx_->conf, MBEDTLS_SSL_VERSION_TLS1_2);
	mbedtls_ssl_conf_max_tls_version(&ctx_->conf, MBEDTLS_SSL_VERSION_TLS1_2);

	// Set read timeout for handshake (in milliseconds)
	// This timeout is passed to BioRecvTimeout callback
	mbedtls_ssl_conf_read_timeout(&ctx_->conf, 30000);

	// Setup SSL context with config
	ret = mbedtls_ssl_setup(&ctx_->ssl, &ctx_->conf);
	if (ret != 0) {
		ctx_->last_error_code = 1;	// INIT_FAILED
		ctx_->last_error = "SSL setup failed: " + FormatMbedTlsError(ret);
		return false;
	}

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
		int ret = mbedtls_ssl_set_hostname(&ctx_->ssl, hostname.c_str());
		if (ret != 0) {
			ctx_->last_error_code = 1;	// INIT_FAILED
			ctx_->last_error = "Failed to set hostname for SNI: " + FormatMbedTlsError(ret);
			MSSQL_TLS_DEBUG_LOG(1, "WrapSocket: FAILED to set hostname - %s", ctx_->last_error.c_str());
			return false;
		}
		MSSQL_TLS_DEBUG_LOG(2, "WrapSocket: SNI hostname set to '%s'", hostname.c_str());
	}

	mbedtls_ssl_set_bio(&ctx_->ssl, ctx_.get(), BioSend, BioRecv, BioRecvTimeout);
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

	auto start = std::chrono::steady_clock::now();

	int ret;
	while ((ret = mbedtls_ssl_handshake(&ctx_->ssl)) != 0) {
		if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
			ctx_->last_error_code = 2;	// HANDSHAKE_FAILED
			ctx_->last_error = "Handshake failed: " + FormatMbedTlsError(ret);
			MSSQL_TLS_DEBUG_LOG(1, "Handshake: FAILED - %s", ctx_->last_error.c_str());
			return false;
		}

		auto elapsed =
			std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
		if (elapsed >= timeout_ms) {
			ctx_->last_error_code = 3;	// HANDSHAKE_TIMEOUT
			ctx_->last_error = "Timeout after " + std::to_string(elapsed) + "ms";
			MSSQL_TLS_DEBUG_LOG(1, "Handshake: TIMEOUT");
			return false;
		}
	}

	ctx_->handshake_complete = true;

	const char *cipher = mbedtls_ssl_get_ciphersuite(&ctx_->ssl);
	const char *version = mbedtls_ssl_get_version(&ctx_->ssl);
	MSSQL_TLS_DEBUG_LOG(1, "Handshake: SUCCESS - %s, %s", version ? version : "unknown", cipher ? cipher : "unknown");

	return true;
}

ssize_t TlsImpl::Send(const uint8_t *data, size_t length) {
	if (!ctx_->handshake_complete) {
		ctx_->last_error_code = 6;	// NOT_INITIALIZED
		ctx_->last_error = "Handshake not complete";
		return -1;
	}

	size_t total_sent = 0;
	while (total_sent < length) {
		int ret = mbedtls_ssl_write(&ctx_->ssl, data + total_sent, length - total_sent);

		if (ret > 0) {
			total_sent += ret;
		} else if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
			continue;
		} else if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
			ctx_->last_error_code = 7;	// PEER_CLOSED
			ctx_->last_error = "Peer closed connection";
			return -1;
		} else {
			ctx_->last_error_code = 4;	// SEND_FAILED
			ctx_->last_error = "Send failed: " + FormatMbedTlsError(ret);
			return -1;
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

	int ret = mbedtls_ssl_read(&ctx_->ssl, buffer, max_length);

	if (ret > 0) {
		return ret;
	} else if (ret == 0 || ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
		ctx_->last_error_code = 7;	// PEER_CLOSED
		ctx_->last_error = "Connection closed by peer";
		return 0;
	} else if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
		return 0;
	} else {
		ctx_->last_error_code = 5;	// RECV_FAILED
		ctx_->last_error = "Receive failed: " + FormatMbedTlsError(ret);
		return -1;
	}
}

void TlsImpl::Close() {
	MSSQL_TLS_DEBUG_LOG(1, "Close: closing TLS connection");

	if (ctx_->handshake_complete) {
		mbedtls_ssl_close_notify(&ctx_->ssl);
	}

	// Reset for potential reuse
	ctx_->initialized = false;
	ctx_->handshake_complete = false;
	ctx_->socket_fd = -1;
	ctx_->last_error.clear();
	ctx_->last_error_code = 0;

	// Reinitialize mbedTLS structures
	mbedtls_ssl_free(&ctx_->ssl);
	mbedtls_ssl_config_free(&ctx_->conf);
	mbedtls_net_free(&ctx_->net_ctx);
	mbedtls_ctr_drbg_free(&ctx_->ctr_drbg);
	mbedtls_entropy_free(&ctx_->entropy);

	mbedtls_ssl_init(&ctx_->ssl);
	mbedtls_ssl_config_init(&ctx_->conf);
	mbedtls_ctr_drbg_init(&ctx_->ctr_drbg);
	mbedtls_entropy_init(&ctx_->entropy);
	mbedtls_net_init(&ctx_->net_ctx);
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
	if (!ctx_->handshake_complete) {
		return "";
	}
	const char *suite = mbedtls_ssl_get_ciphersuite(&ctx_->ssl);
	return suite ? suite : "";
}

std::string TlsImpl::GetTlsVersion() const {
	if (!ctx_->handshake_complete) {
		return "";
	}
	const char *version = mbedtls_ssl_get_version(&ctx_->ssl);
	return version ? version : "";
}

}  // namespace tds
}  // namespace duckdb
