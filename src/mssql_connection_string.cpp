// A connection string or URI read into its parameters: the URI form, the ADO.NET tokenizer with its
// {...} quoting, and the spellings each form takes for a parameter.

#include "mssql_connection_string.hpp"

#include "duckdb/common/string_util.hpp"

namespace duckdb {

// Check if string is a URI format (mssql://...)
bool MSSQLIsUriFormat(const string &str) {
	return StringUtil::StartsWith(StringUtil::Lower(str), "mssql://");
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
case_insensitive_map_t<string> MSSQLParseConnectionParameters(const string &connection_string) {
	return MSSQLIsUriFormat(connection_string) ? ParseUri(connection_string) : ParseConnectionString(connection_string);
}

}  // namespace duckdb
