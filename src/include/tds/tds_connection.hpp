#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include "tds/auth/iauthenticator.hpp"
#include "tds_platform.hpp"
#include "tds_protocol.hpp"
#include "tds_socket.hpp"
#include "tds_types.hpp"

namespace duckdb {
namespace tds {

// Outcome of ONE login attempt against ONE target (spec 068 D1).
//
// The contract, in one rule: a login helper that sees ROUTING in the response
// returns `Route`, whatever the response said about success. [MS-TDS] is
// explicit that a routed session is not usable, so a routed answer that ALSO
// carried a LOGINACK is still a redirect, not a success -- and a routed answer
// with no LOGINACK is still a redirect, not a failure. Before spec 068 we got
// one of each wrong: the FEDAUTH path returned early on `!success` and never
// looked at the routing fields, while SQL auth never looked at them at all and
// kept talking to a gateway that had told it to leave.
//
// A plain bool cannot express this -- it would force the driver to read
// `has_routing_` as a side channel off a `false` return, which is the exact
// arrangement that produced the bug. Hence three states.
enum class LoginAttemptOutcome : uint8_t {
	Success,  // LOGINACK received, no ROUTING -- the session on this socket is usable
	Route,	  // ROUTING received (with OR without LOGINACK) -- hop to routed_server_/routed_port_
	Failure	  // neither -- last_error_ has been set by the attempt
};

// Builds an authenticator for ONE login attempt against ONE target (spec 068 D3).
//
// Integrated auth binds its credential to the target's SPN, so a routing hop
// cannot reuse the gateway's authenticator: it needs a service ticket for the
// ROUTED host. The caller therefore hands over a factory rather than an
// instance, and the hop driver calls it again per hop.
//
// `host` is the TCP/DNS host of the current hop -- instance-stripped, no port
// suffix -- because that is what an AD SPN is registered against
// ("MSSQLSvc/<fqdn>:<port>"). Never pass the LOGIN7 ServerName form
// ("host\instance"), which is not a valid SPN component.
//
// Returning nullptr fails the login: on the first call with the existing
// "compiled without Kerberos / SSPI support" message, on a hop with a message
// naming the routed host. The factory must not throw -- it runs inside the TDS
// layer, which is exception-free by contract -- and must capture by value only,
// since pool refills run on worker threads (issues #178/#179).
using AuthenticatorFactory = std::function<std::shared_ptr<IAuthenticator>(const std::string &host, uint16_t port)>;

// Represents a single TDS connection to SQL Server
// Implements connection state machine per FR-009
class TdsConnection {
public:
	TdsConnection();
	// noexcept (spec 047 T046k): body wraps Close() in try/catch — destructor
	// throws during `~AttachedDatabase` unwind would invoke std::terminate.
	~TdsConnection() noexcept;

	// Non-copyable
	TdsConnection(const TdsConnection &) = delete;
	TdsConnection &operator=(const TdsConnection &) = delete;

	// Movable
	TdsConnection(TdsConnection &&other) noexcept;
	TdsConnection &operator=(TdsConnection &&other) noexcept;

	// Connection establishment (FR-001, FR-002)
	// Establishes TCP connection to host:port
	bool Connect(const std::string &host, uint16_t port, int timeout_seconds = DEFAULT_CONNECTION_TIMEOUT);

	// Authentication (FR-006, FR-007)
	// Performs PRELOGIN and LOGIN7 handshake with SQL Server authentication
	// Parameters:
	//   use_encrypt - if true, enables TLS encryption after PRELOGIN (requires server support)
	// `app_name` (spec 047 FR-014, issue #82) becomes the LOGIN7 program_name —
	// surfaced as `APP_NAME()` / `program_name` in SQL Server-side sys.dm_*
	// views. Empty falls back to the extension default ("DuckDB MSSQL Extension").
	// Caller is expected to pre-clamp to 128 chars via `ResolveAppName()`.
	bool Authenticate(const std::string &username, const std::string &password, const std::string &database,
					  bool use_encrypt = false, const std::string &app_name = "");

	// Azure AD Authentication via FEDAUTH (T018/T020)
	// Performs PRELOGIN with FEDAUTHREQUIRED and LOGIN7 with FEDAUTH feature extension
	// Parameters:
	//   database - initial database to connect to
	//   fedauth_token - UTF-16LE encoded access token from Azure AD
	//   use_encrypt - if true, enables TLS encryption (required for Azure)
	//   app_name    - spec 047 FR-014; see SQL-auth Authenticate above.
	bool AuthenticateWithFedAuth(const std::string &database, const std::vector<uint8_t> &fedauth_token,
								 bool use_encrypt = true, const std::string &app_name = "");

