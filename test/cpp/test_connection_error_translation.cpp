// test/cpp/test_connection_error_translation.cpp
// Unit tests for TranslateConnectionError (issue #262).
//
// The bug this pins: the translator used to decide "this is a credential
// problem" by looking for the substring "authentication" in the error text --
// but the text it was handed is OUR OWN wrapper, and every login failure this
// extension produces begins "Authentication failed (error N, state S): ...".
// So the arm matched unconditionally and every login-time server error, of any
// cause, was reported as "check username and password". A paused Azure
// serverless database resuming (error 40613) told the user to go check a
// password that was never wrong, and the State byte that issue #164 added to
// disambiguate 18456 was discarded on the way.
//
// These cases run without a server: they call the classifier directly.
//
// Build + run: make test-cpp (this file is in STANDALONE_TEST_SOURCES).

#include <iostream>
#include <string>

#include "mssql_storage.hpp"

using namespace duckdb;

namespace {

int g_checks = 0;

#define CHECK(cond)                                                                                             \
	do {                                                                                                        \
		g_checks++;                                                                                             \
		if (!(cond)) {                                                                                          \
			std::cerr << "ASSERT FAILED: " << #cond << " (" << __FILE__ << ":" << __LINE__ << ")" << std::endl; \
			std::exit(1);                                                                                       \
		}                                                                                                       \
	} while (0)

bool Contains(const string &haystack, const string &needle) {
	return haystack.find(needle) != string::npos;
}

// The reported case. Azure SQL 40613 arrives wrapped in our own "Authentication
// failed (error ...)" prefix, which is exactly why the old substring test
// misfired. Classification must come from the number.
void TestResumingAzureDatabaseIsNotACredentialProblem() {
	const string raw =
		"Authentication failed (error 40613, state 1): Database 'sales' on server "
		"'x.database.windows.net' is not currently available. Please retry the connection later.";
	const string out = MSSQLTranslateConnectionError(raw, "x.database.windows.net", 1433, "azure_sql_admin", "sales",
													 /*server_error=*/40613, /*server_state=*/1);

	CHECK(!Contains(out, "check username and password"));  // the whole point
	CHECK(Contains(out, "not available yet"));
	CHECK(Contains(out, "40613"));
	CHECK(Contains(out, "retry"));
	CHECK(Contains(out, "sales"));
	CHECK(Contains(out, "not currently available"));  // the server's own words survive
	std::cout << "  [ok] 40613 (database resuming) is reported as retryable, not as bad credentials" << std::endl;
}

// 40197 / 40501 / 49918 are the same family: transient Azure conditions.
void TestOtherTransientAzureErrorsAreRetryable() {
	for (uint32_t code : {40197u, 40501u, 49918u}) {
		const string out = MSSQLTranslateConnectionError(
			"Authentication failed (error " + std::to_string(code) + ", state 1): transient", "h", 1433, "u", "db",
			code, 1);
		CHECK(!Contains(out, "check username and password"));
		CHECK(Contains(out, "retry"));
	}
	std::cout << "  [ok] 40197 / 40501 / 49918 also classified as retryable" << std::endl;
}

// 18456 IS a credential-family failure, and the State byte must survive to the
// user -- that is the whole point of issue #164, and the old code threw it away.
void TestLoginFailedKeepsStateByte() {
	const string raw = "Authentication failed (error 18456, state 40): Login failed for user 'app_user'.";
	const string out = MSSQLTranslateConnectionError(raw, "sql1", 1433, "app_user", "reporting", 18456, 40);

	CHECK(Contains(out, "app_user"));
	CHECK(Contains(out, "18456"));
	CHECK(Contains(out, "state 40"));		   // issue #164's byte, preserved
	CHECK(Contains(out, "initial database"));  // and explained
	CHECK(Contains(out, "Login failed for user"));
	std::cout << "  [ok] 18456 keeps the State byte and explains what it means (issue #164)" << std::endl;
}

// A wrong password is state 8 -- same number, different cause, and the message
// must not read identically to the state-40 case above.
void TestWrongPasswordAndInaccessibleDatabaseDiffer() {
	const string a = MSSQLTranslateConnectionError("Authentication failed (error 18456, state 8): Login failed.", "s",
												   1433, "u", "db", 18456, 8);
	const string b = MSSQLTranslateConnectionError("Authentication failed (error 18456, state 40): Login failed.", "s",
												   1433, "u", "db", 18456, 40);
	CHECK(a != b);
	CHECK(Contains(a, "state 8"));
	CHECK(Contains(b, "state 40"));
	std::cout << "  [ok] state 8 and state 40 produce different messages" << std::endl;
}

// 4060 is a database-access failure, not a credential one.
void TestCannotOpenDatabase() {
	const string out = MSSQLTranslateConnectionError(
		"Authentication failed (error 4060, state 1): Cannot open database", "s", 1433, "u", "missing_db", 4060, 1);
	CHECK(Contains(out, "missing_db"));
	CHECK(Contains(out, "check database name and permissions"));
	CHECK(!Contains(out, "check username and password"));
	std::cout << "  [ok] 4060 points at the database, not the password" << std::endl;
}

// An unrecognised server error must still surface its number, state and text
// rather than being flattened into a guess.
void TestUnknownServerErrorSurfacesVerbatim() {
	const string out = MSSQLTranslateConnectionError("Authentication failed (error 12345, state 7): something specific",
													 "s", 1433, "u", "db", 12345, 7);
	CHECK(Contains(out, "12345"));
	CHECK(Contains(out, "7"));
	CHECK(Contains(out, "something specific"));
	CHECK(!Contains(out, "check username and password"));
	std::cout << "  [ok] an unknown server error keeps its number, state and text" << std::endl;
}

// Failures that never reached a login carry no number, and the string
// heuristics still have to work.
void TestNonLoginFailuresStillClassified() {
	const string refused = MSSQLTranslateConnectionError("connect: connection refused", "s", 1433, "u", "db");
	CHECK(Contains(refused, "Connection refused"));

	const string tls = MSSQLTranslateConnectionError("TLS handshake failed: bad record", "s", 1433, "u", "db");
	CHECK(Contains(tls, "TLS handshake failed"));

	const string timeout = MSSQLTranslateConnectionError("connect timed out", "s", 1433, "u", "db");
	CHECK(Contains(timeout, "timed out"));
	std::cout << "  [ok] socket / TLS / timeout failures still classified without a server number" << std::endl;
}

// The server's own "Login failed for user" phrase, with no number available,
// should still give the credential hint -- but the word "authentication" alone
// must never trigger it, since our own wrapper always contains it.
void TestServerPhraseWithoutNumber() {
	const string out = MSSQLTranslateConnectionError("Login failed for user 'sa'.", "s", 1433, "sa", "db");
	CHECK(Contains(out, "check username and password"));

	// This is the shape that used to swallow everything: our wrapper's word,
	// no server number. It must NOT be called a credential problem.
	const string wrapper_only =
		MSSQLTranslateConnectionError("Azure AD authentication failed: token audience rejected", "s", 1433, "u", "db");
	CHECK(!Contains(wrapper_only, "check username and password"));
	CHECK(Contains(wrapper_only, "token audience rejected"));
	std::cout << "  [ok] the server's phrase hints at credentials; our own wrapper's word does not" << std::endl;
}

// The heuristic branches (no server error number) used to return a canned
// sentence and DISCARD the original text. These are the three messages
// AuthenticateWithFedAuth actually sets, and the reason it mattered: two of
// them describe a failure BEFORE any handshake, so "TLS handshake failed" was
// not merely lossy but wrong.
//
// Note the shape of the old assertion this replaces: checking for "TLS
// handshake failed" passed both before and after the fix, because the canned
// string contained it. These assert on the part that was being thrown away.
void TestPreHandshakeAzureFailuresKeepTheirText() {
	const string not_sup = MSSQLTranslateConnectionError(
		"TLS required for Azure AD but server does not support encryption", "az.database.windows.net", 1433, "", "db");
	CHECK(Contains(not_sup, "does not support encryption"));

	const string declined = MSSQLTranslateConnectionError("TLS required for Azure AD but server declined encryption",
														  "az.database.windows.net", 1433, "", "db");
	CHECK(Contains(declined, "declined encryption"));

	// A real handshake failure must keep the OpenSSL reason, which is the only
	// part that says what to change.
	const string handshake = MSSQLTranslateConnectionError("TLS handshake failed: sslv3 alert handshake failure",
														   "az.database.windows.net", 1433, "", "db");
	CHECK(Contains(handshake, "sslv3 alert handshake failure"));

	std::cout << "  [ok] pre-handshake and handshake failures keep the server/OpenSSL text" << std::endl;
}

// Every other heuristic branch had the same defect.
void TestOtherHeuristicBranchesKeepTheirText() {
	CHECK(Contains(MSSQLTranslateConnectionError("connect: connection refused", "s", 1433, "u", "db"),
				   "connection refused"));
	CHECK(Contains(MSSQLTranslateConnectionError("cannot resolve host: no such host", "nope", 1433, "u", "db"),
				   "no such host"));
	CHECK(Contains(MSSQLTranslateConnectionError("connect timed out after 30s", "s", 1433, "u", "db"), "after 30s"));
	CHECK(Contains(MSSQLTranslateConnectionError("certificate verify failed: self signed", "s", 1433, "u", "db"),
				   "self signed"));
	std::cout << "  [ok] refused / DNS / timeout / certificate branches keep the original text" << std::endl;
}

// On the token-authenticated paths the identity comes from the token, so
// info.user is empty and "for user ''" is less informative than nothing.
void TestEmptyUserOmitsTheClause() {
	const string out = MSSQLTranslateConnectionError(
		"Authentication failed (error 18456, state 1): Login failed for user '<token-identified principal>'.",
		"az.database.windows.net", 1433, /*user=*/"", "db", 18456, 1);

	CHECK(!Contains(out, "for user ''"));
	CHECK(Contains(out, "state 1"));					 // the byte still surfaces
	CHECK(Contains(out, "token-identified principal"));	 // and the server's own text
	// A named user still gets the clause.
	const string named = MSSQLTranslateConnectionError("Authentication failed (error 18456, state 8): Login failed.",
													   "s", 1433, "app_user", "db", 18456, 8);
	CHECK(Contains(named, "for user 'app_user'"));
	std::cout << "  [ok] the \"for user\" clause is omitted when the identity came from a token" << std::endl;
}

}  // namespace

int main() {
	std::cout << "test_connection_error_translation (issue #262)" << std::endl;

	TestResumingAzureDatabaseIsNotACredentialProblem();
	TestOtherTransientAzureErrorsAreRetryable();
	TestLoginFailedKeepsStateByte();
	TestWrongPasswordAndInaccessibleDatabaseDiffer();
	TestCannotOpenDatabase();
	TestUnknownServerErrorSurfacesVerbatim();
	TestNonLoginFailuresStillClassified();
	TestServerPhraseWithoutNumber();
	TestPreHandshakeAzureFailuresKeepTheirText();
	TestOtherHeuristicBranchesKeepTheirText();
	TestEmptyUserOmitsTheClause();

	std::cout << "All " << g_checks << " checks passed." << std::endl;
	return 0;
}
