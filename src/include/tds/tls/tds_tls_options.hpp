//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// tds_tls_options.hpp
//
// What the TLS layer checks about the server's certificate (spec 074). Shared
// by the OpenSSL implementation and the wrapper that hides it, so it carries
// no OpenSSL types.
//===----------------------------------------------------------------------===//

#pragma once

#include <string>

namespace duckdb {
namespace tds {

struct TlsOptions {
	// TrustServerCertificate=false, the default: the server's certificate chain
	// must validate against the platform trust store and its subject must match
	// expected_host. TrustServerCertificate=true: any certificate is accepted;
	// the channel is still encrypted, what is given up is knowing who is at the
	// other end.
	bool verify_certificate = true;

	// HostNameInCertificate. Empty means the host the socket dialled -- after a
	// login-time routing hop, the routed host, exactly as SNI already does. An IP
	// literal is matched against iPAddress SANs. Ignored when not verifying.
	std::string expected_host;
};

}  // namespace tds
}  // namespace duckdb