	// Integrated Authentication via Kerberos / SSPI -- Spec 042
	// Performs PRELOGIN, then LOGIN7 with the SPNEGO blob in the SSPI field, then
	// drives the multi-round continuation loop via 0xED tokens.
	// Parameters:
	//   database     - initial database to connect to
	//   authenticator_factory - builds an IAuthenticator for a given (host, port)
	//                   (Krb5Authenticator on POSIX, WinSspiAuthenticator on
	//                   Windows). A FACTORY rather than an instance because a
	//                   routing hop needs a service ticket for the routed host's
	//                   SPN, not a retry of the gateway's (spec 068 D3); it is
	//                   called once per login attempt. Returning nullptr on the
	//                   first call reproduces the pre-068 "no authenticator"
	//                   error. There is deliberately NO shared_ptr overload: it
	//                   would silently reuse the gateway's ticket on a hop.
	//   use_encrypt  - if true, enables TLS encryption (typical for production)
	//   app_name     - spec 047 FR-014; see SQL-auth Authenticate above.
	//   login7_max_packet - TDS packet size to fragment the LOGIN7 / SSPI sends
	//                   to. LOGIN7 is sent before packet-size negotiation, so a
	//                   PAC-bearing Kerberos blob can exceed the 4096 default and
	//                   MUST be fragmented (issue #138). 0 (or out of the
	//                   [256, TDS_MAX_PACKET_SIZE] range) -> the 4096 default;
	//                   smaller values are a TEST hook to force the multi-packet
	//                   path. A plain size_t keeps the TDS layer DuckDB-free; the
	//                   value originates from the mssql_login7_max_packet setting.
	bool AuthenticateIntegrated(const std::string &database, AuthenticatorFactory authenticator_factory,
								bool use_encrypt = true, const std::string &app_name = "",
								size_t login7_max_packet = 0);

	// Connection health check (FR-015)
	// Quick state check - no I/O, just checks internal state
	bool IsAlive() const;

	// TDS-level ping - sends empty SQLBATCH and waits for DONE token
	bool Ping(int timeout_ms = 5000);

	// Full validation with ping for long-idle connections
	bool ValidateWithPing();

	// Close connection (FR-003)
	void Close();

	// Send ATTENTION signal for query cancellation
	bool SendAttention();

	// Wait for ATTENTION acknowledgment
	bool WaitForAttentionAck(int timeout_ms = CANCELLATION_TIMEOUT * 1000);

	// Execute SQL batch and start receiving response (FR for User Story 1)
	// Sends SQL_BATCH packet(s) and prepares connection for streaming response
	// Returns true if batch was sent successfully
	// After this, use ReceiveData() to read response packets
	bool ExecuteBatch(const std::string &sql);

	// Receive more response data into provided buffer
	// Returns bytes received, 0 on connection close, -1 on error
	// timeout_ms: 0 = non-blocking, >0 = wait up to timeout_ms
	ssize_t ReceiveData(uint8_t *buffer, size_t buffer_size, int timeout_ms = DEFAULT_QUERY_TIMEOUT * 1000);

	// State management (FR-009, FR-010)
	ConnectionState GetState() const {
		return state_.load(std::memory_order_acquire);
	}

	// Attempt state transition (thread-safe)
	bool TransitionState(ConnectionState from, ConnectionState to);

	// Getters
	uint16_t GetSpid() const {
		return spid_;
	}
	const std::string &GetHost() const {
		return host_;
	}
	uint16_t GetPort() const {
		return port_;
	}
	const std::string &GetDatabase() const {
		return database_;
	}
	const std::string &GetLastError() const {
		return last_error_;
	}
	bool IsTlsEnabled() const {
		return tls_enabled_;
	}

	// Transaction descriptor management (for SQL_BATCH ALL_HEADERS)
	// Set the transaction descriptor (8 bytes) from ENVCHANGE BEGIN_TRANS response
	void SetTransactionDescriptor(const uint8_t *descriptor);

	// Get the current transaction descriptor (returns pointer to 8 bytes, or nullptr if not set)
	const uint8_t *GetTransactionDescriptor() const;

	// Clear the transaction descriptor (e.g., after COMMIT/ROLLBACK)
	void ClearTransactionDescriptor();

	// Check if a transaction descriptor is currently set
	bool HasTransactionDescriptor() const {
		return has_transaction_descriptor_;
	}

