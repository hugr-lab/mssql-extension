// test/cpp/test_connection_string.cpp
//
// MSSQLConnectionInfo's connection-string and URI parsers (ValidateConnectionString,
// FromConnectionString), with no server and no DuckDB instance: they are pure functions of the string.
//
// Two regressions are pinned here, both found reading the parser:
//   * TrustServerCertificate read every value but no / false / 0 as yes, so `TrustServerCertificate=off`,
//     an empty value or a typo turned certificate verification OFF without a word;
//   * the port was read with stoi, which takes the digits it finds: `Server=host,1433x` dialled 1433.
// The rest pins what the parsers already did, which had no test of its own.
//
// Part of STANDALONE_TEST_SOURCES (`make test-cpp`), which CI runs.

#include <iostream>
#include <string>

#include "mssql_storage.hpp"

using namespace duckdb;

namespace {

int failures = 0;

void Check(bool ok, const std::string &what) {
	std::cout << (ok ? "  ok   " : "  FAIL ") << what << std::endl;
	if (!ok) {
		failures++;
	}
}

const std::string BASE = "Server=db.example.com,1433;Database=sales;User Id=u;Password=p";

//! The connection info of a string that must parse.
shared_ptr<MSSQLConnectionInfo> Parse(const std::string &connection_string, bool azure = false) {
	try {
		return MSSQLConnectionInfo::FromConnectionString(connection_string, azure);
	} catch (std::exception &e) {
		std::cout << "  FAIL " << connection_string << " was refused: " << e.what() << std::endl;
		failures++;
		return make_shared_ptr<MSSQLConnectionInfo>();
	}
}

//! What a string is refused with; empty when it is taken.
std::string Refusal(const std::string &connection_string, bool azure = false) {
	return MSSQLConnectionInfo::ValidateConnectionString(connection_string, azure);
}

void TestTrustServerCertificate() {
	std::cout << "TrustServerCertificate" << std::endl;
	Check(!Parse(BASE)->trust_server_certificate, "absent: the certificate is verified");
	for (const char *yes : {"yes", "YES", "true", "True", "1"}) {
		Check(Parse(BASE + ";TrustServerCertificate=" + yes)->trust_server_certificate,
			  std::string("TrustServerCertificate=") + yes + " trusts");
	}
	for (const char *no : {"no", "No", "false", "FALSE", "0"}) {
		Check(!Parse(BASE + ";TrustServerCertificate=" + no)->trust_server_certificate,
			  std::string("TrustServerCertificate=") + no + " verifies");
	}
	// Regression: each of these read as yes.
	for (const char *other : {"off", "", "disabled", "n", "f", "nope", "ture", "2"}) {
		const auto refusal = Refusal(BASE + ";TrustServerCertificate=" + other);
		Check(refusal.find("TrustServerCertificate must be yes or no") == 0 &&
				  refusal.find(std::string("'") + other + "'") != std::string::npos,
			  std::string("TrustServerCertificate=") + other + " is refused, naming the value (" + refusal + ")");
	}
	Check(Refusal("mssql://u:p@h:1433/d?trustservercertificate=off").find("TrustServerCertificate must be") == 0,
		  "in a URI too");
	Check(Parse("mssql://u:p@h:1433/d?trustservercertificate=true")->trust_server_certificate &&
			  !Parse("mssql://u:p@h:1433/d?TrustServerCertificate=false")->trust_server_certificate,
		  "and a URI's yes and no are read");
	bool threw = false;
	try {
		MSSQLConnectionInfo::FromConnectionString(BASE + ";TrustServerCertificate=off");
	} catch (std::exception &e) {
		threw = std::string(e.what()).find("TrustServerCertificate must be yes or no") != std::string::npos;
	}
	Check(threw, "FromConnectionString refuses it as ValidateConnectionString words it");
	// Encrypt keeps its reading: an unknown word leaves the session encrypted.
	Check(Parse(BASE + ";Encrypt=off")->use_encrypt && Parse(BASE + ";Encrypt=strict")->use_encrypt &&
			  !Parse(BASE + ";Encrypt=no")->use_encrypt && !Parse(BASE + ";Encrypt=false")->use_encrypt,
		  "Encrypt: no / false turn it off, any other word keeps it on");
}

void TestPort() {
	std::cout << "Port" << std::endl;
	auto with_port = Parse(BASE);
	Check(with_port->host == "db.example.com" && with_port->port == 1433 && with_port->port_specified,
		  "host,port is split, the port noted as given");
	auto without = Parse("Server=h;Database=d;User Id=u;Password=p");
	Check(without->port == 1433 && !without->port_specified, "no port: 1433, noted as not given");
	Check(Parse("Server= h , 1433 ;Database=d;User Id=u;Password=p")->port == 1433, "blanks around the port are taken");
	// Regression: stoi took "1433" and left the rest.
	for (const char *bad : {"1433x", "14 33", "1433.5", "1433;"}) {
		const auto refusal = Refusal(std::string("Server={h,") + bad + "};Database=d;User Id=u;Password=p");
		Check(refusal.find("Invalid port in Server parameter") == 0,
			  std::string("port '") + bad + "' is refused (" + refusal + ")");
	}
	Check(Refusal("mssql://u:p@h:1433x/d").find("Invalid port in Server parameter") == 0, "in a URI too");
	// An IPv6 literal with no port is split at its last ':' -- "[:" and "1]" -- and used to dial host "[:" on
	// port 1. Refused now; with a port (mssql://u:p@[::1]:1433/d) it has always been read.
	Check(Refusal("mssql://u:p@[::1]/d").find("Invalid port in Server parameter") == 0 &&
			  Parse("mssql://u:p@[::1]:1433/d")->port == 1433,
		  "an IPv6 literal needs its port");
	Check(Refusal("Server=h,0;Database=d;User Id=u;Password=p") == "Port must be between 1 and 65535. Got: 0" &&
			  Refusal("Server=h,70000;Database=d;User Id=u;Password=p") ==
				  "Port must be between 1 and 65535. Got: 70000" &&
			  Refusal("Server=h,-1;Database=d;User Id=u;Password=p") == "Port must be between 1 and 65535. Got: -1",
		  "a number outside 1-65535 is refused by its range, as before");
	Check(Refusal("Server=h,abc;Database=d;User Id=u;Password=p") == "Invalid port in Server parameter: 'abc'" &&
			  Refusal("Server=h,;Database=d;User Id=u;Password=p") == "Invalid port in Server parameter: ''",
		  "no number at all is refused, as before");
}

void TestForms() {
	std::cout << "Keys, quoting and the URI form" << std::endl;
	auto aliases = Parse(
		"Data Source=h\\INST;Initial Catalog=d;UID=u;PWD=p;Application Name=My App;Default Schema=s;"
		"SchemaFilter=^a;TableFilter=b;Transaction Isolation=snapshot;HostNameInCertificate=c.example.com");
	Check(aliases->host == "h" && aliases->instance_name == "INST" && aliases->database == "d" &&
			  aliases->user == "u" && aliases->password == "p" && aliases->application_name == "My App" &&
			  aliases->default_schema == "s" && aliases->schema_filter == "^a" && aliases->table_filter == "b" &&
			  aliases->transaction_isolation == "snapshot" && aliases->host_name_in_certificate == "c.example.com",
		  "the ADO.NET spellings land in their fields, a named instance split off the host");
	auto quoted = Parse("Server=h;Database=d;User Id=u;Password={p;w=}}x}");
	Check(quoted->password == "p;w=}x", "a brace-quoted value keeps its ';' and '=', '}}' is a '}'");
	auto uri = Parse(
		"mssql://us%40er:p%3Aw@host%5CINST:1500/my%20db?encrypt=false&applicationname=a%20b&schema_filter=%5Edbo");
	Check(uri->user == "us@er" && uri->password == "p:w" && uri->host == "host" && uri->instance_name == "INST" &&
			  uri->port == 1500 && uri->database == "my db" && !uri->use_encrypt && uri->application_name == "a b" &&
			  uri->schema_filter == "^dbo",
		  "a URI's parts are URL-decoded and its parameters read");
	Check(Parse("mssql://u:p@ss@h/d")->password == "p@ss", "a URI's password may hold an unencoded '@'");
	Check(Refusal("") == "Connection string cannot be empty." &&
			  Refusal("Database=d;User Id=u;Password=p").find("Missing 'Server'") == 0 &&
			  Refusal("Server=h;User Id=u;Password=p") == "Missing 'Database' in connection string." &&
			  Refusal("Server=h;Database=d;Password=p") == "Missing 'User Id' in connection string." &&
			  Refusal("Server=h;Database=d;User Id=u") == "Missing 'Password' in connection string.",
		  "what is missing is named");
	Check(Refusal("Server=h;Database=d", true).empty(), "with Azure AD authentication no user or password is needed");
	Check(Parse("Server=h;Database=d;Trusted_Connection=yes")->auth_method != AuthMethod::SQL &&
			  Refusal("Server=h;Database=d;Trusted_Connection=yes;Password=p").find("'Password' cannot be combined") ==
				  0 &&
			  Refusal("Server=h;Database=d;authenticator=bogus").empty(),
		  "integrated authentication needs no password and refuses one");
	bool threw = false;
	try {
		MSSQLConnectionInfo::FromConnectionString("Server=h;Database=d;authenticator=bogus");
	} catch (std::exception &e) {
		threw = std::string(e.what()).find("Unsupported authenticator 'bogus'") != std::string::npos;
	}
	Check(threw, "an authenticator that is neither krb5 nor winsspi is refused by name");
}

}  // namespace

int main() {
	TestTrustServerCertificate();
	TestPort();
	TestForms();
	std::cout << (failures ? "FAILED" : "all passed") << std::endl;
	return failures ? 1 : 0;
}
