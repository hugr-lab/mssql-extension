// test/cpp/test_tls_verification.cpp
//
// Spec 074: what the TLS layer does with the server's certificate, without a
// SQL Server. An OpenSSL server runs in-process on a loopback socket with a
// certificate generated at runtime; the client is the extension's own
// TlsTdsContext, driven exactly as TdsSocket::EnableTls drives it minus the
// TDS framing (TlsImpl does direct socket I/O when no callbacks are set). The
// cases are the cells of D2 x D3: verify or trust, and which name is expected.
//
// Part of STANDALONE_TEST_SOURCES (`make test-cpp`), which CI runs.

#ifdef _WIN32
#include <cstdio>
int main() {
	std::printf("test_tls_verification: not built on Windows (CI runs the standalone list on POSIX)\n");
	return 0;
}
#else

#include <openssl/bn.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

#include "tds/tls/tds_tls_context.hpp"

using duckdb::tds::TlsErrorCode;
using duckdb::tds::TlsOptions;
using duckdb::tds::TlsTdsContext;

static int g_failures = 0;

static void Check(bool ok, const std::string &what) {
	if (ok) {
		return;
	}
	std::cerr << "FAIL: " << what << std::endl;
	g_failures++;
}

static bool Contains(const std::string &haystack, const std::string &needle) {
	return haystack.find(needle) != std::string::npos;
}

//------------------------------------------------------------------------------
// A self-signed certificate for "sql.test.invalid" and 127.0.0.1, minted now.
//------------------------------------------------------------------------------
struct ServerIdentity {
	EVP_PKEY *key = nullptr;
	X509 *cert = nullptr;
	std::string pem_path;  // the certificate, for SSL_CERT_FILE in the "trusted" cases

	bool Create() {
		key = EVP_EC_gen("P-256");
		if (!key) {
			return false;
		}
		cert = X509_new();
		X509_set_version(cert, 2);
		ASN1_INTEGER_set(X509_get_serialNumber(cert), 1);
		X509_gmtime_adj(X509_getm_notBefore(cert), -60);
		X509_gmtime_adj(X509_getm_notAfter(cert), 3600);
		X509_NAME *name = X509_get_subject_name(cert);
		X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
								   reinterpret_cast<const unsigned char *>("sql.test.invalid"), -1, -1, 0);
		X509_set_issuer_name(cert, name);
		X509_set_pubkey(cert, key);

		X509V3_CTX ctx;
		X509V3_set_ctx_nodb(&ctx);
		X509V3_set_ctx(&ctx, cert, cert, nullptr, nullptr, 0);
		char san[] = "DNS:sql.test.invalid,IP:127.0.0.1";
		X509_EXTENSION *ext = X509V3_EXT_conf_nid(nullptr, &ctx, NID_subject_alt_name, san);
		if (!ext) {
			return false;
		}
		X509_add_ext(cert, ext, -1);
		X509_EXTENSION_free(ext);
		if (!X509_sign(cert, key, EVP_sha256())) {
			return false;
		}

		char path_template[] = "/tmp/mssql_tls_test_XXXXXX";
		int fd = mkstemp(path_template);
		if (fd < 0) {
			return false;
		}
		FILE *f = fdopen(fd, "w");
		PEM_write_X509(f, cert);
		fclose(f);
		pem_path = path_template;
		return true;
	}

	~ServerIdentity() {
		if (cert) {
			X509_free(cert);
		}
		if (key) {
			EVP_PKEY_free(key);
		}
		if (!pem_path.empty()) {
			unlink(pem_path.c_str());
		}
	}
};

//------------------------------------------------------------------------------
// The server: one accept per case, SSL_accept, then close. It does not care
// whether the client rejected it; the client's verdict is what is tested.
//------------------------------------------------------------------------------
struct TestServer {
	int listen_fd = -1;
	uint16_t port = 0;
	SSL_CTX *ctx = nullptr;

