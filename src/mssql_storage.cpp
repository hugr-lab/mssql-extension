#include "mssql_storage.hpp"
#include "azure/azure_fedauth.hpp"
#include "azure/azure_token.hpp"
#include "catalog/mssql_catalog.hpp"
#include "catalog/mssql_catalog_filter.hpp"
#include "catalog/mssql_transaction.hpp"
#include "connection/instance_resolver.hpp"
#include "connection/mssql_connection_provider.hpp"
#include "connection/mssql_login_retry.hpp"
#include "connection/mssql_settings.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/operator/cast_operators.hpp"
#include "duckdb/logging/logger.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/secret/secret_manager.hpp"
#include "duckdb/parser/parsed_data/attach_info.hpp"
#include "duckdb/storage/storage_extension.hpp"
#include "duckdb/transaction/transaction_manager.hpp"
#include "mssql_platform.hpp"
#include "tds/auth/auth_strategy_factory.hpp"
#include "tds/tds_connection.hpp"

#include <openssl/crypto.h>

#include <cctype>
#include <cstdlib>

// Debug logging (same pattern as tds_socket.cpp)
static int GetMssqlStorageDebugLevel() {
	static const int level = []() {
		const char *env = std::getenv("MSSQL_DEBUG");
		return env ? std::atoi(env) : 0;
	}();
	return level;
}

#define MSSQL_STORAGE_DEBUG_LOG(lvl, fmt, ...)                           \
	do {                                                                 \
		if (GetMssqlStorageDebugLevel() >= lvl)                          \
			fprintf(stderr, "[MSSQL STORAGE] " fmt "\n", ##__VA_ARGS__); \
	} while (0)

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
// Connection String Parsing
//===----------------------------------------------------------------------===//

// Check if string is a URI format (mssql://...)
static bool IsUriFormatImpl(const string &str) {
	return StringUtil::StartsWith(StringUtil::Lower(str), "mssql://");
}

bool MSSQLConnectionInfo::IsUriFormat(const string &str) {
	return IsUriFormatImpl(str);
}

// Check if string is an ADO.NET connection string (contains key=value pairs)
static bool IsConnectionStringImpl(const string &str) {
	// Connection strings have format like "Server=...;Database=..."
	return str.find('=') != string::npos;
}

bool MSSQLConnectionInfo::IsConnectionString(const string &str) {
	return IsConnectionStringImpl(str);
}

// URL decode a string (handles %XX encoding).
//
// Spec 043 FR-010 / FR-011 / Q1 Clarification: malformed escapes are
// passed through literally (deterministic). A `%` is only consumed when
// followed by EXACTLY TWO hex digits (case-insensitive). Anything else —
// `%`, `%X`, `%XG`, trailing `%` — is emitted verbatim and the parser
// advances one character. The previous sscanf-based implementation
// silently consumed an invalid second character when the first was hex
// (e.g. `%aG` produced byte 0x0a and dropped the `G`); this is fixed
// here. Locale-independent — uses ASCII hex check, no setlocale/sscanf.
static inline bool IsHexDigit(char c) {
	return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
}

static inline int HexVal(char c) {
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'A' && c <= 'F')
		return 10 + (c - 'A');
	return 10 + (c - 'a');	// c in [a..f] guaranteed by caller
}

static string UrlDecode(const string &str) {
	string result;
	result.reserve(str.size());
	for (size_t i = 0; i < str.size();) {
		if (str[i] == '%' && i + 2 < str.size() && IsHexDigit(str[i + 1]) && IsHexDigit(str[i + 2])) {
			result.push_back(static_cast<char>((HexVal(str[i + 1]) << 4) | HexVal(str[i + 2])));
			i += 3;
		} else {
			result.push_back(str[i]);
			i += 1;
		}
	}
	return result;
}

//! A spelling of a connection parameter and the one name the rest of the code reads it by.
struct KeyAlias {
	const char *alias;	// lowercase
	const char *canonical;
};

//! `key` = `value` into `result` under the key's canonical name; a key that is none of `aliases` is kept
//! as written (the map is case-insensitive). An authenticator's value is lowercased (Spec 042: krb5 /
//! winsspi, go-mssqldb's names).
template <size_t N>
static void StoreParameter(case_insensitive_map_t<string> &result, const KeyAlias (&aliases)[N], const string &key,
						   const string &value) {
	const auto lower_key = StringUtil::Lower(key);
	for (auto &alias : aliases) {
		if (lower_key == alias.alias) {
			const bool authenticator = string("authenticator") == alias.canonical;
			result[alias.canonical] = authenticator ? StringUtil::Lower(value) : value;
			return;
		}
	}
	result[key] = value;
}

//! A URI's query parameters. URI keys cannot contain spaces, so the spaced ADO.NET spellings
//! (`application name`, Spec 047 FR-014 / issue #82) are not among them.
static const KeyAlias URI_KEYS[] = {{"encrypt", "encrypt"},
									{"ssl", "encrypt"},
									{"use_ssl", "encrypt"},
									{"trustservercertificate", "trustservercertificate"},
									{"hostnameincertificate", "hostnameincertificate"},
									{"host_name_in_certificate", "hostnameincertificate"},
									{"schema_filter", "schema_filter"},
									{"schemafilter", "schema_filter"},
									{"table_filter", "table_filter"},
									{"tablefilter", "table_filter"},
									{"transaction_isolation", "transaction_isolation"},
									{"transactionisolation", "transaction_isolation"},
									{"datetime2_7_as", "datetime2_7_as"},
									{"datetime27as", "datetime2_7_as"},
									{"default_schema", "default_schema"},
									{"defaultschema", "default_schema"},
									{"authenticator", "authenticator"},
									{"krb5-configfile", "krb5_configfile"},
									{"krb5_configfile", "krb5_configfile"},
									{"krb5-keytabfile", "krb5_keytabfile"},
									{"krb5_keytabfile", "krb5_keytabfile"},
									{"krb5-credcachefile", "krb5_credcachefile"},
									{"krb5_credcachefile", "krb5_credcachefile"},
									{"krb5-realm", "krb5_realm"},
									{"krb5_realm", "krb5_realm"},
									{"service_principal_name", "service_principal_name"},
									{"service-principal-name", "service_principal_name"},
									{"serviceprincipalname", "service_principal_name"},
									{"trusted_connection", "trusted_connection"},
									{"trustedconnection", "trusted_connection"},
									{"trusted-connection", "trusted_connection"},
									{"integrated_security", "integrated_security"},
									{"integratedsecurity", "integrated_security"},
									{"integrated-security", "integrated_security"},
									{"applicationname", "application_name"},
									{"application_name", "application_name"}};

