// A failed connection's error, worded for the user: the server's own classification when it gave one,
// else what the dial, the TLS handshake or the name lookup said.

#include "mssql_storage.hpp"

#include "duckdb/common/string_util.hpp"

namespace duckdb {

//===----------------------------------------------------------------------===//
// Connection Validation
//===----------------------------------------------------------------------===//

//! Whether `text` holds any of `needles`.
static bool ContainsAny(const string &text, std::initializer_list<const char *> needles) {
	for (auto needle : needles) {
		if (text.find(needle) != string::npos) {
			return true;
		}
	}
	return false;
}

//! A failure the server classified (its ERROR token's number). These take precedence over every
//! heuristic, because here we know what the server actually said.
static string TranslateServerError(const string &error, const string &host, uint16_t port, const string &user,
								   const string &database, uint32_t server_error, uint8_t server_state) {
	switch (server_error) {
	case 40613:	 // Azure SQL: "Database ... is not currently available"
	case 40197:	 // Azure SQL: error processing the request, reconfiguration
	case 40501:	 // Azure SQL: service is busy
	case 49918:	 // Azure SQL: cannot process request, not enough resources
		return StringUtil::Format(
			"Database '%s' on %s:%d is not available yet (server error %d) - a serverless database may be "
			"resuming, or the service is being reconfigured; retry shortly. Server said: %s",
			database, host, port, server_error, error);
	case 18456:	 // Login failed. State is the only field that says WHY (issue #164).
		// `user` is empty on the token-authenticated paths, where the identity
		// comes from the token rather than a User Id -- naming user '' there
		// is less informative than the server's own text.
		return StringUtil::Format(
			"Authentication failed%s (server error 18456, state %d) - state 8 is a wrong "
			"password, state 38/40 an inaccessible initial database, state 1 an unspecified "
			"cause the server logs but does not disclose. Server said: %s",
			user.empty() ? "" : StringUtil::Format(" for user '%s'", user), server_state, error);
	case 4060:	// Cannot open database requested by the login
		return StringUtil::Format(
			"Cannot access database '%s' - check database name and permissions (server "
			"error 4060). Server said: %s",
			database, error);
	default:
		return StringUtil::Format("Login to %s:%d failed with server error %d (state %d): %s", host, port, server_error,
								  server_state, error);
	}
}

// Translate TDS error message to user-friendly message.
//
// `server_error` / `server_state` are the SERVER's own ERROR-token fields, or 0
// when the failure produced no ERROR token. When a number is present it decides
// the classification; the string heuristics below only run for failures that
// never reached a login (socket, TLS, DNS).
//
// Issue #262: the string arms must never be allowed to classify a login
// failure. Every login failure this extension produces begins "Authentication
// failed (error N, state S): ...", so a `find("authentication")` test matches
// our OWN wrapper rather than the server's message, and collapsed every cause
// -- including Azure 40613, a serverless database resuming -- into "check
// username and password". The server's number and text are always appended now,
// so no diagnosis is lost on the way to the user.
string MSSQLTranslateConnectionError(const string &error, const string &host, uint16_t port, const string &user,
									 const string &database, uint32_t server_error, uint8_t server_state) {
	if (server_error > 0) {
		return TranslateServerError(error, host, port, user, database, server_error, server_state);
	}
	const string lower_error = StringUtil::Lower(error);

	// A login rejection that carried no usable error number. Matches the
	// SERVER's phrase ("Login failed for user ...") and deliberately NOT the
	// word "authentication", which appears in our own wrapper for every login
	// failure and is what made this arm swallow everything (issue #262).
	if (ContainsAny(lower_error, {"login failed"})) {
		return StringUtil::Format("Authentication failed%s - check username and password. Server said: %s",
								  user.empty() ? "" : StringUtil::Format(" for user '%s'", user), error);
	}

	// Database access failures
	if (ContainsAny(lower_error, {"cannot open database"})) {
		return StringUtil::Format("Cannot access database '%s' - check database name and permissions. Details: %s",
								  database, error);
	}

	// TLS failures
	if (ContainsAny(lower_error, {"tls", "ssl", "handshake"})) {
		return StringUtil::Format("TLS negotiation failed to %s:%d - check TLS configuration. Details: %s", host, port,
								  error);
	}

	// Server requires encryption but client disabled it
	if (ContainsAny(lower_error, {"encrypt_req"}) ||
		(ContainsAny(lower_error, {"encryption"}) && ContainsAny(lower_error, {"require"}))) {
		return StringUtil::Format(
			"Server %s:%d requires encryption (ENCRYPT_REQ) but use_encrypt=false. "
			"Set use_encrypt=true or Encrypt=yes in connection string. Details: %s",
			host, port, error);
	}

	// Certificate validation failures
	if (ContainsAny(lower_error, {"certificate", "cert"})) {
		return StringUtil::Format(
			"TLS certificate validation failed to %s:%d - server certificate not trusted. "
			"Details: %s",
			host, port, error);
	}

	// Connection refused
	if (ContainsAny(lower_error, {"connection refused", "econnrefused"})) {
		return StringUtil::Format(
			"Connection refused to %s:%d - check if SQL Server is running and accepting connections. Details: %s", host,
			port, error);
	}

	// Unreachable: no route to the host, the host down, no network -- the dial's own words, not a bare
	// "unreachable" (an integrated-auth hint says the KDC is). Before the DNS arm, whose bare "host" matches
	// "No route to host" (review of #610: an unreachable server was told to check its name).
	if (ContainsAny(lower_error, {"no route to host", "host is down", "network is unreachable"})) {
		return StringUtil::Format("Cannot reach %s:%d - check network connectivity and firewall settings. Details: %s",
								  host, port, error);
	}

	// DNS/hostname resolution
	if (ContainsAny(lower_error, {"resolve", "host", "enoent", "name or service not known"})) {
		return StringUtil::Format("Cannot resolve hostname '%s' - check server name. Details: %s", host, error);
	}

	// Timeout
	if (ContainsAny(lower_error, {"timeout", "timed out"})) {
		return StringUtil::Format(
			"Connection timed out to %s:%d - check network connectivity and firewall settings. Details: %s", host, port,
			error);
	}

	// Generic connection error
	if (!error.empty()) {
		return StringUtil::Format("Connection failed to %s:%d: %s", host, port, error);
	}

	return StringUtil::Format("Connection failed to %s:%d", host, port);
}

}  // namespace duckdb
