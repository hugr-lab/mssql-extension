// MSSQLConnectionInfo: where to connect and as whom, from a secret, a connection string or a URI; what is
// refused of them; the application name sent at login; a named instance resolved to its port.

#include "mssql_storage.hpp"

#include "connection/instance_resolver.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/secret/secret_manager.hpp"
#include "mssql_connection_string.hpp"
#include "mssql_platform.hpp"

#include <openssl/crypto.h>

#include <cctype>

namespace duckdb {

//===----------------------------------------------------------------------===//
// MSSQLConnectionInfo destructor — wipe bearer credentials
//
// SQL `password` and Azure AD `access_token` are bearer credentials that
// should not linger in heap-recycled memory after the struct dies. Use
// OPENSSL_cleanse rather than std::fill / memset so dead-store elimination
// can't optimise the wipe away. Cheap (microseconds) and OpenSSL is
// already linked extension-wide via vcpkg.
//
// `password.data()` returns a pointer to the storage in use (whether SSO
// inline buffer or heap allocation). Wiping `size()` bytes covers the
// live characters; subsequent `string` destruction releases the heap
// allocation (if any) — by then it holds zeros.
//===----------------------------------------------------------------------===//
MSSQLConnectionInfo::~MSSQLConnectionInfo() {
	if (!password.empty()) {
		OPENSSL_cleanse(&password[0], password.size());
	}
	if (!access_token.empty()) {
		OPENSSL_cleanse(&access_token[0], access_token.size());
	}
}

//===----------------------------------------------------------------------===//
// ResolveAppName (Spec 047 US-AN / FR-014 / T065) — see header doc-comment
//
// Two-step normalization:
//   1. Strip C0 controls (bytes 0x00–0x1F) and DEL (0x7F). LOGIN7
//      program_name lands in `sys.dm_exec_sessions.program_name`,
//      `sys.dm_exec_requests`, and the SQL Server error log; un-stripped
//      `\r\n` would let an attacker who controls any ATTACH path inject
//      fake log lines (CR/LF injection — flagged by PR #118 review H2).
//   2. UTF-16 code unit clamp at 128, mirroring SQL Server's program_name
//      limit and the wire encoding in `tds_protocol.cpp::BuildLogin7`.
//      Walks the UTF-8 input forward by codepoint; counts UTF-16 code
//      units (1 for BMP codepoints in 1/2/3-byte UTF-8 sequences, 2 for
//      supplementary plane codepoints in 4-byte UTF-8 sequences =
//      surrogate pair). Invalid UTF-8 bytes advance defensively by 1
//      byte / 1 code unit so a malformed input still terminates rather
//      than looping.
//===----------------------------------------------------------------------===//
static const char *const DEFAULT_APP_NAME = "DuckDB MSSQL Extension";

//! Step 1: strip C0 controls + DEL (log-injection defense).
static string StripControlCharacters(const string &raw) {
	string sanitized;
	sanitized.reserve(raw.size());
	for (char ch : raw) {
		auto c = static_cast<unsigned char>(ch);
		if (c >= 0x20 && c != 0x7F) {
			sanitized.push_back(ch);
		}
	}
	return sanitized;
}

//! The UTF-8 sequence a lead byte starts: its bytes, and the UTF-16 code units it encodes to.
static void Utf8Sequence(unsigned char lead, size_t &advance, size_t &units) {
	units = 1;
	if (lead < 0x80) {
		advance = 1;  // ASCII
	} else if ((lead & 0xE0) == 0xC0) {
		advance = 2;  // 2-byte UTF-8 → 1 UCS-2 code unit
	} else if ((lead & 0xF0) == 0xE0) {
		advance = 3;  // 3-byte UTF-8 → 1 UCS-2 code unit
	} else if ((lead & 0xF8) == 0xF0) {
		advance = 4;
		units = 2;	// 4-byte UTF-8 → surrogate pair (2 code units)
	} else {
		advance = 1;  // invalid lead byte — defensive single-byte step
	}
}

//! Step 2: how many bytes of `s` fit in `max_units` UTF-16 code units, whole sequences only.
static size_t ClampUtf16Units(const string &s, size_t max_units) {
	size_t byte_pos = 0;
	size_t utf16_units = 0;
	while (byte_pos < s.size() && utf16_units < max_units) {
		size_t advance;
		size_t units;
		Utf8Sequence(static_cast<unsigned char>(s[byte_pos]), advance, units);
		if (utf16_units + units > max_units) {
			break;
		}
		if (byte_pos + advance > s.size()) {
			break;	// truncated UTF-8 at end of input
		}
		byte_pos += advance;
		utf16_units += units;
	}
	return byte_pos;
}

string ResolveAppName(const MSSQLConnectionInfo &info) {
	static constexpr size_t MAX_UTF16_CODE_UNITS = 128;
	if (info.application_name.empty()) {
		return DEFAULT_APP_NAME;
	}
	const string sanitized = StripControlCharacters(info.application_name);
	if (sanitized.empty()) {
		return DEFAULT_APP_NAME;
	}
	const size_t byte_pos = ClampUtf16Units(sanitized, MAX_UTF16_CODE_UNITS);
	if (byte_pos == sanitized.size()) {
		return sanitized;
	}
	return sanitized.substr(0, byte_pos);
}

//===----------------------------------------------------------------------===//
// MSSQLConnectionInfo implementation
//===----------------------------------------------------------------------===//

// Split a `host\instance` server string (spec 045). On return `host` holds the
// bare hostname and the instance name (if any) is written to `instance_out`.
// A default instance leaves instance_out empty. Validates the instance name
// against SQL Server's grammar ([A-Za-z0-9_$#], <=16 chars, FR-010).
static void SplitHostInstance(string &host, string &instance_out) {
	auto backslash_pos = host.find('\\');
	if (backslash_pos == string::npos) {
		return;
	}
	string instance = host.substr(backslash_pos + 1);
	host = host.substr(0, backslash_pos);
	if (instance.empty()) {
		throw InvalidInputException("MSSQL Error: empty instance name after backslash in Server");
	}
	if (instance.size() > 16) {
		throw InvalidInputException("MSSQL Error: instance name '%s' exceeds 16 characters", instance);
	}
	for (char c : instance) {
		auto uc = static_cast<unsigned char>(c);
		if (!std::isalnum(uc) && c != '_' && c != '$' && c != '#') {
			throw InvalidInputException(
				"MSSQL Error: invalid character in instance name '%s' (allowed: letters, digits, _ $ #)", instance);
		}
	}
	instance_out = instance;
}

//! A secret's optional value into `out`; left as it is when the secret has none.
static void SecretString(const KeyValueSecret &secret, const char *key, string &out) {
	auto value = secret.TryGetValue(key);
	if (!value.IsNull()) {
		out = value.ToString();
	}
}

static void SecretBool(const KeyValueSecret &secret, const char *key, bool &out) {
	auto value = secret.TryGetValue(key);
	if (!value.IsNull()) {
		out = value.GetValue<bool>();
	}
}

//! Where to connect and as whom, and the TLS and catalog switches. Absent switches keep the struct's
//! defaults: use_encrypt true, the certificate verified against the host dialled (spec 074), the catalog on.
static void ReadSecretEndpoint(const KeyValueSecret &kv_secret, MSSQLConnectionInfo &result) {
	result.host = kv_secret.TryGetValue("host").ToString();
	auto port_val = kv_secret.TryGetValue("port");
	result.port = port_val.IsNull() ? 1433 : static_cast<uint16_t>(port_val.GetValue<int32_t>());
	result.port_specified = !port_val.IsNull();
	// Named instance via secret (spec 045): a `host\instance` value in the
	// secret's host field is split the same way as the connection-string form,
	// then resolved to a port at ATTACH time.
	SplitHostInstance(result.host, result.instance_name);
	result.database = kv_secret.TryGetValue("database").ToString();
	result.user = kv_secret.TryGetValue("user").ToString();
	result.password = kv_secret.TryGetValue("password").ToString();

	SecretBool(kv_secret, "use_encrypt", result.use_encrypt);
	SecretBool(kv_secret, "trust_server_certificate", result.trust_server_certificate);
	SecretString(kv_secret, "host_name_in_certificate", result.host_name_in_certificate);
	// When false, catalog integration is disabled (raw query mode only)
	SecretBool(kv_secret, "catalog", result.catalog_enabled);
}

//! Azure AD: a token given directly (Spec 032), else the azure secret to get one with (T015). Neither:
//! use_azure_auth stays false (SQL auth), auth_method SQL.
static void ReadSecretAzureAuth(const KeyValueSecret &kv_secret, MSSQLConnectionInfo &result) {
	// Read optional access_token for direct token authentication (Spec 032)
	// Takes precedence over azure_secret
	auto access_token_val = kv_secret.TryGetValue("access_token");
	if (!access_token_val.IsNull()) {
		result.access_token = access_token_val.ToString();
		result.use_azure_auth = !result.access_token.empty();  // Manual token uses FEDAUTH flow
		if (!result.access_token.empty()) {
			result.auth_method = AuthMethod::MANUAL_TOKEN;	// Spec 042: keep enum in sync
		}
	}
	if (!result.access_token.empty()) {
		return;
	}

	// Read optional azure_secret for Azure AD authentication (T015)
	// Only applies if access_token is not set
	auto azure_secret_val = kv_secret.TryGetValue("azure_secret");
	if (!azure_secret_val.IsNull()) {
		result.azure_secret_name = azure_secret_val.ToString();
		result.use_azure_auth = !result.azure_secret_name.empty();
		if (!result.azure_secret_name.empty()) {
			result.auth_method = AuthMethod::AZURE_AD;	// Spec 042: keep enum in sync
		}
	}
	// The tenant override that goes with it. mssql_secret.cpp has registered
	// and stored `azure_tenant_id` since spec 032, and nothing read it back
	// until 2026-08-14 -- so an interactive-chain azure secret authenticated
	// against "common" no matter what the user put here.
	SecretString(kv_secret, "azure_tenant_id", result.azure_tenant_id);
}

//! Spec 042: Integrated Authentication fields
static void ReadSecretIntegratedAuth(const KeyValueSecret &kv_secret, const string &secret_name,
									 MSSQLConnectionInfo &result) {
	auto auth_val = kv_secret.TryGetValue("authenticator");
	const string auth_str = auth_val.IsNull() ? string() : StringUtil::Lower(auth_val.ToString());
	if (auth_str == "krb5") {
		result.auth_method = AuthMethod::KRB5;
	} else if (auth_str == "winsspi") {
		result.auth_method = AuthMethod::WINSSPI;
	} else if (!auth_str.empty()) {
		throw InvalidInputException(
			"MSSQL Error: Secret '%s' has unsupported 'authenticator' value '%s'. "
			"Supported: krb5 (POSIX), winsspi (Windows).",
			secret_name, auth_str);
	}
	SecretString(kv_secret, "krb5_configfile", result.krb5_configfile);
	SecretString(kv_secret, "krb5_keytabfile", result.krb5_keytabfile);
	SecretString(kv_secret, "krb5_credcachefile", result.krb5_credcachefile);
	SecretString(kv_secret, "krb5_realm", result.krb5_realm);
	SecretString(kv_secret, "service_principal_name", result.service_principal_name);
}

// Spec 047 FR-014 (issue #82): LOGIN7 program_name from secret. Accept
// both `application_name` (underscore form, the existing convention for
// MSSQL secret fields) and `applicationname` (spaceless ADO.NET form).
// First non-empty wins; underscore form is the documented canonical key.
static void ReadSecretApplicationName(const KeyValueSecret &kv_secret, MSSQLConnectionInfo &result) {
	auto app_name_val = kv_secret.TryGetValue("application_name");
	if (app_name_val.IsNull() || app_name_val.ToString().empty()) {
		app_name_val = kv_secret.TryGetValue("applicationname");
	}
	if (!app_name_val.IsNull()) {
		result.application_name = app_name_val.ToString();
	}
}

shared_ptr<MSSQLConnectionInfo> MSSQLConnectionInfo::FromSecret(ClientContext &context, const string &secret_name) {
	auto &secret_manager = SecretManager::Get(context);

	auto transaction = CatalogTransaction::GetSystemCatalogTransaction(context);
	auto secret_entry = secret_manager.GetSecretByName(transaction, secret_name);
	if (!secret_entry) {
		throw BinderException(
			"MSSQL Error: Secret '%s' not found. Create it first with: CREATE SECRET %s (TYPE "
			"mssql, host '...', port ..., database '...', user '...', password '...')",
			secret_name, secret_name);
	}

	auto &secret = secret_entry->secret;
	if (secret->GetType() != "mssql") {
		throw BinderException("MSSQL Error: Secret '%s' is not of type 'mssql'. Got type: '%s'", secret_name,
							  secret->GetType());
	}

	// Use static_cast - we've already verified it's an MSSQL secret which is always KeyValueSecret
	// This avoids dynamic_cast RTTI warnings when crossing extension boundaries on macOS
	auto &kv_secret = static_cast<const KeyValueSecret &>(*secret);

	auto result = make_shared_ptr<MSSQLConnectionInfo>();
	ReadSecretEndpoint(kv_secret, *result);
	ReadSecretAzureAuth(kv_secret, *result);
	// Catalog visibility filters (Spec 033) and the catalog's other per-connection choices.
	SecretString(kv_secret, "schema_filter", result->schema_filter);
	SecretString(kv_secret, "table_filter", result->table_filter);
	SecretString(kv_secret, "transaction_isolation", result->transaction_isolation);
	SecretString(kv_secret, "datetime2_7_as", result->datetime2_7_as);
	SecretString(kv_secret, "default_schema", result->default_schema);
	ReadSecretIntegratedAuth(kv_secret, secret_name, *result);
	ReadSecretApplicationName(kv_secret, *result);

	result->connected = false;
	return result;
}

//===----------------------------------------------------------------------===//
// Endpoint Detection Helpers (T007)
//===----------------------------------------------------------------------===//

bool MSSQLConnectionInfo::IsAzureEndpoint() const {
	return mssql::IsAzureEndpoint(host);
}

bool MSSQLConnectionInfo::IsFabricEndpoint() const {
	return mssql::IsFabricEndpoint(host);
}

bool MSSQLConnectionInfo::IsSynapseEndpoint() const {
	return mssql::IsSynapseEndpoint(host);
}

//===----------------------------------------------------------------------===//
// Connection String Parsing (the parsers themselves: mssql_connection_string.cpp)
//===----------------------------------------------------------------------===//

bool MSSQLConnectionInfo::IsUriFormat(const string &str) {
	return MSSQLIsUriFormat(str);
}

// Check if string is an ADO.NET connection string (contains key=value pairs)
static bool IsConnectionStringImpl(const string &str) {
	// Connection strings have format like "Server=...;Database=..."
	return str.find('=') != string::npos;
}

bool MSSQLConnectionInfo::IsConnectionString(const string &str) {
	return IsConnectionStringImpl(str);
}

//===----------------------------------------------------------------------===//
// Integrated Authentication helpers (Spec 042)
//===----------------------------------------------------------------------===//

// Truthy check for Trusted_Connection / Integrated Security values.
// pyodbc accepts: yes / true / 1 / SSPI (the last only for Integrated Security)
static bool IsTrustedConnectionEnabled(const string &raw_value) {
	auto v = StringUtil::Lower(raw_value);
	return v == "yes" || v == "true" || v == "1" || v == "sspi";
}

// Resolve Trusted_Connection / Integrated Security aliases into an authenticator
// value. On POSIX returns "krb5"; on Windows "winsspi". Returns empty if no
// integrated auth is requested.
//
// The result is the canonical authenticator name; if the user supplied
// `authenticator=...` explicitly, that takes precedence and is returned as-is
// (downcased by the parsers already).
static string ResolveIntegratedAuth(const case_insensitive_map_t<string> &params) {
	auto auth_it = params.find("authenticator");
	if (auth_it != params.end() && !auth_it->second.empty()) {
		return auth_it->second;	 // already lowercased
	}
	auto trusted_it = params.find("trusted_connection");
	if (trusted_it != params.end() && IsTrustedConnectionEnabled(trusted_it->second)) {
#ifdef _WIN32
		return "winsspi";
#else
		return "krb5";
#endif
	}
	auto integ_it = params.find("integrated_security");
	if (integ_it != params.end() && IsTrustedConnectionEnabled(integ_it->second)) {
#ifdef _WIN32
		return "winsspi";
#else
		return "krb5";
#endif
	}
	return "";
}

// Map an authenticator string to the AuthMethod enum. Empty => SQL.
// Throws InvalidInputException for unrecognized values so the user gets a
// clear error instead of a silent fallback to SQL auth.
static AuthMethod AuthenticatorToMethod(const string &name) {
	if (name.empty()) {
		return AuthMethod::SQL;
	}
	if (name == "krb5") {
		return AuthMethod::KRB5;
	}
	if (name == "winsspi") {
		return AuthMethod::WINSSPI;
	}
	throw InvalidInputException(
		"MSSQL Error: Unsupported authenticator '%s'. Supported values: krb5 (POSIX), winsspi (Windows). "
		"For Azure AD use an MSSQL secret with azure_secret or access_token.",
		name);
}

//! Whether `key` is given with a value that is not empty.
static bool HasValue(const case_insensitive_map_t<string> &params, const char *key) {
	auto it = params.find(key);
	return it != params.end() && !it->second.empty();
}

//! Whether `key` (Trusted_Connection or Integrated Security) is given and asks for integrated auth.
static bool AliasEnabled(const case_insensitive_map_t<string> &params, const char *key) {
	auto it = params.find(key);
	return it != params.end() && IsTrustedConnectionEnabled(it->second);
}

//! What integrated auth (`authenticator`) cannot be combined with; empty when nothing conflicts.
static string IntegratedAuthConflict(const case_insensitive_map_t<string> &params, const string &authenticator,
									 bool has_azure) {
	// Was integrated auth requested via the pyodbc-style aliases (Trusted_Connection
	// / Integrated Security), as opposed to the explicit authenticator=... form?
	const bool requested_via_alias =
		AliasEnabled(params, "trusted_connection") || AliasEnabled(params, "integrated_security");
	if (HasValue(params, "password")) {
		return "'Password' cannot be combined with 'Trusted_Connection' / 'authenticator=krb5'. "
			   "Use a Kerberos credential cache (kinit), a keytab via 'krb5-keytabfile', "
			   "or place raw credentials in an MSSQL secret.";
	}
	if (HasValue(params, "user") && requested_via_alias) {
		return "'User Id' cannot be combined with 'Trusted_Connection' / 'Integrated Security'. "
			   "If you need to supply a principal for keytab or raw-credentials mode, use the explicit "
			   "form: authenticator=krb5;User Id=<principal>.";
	}
	if (has_azure) {
		return "'Trusted_Connection' / 'authenticator=krb5' cannot be combined with Azure AD "
			   "authentication (access_token or azure_secret). Choose one auth method.";
	}
#ifdef _WIN32
	if (authenticator == "krb5") {
		return "'authenticator=krb5' is only supported on POSIX. Use 'authenticator=winsspi' or "
			   "'Trusted_Connection=yes' on Windows.";
	}
#else
	if (authenticator == "winsspi") {
		return "'authenticator=winsspi' is only supported on Windows. Use 'authenticator=krb5' or "
			   "'Trusted_Connection=yes' on POSIX.";
	}
#endif
	return "";
}

// Validate conflicts between integrated-auth requests and other credential
// modes. Returns an empty string if no conflict, or an error message otherwise.
//
// Semantics (matches pyodbc / mssql-jdbc):
//   * Trusted_Connection=yes / Integrated Security=SSPI are mutually exclusive
//     with User Id and Password. Users who need a principal name for keytab or
//     raw-creds modes must use the explicit form: authenticator=krb5 + User Id.
//   * authenticator=krb5 / authenticator=winsspi may carry a User Id (interpreted
//     as the Kerberos principal for keytab / raw modes; ignored by cred-cache
//     mode). Password is still rejected -- raw-creds mode must go through an
//     MSSQL secret so the password isn't echoed in connection-string logs.
static string ValidateAuthConflicts(const case_insensitive_map_t<string> &params, bool azure_auth_option) {
	auto authenticator = ResolveIntegratedAuth(params);
	if (!authenticator.empty()) {
		// azure_auth_option: ATTACH-provided access_token / azure_secret
		const string conflict = IntegratedAuthConflict(params, authenticator, azure_auth_option);
		if (!conflict.empty()) {
			return conflict;
		}
	}

	// Trusted_Connection + Integrated Security must agree if both supplied
	auto t_it = params.find("trusted_connection");
	auto i_it = params.find("integrated_security");
	if (t_it != params.end() && i_it != params.end()) {
		bool t_on = IsTrustedConnectionEnabled(t_it->second);
		bool i_on = IsTrustedConnectionEnabled(i_it->second);
		if (t_on != i_on) {
			return "'Trusted_Connection' and 'Integrated Security' specify conflicting values.";
		}
	}

	return "";
}

//! Whether a value is one of the words a flag is written with: yes / true / 1, no / false / 0, any case.
static bool IsYesOrNo(const string &raw) {
	const auto value = StringUtil::Lower(raw);
	return value == "yes" || value == "true" || value == "1" || value == "no" || value == "false" || value == "0";
}

//! The port of a `host,port` server value: an error message when it is not a number from 1 to 65535.
static string ValidateServerPort(const string &server) {
	auto comma_pos = server.find(',');
	if (comma_pos == string::npos) {
		return "";
	}
	// What is refused is said without the text that was read as the port. A URI whose password holds an
	// unencoded '?' is cut there, and the password's start is then where the port is looked for
	// (mssql://u:12ab?cd@host/db: host "u", port "12ab"): quoted, an ATTACH error echoed part of a
	// password, which it must never do (spec 047, T028a; review of #625). The number is not said either:
	// it can be such a password's digits.
	static const char *const NOT_A_NUMBER = "Invalid port in Server parameter: it is not a number from 1 to 65535.";
	auto port_str = server.substr(comma_pos + 1);
	try {
		// The whole value, but for blanks around it: stoi alone takes the digits it finds and leaves the
		// rest, so `Server=host,1433x` dialled port 1433.
		size_t parsed = 0;
		int port = std::stoi(port_str, &parsed);
		if (port_str.find_first_not_of(" \t", parsed) != string::npos) {
			return NOT_A_NUMBER;
		}
		if (port < 1 || port > 65535) {
			return "Port must be between 1 and 65535.";
		}
	} catch (...) {
		return NOT_A_NUMBER;
	}
	return "";
}

string MSSQLConnectionInfo::ValidateConnectionString(const string &connection_string, bool azure_auth) {
	if (connection_string.empty()) {
		return "Connection string cannot be empty.";
	}

	// Parse based on format
	case_insensitive_map_t<string> params = MSSQLParseConnectionParameters(connection_string);

	// Check required fields
	if (params.find("server") == params.end()) {
		return "Missing 'Server' in connection string. Format: Server=host,port;Database=...;User Id=...;Password=...";
	}
	if (params.find("database") == params.end()) {
		return "Missing 'Database' in connection string.";
	}

	// Spec 042: Resolve Integrated Auth aliases and check conflicts before
	// requiring user/password.
	bool integrated_auth = !ResolveIntegratedAuth(params).empty();
	string conflict = ValidateAuthConflicts(params, azure_auth);
	if (!conflict.empty()) {
		return conflict;
	}

	// User/password are only required for SQL authentication
	if (!azure_auth && !integrated_auth) {
		if (params.find("user") == params.end()) {
			return "Missing 'User Id' in connection string.";
		}
		if (params.find("password") == params.end()) {
			return "Missing 'Password' in connection string.";
		}
	}

	// Validate server format (host or host,port)
	const string port_error = ValidateServerPort(params["server"]);
	if (!port_error.empty()) {
		return port_error;
	}

	// TrustServerCertificate turns certificate verification off, so it takes the words that mean yes or no
	// and nothing else. Any other value used to read as yes -- `TrustServerCertificate=off`, an empty
	// value, a typo -- and the connection then accepted any certificate, without a word.
	auto trust = params.find("trustservercertificate");
	if (trust != params.end() && !IsYesOrNo(trust->second)) {
		// The value is not quoted: with the ';' after it missing, what follows -- a Password, say -- is
		// part of it (review of #625).
		return "TrustServerCertificate must be yes or no (also true / false, 1 / 0).";
	}

	return "";	// Valid
}

//! A parameter's value, empty when it is not given.
static string ParamOrEmpty(const case_insensitive_map_t<string> &params, const char *key) {
	auto it = params.find(key);
	return it == params.end() ? string() : it->second;
}

//! A parameter's value into `out` when it is given; `out` keeps its default otherwise.
static void TakeParam(const case_insensitive_map_t<string> &params, const char *key, string &out) {
	auto it = params.find(key);
	if (it != params.end()) {
		out = it->second;
	}
}

//! "no" / "false" / "0" clear a flag; anything else sets it. Encrypt is read so (an unknown word keeps the
//! session encrypted, which is the safe side); TrustServerCertificate only after ValidateConnectionString
//! has refused every word but yes and no.
static bool FlagValue(const string &raw) {
	auto value = StringUtil::Lower(raw);
	return !(value == "no" || value == "false" || value == "0");
}

static void TakeFlag(const case_insensitive_map_t<string> &params, const char *key, bool &out) {
	auto it = params.find(key);
	if (it != params.end()) {
		out = FlagValue(it->second);
	}
}

//! The server (`host,port` or just `host`), its named instance split off (spec 045). The URI parser
//! URL-decodes `%5C` to `\` before this, so both connection-string forms land here. The bare hostname
//! stays in host; the instance name is resolved to a port later by ResolveNamedInstance() at ATTACH time.
static void ParseServer(const string &server, MSSQLConnectionInfo &result) {
	auto comma_pos = server.find(',');
	if (comma_pos != string::npos) {
		result.host = server.substr(0, comma_pos);
		result.port = static_cast<uint16_t>(std::stoi(server.substr(comma_pos + 1)));
		result.port_specified = true;
	} else {
		result.host = server;
		result.port = 1433;	 // Default MSSQL port
	}
	SplitHostInstance(result.host, result.instance_name);
}

shared_ptr<MSSQLConnectionInfo> MSSQLConnectionInfo::FromConnectionString(const string &connection_string,
																		  bool azure_auth) {
	// Validate first
	string error = ValidateConnectionString(connection_string, azure_auth);
	if (!error.empty()) {
		throw InvalidInputException("MSSQL Error: %s", error);
	}

	// Parse based on format
	const case_insensitive_map_t<string> params = MSSQLParseConnectionParameters(connection_string);

	auto result = make_shared_ptr<MSSQLConnectionInfo>();
	ParseServer(ParamOrEmpty(params, "server"), *result);
	result->database = ParamOrEmpty(params, "database");
	result->user = ParamOrEmpty(params, "user");
	result->password = ParamOrEmpty(params, "password");

	// Spec 074: the three TLS options as the Microsoft drivers define them.
	// Encrypt says whether the session is encrypted (default true).
	// TrustServerCertificate says whether the server's certificate is verified
	// against the platform trust store and the connected host name (default
	// false, i.e. verify). HostNameInCertificate is the name the certificate
	// must carry when it differs from the address dialled. They are
	// independent: `Encrypt=true;TrustServerCertificate=false` is the canonical
	// secure string. Under Encrypt=false there is no certificate and the other
	// two are ignored. Absent keys keep the struct defaults.
	TakeFlag(params, "encrypt", result->use_encrypt);
	TakeFlag(params, "trustservercertificate", result->trust_server_certificate);
	TakeParam(params, "hostnameincertificate", result->host_name_in_certificate);

	// Parse optional Catalog parameter (defaults to true)
	// When false, catalog integration is disabled (raw query mode only)
	if (params.find("catalog") != params.end()) {
		auto catalog_val = StringUtil::Lower(ParamOrEmpty(params, "catalog"));
		result->catalog_enabled = (catalog_val == "yes" || catalog_val == "true" || catalog_val == "1");
	}

	// Parse optional catalog visibility filters from connection string (Spec 033)
	TakeParam(params, "schema_filter", result->schema_filter);
	TakeParam(params, "table_filter", result->table_filter);
	TakeParam(params, "transaction_isolation", result->transaction_isolation);
	TakeParam(params, "datetime2_7_as", result->datetime2_7_as);
	TakeParam(params, "default_schema", result->default_schema);

	// Spec 042: Integrated Authentication parameters
	string authenticator = ResolveIntegratedAuth(params);
	if (!authenticator.empty()) {
		result->auth_method = AuthenticatorToMethod(authenticator);
	}
	result->krb5_configfile = ParamOrEmpty(params, "krb5_configfile");
	result->krb5_keytabfile = ParamOrEmpty(params, "krb5_keytabfile");
	result->krb5_credcachefile = ParamOrEmpty(params, "krb5_credcachefile");
	result->krb5_realm = ParamOrEmpty(params, "krb5_realm");
	result->service_principal_name = ParamOrEmpty(params, "service_principal_name");

	// Spec 047 FR-014 (issue #82): custom LOGIN7 program_name. Routed through
	// the canonical `application_name` key by both ADO.NET and URI parsers.
	TakeParam(params, "application_name", result->application_name);

	result->connected = false;
	return result;
}

//===----------------------------------------------------------------------===//
// Named-instance resolution (spec 045)
//===----------------------------------------------------------------------===//

void MSSQLConnectionInfo::ResolveNamedInstance(ClientContext &context) {
	// Nothing to resolve for a default instance, and an explicit port always
	// wins over the Browser-advertised one.
	if (instance_name.empty() || port_specified) {
		return;
	}

	Value val;
	bool enabled = true;
	if (context.TryGetCurrentSetting("mssql_named_instance_resolution", val)) {
		enabled = val.GetValue<bool>();
	}
	if (!enabled) {
		throw InvalidInputException(
			"MSSQL Error: named-instance resolution is disabled; set mssql_named_instance_resolution=true "
			"or connect with an explicit port (Server=host,port). Instance requested: '%s\\%s'",
			host, instance_name);
	}

	int timeout_seconds = 3;
	if (context.TryGetCurrentSetting("mssql_browser_timeout_seconds", val)) {
		timeout_seconds = static_cast<int>(val.GetValue<int64_t>());
	}

	auto resolved = mssql::InstanceResolver::Resolve(host, instance_name, timeout_seconds);
	if (!resolved.ok) {
		// Surface the resolver's typed diagnostic verbatim (it already names the
		// host + instance and distinguishes browser-down from instance-not-found).
		throw IOException("MSSQL Error: %s", resolved.error.message);
	}

	// The Browser may advertise the engine on a different hostname than the one
	// running Browser (failover cluster, two-hostname layouts); honour it.
	if (!resolved.host.empty()) {
		host = resolved.host;
	}
	port = resolved.port;
}

}  // namespace duckdb
