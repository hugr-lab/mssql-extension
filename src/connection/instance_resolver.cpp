//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// instance_resolver.cpp
//
// Implementation of the MC-SQLR client. See instance_resolver.hpp for the
// public interface and specs/046-named-instance-resolution/ for the design.
//
//   Phase 0: ParseBrowserResponse — pure parser over canned byte buffers.
//   Phase 1: InstanceResolver::Resolve — UDP transport + retry + match.
//===----------------------------------------------------------------------===//

#include "connection/instance_resolver.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <stdexcept>

// Cross-platform socket headers. Mirror src/tds/tds_socket.cpp so the Windows
// build picks up WSAStartup via the same path as the TDS layer (spec 019).
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#define CLOSE_SOCKET closesocket
#define SOCK_BUF_CAST(x) reinterpret_cast<char *>(x)
#define SOCK_BUF_CONST_CAST(x) reinterpret_cast<const char *>(x)
#define SOCK_INVALID INVALID_SOCKET
typedef SOCKET sock_t;
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
typedef int sock_t;
#define CLOSE_SOCKET ::close
#define SOCK_BUF_CAST(x) reinterpret_cast<void *>(x)
#define SOCK_BUF_CONST_CAST(x) reinterpret_cast<const void *>(x)
#define SOCK_INVALID (-1)
#endif

namespace duckdb {
namespace mssql {

namespace {

//===--------------------------------------------------------------------===//
// HexDump - first N bytes as "XX XX XX ..." for diagnostic error messages.
// We never include more than 32 bytes; a hex dump in the error is for support
// triage, not a debugger replacement.
//===--------------------------------------------------------------------===//
std::string HexDump(const uint8_t *data, std::size_t len, std::size_t max_bytes = 32) {
	std::string out;
	const std::size_t n = (len < max_bytes) ? len : max_bytes;
	out.reserve(n * 3);
	for (std::size_t i = 0; i < n; ++i) {
		char buf[4];
		std::snprintf(buf, sizeof(buf), "%02X ", data[i]);
		out += buf;
	}
	if (!out.empty()) {
		out.pop_back();	 // trailing space
	}
	if (len > max_bytes) {
		out += " ... (";
		out += std::to_string(len);
		out += " bytes total)";
	}
	return out;
}

//===--------------------------------------------------------------------===//
// Token scanner over the MBCS body of a SVR_RESP. Spec is MBCS per
// [MC-SQLR] §2.2; in practice every documented field is ASCII (keys,
// digits, version dotted-quad, instance-name grammar [A-Za-z0-9_$#]).
// The state machine only compares against `;` (0x3B), which is
// invariant across every MBCS codepage, so byte-level scanning is safe.
//
// Body format (NUL-terminated):
//   key;value;key;value;...;tcp;<port>;np;<pipe>;;next_record_first_key;...
//
// Field separator: ';'. Record separator: ";;" or the trailing '\0'.
// We accept either as end-of-record.
//===--------------------------------------------------------------------===//
class TokenScanner {
public:
	TokenScanner(const char *p, std::size_t len) : begin_(p), end_(p + len), cur_(p) {}

	// Reads up to the next ';'. Returns true on success and fills `out`.
	// Returns false at end of input. Throws on overflow (no terminator).
	bool Next(std::string &out) {
		if (cur_ >= end_) {
			return false;
		}
		const char *start = cur_;
		while (cur_ < end_ && *cur_ != ';') {
			++cur_;
		}
		if (cur_ >= end_) {
			// Hit end-of-buffer without finding ';' - tolerate the final
			// trailing token (some real-world responses omit the closing ';'
			// before the NUL).
			out.assign(start, static_cast<std::size_t>(cur_ - start));
			return !out.empty();
		}
		out.assign(start, static_cast<std::size_t>(cur_ - start));
		++cur_;	 // skip ';'
		return true;
	}

	// Returns true if the *next* character is ';' (i.e. we just hit ";;"),
	// indicating end-of-record. Does NOT advance.
	bool AtRecordEnd() const {
		return cur_ < end_ && *cur_ == ';';
	}