//! `user[:password]@` taken off the front of `rest` (before the last @).
static void ParseUriCredentials(string &rest, case_insensitive_map_t<string> &result) {
	// Use rfind to support passwords containing unencoded '@' characters
	auto at_pos = rest.rfind('@');
	if (at_pos == string::npos) {
		return;
	}
	string user_pass = rest.substr(0, at_pos);
	rest = rest.substr(at_pos + 1);

	auto colon_pos = user_pass.find(':');
	if (colon_pos != string::npos) {
		result["user"] = UrlDecode(user_pass.substr(0, colon_pos));
		result["password"] = UrlDecode(user_pass.substr(colon_pos + 1));
	} else {
		result["user"] = UrlDecode(user_pass);
	}
}

//! `host[:port][/database]`
static void ParseUriHostAndDatabase(const string &rest, case_insensitive_map_t<string> &result) {
	auto slash_pos = rest.find('/');
	string host_port;
	if (slash_pos != string::npos) {
		host_port = rest.substr(0, slash_pos);
		result["database"] = UrlDecode(rest.substr(slash_pos + 1));
	} else {
		host_port = rest;
	}

	// Parse host:port. URL-decode the host so a `%5C`-encoded instance
	// separator (`host%5Cinstance`) becomes `host\instance` and flows through
	// the same `\`-split as the ADO.NET form (spec 045 T021). The port digits
	// carry no reserved characters, so they are left as-is.
	auto colon_pos = host_port.rfind(':');
	if (colon_pos != string::npos) {
		result["server"] = UrlDecode(host_port.substr(0, colon_pos)) + "," + host_port.substr(colon_pos + 1);
	} else {
		result["server"] = UrlDecode(host_port);
	}
}

//! `param=value&...`; a parameter with no '=' is dropped.
static void ParseUriQuery(const string &query_string, case_insensitive_map_t<string> &result) {
	if (query_string.empty()) {
		return;
	}
	for (auto &param : StringUtil::Split(query_string, '&')) {
		auto eq_pos = param.find('=');
		if (eq_pos != string::npos) {
			StoreParameter(result, URI_KEYS, UrlDecode(param.substr(0, eq_pos)), UrlDecode(param.substr(eq_pos + 1)));
		}
	}
}

// Parse URI format: mssql://user:password@host:port/database?param=value
static case_insensitive_map_t<string> ParseUri(const string &uri) {
	case_insensitive_map_t<string> result;

	// Skip "mssql://"
	string rest = uri.substr(8);

	// Extract query parameters first (after ?)
	string query_string;
	auto query_pos = rest.find('?');
	if (query_pos != string::npos) {
		query_string = rest.substr(query_pos + 1);
		rest = rest.substr(0, query_pos);
	}

	ParseUriCredentials(rest, result);
	ParseUriHostAndDatabase(rest, result);
	ParseUriQuery(query_string, result);
	return result;
}

// Tokenize a connection string into `key=value` pairs.
//
// Spec 043 FR-012: honor ADO.NET-style `{...}` quoting around values so
// that a `;` inside a quoted value does not split the pair. Inside braces
// a literal `}` is escaped as `}}`. Outside braces, behavior is the same
// as `Split(...; ';')`. `"..."` / `'...'` quoting is intentionally out of
// scope for spec 043.
//
// Returns trimmed key/value pairs; brace markers are stripped from the
// returned value but every inner character (including `;`, `=`, raw `{`,
// and the post-`}}` literal `}`) is preserved verbatim.
struct ConnStringPair {
	string key;
	string value;
};

static bool IsBlank(char c) {
	return c == ' ' || c == '\t';
}

static void SkipBlanks(const string &cs, size_t &i) {
	while (i < cs.size() && IsBlank(cs[i])) {
		i++;
	}
}

//! The key at `i`, up to its '=' (no quoting on the key side), which is consumed. False for a pair with
//! no '=': skipped, `i` left at the next ';'.
static bool ReadKey(const string &cs, size_t &i, string &key) {
	const size_t n = cs.size();
	const size_t key_start = i;
	while (i < n && cs[i] != '=' && cs[i] != ';') {
		i++;
	}
	if (i >= n || cs[i] != '=') {
		// Malformed pair (no '='); skip to next ';'.
		while (i < n && cs[i] != ';') {
			i++;
		}
		return false;
	}
	key = cs.substr(key_start, i - key_start);
	StringUtil::Trim(key);
	i++;  // consume '='
	return true;
}

//! A brace-quoted value, `i` at its '{': read until the matching unescaped '}' (inside braces, `}}` is a
//! literal `}`), then past trailing blanks and the ';'.
static string ReadBracedValue(const string &cs, size_t &i) {
	const size_t n = cs.size();
	string value;
	i++;  // consume opening '{'
	while (i < n) {
		if (cs[i] == '}') {
			if (i + 1 < n && cs[i + 1] == '}') {
				value.push_back('}');
				i += 2;
				continue;
			}
			i++;  // consume closing '}'
			break;
		}
		value.push_back(cs[i]);
		i++;
	}
	// Skip optional trailing whitespace + ';'.
	SkipBlanks(cs, i);
	if (i < n && cs[i] == ';') {
		i++;
	}
	return value;
}

//! An unquoted value: up to the ';', which is consumed, its surrounding whitespace trimmed.
static string ReadPlainValue(const string &cs, size_t &i) {
	const size_t n = cs.size();
	const size_t val_start = i;
	while (i < n && cs[i] != ';') {
		i++;
	}
	string value = cs.substr(val_start, i - val_start);
	StringUtil::Trim(value);
	if (i < n && cs[i] == ';') {
		i++;
	}
	return value;
}

static std::vector<ConnStringPair> TokenizeConnectionString(const string &cs) {
	std::vector<ConnStringPair> out;
	const size_t n = cs.size();
	size_t i = 0;
	while (i < n) {
		// Skip leading whitespace + stray separators.
		while (i < n && (cs[i] == ';' || IsBlank(cs[i]))) {
			i++;
		}
		if (i >= n) {
			break;
		}
		string key;
		if (!ReadKey(cs, i, key)) {
			continue;
		}
		SkipBlanks(cs, i);
		string value = i < n && cs[i] == '{' ? ReadBracedValue(cs, i) : ReadPlainValue(cs, i);
		if (key.empty()) {
			continue;
		}
		out.push_back({std::move(key), std::move(value)});
	}
	return out;
}

