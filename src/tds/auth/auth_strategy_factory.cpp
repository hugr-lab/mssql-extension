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

#if defined(MSSQL_ENABLE_SSPI)
#include "tds/auth/winsspi_authenticator.hpp"
#endif

#include <cstring>
#include <map>
#include <mutex>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

namespace duckdb {
namespace tds {

namespace {

// Spec 042 R2: derive the SPN. SQL Server SPNs in AD are registered in the
// canonical form "MSSQLSvc/<fqdn>:<port>" (or ":<instance>" for named
// instances). Passing the port-less hostbased-service form to GSSAPI -- as
// the previous implementation did -- silently strips the port and fails on
// any KDC that registered only the port-suffixed variant (which includes the
// bundled test KDC and any non-default-port production deployment).
//
// We therefore route through Krb5Authenticator in the "principal-name" form
// (slash + colon + port) whenever we know the port, and fall back to the
// hostbased-service form only when the user explicitly omitted port info.
// The authenticator's gss_import_name call picks the name type based on
// whether the SPN contains a '/' (see krb5_authenticator.cpp).
//
// service_principal_name (if supplied) is honored verbatim -- the user knows
// their AD registration. We accept all three syntaxes:
//   "MSSQLSvc/host:1433"           -> used verbatim, principal-name type
//   "MSSQLSvc/host:1433@REALM"     -> realm trimmed, principal-name type
//   "MSSQLSvc@host"                -> used verbatim, hostbased-service type
static std::string DeriveSpn(const MSSQLConnectionInfo &info) {
	if (!info.service_principal_name.empty()) {
		const std::string &spn = info.service_principal_name;
		auto slash = spn.find('/');
		auto at = spn.find('@');
		if (slash != std::string::npos) {
			// Canonical slash form. If it ends in @REALM, strip the realm --
			// gss_import_name with GSS_KRB5_NT_PRINCIPAL_NAME does its own
			// realm lookup (and double-realm causes KRB5_PARSE_MALFORMED on
			// some MIT versions).
			if (at != std::string::npos && at > slash) {
				return spn.substr(0, at);
			}
			return spn;
		}
		// Hostbased form (@) -- pass through unchanged.
		return spn;
	}
	// No override: the canonical "MSSQLSvc/<fqdn>:<port>" form, shared with the
	// diagnostic functions so they cannot disagree about it (issue #259).
	return DeriveDefaultSpn(info.host, info.port);
}

}  // namespace

std::string ResolveHostForSpn(const std::string &host) {
	if (host.empty()) {
		return host;
	}

	// Memoized because this sits on the pool-refill path: a fresh strategy is
	// built per login attempt and again per routing hop, and getnameinfo is a
	// blocking resolver call. The negative results are the ones that matter --
	// an IP with no PTR record costs the full resolver timeout every time, and
	// the answer is always the same. A host's PTR record changing mid-process
	// therefore needs a restart to be picked up, which is the right trade for a
	// value that is fixed for the life of an ATTACH.
	static std::mutex cache_mutex;
	static std::map<std::string, std::string> cache;
	{
		std::lock_guard<std::mutex> guard(cache_mutex);
		auto it = cache.find(host);
		if (it != cache.end()) {
			return it->second;
		}
	}

	// Only IP literals need resolving. A name is already what AD registers
	// against, and looking it up would risk turning a working SPN into a
	// different one (a CNAME resolving to some other canonical name).
	sockaddr_in v4 = {};
	sockaddr_in6 v6 = {};
	const sockaddr *addr = nullptr;
	socklen_t addr_len = 0;
	if (inet_pton(AF_INET, host.c_str(), &v4.sin_addr) == 1) {
		v4.sin_family = AF_INET;
		addr = reinterpret_cast<const sockaddr *>(&v4);
		addr_len = sizeof(v4);
	} else if (inet_pton(AF_INET6, host.c_str(), &v6.sin6_addr) == 1) {
		v6.sin6_family = AF_INET6;
		addr = reinterpret_cast<const sockaddr *>(&v6);
		addr_len = sizeof(v6);
	} else {
		std::lock_guard<std::mutex> guard(cache_mutex);
		cache[host] = host;	 // already a name; no lookup was performed
		return host;
	}

	char fqdn[NI_MAXHOST] = {0};
	// NI_NAMEREQD: fail rather than hand back the numeric form, which is what
	// getnameinfo does by default and would silently reproduce the bug.
	if (getnameinfo(addr, addr_len, fqdn, sizeof(fqdn), nullptr, 0, NI_NAMEREQD) != 0) {
		std::lock_guard<std::mutex> guard(cache_mutex);
		cache[host] = std::string();
		return std::string();  // caller reports this; see the header
	}
	std::string resolved(fqdn);
	// A trailing dot is legal in DNS and not wanted in an SPN.
	if (!resolved.empty() && resolved.back() == '.') {
		resolved.pop_back();
	}
	{
		std::lock_guard<std::mutex> guard(cache_mutex);
		cache[host] = resolved;
	}
	return resolved;
}

std::string DeriveDefaultSpn(const std::string &host, uint16_t port) {
	const std::string spn_host = ResolveHostForSpn(host);
	if (spn_host.empty()) {
		if (host.empty()) {
			throw InvalidInputException("Cannot derive a Kerberos SPN: no host was supplied.");
		}
		throw InvalidInputException(
			"Cannot derive a Kerberos SPN from the IP address '%s': Active Directory registers SQL Server SPNs "
			"against the host's FQDN, and no reverse DNS record was found for it. Connect by name, add a PTR "
			"record, or pass the SPN explicitly: service_principal_name=MSSQLSvc/<fqdn>:%d",
			host, port);
	}
	return std::string("MSSQLSvc/") + spn_host + ":" + std::to_string(port);
}

AuthStrategyPtr AuthStrategyFactory::Create(const MSSQLConnectionInfo &conn_info, ClientContext *context) {
	// Spec 047 FR-014 / T065: single resolution point for LOGIN7 program_name.
	// 128-char clamp + default fallback inside ResolveAppName; the resolved
	// value is forwarded to every concrete strategy below so the wire format
	// is consistent across SQL / FEDAUTH / manual-token / integrated auth.
	std::string app_name = ResolveAppName(conn_info);

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
		// raw_username / raw_password are populated for keytab-mode principal
		// (User Id is the AD principal) and for raw-credentials mode (Password
		// is the AD password). Raw mode is SECRET-ONLY: the validator in
		// mssql_storage.cpp rejects Password in any connection string for
		// integrated auth, so this path is only reachable when info came from
		// an MSSQL secret. Per spec 042 ultrareview bug_004 the goal is to
		// keep cleartext passwords out of connection-string logs.
		kc.raw_username = conn_info.user;
		kc.raw_password = conn_info.password;
		auto authn = std::make_shared<Krb5Authenticator>(std::move(kc));
		return std::make_shared<IntegratedAuthStrategy>(std::move(authn), conn_info.database, "IntegratedAuth(krb5)",
														conn_info.use_encrypt, app_name);
#else
		throw std::runtime_error(
			"MSSQL Error: This build of the mssql extension was compiled without Kerberos support. "
			"Rebuild with -DENABLE_KRB5=ON or use SQL authentication.");
#endif
	}
	if (conn_info.auth_method == AuthMethod::WINSSPI) {
#if defined(MSSQL_ENABLE_SSPI)
		WinSspiConfig wc;
		wc.spn = DeriveSpn(conn_info);
		auto authn = std::make_shared<WinSspiAuthenticator>(std::move(wc));
		return std::make_shared<IntegratedAuthStrategy>(std::move(authn), conn_info.database, "IntegratedAuth(winsspi)",
														conn_info.use_encrypt, app_name);
#else
		throw std::runtime_error(
			"MSSQL Error: Windows SSPI authentication requires a Windows build of the extension. "
			"On POSIX hosts, use 'authenticator=krb5' (or 'Trusted_Connection=yes' which auto-resolves "
			"to krb5 on POSIX) after running 'kinit'.");
#endif
	}