	void ConsumeRecordEnd() {
		if (AtRecordEnd()) {
			++cur_;
		}
	}

	bool AtEnd() const {
		return cur_ >= end_;
	}

private:
	const char *begin_;
	const char *end_;
	const char *cur_;
};

//===--------------------------------------------------------------------===//
// ParseRecord - reads alternating key/value tokens until end-of-record or
// end-of-buffer. Returns the populated BrowserInstance.
//
// Unknown fields are silently skipped (forward-compat with new SQL Server
// builds that add fields we don't care about). Order-independent; we key
// off the field name, not the position.
//===--------------------------------------------------------------------===//
BrowserInstance ParseRecord(TokenScanner &scanner) {
	BrowserInstance inst{};
	inst.tcp_port = 0;
	inst.tcp_enabled = false;
	inst.is_clustered = false;

	std::string key;
	std::string value;
	while (scanner.Next(key)) {
		if (scanner.AtRecordEnd()) {
			// Key without value at end-of-record. SQL Server doesn't emit this
			// but be lenient - treat as empty value and stop.
			scanner.ConsumeRecordEnd();
			break;
		}
		if (!scanner.Next(value)) {
			// Key with no value and no terminator - tolerate.
			break;
		}

		// Field dispatch. Field names are case-sensitive per the SQL Browser
		// implementation; we match documented spellings only.
		if (key == "ServerName") {
			inst.server_name = value;
		} else if (key == "InstanceName") {
			inst.instance_name = value;
		} else if (key == "Version") {
			inst.version = value;
		} else if (key == "IsClustered") {
			inst.is_clustered = (value == "Yes" || value == "yes" || value == "YES");
		} else if (key == "tcp") {
			if (!value.empty()) {
				try {
					int port = std::stoi(value);
					if (port > 0 && port <= 65535) {
						inst.tcp_port = static_cast<uint16_t>(port);
						inst.tcp_enabled = true;
					}
				} catch (...) {
					// Malformed port - leave tcp_enabled false. The caller
					// distinguishes "not present" from "garbage" only at the
					// resolver layer, not the parser.
				}
			}
		}
		// All other keys (np, rpc, via, ...) are silently ignored.

		if (scanner.AtRecordEnd()) {
			scanner.ConsumeRecordEnd();
			break;
		}
	}
	return inst;
}

}  // namespace

std::vector<BrowserInstance> ParseBrowserResponse(const uint8_t *data, std::size_t len) {
	// Header: opcode (1 byte) + size (LE u16) + body.
	if (len < 3) {
		std::ostringstream oss;
		oss << "malformed SQL Browser response: too short (" << len
			<< " bytes; need at least 3); hex: " << HexDump(data, len);
		throw std::runtime_error(oss.str());
	}

	if (data[0] != 0x05) {
		std::ostringstream oss;
		oss << "malformed SQL Browser response: unexpected opcode 0x" << std::hex << static_cast<int>(data[0])
			<< " (expected 0x05); hex: " << HexDump(data, len);
		throw std::runtime_error(oss.str());
	}

	const std::size_t advertised = static_cast<std::size_t>(data[1]) | (static_cast<std::size_t>(data[2]) << 8);
	const std::size_t available = len - 3;

	// The size field is the byte count of the body INCLUDING the trailing NUL.
	// Truncated payloads are a hard error - we don't try to recover partial
	// data, because the part we lost may contain the only tcp port we need.
	if (advertised > available) {
		std::ostringstream oss;
		oss << "malformed SQL Browser response: advertised size " << advertised << " exceeds payload " << available
			<< "; hex: " << HexDump(data, len);
		throw std::runtime_error(oss.str());
	}

	// Some real-world payloads include trailing garbage past the advertised
	// size. Trust the size field, not the buffer length.
	std::size_t body_len = advertised;

	// Strip optional trailing NUL from the body before tokenising.
	if (body_len > 0 && data[3 + body_len - 1] == 0x00) {
		body_len -= 1;
	}

	std::vector<BrowserInstance> records;
	if (body_len == 0) {
		return records;	 // empty response - instance not found
	}

	TokenScanner scanner(reinterpret_cast<const char *>(data + 3), body_len);
	while (!scanner.AtEnd()) {
		BrowserInstance inst = ParseRecord(scanner);
		// Require InstanceName to count as a valid record; otherwise the
		// response is structurally broken (or we read past the end into
		// alignment padding).
		if (inst.instance_name.empty()) {
			break;
		}
		records.push_back(std::move(inst));
	}
	return records;
}

//===----------------------------------------------------------------------===//
// Phase 1: UDP transport
//===----------------------------------------------------------------------===//

namespace {

// Convert legacy aliases that real-world users (and sqlcmd) accept.
// (local) and "." are documented forms for "this machine". The TDS layer
// itself only knows TCP, so we map them to "localhost" before resolution.
std::string NormaliseHost(const std::string &host) {
	if (host == "(local)" || host == ".") {
		return "localhost";
	}
	return host;
}

bool IEquals(const std::string &a, const std::string &b) {
	if (a.size() != b.size()) {
		return false;
	}
	for (std::size_t i = 0; i < a.size(); ++i) {
		if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) {
			return false;
		}
	}
	return true;
}

// Build the CLNT_UCAST_INST datagram: 0x04 + MBCS instance name +
// 0x00. Per [MC-SQLR] §2.2.3 the instance name is MBCS; in practice
// SQL Server's own instance-name grammar restricts it to ASCII.
std::vector<uint8_t> BuildUcastInstRequest(const std::string &instance) {
	std::vector<uint8_t> pkt;
	pkt.reserve(2 + instance.size());
	pkt.push_back(0x04);
	pkt.insert(pkt.end(), instance.begin(), instance.end());
	pkt.push_back(0x00);
	return pkt;
}

// Set the socket receive timeout. On POSIX, takes a timeval (seconds +
// microseconds). On Winsock, takes a DWORD of milliseconds — different
// type AND different field. Centralised here so the call sites stay
// readable.
bool SetRecvTimeout(sock_t s, int seconds) {
#ifdef _WIN32
	DWORD ms = static_cast<DWORD>(seconds) * 1000U;
	return ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&ms), sizeof(ms)) == 0;
#else
	struct timeval tv;
	tv.tv_sec = seconds;
	tv.tv_usec = 0;
	return ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0;
#endif
}