//! An ADO.NET connection string's keys. ADO.NET spells it HostNameInCertificate, ODBC
//! HostnameInCertificate (both lower to one); Trusted_Connection is pyodbc's / mssql-jdbc's alias,
//! Integrated Security ADO.NET's; the application name's spellings (Spec 047 FR-014, issue #82) are
//! ADO.NET's and the underscore form, so one shape works in connection strings, URIs and secrets.
static const KeyAlias CONNECTION_STRING_KEYS[] = {{"server", "server"},
												  {"data source", "server"},
												  {"database", "database"},
												  {"initial catalog", "database"},
												  {"user id", "user"},
												  {"uid", "user"},
												  {"user", "user"},
												  {"password", "password"},
												  {"pwd", "password"},
												  {"encrypt", "encrypt"},
												  {"use encryption for data", "encrypt"},
												  {"trustservercertificate", "trustservercertificate"},
												  {"hostnameincertificate", "hostnameincertificate"},
												  {"schemafilter", "schema_filter"},
												  {"schema_filter", "schema_filter"},
												  {"tablefilter", "table_filter"},
												  {"table_filter", "table_filter"},
												  {"transactionisolation", "transaction_isolation"},
												  {"transaction_isolation", "transaction_isolation"},
												  {"transaction isolation", "transaction_isolation"},
												  {"datetime2_7_as", "datetime2_7_as"},
												  {"datetime27as", "datetime2_7_as"},
												  {"defaultschema", "default_schema"},
												  {"default_schema", "default_schema"},
												  {"default schema", "default_schema"},
												  {"authenticator", "authenticator"},
												  {"krb5-configfile", "krb5_configfile"},
												  {"krb5_configfile", "krb5_configfile"},
												  {"krb5-keytabfile", "krb5_keytabfile"},
												  {"krb5_keytabfile", "krb5_keytabfile"},
												  {"krb5-credcachefile", "krb5_credcachefile"},
												  {"krb5_credcachefile", "krb5_credcachefile"},
												  {"krb5-realm", "krb5_realm"},
												  {"krb5_realm", "krb5_realm"},
												  {"service_principal_name", "service_principal_name"},
												  {"service-principal-name", "service_principal_name"},
												  {"serviceprincipalname", "service_principal_name"},
												  {"service principal name", "service_principal_name"},
												  {"trusted_connection", "trusted_connection"},
												  {"trustedconnection", "trusted_connection"},
												  {"trusted connection", "trusted_connection"},
												  {"trusted-connection", "trusted_connection"},
												  {"integrated_security", "integrated_security"},
												  {"integratedsecurity", "integrated_security"},
												  {"integrated security", "integrated_security"},
												  {"integrated-security", "integrated_security"},
												  {"application name", "application_name"},
												  {"applicationname", "application_name"},
												  {"app name", "application_name"},
												  {"application_name", "application_name"}};

// Parse key=value pairs from connection string
// Format: "Server=host,port;Database=db;User Id=user;Password=pass;Encrypt=yes/no"
// Supports ADO.NET-style {...} value quoting (spec 043 FR-012).
static case_insensitive_map_t<string> ParseConnectionString(const string &connection_string) {
	case_insensitive_map_t<string> result;
	for (auto &pair : TokenizeConnectionString(connection_string)) {
		StoreParameter(result, CONNECTION_STRING_KEYS, pair.key, pair.value);
	}
	return result;
}

//! The parameters of either form: a URI's, or an ADO.NET connection string's.
static case_insensitive_map_t<string> ParseConnectionParameters(const string &connection_string) {
	return IsUriFormatImpl(connection_string) ? ParseUri(connection_string) : ParseConnectionString(connection_string);
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
	auto port_str = server.substr(comma_pos + 1);
	try {
		// The whole value, but for blanks around it: stoi alone takes the digits it finds and leaves the
		// rest, so `Server=host,1433x` dialled port 1433.
		size_t parsed = 0;
		int port = std::stoi(port_str, &parsed);
		if (port_str.find_first_not_of(" \t", parsed) != string::npos) {
			return StringUtil::Format("Invalid port in Server parameter: '%s'", port_str);
		}
		if (port < 1 || port > 65535) {
			return StringUtil::Format("Port must be between 1 and 65535. Got: %d", port);
		}
	} catch (...) {
		return StringUtil::Format("Invalid port in Server parameter: '%s'", port_str);
	}
	return "";
}