	bool Start(const ServerIdentity &id) {
		ctx = SSL_CTX_new(TLS_server_method());
		if (!ctx || SSL_CTX_use_certificate(ctx, id.cert) != 1 || SSL_CTX_use_PrivateKey(ctx, id.key) != 1) {
			return false;
		}
		listen_fd = socket(AF_INET, SOCK_STREAM, 0);
		sockaddr_in addr;
		std::memset(&addr, 0, sizeof(addr));
		addr.sin_family = AF_INET;
		addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		addr.sin_port = 0;
		if (bind(listen_fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0 || listen(listen_fd, 4) != 0) {
			return false;
		}
		socklen_t len = sizeof(addr);
		getsockname(listen_fd, reinterpret_cast<sockaddr *>(&addr), &len);
		port = ntohs(addr.sin_port);
		return true;
	}

	// Runs on its own thread for the duration of one client handshake.
	void ServeOne() {
		int fd = accept(listen_fd, nullptr, nullptr);
		if (fd < 0) {
			return;
		}
		SSL *ssl = SSL_new(ctx);
		SSL_set_fd(ssl, fd);
		if (SSL_accept(ssl) == 1) {
			SSL_shutdown(ssl);
		}
		SSL_free(ssl);
		close(fd);
	}

	~TestServer() {
		if (listen_fd >= 0) {
			close(listen_fd);
		}
		if (ctx) {
			SSL_CTX_free(ctx);
		}
	}
};

//------------------------------------------------------------------------------
// The client, driven like TdsSocket::EnableTls.
//------------------------------------------------------------------------------
struct Outcome {
	bool ok = false;
	std::string error;
	TlsErrorCode code = TlsErrorCode::NONE;
	std::string cipher;
};

static Outcome Connect(TestServer &server, const TlsOptions &options, const std::string &sni,
					   const std::string &trust_file) {
	if (trust_file.empty()) {
		unsetenv("SSL_CERT_FILE");
	} else {
		setenv("SSL_CERT_FILE", trust_file.c_str(), 1);
	}
	std::thread serve([&server]() { server.ServeOne(); });

	Outcome out;
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	sockaddr_in addr;
	std::memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = htons(server.port);
	if (connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
		out.error = "loopback connect failed";
		close(fd);
		serve.join();
		return out;
	}

	TlsTdsContext tls;
	if (!tls.Initialize(options)) {
		out.error = "Initialize: " + tls.GetLastError();
	} else if (!tls.WrapSocket(fd, sni)) {
		out.error = "WrapSocket: " + tls.GetLastError();
	} else if (tls.Handshake(3000)) {
		out.ok = true;
		out.cipher = tls.GetCipherSuite();
	} else {
		out.error = tls.GetLastError();
		out.code = tls.GetLastErrorCode();
	}
	tls.Close();
	close(fd);
	serve.join();
	return out;
}

int main() {
	ServerIdentity id;
	if (!id.Create()) {
		std::cerr << "could not mint the test certificate" << std::endl;
		return 1;
	}
	TestServer server;
	if (!server.Start(id)) {
		std::cerr << "could not start the loopback TLS server" << std::endl;
		return 1;
	}
	const std::string kName = "sql.test.invalid";

	// D2 verify, self-signed, not trusted: the default rejects it and says why.
	{
		TlsOptions verify;
		auto out = Connect(server, verify, kName, "");
		Check(!out.ok, "verify/untrusted: handshake rejected");
		Check(out.code == TlsErrorCode::CERT_VERIFY_FAILED, "verify/untrusted: CERT_VERIFY_FAILED");
		Check(Contains(out.error, "certificate verification failed for sql.test.invalid"),
			  "verify/untrusted: names the expected host (" + out.error + ")");
		Check(Contains(out.error, "self-signed certificate"),
			  "verify/untrusted: OpenSSL's reason verbatim (" + out.error + ")");
	}

	// D2 trust: the same certificate is accepted, and the channel is encrypted.
	{
		TlsOptions trust;
		trust.verify_certificate = false;
		auto out = Connect(server, trust, kName, "");
		Check(out.ok, "trust: handshake succeeds (" + out.error + ")");
		Check(!out.cipher.empty(), "trust: a cipher was negotiated");
	}

	// D2 verify with the certificate in the trust store, expected name = SNI name.
	{
		TlsOptions verify;
		auto out = Connect(server, verify, kName, id.pem_path);
		Check(out.ok, "verify/trusted: handshake succeeds (" + out.error + ")");
	}

	// D3 a different expected name: the chain is fine, the name is not.
	{
		TlsOptions verify;
		verify.expected_host = "other.test.invalid";
		auto out = Connect(server, verify, kName, id.pem_path);
		Check(!out.ok, "verify/trusted/other name: rejected");
		Check(out.code == TlsErrorCode::CERT_VERIFY_FAILED, "verify/trusted/other name: CERT_VERIFY_FAILED");
		Check(Contains(out.error, "hostname mismatch"),
			  "verify/trusted/other name: 'hostname mismatch' (" + out.error + ")");
		Check(Contains(out.error, "for other.test.invalid"),
			  "verify/trusted/other name: names the name it looked for (" + out.error + ")");
	}

	// D5 an IP literal is matched against the iPAddress SAN, not dNSName.
	{
		TlsOptions verify;
		verify.expected_host = "127.0.0.1";
		auto out = Connect(server, verify, kName, id.pem_path);
		Check(out.ok, "verify/trusted/IP in SAN: handshake succeeds (" + out.error + ")");
	}
	{
		TlsOptions verify;
		verify.expected_host = "10.9.8.7";
		auto out = Connect(server, verify, kName, id.pem_path);
		Check(!out.ok, "verify/trusted/other IP: rejected");
		Check(Contains(out.error, "IP address mismatch"),
			  "verify/trusted/other IP: 'IP address mismatch' (" + out.error + ")");
	}

	// D3 under trust the expected name is ignored.
	{
		TlsOptions trust;
		trust.verify_certificate = false;
		trust.expected_host = "other.test.invalid";
		auto out = Connect(server, trust, kName, "");
		Check(out.ok, "trust/other name: still accepted (" + out.error + ")");
	}

	if (g_failures > 0) {
		std::cerr << g_failures << " check(s) failed" << std::endl;
		return 1;
	}
	std::cout << "All TLS verification tests passed" << std::endl;
	return 0;
}

#endif	// _WIN32