	// Connection reset — flag the next SQL_BATCH to include RESET_CONNECTION in TDS header
	void SetNeedsReset(bool reset) {
		needs_reset_ = reset;
	}
	bool NeedsReset() const {
		return needs_reset_;
	}

	// Timestamps for pool management
	std::chrono::steady_clock::time_point GetCreatedAt() const {
		return created_at_;
	}
	std::chrono::steady_clock::time_point GetLastUsedAt() const {
		return last_used_at_;
	}
	void UpdateLastUsed() {
		last_used_at_ = std::chrono::steady_clock::now();
	}

	// Check if connection has been idle longer than threshold
	bool IsLongIdle() const;

	// Get underlying socket for advanced operations
	TdsSocket *GetSocket() {
		return socket_.get();
	}

	// Get negotiated packet size (from ENVCHANGE during login)
	uint32_t GetNegotiatedPacketSize() const {
		return negotiated_packet_size_;
	}

	// Spec 055: set the TDS frame size to request in LOGIN7. Must be called
	// BEFORE any Authenticate* call — LOGIN7 carries the request, and the server
	// answers with min(requested, its own maximum) in the PACKETSIZE ENVCHANGE.
	// It never negotiates upward, so the extension's long-standing habit of
	// always asking for TDS_DEFAULT_PACKET_SIZE pinned every connection at 4096.
	// 0 selects the default; anything else is clamped to [512, 32767].
	void SetRequestedPacketSize(size_t packet_size) {
		if (packet_size == 0) {
			requested_packet_size_ = static_cast<uint32_t>(TDS_DEFAULT_PACKET_SIZE);
			return;
		}
		if (packet_size < TDS_MIN_PACKET_SIZE) {
			packet_size = TDS_MIN_PACKET_SIZE;
		} else if (packet_size > TDS_MAX_PACKET_SIZE) {
			packet_size = TDS_MAX_PACKET_SIZE;
		}
		requested_packet_size_ = static_cast<uint32_t>(packet_size);
	}

	// Issue #225: whether LOGIN7 advertises UTF8SUPPORT. Server support is not
	// assumed — the acknowledgement in FEATUREEXTACK is what decides, and
	// UTF8SupportAcked() reports it after login.
	void SetRequestUtf8Support(bool request) {
		request_utf8_support_ = request;
	}

	// True only if this connection asked for UTF8SUPPORT and the server acked it,
	// i.e. UTF-8-collation columns arrive as UTF-8 rather than transcoded UTF-16.
	bool UTF8SupportAcked() const {
		return utf8_support_acked_;
	}

private:
	// Issue #225: record what the server granted. Assignment, not accumulation —
	// a routed reconnect logs in again, and the new server's answer is the one
	// that counts. Only a feature this client asked for can be granted: the
	// server answers requests, so an ack for anything else is not ours to honour.
	void NoteFeatureAcks(const LoginResponse &login_response) {
		utf8_support_acked_ = request_utf8_support_ && login_response.utf8_support_acked;
	}

	// Size the socket's receive staging from the frame size the server confirmed
	// (spec 055). Called from every login path once the PACKETSIZE ENVCHANGE has
	// been applied. Reads are made in whole frames, several at a time.
	void ApplyNegotiatedFraming();

	std::unique_ptr<TdsSocket> socket_;
	std::atomic<ConnectionState> state_;

	// Issue #225: requested in LOGIN7, confirmed by FEATUREEXTACK.
	bool request_utf8_support_ = true;
	bool utf8_support_acked_ = false;

	// Connection info
	std::string host_;
	uint16_t port_;
	std::string database_;
	uint16_t spid_;	 // Server Process ID

	// Timing
	std::chrono::steady_clock::time_point created_at_;
	std::chrono::steady_clock::time_point last_used_at_;

	// Error tracking
	std::string last_error_;

	// Packet sequencing
	uint8_t next_packet_id_;

	// TLS state
	bool tls_enabled_;

	// Connect timeout the caller supplied to Connect(), remembered so a routing
	// hop dials on the SAME budget (spec 068). Without this a user who sets
	// mssql_connection_timeout=5 and gets routed to an unreachable replica waits
	// the compiled-in default per hop instead of the 5 they asked for.
	//
	// Scope, precisely: this covers the TCP DIAL. The handshake receives that
	// follow -- PRELOGIN response, TLS enable, LOGIN7 response, on all three
	// auth paths -- still pass DEFAULT_CONNECTION_TIMEOUT, on the first attempt
	// as well as on a hop. So a replica that accepts the connection and then
	// never answers still costs the compiled-in default. Threading the caller's
	// value through those sites would change every login's behaviour, not just a
	// routed one, and belongs in its own change.
	int connect_timeout_seconds_ = DEFAULT_CONNECTION_TIMEOUT;