string MSSQLConnectionInfo::ValidateConnectionString(const string &connection_string, bool azure_auth) {
	if (connection_string.empty()) {
		return "Connection string cannot be empty.";
	}

	// Parse based on format
	case_insensitive_map_t<string> params = ParseConnectionParameters(connection_string);

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
		return StringUtil::Format("TrustServerCertificate must be yes or no (also true / false, 1 / 0). Got: '%s'",
								  trust->second);
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
	const case_insensitive_map_t<string> params = ParseConnectionParameters(connection_string);

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

//===----------------------------------------------------------------------===//
// Connection Validation
//===----------------------------------------------------------------------===//

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

//===----------------------------------------------------------------------===//
// Storage Extension callbacks
//===----------------------------------------------------------------------===//

// transaction_isolation (issue #331), from whichever source set it, to its
// canonical form: lowercase, words joined by '_' ("READ COMMITTED",
// "read-committed" and "read_committed" are one value). "" and "default" mean
// send nothing, as before the option existed.
static string NormalizeTransactionIsolation(const string &raw) {
	string value = StringUtil::Lower(raw);
	StringUtil::Trim(value);
	for (auto &c : value) {
		if (c == ' ' || c == '-') {
			c = '_';
		}
	}
	if (value.empty() || value == "default") {
		return "";
	}
	static const char *const LEVELS[] = {"read_uncommitted", "read_committed", "repeatable_read",
										 "serializable",	 "snapshot",	   "auto"};
	for (auto level : LEVELS) {
		if (value == level) {
			return value;
		}
	}
	throw InvalidInputException(
		"MSSQL ATTACH error: transaction_isolation '%s' is not one of default, read_uncommitted, read_committed, "
		"repeatable_read, serializable, snapshot, auto",
		raw);
}

// A boolean ATTACH option arrives as a BOOLEAN when it is written as one
// (`lazy_validation true`) and as a VARCHAR when something rendered it as a
// string on the way: DuckLake's METADATA_PARAMETERS is a MAP(VARCHAR, VARCHAR)
// and forwards every value to the inner ATTACH as a quoted string (issue #325).
// Value::GetValue<bool>() reads a VARCHAR through an int8 cast and refuses
// 'true', so a string goes through DuckDB's own string-to-boolean cast instead
// -- the one `SET` uses: true/false, t/f, yes/no, y/n, 1/0, any case.
// A NULL never gets here: DuckDB's binder refuses it for every ATTACH option.
static bool BooleanAttachOption(const string &name, const Value &value) {
	if (value.type().id() != LogicalTypeId::VARCHAR) {
		return value.GetValue<bool>();
	}
	const auto &text = StringValue::Get(value);
	bool result = false;
	if (!TryCast::Operation(string_t(text), result, false)) {
		throw InvalidInputException(
			"MSSQL Error: ATTACH option '%s' expects a boolean (true/false, yes/no, 1/0), got '%s'", name, text);
	}
	return result;
}

// The integer peer (issue #324): `min_connections 4` or, through DuckLake's
// METADATA_PARAMETERS, `'4'`.
static int64_t IntegerAttachOption(const string &name, const Value &value) {
	if (value.type().id() != LogicalTypeId::VARCHAR) {
		return value.GetValue<int64_t>();
	}
	const auto &text = StringValue::Get(value);
	int64_t result = 0;
	if (!TryCast::Operation(string_t(text), result, false)) {
		throw InvalidInputException("MSSQL Error: ATTACH option '%s' expects an integer, got '%s'", name, text);
	}
	return result;
}

//! What the ATTACH's own options say. They are taken out of AttachOptions so DuckDB's StorageOptions does
//! not refuse them as unrecognized. A `_specified` flag tells an option given empty (which clears the
//! connection's own value) from one not given; -1 is "not given" for the switches a setting backs.
struct MSSQLAttachOptions {
	string secret_name;
	string azure_secret_name;
	string azure_tenant_id;	  // Tenant override for the azure secret (interactive auth)
	string access_token;	  // Spec 032: Direct Azure AD JWT token
	string application_name;  // Spec 047 (US-AN): ATTACH-level program_name override
	bool catalog_specified = false;
	bool catalog_enabled = true;
	string schema_filter;  // Spec 033: ATTACH-level schema filter
	bool schema_filter_specified = false;
	string table_filter;  // Spec 033: ATTACH-level table filter
	bool table_filter_specified = false;
	string transaction_isolation;  // Issue #331: ATTACH-level isolation level
	bool transaction_isolation_specified = false;
	string default_schema;	// Issue #322: ATTACH-level default schema
	bool default_schema_specified = false;
	string datetime2_7_as;	// Spec 093 D8: mssql_datetime2_7_as per catalog
	bool datetime2_7_as_specified = false;
	//! Spec 093 T5: what this catalog's geometry / geography columns are read as.
	tds::GeometryAs geometry_as = tds::GeometryAs::Geometry;
	int8_t order_pushdown = -1;	   // Spec 039: ORDER BY pushdown
	int8_t remote_pushdown = -1;   // Spec 079 PR E2: mssql_remote_pushdown per catalog
	int8_t native_types = -1;	   // mssql_catalog_native_types per catalog
	int8_t client_transcode = -1;  // Spec 092: mssql_client_transcode per catalog
	bool lazy_validation = false;  // Spec 047 (US2): opt out of eager creds check
	bool preload = false;		   // Issue #324: preload the catalog at ATTACH
	int64_t min_connections = -1;  // Issue #324: ATTACH-level mssql_min_connections
};

//! Who to connect as, and under what name. False for an option that is not one of these.
static bool TakeIdentityOption(const string &lower_name, const Value &value, MSSQLAttachOptions &out) {
	if (lower_name == "secret") {
		out.secret_name = value.ToString();
	} else if (lower_name == "azure_secret") {
		out.azure_secret_name = value.ToString();
	} else if (lower_name == "azure_tenant_id" || lower_name == "azure_tenant") {
		// Tenant override for the azure secret. The mssql-secret form of this
		// (`azure_tenant_id`) has existed since spec 032; this is the ATTACH
		// peer, and it wins over the secret the same way schema_filter does.
		out.azure_tenant_id = value.ToString();
	} else if (lower_name == "access_token") {
		// Spec 032: Parse ACCESS_TOKEN ATTACH option
		out.access_token = value.ToString();
	} else if (lower_name == "application_name" || lower_name == "applicationname" ||
			   lower_name == "application name" || lower_name == "app name") {
		// Spec 047 (US-AN / FR-014): ATTACH-level override for LOGIN7
		// program_name. Accept the canonical underscore form, the spaceless
		// ADO.NET-style alias, and the two ADO.NET spaced variants. Wins
		// over connection-string / secret value via the precedence pattern
		// established by schema_filter / table_filter (ATTACH > secret).
		out.application_name = value.ToString();
	} else {
		return false;
	}
	return true;
}

//! What the catalog shows and how it reads. False for an option that is not one of these.
static bool TakeCatalogOption(const string &lower_name, const string &name, const Value &value,
							  MSSQLAttachOptions &out) {
	if (lower_name == "catalog") {
		out.catalog_specified = true;
		out.catalog_enabled = BooleanAttachOption(name, value);
	} else if (lower_name == "schema_filter") {
		out.schema_filter = value.ToString();
		out.schema_filter_specified = true;
	} else if (lower_name == "table_filter") {
		out.table_filter = value.ToString();
		out.table_filter_specified = true;
	} else if (lower_name == "transaction_isolation" || lower_name == "transactionisolation") {
		out.transaction_isolation = value.ToString();
		out.transaction_isolation_specified = true;
	} else if (lower_name == "default_schema" || lower_name == "defaultschema") {
		out.default_schema = value.ToString();
		out.default_schema_specified = true;
	} else if (lower_name == "datetime2_7_as") {
		// Spec 093 D8: mssql_datetime2_7_as for this catalog only (DuckLake's
		// METADATA_PARAMETERS reaches ATTACH options, not settings).
		// Validated with the connection's own value, before anything touches the network.
		out.datetime2_7_as = value.ToString();
		out.datetime2_7_as_specified = true;
	} else if (lower_name == "geometry_as") {
		// Spec 093 T5 (D6, DQ-45): what this catalog's geometry / geography columns are read as.
		// An ATTACH option only, as the owner decided; refused by name before the network.
		out.geometry_as = ParseGeometryAs(value.ToString(), "MSSQL ATTACH error: the ATTACH option geometry_as");
	} else {
		return false;
	}
	return true;
}

//! The switches: a setting for this catalog only, or how the ATTACH itself behaves. False for an option
//! that is not one of these.
static bool TakeSwitchOption(const string &lower_name, const string &name, const Value &value,
							 MSSQLAttachOptions &out) {
	if (lower_name == "order_pushdown") {
		out.order_pushdown = BooleanAttachOption(name, value) ? 1 : 0;
	} else if (lower_name == "remote_pushdown") {
		// Spec 079 PR E2: the catalog's own answer to the rewriter, over
		// mssql_remote_pushdown -- what DuckLake's METADATA_PARAMETERS can
		// reach (it forwards ATTACH options, not settings).
		out.remote_pushdown = BooleanAttachOption(name, value) ? 1 : 0;
	} else if (lower_name == "native_types") {
		// mssql_catalog_native_types for this catalog only: the setting is
		// GLOBAL, and DuckLake's METADATA_PARAMETERS reaches ATTACH options.
		out.native_types = BooleanAttachOption(name, value) ? 1 : 0;
	} else if (lower_name == "client_transcode") {
		// Spec 092 D4: mssql_client_transcode for this catalog only. Two ATTACHes
		// of one DSN are how one session measures both forms.
		out.client_transcode = BooleanAttachOption(name, value) ? 1 : 0;
	} else if (lower_name == "lazy_validation" || lower_name == "lazyvalidation") {
		// Spec 047 (US2): suppress the eager TCP+LOGIN7 round trip.
		// Match the ADO.NET-style alias `LazyValidation` (lowercase via
		// StringUtil::Lower) alongside the canonical `lazy_validation`.
		out.lazy_validation = BooleanAttachOption(name, value);
	} else if (lower_name == "preload") {
		out.preload = BooleanAttachOption(name, value);
	} else if (lower_name == "min_connections" || lower_name == "minconnections") {
		out.min_connections = IntegerAttachOption(name, value);
		if (out.min_connections < 0) {
			throw InvalidInputException("MSSQL ATTACH error: min_connections must be 0 or more, got %lld",
										(long long)out.min_connections);
		}
	} else {
		return false;
	}
	return true;
}

//! Every option of ours taken out of `options`, the rest left for DuckDB.
static MSSQLAttachOptions TakeAttachOptions(AttachOptions &options) {
	MSSQLAttachOptions taken;
	for (auto it = options.options.begin(); it != options.options.end();) {
		const auto lower_name = StringUtil::Lower(it->first);
		if (TakeIdentityOption(lower_name, it->second, taken) ||
			TakeCatalogOption(lower_name, it->first, it->second, taken) ||
			TakeSwitchOption(lower_name, it->first, it->second, taken)) {
			it = options.options.erase(it);
		} else {
			++it;
		}
	}
	return taken;
}

//! The contradictions the options alone decide, refused before anything dials: a device-code MFA prompt
//! must not come before an error the options alone decide (review of #386).
static void CheckAttachOptionConflicts(ClientContext &context, const MSSQLAttachOptions &attach) {
	// Issue #324: `preload` asks the server for the whole catalog at ATTACH,
	// which is exactly what lazy_validation refuses -- say so rather than pick
	// one.
	if (attach.preload && attach.lazy_validation) {
		throw InvalidInputException(
			"MSSQL ATTACH error: preload and lazy_validation contradict each other -- "
			"preload reads the whole catalog at ATTACH, lazy_validation defers every "
			"connection to the first query. Drop one of them");
	}
	if (attach.min_connections > 0 && attach.lazy_validation) {
		throw InvalidInputException(
			"MSSQL ATTACH error: min_connections and lazy_validation contradict each other -- "
			"min_connections opens connections at ATTACH, lazy_validation defers every "
			"connection to the first query. Drop one of them");
	}
	// The #380 refusal of a catalog-wide load inside a transaction that has used
	// MSSQL, here with the other decisions the ATTACH's own options make -- before
	// the ATTACH dials, logs in and opens min_connections only to throw them away
	// (review of #386).
	if (attach.preload && ConnectionProvider::HasUsedAnyMSSQLCatalogInTransaction(context)) {
		throw InvalidInputException(
			"MSSQL ATTACH error: preload cannot run inside a transaction that has used an MSSQL catalog -- "
			"the load would wait on that transaction's uncommitted changes. ATTACH outside the transaction, or "
			"drop the preload option");
	}
}

//! The connection info, from the SECRET or from the connection string (the first argument to ATTACH).
//! Spec 047 T020: MSSQLContext / MSSQLContextManager singletons removed; the catalog itself is the
//! per-instance owner of connection_info + pool.
static shared_ptr<MSSQLConnectionInfo> BuildConnectionInfo(ClientContext &context, const string &name,
														   const string &connection_string,
														   const MSSQLAttachOptions &attach) {
	if (!attach.secret_name.empty()) {
		// SECRET provided - use secret-based connection
		return MSSQLConnectionInfo::FromSecret(context, attach.secret_name);
	}
	if (connection_string.empty()) {
		// Neither SECRET nor connection string provided
		throw InvalidInputException(
			"MSSQL Error: Either SECRET or connection string is required for ATTACH.\n"
			"With secret: ATTACH '' AS %s (TYPE mssql, SECRET <secret_name>)\n"
			"With connection string: ATTACH 'Server=host;Database=db;User Id=user;Password=pass' AS %s (TYPE mssql)",
			name, name);
	}
	// Connection string provided - parse it
	// If access_token or azure_secret is provided as option, allow missing user/password
	const bool azure_auth_option = !attach.access_token.empty() || !attach.azure_secret_name.empty();
	auto connection_info = MSSQLConnectionInfo::FromConnectionString(connection_string, azure_auth_option);

	// Set access_token from ATTACH option if provided (Spec 032: takes precedence)
	if (!attach.access_token.empty()) {
		connection_info->access_token = attach.access_token;
		connection_info->use_azure_auth = true;
		connection_info->auth_method = AuthMethod::MANUAL_TOKEN;  // Spec 042: keep enum in sync
	} else if (!attach.azure_secret_name.empty()) {
		// Set azure_secret from ATTACH option if provided
		connection_info->azure_secret_name = attach.azure_secret_name;
		connection_info->use_azure_auth = true;
		connection_info->auth_method = AuthMethod::AZURE_AD;  // Spec 042: keep enum in sync
	}
	return connection_info;
}

//! Catalog visibility filters from ATTACH options (Spec 033): they override connection string and secret
//! values, and a pattern that is no regex is refused.
static void ApplyCatalogFilters(const MSSQLAttachOptions &attach, MSSQLConnectionInfo &connection_info) {
	if (attach.schema_filter_specified) {
		auto error = MSSQLCatalogFilter::ValidatePattern(attach.schema_filter);
		if (!error.empty()) {
			throw InvalidInputException("MSSQL ATTACH error: %s", error);
		}
		connection_info.schema_filter = attach.schema_filter;
	}
	if (attach.table_filter_specified) {
		auto error = MSSQLCatalogFilter::ValidatePattern(attach.table_filter);
		if (!error.empty()) {
			throw InvalidInputException("MSSQL ATTACH error: %s", error);
		}
		connection_info.table_filter = attach.table_filter;
	}
}

// Spec 093 D8: the ATTACH option, then the connection's own (a secret, a connection string, a URI),
// then the setting -- decided before a Browser lookup or an Azure login, since the options
// alone decide it. Empty from any source is unset, and an explicit '' on the ATTACH clears the
// connection's, handing the choice back to the setting (as default_schema's does).
static MSSQLDatetime2Read ResolveDatetime2Read(ClientContext &context, const MSSQLAttachOptions &attach,
											   MSSQLConnectionInfo &connection_info) {
	if (attach.datetime2_7_as_specified) {
		connection_info.datetime2_7_as = attach.datetime2_7_as;
	}
	StringUtil::Trim(connection_info.datetime2_7_as);
	if (!connection_info.datetime2_7_as.empty()) {
		return ParseDatetime2Read(connection_info.datetime2_7_as,
								  attach.datetime2_7_as_specified
									  ? "MSSQL ATTACH error: the ATTACH option datetime2_7_as"
									  : "MSSQL ATTACH error: the datetime2_7_as of the connection's secret, "
										"connection string or URI");
	}
	Value datetime2_7_as;
	if (context.TryGetCurrentSetting("mssql_datetime2_7_as", datetime2_7_as) && !datetime2_7_as.IsNull()) {
		return ParseDatetime2Read(datetime2_7_as.ToString(), "mssql_datetime2_7_as");
	}
	return MSSQLDatetime2Read::Nanoseconds;
}

static void ApplyDefaultSchema(const MSSQLAttachOptions &attach, MSSQLConnectionInfo &connection_info) {
	if (attach.default_schema_specified) {
		connection_info.default_schema = attach.default_schema;
	}
	// Surrounding whitespace is never part of a schema name the user means
	// (review of #379); an empty value -- including an explicit '' on the ATTACH,
	// which clears a secret's -- means the default, `dbo`.
	StringUtil::Trim(connection_info.default_schema);
	// A default schema the schema_filter hides would resolve every unqualified
	// name against a schema the catalog refuses to show: the configuration
	// contradicts itself, so it fails here rather than on the first query.
	// Only an EXPLICIT default is checked (review of #379): an unset one is `dbo`,
	// and refusing a filter that hides `dbo` would break every existing ATTACH
	// that pairs such a filter with fully qualified names.
	if (!connection_info.default_schema.empty() && !connection_info.schema_filter.empty()) {
		MSSQLCatalogFilter filter;
		filter.SetSchemaFilter(connection_info.schema_filter);
		if (!filter.MatchesSchema(connection_info.default_schema)) {
			throw InvalidInputException("MSSQL ATTACH error: default_schema '%s' is hidden by schema_filter '%s'",
										connection_info.default_schema, connection_info.schema_filter);
		}
	}
}

//! The ATTACH's options over what the secret or the connection string said (ATTACH > secret), each
//! checked; all of it before anything touches the network. Returns what a datetime2(7) is read as.
static MSSQLDatetime2Read ApplyAttachOptions(ClientContext &context, const MSSQLAttachOptions &attach,
											 MSSQLConnectionInfo &connection_info) {
	// The tenant override applies to both the SECRET and the connection-string path.
	if (!attach.azure_tenant_id.empty()) {
		connection_info.azure_tenant_id = attach.azure_tenant_id;
		MSSQL_STORAGE_DEBUG_LOG(1, "AZURE_TENANT_ID option from ATTACH: %s", attach.azure_tenant_id.c_str());
	}

	// Apply ORDER BY pushdown option from ATTACH if specified (Spec 039)
	if (attach.order_pushdown >= 0) {
		connection_info.order_pushdown = attach.order_pushdown;
		MSSQL_STORAGE_DEBUG_LOG(1, "ORDER_PUSHDOWN option from ATTACH: %s", attach.order_pushdown ? "true" : "false");
	}

	// Apply CATALOG option from ATTACH if specified (overrides connection string/secret value)
	if (attach.catalog_specified) {
		connection_info.catalog_enabled = attach.catalog_enabled;
		MSSQL_STORAGE_DEBUG_LOG(1, "CATALOG option from ATTACH: %s", attach.catalog_enabled ? "true" : "false");
	}
	// Here, once the secret / connection string / option have settled it, and
	// before validation dials.
	if (attach.preload && !connection_info.catalog_enabled) {
		throw InvalidInputException(
			"MSSQL ATTACH error: preload needs the catalog, and this ATTACH sets catalog false");
	}

	ApplyCatalogFilters(attach, connection_info);
	if (attach.transaction_isolation_specified) {
		connection_info.transaction_isolation = attach.transaction_isolation;
	}
	connection_info.transaction_isolation = NormalizeTransactionIsolation(connection_info.transaction_isolation);
	const MSSQLDatetime2Read datetime2_read = ResolveDatetime2Read(context, attach, connection_info);
	ApplyDefaultSchema(attach, connection_info);
	if (!attach.application_name.empty()) {
		// Spec 047 FR-014: ATTACH-level value wins over connection-string /
		// secret embedded value. ResolveAppName at the auth fan-out applies
		// the 128-UTF-16-code-unit clamp + control-char strip.
		connection_info.application_name = attach.application_name;
	}
	return datetime2_read;
}

//! The pooled connections' login settings, from the session's settings onto the connection info.
static void ApplyLoginSettings(const MSSQLPoolConfig &pool_config, MSSQLConnectionInfo &connection_info) {
	// issue #138 (test-only): carry the LOGIN7 fragmentation boundary on the
	// connection info so both the ATTACH-time validation and the per-connection
	// pool factory (which only sees connection_info) can pass it through to
	// AuthenticateIntegrated. 0 = production default (4096).
	connection_info.login7_max_packet =
		pool_config.login7_max_packet > 0 ? static_cast<size_t>(pool_config.login7_max_packet) : 0;
	// Spec 055: TDS frame size requested at login. Bounds recv()/send() counts in
	// both directions for the whole life of every pooled connection.
	connection_info.tds_packet_size =
		pool_config.tds_packet_size > 0 ? static_cast<size_t>(pool_config.tds_packet_size) : 0;
	// Issue #225: ask for UTF-8 unless the user turned the request off.
	connection_info.utf8_support = pool_config.utf8_support;
	connection_info.vector_support = pool_config.vector_support;
}

//! The FEDAUTH token the pool's logins send, pre-encoded; empty for SQL and integrated authentication.
//! Under lazy_validation the token is still built -- it does not touch SQL Server -- and only the
//! TCP+LOGIN7 round trip is skipped.
static std::vector<uint8_t> BuildFedAuthToken(ClientContext &context, const MSSQLConnectionInfo &connection_info,
											  bool lazy_validation) {
	if (!connection_info.access_token.empty()) {
		// Spec 032: Manual token authentication - validate token format and audience at ATTACH time
		MSSQL_STORAGE_DEBUG_LOG(
			1, "Manual token auth: %s at ATTACH time",
			lazy_validation ? "skipping network validation (lazy_validation=true)" : "validating connection");

		// Create auth strategy - this validates JWT format, audience, and expiration.
		// JWT-shape validation runs even under lazy_validation: it's local and
		// catches obviously-malformed tokens up front (cheap; no network).
		// app_name (spec 047 FR-014) threaded through so the validation-time and
		// pool-time strategies agree on program_name (this strategy is throw-away
		// for the validator's token-encode, but the pool factory in catalog
		// Initialize builds its own with the same resolved name).
		auto auth_strategy = tds::AuthStrategyFactory::CreateManualToken(
			connection_info.access_token, connection_info.database, ResolveAppName(connection_info));

		// Get the pre-encoded UTF-16LE token for pool creation
		tds::FedAuthInfo dummy_info;  // Not used by ManualTokenAuthStrategy
		return auth_strategy->GetFedAuthToken(dummy_info);
	}
	if (connection_info.use_azure_auth) {
		// Validate FEDAUTH connections at ATTACH time (fail-fast).
		// Always acquire the token (it's needed by the pool factory anyway);
		// only the TCP+LOGIN7 verification step is governed by lazy_validation.
		MSSQL_STORAGE_DEBUG_LOG(
			1, "Azure auth: %s at ATTACH time",
			lazy_validation ? "skipping network validation (lazy_validation=true)" : "validating connection");
		// With a ClientContext here and nowhere later: an interactive chain
		// (device code) can prompt only now, and the token it yields is what
		// the pool factory -- which never prompts -- finds in TokenCache.
		if (!lazy_validation) {
			auto token_result =
				mssql::azure::AcquireToken(context, connection_info.azure_secret_name, connection_info.azure_tenant_id);
			if (!token_result.success) {
				throw InvalidInputException("MSSQL Azure AD authentication failed: %s", token_result.error_message);
			}
		}
		// Build FEDAUTH token for pool factory (uses validated credentials when
		// not lazy; on lazy path the token still has to exist for pool fills).
		auto fedauth_data = mssql::azure::BuildFedAuthExtension(context, connection_info.azure_secret_name,
																connection_info.azure_tenant_id);
		return std::move(fedauth_data.token_utf16le);
	}
	if (connection_info.auth_method == AuthMethod::KRB5 || connection_info.auth_method == AuthMethod::WINSSPI) {
		// Spec 042 Phase 3 / 4: validate the integrated-auth connection at ATTACH
		// time so credential / SPN / clock-skew errors surface immediately.
		MSSQL_STORAGE_DEBUG_LOG(
			1, "Integrated Auth: %s at ATTACH time",
			lazy_validation ? "skipping network validation (lazy_validation=true)" : "validating connection");
	}
	return {};
}

// Spec 047: translate MSSQL pool config (DuckDB settings layer) to the
// tds::PoolConfiguration the pool itself consumes. The MSSQLCatalog now
// owns its pool — no MssqlPoolManager singleton, no cross-instance map.
static tds::PoolConfiguration ToTdsPoolConfiguration(const MSSQLPoolConfig &pool_config, int64_t min_connections) {
	tds::PoolConfiguration tds_pool_config;
	tds_pool_config.connection_limit = pool_config.connection_limit;
	tds_pool_config.connection_cache = pool_config.connection_cache;
	tds_pool_config.connection_timeout = pool_config.connection_timeout;
	tds_pool_config.idle_timeout = pool_config.idle_timeout;
	tds_pool_config.min_connections =
		min_connections >= 0 ? static_cast<size_t>(min_connections) : pool_config.min_connections;
	tds_pool_config.acquire_timeout = pool_config.acquire_timeout;
	return tds_pool_config;
}

//! How the catalog starts: whether it validates and prewarms now, and the choices fixed at ATTACH for its
//! life, each the ATTACH's option when given, else the setting.
static MSSQLCatalogStartup BuildCatalogStartup(ClientContext &context, const MSSQLAttachOptions &attach,
											   int validation_timeout_seconds, MSSQLDatetime2Read datetime2_read) {
	// Eager validation (spec 047 FR-011) is the pool's first login (issue #324,
	// review of #386), and mssql_min_connections is opened with it -- unless the
	// ATTACH asked not to touch the server yet.
	MSSQLCatalogStartup startup;
	startup.validate = !attach.lazy_validation;
	startup.validation_timeout_seconds = validation_timeout_seconds;
	startup.prewarm = !attach.lazy_validation;
	if (attach.remote_pushdown >= 0) {
		startup.remote_pushdown = attach.remote_pushdown == 1;
	} else {
		Value remote_pushdown;
		startup.remote_pushdown = context.TryGetCurrentSetting("mssql_remote_pushdown", remote_pushdown) &&
								  !remote_pushdown.IsNull() && remote_pushdown.GetValue<bool>();
	}
	startup.native_types = attach.native_types;
	if (attach.client_transcode >= 0) {
		startup.client_transcode = attach.client_transcode == 1;
	} else {
		Value client_transcode;
		startup.client_transcode = !context.TryGetCurrentSetting("mssql_client_transcode", client_transcode) ||
								   client_transcode.IsNull() || client_transcode.GetValue<bool>();
	}
	startup.datetime2_read = datetime2_read;  // decided before the network
	startup.geometry_as = attach.geometry_as;
	Value dml_platform;
	if (context.TryGetCurrentSetting("mssql_test_dml_platform", dml_platform) && !dml_platform.IsNull()) {
		startup.dml_platform = StringUtil::Lower(dml_platform.ToString());
	}
	return startup;
}

unique_ptr<Catalog> MSSQLAttach(optional_ptr<StorageExtensionInfo> storage_info, ClientContext &context,
								AttachedDatabase &db, const string &name, AttachInfo &info, AttachOptions &options) {
	// Issue #452: before anything is read or dialled. A lazy_validation ATTACH
	// dials at its first query instead, so it is refused here as well.
	MSSQLRequireExternalAccess(context, "ATTACH of '" + name + "' (TYPE mssql)", "connects to SQL Server");

	// The options, and what they and the secret or connection string settle -- all before the network.
	const MSSQLAttachOptions attach = TakeAttachOptions(options);
	CheckAttachOptionConflicts(context, attach);
	shared_ptr<MSSQLConnectionInfo> connection_info = BuildConnectionInfo(context, name, info.path, attach);
	const MSSQLDatetime2Read datetime2_read = ApplyAttachOptions(context, attach, *connection_info);

	// Spec 045: resolve a named instance (host\instance) to its dynamic TCP
	// port via the SQL Server Browser. Runs here, at ATTACH, because the
	// ClientContext — and thus mssql_named_instance_resolution /
	// mssql_browser_timeout_seconds — is available. No-op for default
	// instances and for connection strings that gave an explicit port. Must
	// precede endpoint-type caching and the eager credential check below,
	// both of which read the (now resolved) host/port.
	connection_info->ResolveNamedInstance(context);

	// Cache the endpoint type at ATTACH time: the test is a host-name match and
	// several paths ask repeatedly.
	//
	// This does NOT disable BCP, whatever this comment used to say. The flag's
	// only consumer is MSSQLCatalog::RequiresSingleByteText(), which exists
	// because Fabric Warehouse has no `nvarchar` type at all; through it the flag
	// reaches the CTAS planner, COPY's target types and the collation choice, and
	// nothing else. Grep src/copy/ for it and there is no hit — the bulk-load path
	// has never tested it, so the "fallback to INSERT" it described did not exist.
	//
	// The claim was wrong about Fabric too, not merely unimplemented: `bcp` is
	// supported there as a preview feature (Microsoft's "T-SQL surface area in
	// Fabric Data Warehouse" lists BULK LOAD as unsupported and exempts `bcp`),
	// and INSERT BULK is the protocol `bcp` itself speaks. Our own docs already
	// implied the load runs: the Fabric known-issue says a `#temp` table cannot be
	// a BCP TARGET there, which is a limit only a working load can reach.
	connection_info->is_fabric_endpoint = connection_info->IsFabricEndpoint();
	if (connection_info->is_fabric_endpoint) {
		MSSQL_STORAGE_DEBUG_LOG(1, "Fabric endpoint detected: %s (no nvarchar — single-byte text types)",
								connection_info->host.c_str());
	}

	// Validate connection before creating the catalog so invalid credentials
	// surface immediately rather than at first query.
	//
	// Spec 047 (US2): the eager validation is governed by the
	// `lazy_validation` ATTACH option. When the user opts out (typically
	// container/orchestration startup where ATTACH must succeed even if the
	// SQL Server isn't ready yet), the FEDAUTH and ManualToken paths still
	// pre-build their tokens — those are local-only operations that don't
	// hit the server — and only the actual TCP+LOGIN7 round trip is skipped.
	// Errors will then surface on the first real query (today's pre-US2
	// behaviour).
	//
	// The validation timeout itself is `mssql_attach_validation_timeout`
	// (spec 047 T026), which defaults to `mssql_connection_timeout` so
	// existing deployments see no observable change in the steady state.
	auto pool_config = LoadPoolConfig(context);
	ApplyLoginSettings(pool_config, *connection_info);
	auto attach_validation_timeout = LoadAttachValidationTimeout(context);
	MSSQL_STORAGE_DEBUG_LOG(1, "ATTACH %s: lazy_validation=%s, attach_validation_timeout=%ds", name.c_str(),
							attach.lazy_validation ? "true" : "false", attach_validation_timeout);

	std::vector<uint8_t> fedauth_token_utf16le = BuildFedAuthToken(context, *connection_info, attach.lazy_validation);
	tds::PoolConfiguration tds_pool_config = ToTdsPoolConfiguration(pool_config, attach.min_connections);

	// Create MSSQLCatalog with connection info, pool config, FEDAUTH token,
	// and access mode. The constructor stores them; Initialize() builds the
	// pool inline based on connection_info_->auth_method.
	// options.access_mode is set by DuckDB based on the READ_ONLY option in ATTACH.
	// catalog_enabled flag determines whether schema discovery is available.
	auto catalog_enabled = connection_info->catalog_enabled;
	const MSSQLCatalogStartup startup = BuildCatalogStartup(context, attach, attach_validation_timeout, datetime2_read);
	auto catalog =
		make_uniq<MSSQLCatalog>(db, name, std::move(connection_info), std::move(tds_pool_config),
								std::move(fedauth_token_utf16le), options.access_mode, catalog_enabled, startup);
	{
		// Issue #481: the ATTACH's own logins (the validation, the collation query) retry a
		// refusal Azure SQL calls transient; Ctrl-C ends the wait, and each retry is logged
		// under this context. The prewarm threads have none: they retry, uninterruptibly.
		mssql::LoginContextScope login_scope(&context);
		catalog->Initialize(false);
	}
	if (!catalog->GetPrewarmShortfall().empty()) {
		DUCKDB_LOG_WARNING(context, "mssql ATTACH %s: min_connections not all opened: %s", name,
						   catalog->GetPrewarmShortfall());
	}
	catalog->CheckTransactionIsolation();
	if (attach.preload) {
		idx_t schema_count = 0, table_count = 0, column_count = 0;
		catalog->Preload(context, string(), "preload", schema_count, table_count, column_count);
	}

	return std::move(catalog);
}

unique_ptr<TransactionManager> MSSQLCreateTransactionManager(optional_ptr<StorageExtensionInfo> storage_info,
															 AttachedDatabase &db, Catalog &catalog) {
	// Use custom transaction manager for external MSSQL catalog
	auto &mssql_catalog = catalog.Cast<MSSQLCatalog>();
	return make_uniq<MSSQLTransactionManager>(db, mssql_catalog);
}

//===----------------------------------------------------------------------===//
// Registration
//===----------------------------------------------------------------------===//

void RegisterMSSQLStorageExtension(ExtensionLoader &loader) {
	auto &db = loader.GetDatabaseInstance();
	auto &config = DBConfig::GetConfig(db);

	auto storage_ext = make_shared_ptr<StorageExtension>();
	storage_ext->attach = MSSQLAttach;
	storage_ext->create_transaction_manager = MSSQLCreateTransactionManager;
	StorageExtension::Register(config, "mssql", std::move(storage_ext));
}

}  // namespace duckdb
