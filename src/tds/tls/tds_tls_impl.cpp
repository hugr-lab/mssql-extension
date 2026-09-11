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
#include <openssl/x509.h>
#include <openssl/x509_vfy.h>
#include <openssl/x509v3.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>

// After winsock2.h (the blank line keeps clang-format from sorting it first):
// wincrypt.h is where the ROOT / CA store enumeration for spec 074 lives.
#include <wincrypt.h>
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

#ifdef __APPLE__
// After the OpenSSL headers, as cpp-httplib orders them.
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#endif

namespace duckdb {
namespace tds {

// Debug logging controlled by MSSQL_DEBUG environment variable
static int GetMssqlDebugLevel() {
	static const int level = []() {
		const char *env = std::getenv("MSSQL_DEBUG");
		return env ? std::atoi(env) : 0;
	}();
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

	// Spec 074: the certificate policy Initialize() was given, and the name the
	// certificate is matched against (resolved in WrapSocket, kept for messages).
	bool verify_certificate;
	std::string expected_host;

	TlsImplContext()
		: ssl_ctx(nullptr),
		  ssl(nullptr),
		  bio(nullptr),
		  initialized(false),
		  handshake_complete(false),
		  socket_fd(-1),
		  last_error_code(0),
		  current_timeout_ms(30000),
		  verify_certificate(true) {}

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
// Platform trust store (spec 074 D4)
// =============================================================================
// The routine cpp-httplib uses for the Azure OAuth client, so the TDS tunnel
// trusts what the token request trusts: the Windows ROOT + CA system stores,
// the macOS keychain trust settings, and OpenSSL's own default paths -- on
// macOS in ADDITION to the keychain, so SSL_CERT_FILE / SSL_CERT_DIR are
// honoured there too (httplib skips the paths once the keychain yields
// anything). Nothing is reported from here: an empty store is not an error at
// this point, and the handshake's "unable to get local issuer certificate"
// names the problem better than a failure here could.

static void AddDerCertificate(X509_STORE *store, const unsigned char *der, long der_len) {
	const unsigned char *p = der;
	X509 *x509 = d2i_X509(nullptr, &p, der_len);
	if (!x509) {
		ERR_clear_error();
		return;
	}
	// The same root can sit in more than one store or domain. OpenSSL 1.x
	// reported the duplicate as an error and 3.x accepts it; neither matters.
	X509_STORE_add_cert(store, x509);
	ERR_clear_error();
	X509_free(x509);
}

static void LoadPlatformTrustStore(SSL_CTX *ssl_ctx) {
	X509_STORE *store = SSL_CTX_get_cert_store(ssl_ctx);
	if (!store) {
		return;
	}
#ifdef _WIN32
	static const wchar_t *store_names[] = {L"ROOT", L"CA"};
	for (const wchar_t *store_name : store_names) {
		HCERTSTORE system_store = CertOpenSystemStoreW(0, store_name);
		if (!system_store) {
			continue;
		}
		PCCERT_CONTEXT cert = nullptr;
		while ((cert = CertEnumCertificatesInStore(system_store, cert)) != nullptr) {
			AddDerCertificate(store, cert->pbCertEncoded, static_cast<long>(cert->cbCertEncoded));
		}
		CertCloseStore(system_store, 0);
	}
#endif
#ifdef __APPLE__
	const SecTrustSettingsDomain domains[] = {kSecTrustSettingsDomainSystem, kSecTrustSettingsDomainAdmin,
											  kSecTrustSettingsDomainUser};
	for (SecTrustSettingsDomain domain : domains) {
		CFArrayRef certs = nullptr;
		if (SecTrustSettingsCopyCertificates(domain, &certs) != errSecSuccess || !certs) {
			if (certs) {
				CFRelease(certs);
			}
			continue;
		}
		const CFIndex count = CFArrayGetCount(certs);
		for (CFIndex i = 0; i < count; i++) {
			auto cert = reinterpret_cast<SecCertificateRef>(const_cast<void *>(CFArrayGetValueAtIndex(certs, i)));
			CFDataRef der = SecCertificateCopyData(cert);
			if (!der) {
				continue;
			}
			AddDerCertificate(store, CFDataGetBytePtr(der), static_cast<long>(CFDataGetLength(der)));
			CFRelease(der);
		}
		CFRelease(certs);
	}
#endif
	// OpenSSL's own paths on every platform: the OPENSSLDIR the library was
	// built with (/etc/ssl on the vcpkg build), or SSL_CERT_FILE / SSL_CERT_DIR.
	if (SSL_CTX_set_default_verify_paths(ssl_ctx) != 1) {
		ERR_clear_error();
	}
}

// An expected name that is an IP literal is matched against iPAddress SANs
// (X509_VERIFY_PARAM_set1_ip_asc), a DNS name against dNSName / CN
// (X509_VERIFY_PARAM_set1_host). OpenSSL does not decide this by itself, but
// its own address parser is the one to ask -- not inet_pton, which MinGW gates
// behind _WIN32_WINNT.
static bool IsIpLiteral(const std::string &name) {
	ASN1_OCTET_STRING *ip = a2i_IPADDRESS(name.c_str());
	if (!ip) {
		ERR_clear_error();
		return false;
	}
	ASN1_OCTET_STRING_free(ip);
	return true;
}

// =============================================================================
// TlsImpl class implementation
// =============================================================================

TlsImpl::TlsImpl() : ctx_(new TlsImplContext()) {}

TlsImpl::~TlsImpl() noexcept {
	try {
		Close();
	} catch (const std::exception &e) {
		// PR #118 review M1: debug-gated stderr surfaces the swallow.
		MSSQL_TLS_DEBUG_LOG(1, "~TlsImpl: swallowed exception during Close: %s", e.what());
	} catch (...) {
		MSSQL_TLS_DEBUG_LOG(1, "~TlsImpl: swallowed unknown exception during Close");
	}
}

bool TlsImpl::Initialize(const TlsOptions &options) {
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

	// Spec 074 D2: TrustServerCertificate=false verifies the chain against the
	// platform store and (in WrapSocket) the name; =true accepts any certificate.
	ctx_->verify_certificate = options.verify_certificate;
	ctx_->expected_host = options.expected_host;
	if (options.verify_certificate) {
		SSL_CTX_set_verify(ctx_->ssl_ctx, SSL_VERIFY_PEER, nullptr);
		LoadPlatformTrustStore(ctx_->ssl_ctx);
	} else {
		SSL_CTX_set_verify(ctx_->ssl_ctx, SSL_VERIFY_NONE, nullptr);
	}

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

	// Spec 074 D3/D5: the name the certificate must carry. HostNameInCertificate
	// if given, else the SNI name, which is the host dialled (the routed host
	// after a hop). Matching itself is OpenSSL's: SAN dNSName, CN fallback under
	// its rules, one-label wildcards only.
	if (ctx_->verify_certificate) {
		const std::string expected = ctx_->expected_host.empty() ? hostname : ctx_->expected_host;
		if (expected.empty()) {
			ctx_->last_error_code = 1;	// INIT_FAILED
			ctx_->last_error = "certificate verification needs a host name to match and none was given";
			return false;
		}
		X509_VERIFY_PARAM *param = SSL_get0_param(ctx_->ssl);
		int ok;
		if (IsIpLiteral(expected)) {
			ok = X509_VERIFY_PARAM_set1_ip_asc(param, expected.c_str());
		} else {
			X509_VERIFY_PARAM_set_hostflags(param, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
			ok = X509_VERIFY_PARAM_set1_host(param, expected.c_str(), 0);
		}
		if (ok != 1) {
			ctx_->last_error_code = 1;	// INIT_FAILED
			ctx_->last_error =
				"Failed to set the expected certificate name '" + expected + "': " + FormatOpenSSLError();
			return false;
		}
		ctx_->expected_host = expected;
		MSSQL_TLS_DEBUG_LOG(2, "WrapSocket: certificate must be for '%s'", expected.c_str());
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

		// Spec 074 D2: a rejected certificate is reported with OpenSSL's own reason
		// -- "self-signed certificate", "hostname mismatch", "unable to get local
		// issuer certificate", "certificate has expired" -- so it can be searched for.
		if (ctx_->verify_certificate) {
			const long verify_result = SSL_get_verify_result(ctx_->ssl);
			if (verify_result != X509_V_OK) {
				ctx_->last_error_code = 10;	 // CERT_VERIFY_FAILED
				ctx_->last_error = "certificate verification failed for " + ctx_->expected_host + ": " +
								   X509_verify_cert_error_string(verify_result);
				ClearOpenSSLErrors();
				MSSQL_TLS_DEBUG_LOG(1, "Handshake: FAILED - %s", ctx_->last_error.c_str());
				return false;
			}
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
	MSSQL_TLS_DEBUG_LOG(1, "Handshake: SUCCESS - %s, %s, peer certificate %s", version ? version : "unknown",
						cipher ? cipher : "unknown",
						ctx_->verify_certificate ? "verified" : "not verified (TrustServerCertificate)");

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

	// Set SO_RCVTIMEO on the underlying socket so that SSL_read() will also
	// time out when waiting for data.  poll()/select() alone is not sufficient
	// because poll() may return "ready" for TLS protocol data while the actual
	// application-level response has not yet arrived, causing SSL_read() to
	// block indefinitely.
	if (timeout_ms > 0) {
#ifdef _WIN32
		DWORD tv = static_cast<DWORD>(timeout_ms);
		setsockopt(ctx_->socket_fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&tv), sizeof(tv));
#else
		struct timeval tv;
		tv.tv_sec = timeout_ms / 1000;
		tv.tv_usec = (timeout_ms % 1000) * 1000;
		setsockopt(ctx_->socket_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
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
		if (sys_err == WSAETIMEDOUT || sys_err == WSAEWOULDBLOCK) {
			// SO_RCVTIMEO fired inside SSL_read — treat as timeout
			return 0;
		}
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
		if (sys_err == EAGAIN || sys_err == EWOULDBLOCK) {
			// SO_RCVTIMEO fired inside SSL_read — treat as timeout
			return 0;
		}
		if (sys_err == 0 && ret == 0) {
			// EOF - peer closed connection unexpectedly
			ctx_->last_error_code = 7;	// PEER_CLOSED
			ctx_->last_error = "Connection reset by peer (unexpected EOF during TLS read)";
			return 0;
		}
		ctx_->last_error_code = 5;	// RECV_FAILED
		ctx_->last_error = "Receive failed (SSL_ERROR_SYSCALL, ret=" + std::to_string(ret) +
						   "): " + std::string(strerror(sys_err)) + " (errno=" + std::to_string(sys_err) + ")";
#endif
		return -1;
	} else {
		ctx_->last_error_code = 5;	// RECV_FAILED
		ctx_->last_error = "Receive failed (ssl_error=" + std::to_string(ssl_error) + ", ret=" + std::to_string(ret) +
						   "): " + FormatOpenSSLError();
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