// Resolve `host` to a UDP socket address. Returns 0-length vector on
// failure. Each entry is a fully-populated sockaddr that can be passed to
// sendto/connect. We try them in order; the first that successfully
// receives a reply wins. (Practically there's always one address; the
// loop exists for IPv6/IPv4 dual-stack hosts.)
struct ResolvedAddr {
	int family;
	int socktype;
	int protocol;
	std::vector<uint8_t> addr;	// raw sockaddr bytes
};

std::vector<ResolvedAddr> ResolveAddresses(const std::string &host, uint16_t port, std::string &error_out) {
	std::vector<ResolvedAddr> out;
	struct addrinfo hints;
	std::memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_DGRAM;
	hints.ai_protocol = IPPROTO_UDP;

	char port_str[16];
	std::snprintf(port_str, sizeof(port_str), "%u", static_cast<unsigned>(port));

	struct addrinfo *result = nullptr;
	int rc = ::getaddrinfo(host.c_str(), port_str, &hints, &result);
	if (rc != 0) {
		// gai_strerrorA is the ANSI variant; both POSIX and Win32
		// expose it as thread-safe for the strings getaddrinfo returns.
		// On Windows getaddrinfo also surfaces the WSA error code via
		// WSAGetLastError - we'd lose that, but the gai message is
		// already a translated human-readable string, so it suffices.
#ifdef _WIN32
		error_out = std::string("DNS lookup failed for '") + host + "': " + gai_strerrorA(rc);
#else
		error_out = std::string("DNS lookup failed for '") + host + "': " + gai_strerror(rc);
#endif
		return out;
	}
	for (struct addrinfo *rp = result; rp != nullptr; rp = rp->ai_next) {
		ResolvedAddr a;
		a.family = rp->ai_family;
		a.socktype = rp->ai_socktype;
		a.protocol = rp->ai_protocol;
		a.addr.assign(reinterpret_cast<uint8_t *>(rp->ai_addr),
					  reinterpret_cast<uint8_t *>(rp->ai_addr) + rp->ai_addrlen);
		out.push_back(std::move(a));
	}
	::freeaddrinfo(result);
	return out;
}

