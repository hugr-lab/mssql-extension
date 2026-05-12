//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// auth_strategy_factory.cpp
//
// Factory for creating authentication strategies
// Spec 031: Connection & FEDAUTH Refactoring - US7
//===----------------------------------------------------------------------===//

#include "tds/auth/auth_strategy_factory.hpp"
#include "azure/azure_token.hpp"
#include "mssql_storage.hpp"
#include "tds/auth/integrated_auth_strategy.hpp"

#if defined(MSSQL_ENABLE_KRB5)
#include "tds/auth/krb5_authenticator.hpp"
#endif

#include <string>

namespace duckdb {
namespace tds {

namespace {

// Spec 042 R2: Derive default SPN of the form "MSSQLSvc@<fqdn>". The @ separator
// is critical -- GSS_C_NT_HOSTBASED_SERVICE expects @, not /. The mech rewrites
// internally to "MSSQLSvc/<fqdn>:<port>" before querying the KDC.
//
// If the user overrode service_principal_name explicitly, we honor that
// verbatim -- they may have a custom SPN registered against a specific
// service account.
static std::string DeriveSpn(const MSSQLConnectionInfo &info) {
	if (!info.service_principal_name.empty()) {
		// Accept either MSSQLSvc/host:port (canonical form) or
		// MSSQLSvc@host (already in hostbased form). If the user gave the
		// canonical slash form, rewrite to @ form for GSSAPI.
		const std::string &spn = info.service_principal_name;
		auto slash = spn.find('/');
		if (slash != std::string::npos && spn.find('@') == std::string::npos) {
			// "MSSQLSvc/host:port" -> "MSSQLSvc@host"
			auto host_start = slash + 1;
			auto colon = spn.find(':', host_start);
			std::string host_part = (colon == std::string::npos) ? spn.substr(host_start)
																 : spn.substr(host_start, colon - host_start);
			return spn.substr(0, slash) + "@" + host_part;
		}
		return spn;
	}
	return std::string("MSSQLSvc@") + info.host;
}

}  // namespace

AuthStrategyPtr AuthStrategyFactory::Create(const MSSQLConnectionInfo &conn_info, ClientContext *context) {
	// Spec 042: Integrated Authentication (Kerberos / SSPI) takes precedence
	// over the other paths only when explicitly requested.
	if (conn_info.auth_method == AuthMethod::KRB5) {
#if defined(MSSQL_ENABLE_KRB5)
		Krb5Config kc;
		kc.spn = DeriveSpn(conn_info);
		kc.configfile = conn_info.krb5_configfile;
		kc.keytabfile = conn_info.krb5_keytabfile;
		kc.credcachefile = conn_info.krb5_credcachefile;
		kc.realm = conn_info.krb5_realm;
		kc.raw_username = conn_info.user;	  // raw mode reads user/password from these fields
		kc.raw_password = conn_info.password;  // (validator rejects this combo for Trusted_Connection; allowed via authenticator=krb5)
		if (conn_info.krb5_dnslookupkdc != -1) {
			kc.dns_lookup_kdc_specified = true;
			kc.dns_lookup_kdc_value = (conn_info.krb5_dnslookupkdc != 0);
		}
		auto authn = std::make_shared<Krb5Authenticator>(std::move(kc));
		return std::make_shared<IntegratedAuthStrategy>(std::move(authn), conn_info.database, "IntegratedAuth(krb5)",
														conn_info.use_encrypt);
#else
		throw std::runtime_error(
			"MSSQL Error: This build of the mssql extension was compiled without Kerberos support. "
			"Rebuild with -DENABLE_KRB5=ON or use SQL authentication.");
#endif
	}
	if (conn_info.auth_method == AuthMethod::WINSSPI) {
		// Phase 4 will provide WinSspiAuthenticator. For now, error out cleanly.
		throw std::runtime_error(
			"MSSQL Error: Windows SSPI authentication is not yet implemented in this build (spec 042 Phase 4). "
			"Use SQL authentication or Azure AD in the meantime.");
	}

	// Priority for non-integrated paths: access_token > azure_secret > SQL auth (Spec 032)
	if (!conn_info.access_token.empty()) {
		// Manual token authentication (Spec 032)
		return CreateManualToken(conn_info.access_token, conn_info.database);
	} else if (conn_info.use_azure_auth) {
		// Azure secret-based authentication
		if (!context) {
			throw std::runtime_error("AuthStrategyFactory: ClientContext required for Azure authentication");
		}
		return CreateFedAuth(*context, conn_info.azure_secret_name, conn_info.database, conn_info.host);
	} else {
		// SQL Server authentication
		return CreateSqlAuth(conn_info.user, conn_info.password, conn_info.database, conn_info.use_encrypt);
	}
}

AuthStrategyPtr AuthStrategyFactory::CreateSqlAuth(const std::string &username, const std::string &password,
												   const std::string &database, bool use_encrypt) {
	return std::make_shared<SqlServerAuthStrategy>(username, password, database, use_encrypt);
}

AuthStrategyPtr AuthStrategyFactory::CreateFedAuth(ClientContext &context, const std::string &secret_name,
												   const std::string &database, const std::string &host,
												   const std::string &tenant_override) {
	auto strategy = std::make_shared<FedAuthStrategy>(secret_name, database, host, tenant_override);

	// Set up token acquirer that uses the DuckDB context
	strategy->SetTokenAcquirer(BuildTokenAcquirer(context));

	return strategy;
}

AuthStrategyPtr AuthStrategyFactory::CreateManualToken(const std::string &access_token, const std::string &database) {
	// Spec 032: Create ManualTokenAuthStrategy with pre-provided token
	// Token validation (JWT format, audience, expiration) is done in the constructor
	return std::make_shared<ManualTokenAuthStrategy>(access_token, database);
}

TokenAcquirer AuthStrategyFactory::BuildTokenAcquirer(ClientContext &context) {
	// Capture context by reference - caller must ensure context lifetime
	// In practice, context outlives the connection pool that uses this acquirer
	return [&context](const std::string &secret_name, const std::string &tenant_override) -> std::string {
		// Try to get cached token first
		std::string cached = mssql::azure::TokenCache::Instance().GetToken(secret_name);
		if (!cached.empty()) {
			return cached;
		}

		// Acquire fresh token using DuckDB Azure extension
		auto result = mssql::azure::AcquireToken(context, secret_name, tenant_override);
		if (!result.success) {
			throw std::runtime_error("Azure token acquisition failed: " + result.error_message);
		}
		return result.access_token;
	};
}

}  // namespace tds
}  // namespace duckdb