	// Frame size we ask for in LOGIN7 (spec 055). Defaults to the pre-055
	// behaviour so anything that does not call SetRequestedPacketSize is
	// unchanged.
	uint32_t requested_packet_size_ = static_cast<uint32_t>(TDS_DEFAULT_PACKET_SIZE);

	// Negotiated packet size from server (from ENVCHANGE during login)
	uint32_t negotiated_packet_size_;

	// Transaction descriptor (8 bytes) for SQL_BATCH ALL_HEADERS
	// Set via SetTransactionDescriptor() after BEGIN TRANSACTION response
	uint8_t transaction_descriptor_[8] = {0};
	bool has_transaction_descriptor_ = false;

	// Connection reset flag — when true, next SQL_BATCH sets RESET_CONNECTION in TDS header
	bool needs_reset_ = false;

	// FEDAUTH echo flag — set during PRELOGIN if server's FEDAUTHREQUIRED was non-zero
	// Per MS-TDS: client must echo this value back in LOGIN7's FEDAUTH options byte (bit 0)
	bool fedauth_echo_ = false;

	// Routing info from ENVCHANGE type 20 (Azure SQL/Fabric gateway redirection)
	// Set during LOGIN7 response parsing if server requests routing
	bool has_routing_ = false;
	std::string routed_server_;
	uint16_t routed_port_ = 0;

	// TDS ServerName for LOGIN7 - may differ from host_ when routing includes instance name
	// Format: "hostname" or "hostname\instance" (instance name is for TDS protocol, not DNS)
	std::string tds_server_name_;

	// Original gateway hostname for TLS SNI after routing (Azure Fabric session tracking)
	std::string original_sni_hostname_;

	// Login-time routing driver (spec 068 D2). Runs `attempt` against the
	// current target and follows ROUTING up to MAX_ROUTING_HOPS times, with a
	// full reconnect + handshake per hop.
	//
	// Pre:  state_ == Authenticating, socket_ connected, host_/port_ set.
	// Post: true  -> state_ == Idle, socket_ connected to the final target;
	//       false -> state_ == Disconnected, socket_ closed, last_error_ set.
	//
	// Guarantees to `attempt`: host_/port_/tds_server_name_ are already
	// retargeted for this hop, the per-hop state is already reset, and the
	// socket is connected and un-TLS'd (the attempt does its own PRELOGIN).
	// Obligations on `attempt`: set last_error_ when returning Failure, and
	// NEVER touch state_ or close the socket -- the driver owns both, because a
	// hop has to close and reopen the socket while the state machine stays in
	// Authenticating.
	bool RunWithRoutingHops(const std::function<LoginAttemptOutcome()> &attempt);

	// Split a ROUTING ENVCHANGE target into what TCP needs and what LOGIN7
	// needs, and set tds_server_name_ as a side effect.
	//
	// Azure Fabric sends "hostname.pbidedicated.windows.net\INSTANCE:port": a
	// trailing ":port" (digits only) wins over the ENVCHANGE port, the
	// "hostname\instance" form (port stripped) is what LOGIN7's ServerName
	// field wants, and everything from the backslash on is stripped for
	// DNS/TCP. The SQL Browser is deliberately NOT consulted on a hop -- every
	// observed gateway supplies the port, and inventing UDP traffic mid-login
	// is how timeouts happen.
	void NormalizeRoutedTarget(const std::string &routed_server, uint16_t routed_port, std::string &out_host,
							   uint16_t &out_port);

	// Internal helpers
	bool DoPrelogin(bool use_encrypt);
	LoginAttemptOutcome DoLogin7(const std::string &username, const std::string &password, const std::string &database,
								 const std::string &app_name);

	// Azure AD FEDAUTH helpers (T018/T020)
	// Optional sni_hostname overrides default for TLS SNI (for Azure routing)
	bool DoPreloginWithFedAuth(bool use_encrypt, const std::string &sni_hostname = "");
	LoginAttemptOutcome DoLogin7WithFedAuth(const std::string &database, const std::vector<uint8_t> &fedauth_token,
											const std::string &app_name);
};

}  // namespace tds
}  // namespace duckdb