// One send + one recv attempt. Returns true on success (resp_out populated).
// Sets timed_out = true if the timeout fired, leaves it false on other
// failures (logged into err_out).
bool SendAndRecvOnce(const ResolvedAddr &addr, const std::vector<uint8_t> &req, int timeout_seconds,
					 std::vector<uint8_t> &resp_out, bool &timed_out, std::string &err_out) {
	timed_out = false;
	sock_t s = ::socket(addr.family, addr.socktype, addr.protocol);
	if (s == SOCK_INVALID) {
		err_out = "UDP socket() failed";
		return false;
	}

	if (!SetRecvTimeout(s, timeout_seconds)) {
		err_out = "setsockopt(SO_RCVTIMEO) failed";
		CLOSE_SOCKET(s);
		return false;
	}

	// connect() on a UDP socket sets the default peer for send/recv. Using
	// it lets us call send()/recv() instead of sendto()/recvfrom(), and
	// (more usefully) makes the kernel return ECONNREFUSED on the next
	// recv if the host sent an ICMP "port unreachable" — without it, the
	// recv would just block until timeout. ECONNREFUSED is a much faster
	// "Browser not running" signal than waiting 3 seconds.
	if (::connect(s, reinterpret_cast<const struct sockaddr *>(addr.addr.data()),
				  static_cast<socklen_t>(addr.addr.size())) != 0) {
		err_out = "UDP connect() failed";
		CLOSE_SOCKET(s);
		return false;
	}

#ifdef _WIN32
	int sent = ::send(s, SOCK_BUF_CONST_CAST(req.data()), static_cast<int>(req.size()), 0);
#else
	ssize_t sent = ::send(s, req.data(), req.size(), 0);
#endif
	if (sent < 0 || static_cast<std::size_t>(sent) != req.size()) {
		err_out = "UDP send() failed";
		CLOSE_SOCKET(s);
		return false;
	}

	// One MTU is the practical cap for a single UDP datagram on common
	// networks (1500 - 20 IP - 8 UDP = 1472 payload). SQL Browser responses
	// for a single instance are well under 200 bytes; multi-instance
	// responses are still under a kilobyte in practice.
	std::vector<uint8_t> buf(1472);
#ifdef _WIN32
	int n = ::recv(s, SOCK_BUF_CAST(buf.data()), static_cast<int>(buf.size()), 0);
#else
	ssize_t n = ::recv(s, buf.data(), buf.size(), 0);
#endif
	if (n < 0) {
#ifdef _WIN32
		int e = WSAGetLastError();
		if (e == WSAETIMEDOUT) {
			timed_out = true;
		} else {
			err_out = "UDP recv() failed (winsock error " + std::to_string(e) + ")";
		}
#else
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			timed_out = true;
		} else if (errno == ECONNREFUSED) {
			// ICMP port unreachable arrived synchronously — Browser is
			// definitely not listening. Surface this distinctly from a
			// generic timeout so the caller's retry isn't wasted.
			err_out = "UDP port unreachable (ICMP ECONNREFUSED)";
		} else if (errno == ENETUNREACH) {
			err_out = "network unreachable (ENETUNREACH)";
		} else if (errno == EHOSTUNREACH) {
			err_out = "host unreachable (EHOSTUNREACH)";
		} else {
			err_out = std::string("UDP recv() failed: ") + std::strerror(errno);
		}
#endif
		CLOSE_SOCKET(s);
		return false;
	}

	buf.resize(static_cast<std::size_t>(n));
	resp_out = std::move(buf);
	CLOSE_SOCKET(s);
	return true;
}

