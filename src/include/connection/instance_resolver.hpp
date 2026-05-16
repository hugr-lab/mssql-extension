//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// instance_resolver.hpp
//
// SQL Server Browser (MC-SQLR) client. Resolves a named instance
// (host\instance) to a TCP port by sending a CLNT_UCAST_INST query to
// host:1434/udp and parsing the SVR_RESP response.
//
// Spec: specs/045-named-instance-resolution/
//
// IMPORTANT: This header MUST NOT include any DuckDB headers. The resolver
// is logically reusable outside DuckDB (mirrors iauthenticator.hpp). Errors
// from the parser are std::runtime_error; the calling site translates them
// into DuckDB exceptions.
//===----------------------------------------------------------------------===//

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace duckdb {
namespace mssql {

//===----------------------------------------------------------------------===//
// BrowserInstance - one record from a SQL Server Browser SVR_RESP body
//
// A single CLNT_UCAST_INST query for an existing instance returns exactly one
// record. The multi-instance shape is here so the parser handles every payload
// SQL Browser is documented to emit (CLNT_UCAST_EX, broadcast responses, etc.)
// without a separate code path, even though v1 only consumes UCAST_INST.
//===----------------------------------------------------------------------===//

struct BrowserInstance {
	std::string server_name;	// "ServerName" - hostname the engine binds (often == query host)
	std::string instance_name;	// "InstanceName" - case-preserving as advertised
	std::string version;		// "Version" - e.g. "15.0.4123.1"
	uint16_t tcp_port;			// from "tcp;<port>"; 0 when tcp_enabled == false
	bool tcp_enabled;			// true iff a non-empty "tcp" value was present
	bool is_clustered;			// "IsClustered" parsed as Yes/No
};

//===----------------------------------------------------------------------===//
// ResolveError - typed error returned by InstanceResolver::Resolve
//
// Four distinct categories so user-facing error messages can distinguish
// "browser broken" from "sql server broken". See research.md §R8.
//===----------------------------------------------------------------------===//

struct ResolveError {
	enum class Kind {
		Unreachable,	   // UDP timeout (after the one retry) or send error
		InstanceNotFound,  // Browser responded but the requested instance was absent
		TcpDisabled,	   // Instance found, but TCP transport disabled on it
		Malformed,		   // Browser returned bytes we couldn't parse
	};

	Kind kind;
	std::string message;  // Human-readable, includes host + instance + diagnostic
};

//===----------------------------------------------------------------------===//
// ResolveResult - success carries the resolved TCP host + port; failure
// carries the typed error. Returned by InstanceResolver::Resolve.
// C++11-clean (no std::variant / std::expected dependency).
//
// Note: `host` may differ from the host argument that was passed to
// Resolve(). SQL Server Browser is allowed to advertise the engine on a
// different hostname than the one running Browser itself (failover
// cluster, two-hostname test layout, etc.). When the advertised
// ServerName is empty the resolver falls back to the queried host.
//===----------------------------------------------------------------------===//

struct ResolveResult {
	bool ok;
	std::string host;	 // valid when ok == true; advertised ServerName from SVR_RESP
	uint16_t port;		 // valid when ok == true
	ResolveError error;	 // valid when ok == false

	static ResolveResult Success(std::string h, uint16_t p) {
		ResolveResult r;
		r.ok = true;
		r.host = std::move(h);
		r.port = p;
		return r;
	}

	static ResolveResult Failure(ResolveError::Kind k, std::string msg) {
		ResolveResult r;
		r.ok = false;
		r.port = 0;
		r.error.kind = k;
		r.error.message = std::move(msg);
		return r;
	}
};

//===----------------------------------------------------------------------===//
// Parser - pure function over a byte buffer.
//
// Walks an MC-SQLR SVR_RESP packet (opcode 0x05 + LE u16 size + MBCS body
// per [MC-SQLR] §2.2; ASCII in practice for all documented fields)
// and returns one BrowserInstance per record. Throws std::runtime_error
// with a hex-dump diagnostic message on malformed input.
//
// Inputs:
//   data - pointer to the start of the UDP datagram payload (the 0x05 byte)
//   len  - total payload length in bytes
//
// Public for unit testing; production code goes through InstanceResolver.
//===----------------------------------------------------------------------===//

std::vector<BrowserInstance> ParseBrowserResponse(const uint8_t *data, std::size_t len);

//===----------------------------------------------------------------------===//
// InstanceResolver - stateless, one UDP RTT per call
//
// Resolve(host, instance, timeout):
//   1. Normalises legacy aliases ((local) and "." -> "localhost").
//   2. Sends a CLNT_UCAST_INST datagram to host:1434/udp.
//   3. Waits up to `timeout_seconds` for the reply.
//   4. On timeout, retries once before giving up.
//   5. Parses the response and returns the tcp port for the matching instance
//      (case-insensitive match against InstanceName).
//
// Thread-safe: no shared state. One UDP socket per call.
//===----------------------------------------------------------------------===//

class InstanceResolver {
public:
	// Default port for the SQL Server Browser UDP service.
	static constexpr uint16_t BROWSER_PORT = 1434;

	// Production entry point. Resolves `instance` on `host` via MC-SQLR.
	static ResolveResult Resolve(const std::string &host, const std::string &instance, int timeout_seconds);

	// Test seam: same as Resolve, but lets the caller override the UDP port
	// (so a unit test can spin a loopback listener on an ephemeral port).
	// Not for production use.
	static ResolveResult ResolveForTest(const std::string &host, uint16_t browser_port, const std::string &instance,
										int timeout_seconds);
};

}  // namespace mssql
}  // namespace duckdb
