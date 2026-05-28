//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// krb5_test_function.cpp
//
// mssql_kerberos_auth_test() scalar function -- exercises the POSIX
// Kerberos authentication path without actually connecting to SQL Server.
// Spec 042.
//===----------------------------------------------------------------------===//

#include "tds/auth/krb5_test_function.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/vector_operations/binary_executor.hpp"
#include "mssql_compat.hpp"
#include "duckdb/common/vector_operations/generic_executor.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "mssql_storage.hpp"

#include <string>

#if defined(MSSQL_ENABLE_KRB5)
#include "tds/auth/krb5_authenticator.hpp"

// Pulled in for the principal-name lookup on success. Same headers
// krb5_authenticator.cpp already uses.
#include <gssapi/gssapi.h>
#if !defined(__APPLE__)
#include <krb5.h>
#endif
#endif

namespace duckdb {
namespace mssql {
namespace krb5 {

#if defined(MSSQL_ENABLE_KRB5)

//===----------------------------------------------------------------------===//
// LookupCcachePrincipal - read the default ccache's client principal name.
//
// Best-effort: returns "<unknown>" on any failure since we don't want a
// principal-name lookup failure to mask the actual auth result.
//===----------------------------------------------------------------------===//
static std::string LookupCcachePrincipal() {
#if !defined(__APPLE__)
	krb5_context kctx = nullptr;
	if (krb5_init_context(&kctx) != 0) {
		return "<unknown>";
	}
	krb5_ccache cc = nullptr;
	if (krb5_cc_default(kctx, &cc) != 0) {
		krb5_free_context(kctx);
		return "<unknown>";
	}
	krb5_principal client = nullptr;
	if (krb5_cc_get_principal(kctx, cc, &client) != 0) {
		krb5_cc_close(kctx, cc);
		krb5_free_context(kctx);
		return "<no ticket>";
	}
	char *name = nullptr;
	std::string result = "<unknown>";
	if (krb5_unparse_name(kctx, client, &name) == 0 && name) {
		result.assign(name);
		krb5_free_unparsed_name(kctx, name);
	}
	krb5_free_principal(kctx, client);
	krb5_cc_close(kctx, cc);
	krb5_free_context(kctx);
	return result;
#else
	// macOS GSS framework lacks the krb5_cc_* extensions in the public SDK.
	// On macOS the user can run `klist` from the shell to see the principal.
	return "<macOS: run klist>";
#endif
}

//===----------------------------------------------------------------------===//
// RunKrb5Test - the shared core
//
// Constructs a Krb5Authenticator with the supplied config, runs InitialBytes()
// (which acquires creds + imports SPN + calls gss_init_sec_context), returns
// a one-line status string. Catches std::exception so the verbatim "MSSQL
// Kerberos auth failed: ..." message from Krb5Authenticator surfaces to the
// caller verbatim instead of being thrown as a DuckDB error.
//===----------------------------------------------------------------------===//
static std::string RunKrb5Test(tds::Krb5Config config) {
	try {
		tds::Krb5Authenticator authn(std::move(config));
		auto blob = authn.InitialBytes();
		std::string principal = LookupCcachePrincipal();
		return "OK: principal=" + principal + ", spn=" + authn.GetSpn() +
			   ", mech=SPNEGO, token_size=" + std::to_string(blob.size()) + " bytes";
	} catch (const std::exception &e) {
		return std::string(e.what());
	}
}

//===----------------------------------------------------------------------===//
// DeriveDefaultSpn - same logic as auth_strategy_factory.cpp's DeriveSpn
// for the (host, port) form. Kept local here to avoid a circular include.
//===----------------------------------------------------------------------===//
static std::string DeriveDefaultSpn(const std::string &host, uint16_t port) {
	return "MSSQLSvc/" + host + ":" + std::to_string(port);
}

//===----------------------------------------------------------------------===//
// Krb5TestHost - mssql_kerberos_auth_test(host VARCHAR) -> VARCHAR
//===----------------------------------------------------------------------===//
static void Krb5TestHost(DataChunk &args, ExpressionState & /*state*/, Vector &result) {
	auto &host_vec = args.data[0];
	UnaryExecutor::Execute<string_t, string_t>(host_vec, result, args.size(), [&](string_t host) {
		std::string h = host.GetString();
		tds::Krb5Config cfg;
		cfg.spn = DeriveDefaultSpn(h, 1433);
		return StringVector::AddString(result, RunKrb5Test(std::move(cfg)));
	});
}

//===----------------------------------------------------------------------===//
// Krb5TestHostPort - mssql_kerberos_auth_test(host VARCHAR, port INTEGER) -> VARCHAR
//===----------------------------------------------------------------------===//
static void Krb5TestHostPort(DataChunk &args, ExpressionState & /*state*/, Vector &result) {
	auto &host_vec = args.data[0];
	auto &port_vec = args.data[1];
	BinaryExecutor::Execute<string_t, int32_t, string_t>(
		host_vec, port_vec, result, args.size(), [&](string_t host, int32_t port) {
			std::string h = host.GetString();
			tds::Krb5Config cfg;
			cfg.spn = DeriveDefaultSpn(h, static_cast<uint16_t>(port));
			return StringVector::AddString(result, RunKrb5Test(std::move(cfg)));
		});
}

//===----------------------------------------------------------------------===//
// Krb5TestSecret - mssql_kerberos_auth_test_secret(secret_name VARCHAR) -> VARCHAR
//
// Reads host / port / authenticator / krb5_* / service_principal_name from
// the MSSQL secret. Exercises the same Krb5Config that ATTACH would build.
//===----------------------------------------------------------------------===//
static void Krb5TestSecret(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &secret_vec = args.data[0];
	UnaryExecutor::Execute<string_t, string_t>(secret_vec, result, args.size(), [&](string_t secret_name) {
		auto &context = state.GetContext();
		std::string name = secret_name.GetString();

		// Build a MSSQLConnectionInfo from the secret (same path ATTACH uses)
		// then construct Krb5Config like AuthStrategyFactory::Create does.
		// (Uses duckdb::shared_ptr, exposed via the FromSecret return type.)
		decltype(MSSQLConnectionInfo::FromSecret(context, name)) info;
		try {
			info = MSSQLConnectionInfo::FromSecret(context, name);
		} catch (const std::exception &e) {
			return StringVector::AddString(result, std::string("MSSQL Kerberos auth test: ") + e.what());
		}
		if (info->auth_method != AuthMethod::KRB5) {
			return StringVector::AddString(result, std::string("MSSQL Kerberos auth test: secret '") + name +
													   "' is not configured for Kerberos (authenticator != 'krb5'). "
													   "Add authenticator 'krb5' to the secret.");
		}

		tds::Krb5Config cfg;
		// Honor service_principal_name override exactly the way the factory does.
		if (!info->service_principal_name.empty()) {
			const std::string &spn = info->service_principal_name;
			auto slash = spn.find('/');
			auto at = spn.find('@');
			if (slash != std::string::npos && at != std::string::npos && at > slash) {
				cfg.spn = spn.substr(0, at);  // strip @REALM
			} else {
				cfg.spn = spn;
			}
		} else {
			cfg.spn = DeriveDefaultSpn(info->host, info->port);
		}
		cfg.configfile = info->krb5_configfile;
		cfg.keytabfile = info->krb5_keytabfile;
		cfg.credcachefile = info->krb5_credcachefile;
		cfg.realm = info->krb5_realm;
		cfg.raw_username = info->user;
		cfg.raw_password = info->password;

		return StringVector::AddString(result, RunKrb5Test(std::move(cfg)));
	});
}

#else  // !MSSQL_ENABLE_KRB5

//===----------------------------------------------------------------------===//
// Stub implementations when the extension was built without Kerberos
//===----------------------------------------------------------------------===//

static const char *kNoKrb5Msg =
	"MSSQL Kerberos auth test: this build of the mssql extension was compiled without "
	"Kerberos support (MSSQL_ENABLE_KRB5 was not defined). Rebuild with -DENABLE_KRB5=ON.";

static void Krb5TestHost(DataChunk &args, ExpressionState & /*state*/, Vector &result) {
	auto &host_vec = args.data[0];
	UnaryExecutor::Execute<string_t, string_t>(host_vec, result, args.size(),
											   [&](string_t) { return StringVector::AddString(result, kNoKrb5Msg); });
}
static void Krb5TestHostPort(DataChunk &args, ExpressionState & /*state*/, Vector &result) {
	auto &host_vec = args.data[0];
	auto &port_vec = args.data[1];
	BinaryExecutor::Execute<string_t, int32_t, string_t>(
		host_vec, port_vec, result, args.size(),
		[&](string_t, int32_t) { return StringVector::AddString(result, kNoKrb5Msg); });
}
static void Krb5TestSecret(DataChunk &args, ExpressionState & /*state*/, Vector &result) {
	auto &secret_vec = args.data[0];
	UnaryExecutor::Execute<string_t, string_t>(secret_vec, result, args.size(),
											   [&](string_t) { return StringVector::AddString(result, kNoKrb5Msg); });
}

#endif	// MSSQL_ENABLE_KRB5

void RegisterKrb5TestFunction(ExtensionLoader &loader) {
	// mssql_kerberos_auth_test(host)
	ScalarFunction func1("mssql_kerberos_auth_test", {LogicalType::VARCHAR}, LogicalType::VARCHAR, Krb5TestHost);
	loader.RegisterFunction(func1);

	// mssql_kerberos_auth_test(host, port)
	ScalarFunction func2("mssql_kerberos_auth_test", {LogicalType::VARCHAR, LogicalType::INTEGER}, LogicalType::VARCHAR,
						 Krb5TestHostPort);
	loader.RegisterFunction(func2);

	// mssql_kerberos_auth_test_secret(secret_name)
	ScalarFunction func3("mssql_kerberos_auth_test_secret", {LogicalType::VARCHAR}, LogicalType::VARCHAR,
						 Krb5TestSecret);
	loader.RegisterFunction(func3);
}

}  // namespace krb5
}  // namespace mssql
}  // namespace duckdb