ResolveResult ResolveCore(const std::string &raw_host, uint16_t browser_port, const std::string &instance,
						  int timeout_seconds) {
	const std::string host = NormaliseHost(raw_host);

	std::string err;
	auto addrs = ResolveAddresses(host, browser_port, err);
	if (addrs.empty()) {
		return ResolveResult::Failure(ResolveError::Kind::Unreachable, err);
	}

	const auto req = BuildUcastInstRequest(instance);

	// We try the first address. UDP is best-effort and the second address
	// (e.g. an IPv6 literal advertised in DNS but not routed) would have
	// the same retry budget anyway. Most hosts return one address.
	const ResolvedAddr &addr = addrs.front();

	std::vector<uint8_t> resp;
	bool timed_out = false;
	std::string send_err;
	bool ok = SendAndRecvOnce(addr, req, timeout_seconds, resp, timed_out, send_err);

	// FR-006: one retry on timeout. UDP is lossy; the cost of a single
	// retry is small relative to the cost of a spurious ATTACH failure.
	if (!ok && timed_out) {
		ok = SendAndRecvOnce(addr, req, timeout_seconds, resp, timed_out, send_err);
	}

	if (!ok) {
		std::ostringstream oss;
		oss << "SQL Browser unreachable at " << host << ":" << browser_port << "/udp";
		if (timed_out) {
			oss << " after " << (timeout_seconds * 2) << "s (1 send + 1 retry)";
		} else if (!send_err.empty()) {
			oss << ": " << send_err;
		}
		return ResolveResult::Failure(ResolveError::Kind::Unreachable, oss.str());
	}

	std::vector<BrowserInstance> records;
	try {
		records = ParseBrowserResponse(resp.data(), resp.size());
	} catch (const std::runtime_error &e) {
		std::ostringstream oss;
		oss << "SQL Browser at " << host << ":" << browser_port << "/udp returned " << e.what();
		return ResolveResult::Failure(ResolveError::Kind::Malformed, oss.str());
	}

	for (const auto &r : records) {
		if (IEquals(r.instance_name, instance)) {
			if (!r.tcp_enabled) {
				std::ostringstream oss;
				oss << "instance '" << instance << "' exists on " << host << " but TCP transport is disabled";
				return ResolveResult::Failure(ResolveError::Kind::TcpDisabled, oss.str());
			}
			// Honour the advertised ServerName from the SVR_RESP. Browser
			// is allowed to point the client at a different host than
			// itself (failover-cluster scenarios, two-hostname test
			// layout). Fall back to the queried host when the response
			// didn't include a ServerName field (defensive; real SQL
			// Server always emits it).
			std::string advertised_host = r.server_name.empty() ? host : r.server_name;
			return ResolveResult::Success(std::move(advertised_host), r.tcp_port);
		}
	}

	std::ostringstream oss;
	oss << "instance '" << instance << "' not found on host '" << host << "'";
	if (!records.empty()) {
		oss << "; available: ";
		for (std::size_t i = 0; i < records.size(); ++i) {
			if (i > 0)
				oss << ", ";
			oss << records[i].instance_name;
		}
	} else {
		oss << " (browser returned no instances)";
	}
	return ResolveResult::Failure(ResolveError::Kind::InstanceNotFound, oss.str());
}

}  // namespace

ResolveResult InstanceResolver::Resolve(const std::string &host, const std::string &instance, int timeout_seconds) {
	return ResolveCore(host, BROWSER_PORT, instance, timeout_seconds);
}

ResolveResult InstanceResolver::ResolveForTest(const std::string &host, uint16_t browser_port,
											   const std::string &instance, int timeout_seconds) {
	return ResolveCore(host, browser_port, instance, timeout_seconds);
}

}  // namespace mssql
}  // namespace duckdb