	// Priority for non-integrated paths: access_token > azure_secret > SQL auth (Spec 032)
	if (!conn_info.access_token.empty()) {
		// Manual token authentication (Spec 032)
		return CreateManualToken(conn_info.access_token, conn_info.database, app_name);
	} else if (conn_info.use_azure_auth) {
		// Azure secret-based authentication
		if (!context) {
			throw std::runtime_error("AuthStrategyFactory: ClientContext required for Azure authentication");
		}
		return CreateFedAuth(*context, conn_info.azure_secret_name, conn_info.database, conn_info.host,
							 /*tenant_override=*/"", app_name);
	} else {
		// SQL Server authentication
		return CreateSqlAuth(conn_info.user, conn_info.password, conn_info.database, conn_info.use_encrypt, app_name);
	}
}

AuthStrategyPtr AuthStrategyFactory::CreateSqlAuth(const std::string &username, const std::string &password,
												   const std::string &database, bool use_encrypt,
												   const std::string &app_name) {
	return std::make_shared<SqlServerAuthStrategy>(username, password, database, use_encrypt, app_name);
}

AuthStrategyPtr AuthStrategyFactory::CreateFedAuth(ClientContext &context, const std::string &secret_name,
												   const std::string &database, const std::string &host,
												   const std::string &tenant_override, const std::string &app_name) {
	// Spec 047 FR-012: strategy holds the DatabaseInstance reference so its
	// InvalidateToken/IsTokenExpired calls hit the correct TokenCache namespace.
	auto strategy =
		std::make_shared<FedAuthStrategy>(*context.db, secret_name, database, host, tenant_override, app_name);

	// Set up token acquirer that uses the DuckDB context
	strategy->SetTokenAcquirer(BuildTokenAcquirer(context));

	return strategy;
}

AuthStrategyPtr AuthStrategyFactory::CreateManualToken(const std::string &access_token, const std::string &database,
													   const std::string &app_name) {
	// Spec 032: Create ManualTokenAuthStrategy with pre-provided token
	// Token validation (JWT format, audience, expiration) is done in the constructor
	return std::make_shared<ManualTokenAuthStrategy>(access_token, database, app_name);
}

TokenAcquirer AuthStrategyFactory::BuildTokenAcquirer(ClientContext &context) {
	// Capture context by reference - caller must ensure context lifetime
	// In practice, context outlives the connection pool that uses this acquirer
	return [&context](const std::string &secret_name, const std::string &tenant_override) -> std::string {
		// Spec 047 FR-012: cache reads are namespaced by DatabaseInstance.
		// Build the same cache key shape AcquireToken uses (secret_name[:tenant]).
		auto &db_instance = *context.db;
		std::string cache_key = secret_name;
		if (!tenant_override.empty()) {
			cache_key += ":" + tenant_override;
		}

		std::string cached = mssql::azure::TokenCache::Instance().GetToken(db_instance, cache_key);
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
