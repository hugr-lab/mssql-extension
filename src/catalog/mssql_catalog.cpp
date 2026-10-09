#include "catalog/mssql_catalog.hpp"

#include <openssl/crypto.h>
#include <cctype>
#include <cstdlib>
#include <unordered_map>
#include "catalog/mssql_transaction.hpp"
#include "codec/target_string_type.hpp"
#include "duckdb/transaction/meta_transaction.hpp"

#include "azure/azure_fedauth.hpp"
#include "azure/azure_token.hpp"
#include "azure/jwt_parser.hpp"
#include "catalog/mssql_bind_anchors.hpp"
#include "catalog/mssql_ddl_translator.hpp"
#include "catalog/mssql_schema_entry.hpp"
#include "catalog/mssql_statistics.hpp"
#include "catalog/mssql_table_entry.hpp"
#include "connection/mssql_connection_provider.hpp"
#include "connection/mssql_settings.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/uuid.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/parser/parsed_data/create_schema_info.hpp"
#include "duckdb/parser/parsed_data/drop_info.hpp"
#include "duckdb/planner/parsed_data/bound_create_table_info.hpp"
#include "mssql_storage.hpp"
#include "query/mssql_simple_query.hpp"
#include "tds/auth/auth_strategy_factory.hpp"

#include <cstdio>

namespace duckdb {

//===----------------------------------------------------------------------===//
// SQL Query for Database Collation
//===----------------------------------------------------------------------===//

// The database's collation and, since issue #361, the code page it stores
// varchar in: a varchar PARAMETER of an sp_executesql batch takes this page,
// so the filter encoder needs it to decide varchar against nvarchar.
static const char *DATABASE_COLLATION_SQL =
	"SELECT CAST(DATABASEPROPERTYEX(DB_NAME(), 'Collation') AS NVARCHAR(128)) AS db_collation, "
	"CAST(COLLATIONPROPERTY(CAST(DATABASEPROPERTYEX(DB_NAME(), 'Collation') AS NVARCHAR(128)), 'CodePage') AS INT) "
	"AS code_page, "
	// Spec 080 D0: whether the server has IS NOT DISTINCT FROM. Always present,
	// so the snapshot column below stays at a fixed position.
	"CAST(SERVERPROPERTY('EngineEdition') AS INT) AS engine_edition, "
	"TRY_CAST(CAST(SERVERPROPERTY('ProductMajorVersion') AS NVARCHAR(16)) AS INT) AS product_major_version";

// The query before spec 080, the fallback when the one above fails: a platform
// that refuses a server property must not fail an ATTACH or lose the collation.
static const char *DATABASE_COLLATION_PLAIN_SQL =
	"SELECT CAST(DATABASEPROPERTYEX(DB_NAME(), 'Collation') AS NVARCHAR(128)) AS db_collation, "
	"CAST(COLLATIONPROPERTY(CAST(DATABASEPROPERTYEX(DB_NAME(), 'Collation') AS NVARCHAR(128)), 'CodePage') AS INT) "
	"AS code_page";

// Issue #331: whether SNAPSHOT isolation is allowed here, appended to the
// collation query only when transaction_isolation needs the answer (see
// QueryDatabaseCollation). Only sys.databases has it (DATABASEPROPERTYEX has no
// such property), and a principal sees the row of a database it can connect to
// without any grant. NULL -- row not visible -- reads as unknown, and unknown
// means `auto` sends nothing.
static const char *SNAPSHOT_STATE_COLUMN =
	", (SELECT CAST(snapshot_isolation_state AS INT) FROM sys.databases WHERE database_id = DB_ID()) "
	"AS snapshot_isolation_state";

//===----------------------------------------------------------------------===//
// Constructor / Destructor
//===----------------------------------------------------------------------===//

MSSQLCatalog::MSSQLCatalog(AttachedDatabase &db, const string &context_name,
						   shared_ptr<MSSQLConnectionInfo> connection_info, tds::PoolConfiguration pool_config,
						   std::vector<uint8_t> fedauth_token_utf16le, AccessMode access_mode, bool catalog_enabled,
						   MSSQLCatalogStartup startup)
	: Catalog(db),
	  context_name_(context_name),
	  connection_info_(std::move(connection_info)),
	  pool_config_(std::move(pool_config)),
	  fedauth_token_utf16le_(std::move(fedauth_token_utf16le)),
	  access_mode_(access_mode),
	  startup_(startup),
	  connect_timeout_(std::make_shared<std::atomic<int>>(pool_config_.connection_timeout)),
	  catalog_enabled_(catalog_enabled),
	  default_schema_(connection_info_ && !connection_info_->default_schema.empty() ? connection_info_->default_schema
																					: string("dbo")) {
	// Create metadata cache with TTL from settings (0 = manual refresh only)
	int64_t cache_ttl = 0;	// Default: manual refresh only
	metadata_cache_ = make_uniq<MSSQLMetadataCache>(cache_ttl);

	// Configure catalog visibility filters from connection info (Spec 033)
	if (!connection_info_->schema_filter.empty()) {
		catalog_filter_.SetSchemaFilter(connection_info_->schema_filter);
	}
	if (!connection_info_->table_filter.empty()) {
		catalog_filter_.SetTableFilter(connection_info_->table_filter);
	}
	if (catalog_filter_.HasFilters()) {
		metadata_cache_->SetFilter(&catalog_filter_);
	}
	// Spec 084 D1: the whole-catalog row counts from one DMV pass, except where
	// the DMV and table variables are unverified.
	metadata_cache_->SetRowCountPass(!connection_info_->IsFabricEndpoint() && !connection_info_->IsSynapseEndpoint());

	// Create statistics provider with default TTL (will be configured from settings later)
	statistics_provider_ = make_uniq<MSSQLStatisticsProvider>();
}

MSSQLCatalog::~MSSQLCatalog() noexcept {
	// Wipe the cached FEDAUTH token (UTF-16LE bytes) on catalog teardown so
	// the bearer credential does not linger in heap-recycled memory. Same
	// rationale as MSSQLConnectionInfo::~MSSQLConnectionInfo — use
	// OPENSSL_cleanse to defeat dead-store elimination.
	if (!fedauth_token_utf16le_.empty()) {
		OPENSSL_cleanse(fedauth_token_utf16le_.data(), fedauth_token_utf16le_.size());
	}
}

//===----------------------------------------------------------------------===//
// Result Stream Registry (spec 047 / US3)
//===----------------------------------------------------------------------===//

std::string MSSQLCatalog::RegisterStream(std::unique_ptr<MSSQLResultStream> stream) {
	// PR #118 review L3: UUID::GenerateRandomUUID is backed by std::mt19937
	// in DuckDB (NOT a CSPRNG). Acceptable here because the registry is
	// per-catalog and in-process; an attacker who could brute-force a stream
	// ID is already executing in the same process and has access to the
	// catalog directly. Replace with a CSPRNG only if the registry ever
	// becomes externally addressable.
	auto uuid = UUID::ToString(UUID::GenerateRandomUUID());
	std::lock_guard<std::mutex> lock(streams_mutex_);
	active_streams_.emplace(uuid, std::move(stream));
	return uuid;
}

std::unique_ptr<MSSQLResultStream> MSSQLCatalog::RetrieveStream(const std::string &uuid) {
	std::lock_guard<std::mutex> lock(streams_mutex_);
	auto it = active_streams_.find(uuid);
	if (it == active_streams_.end()) {
		return nullptr;
	}
	auto stream = std::move(it->second);
	active_streams_.erase(it);
	return stream;
}

//===----------------------------------------------------------------------===//
// Initialization
//===----------------------------------------------------------------------===//

void MSSQLCatalog::Initialize(bool load_builtin) {
	// Spec 076 W2: this runs TWICE per ATTACH -- once from the storage
	// extension's attach callback (mssql_storage.cpp) and once from DuckDB's
	// AttachedDatabase::Initialize right after it. The second call used to
	// build a second pool (the first, with its freshly logged-in connection,
	// was destroyed), log in again, and ask the collation again: two of the
	// three logins an ATTACH cost (#324) and the "collation query twice" of
	// spec 076 § 0.2. The pool is the artifact; once it exists, there is
	// nothing to do.
	if (connection_pool_) {
		return;
	}
	// Spec 047: build the connection pool inline (per-catalog ownership).
	// Replaces the MssqlPoolManager singleton lookup that lived here before.
	// The pool is owned by this catalog and torn down via unique_ptr in the
	// catalog destructor — no singleton, no cross-instance sharing.
	// Spec 047 FR-014: resolve the LOGIN7 program_name once at factory build
	// time; the same value is captured by each closure variant below so every
	// connection refilled into the pool advertises the same APP_NAME().
	const std::string app_name = ResolveAppName(*connection_info_);

	tds::ConnectionFactory factory;
	switch (connection_info_->auth_method) {
	case AuthMethod::AZURE_AD: {
		// Issue #302 / spec 073: the token is resolved when a CONNECTION is
		// created, not when the catalog was. ATTACH still acquires one -- that
		// is what makes a wrong secret fail at ATTACH -- but the factory does
		// not keep it: an Azure AD token lives 60 minutes, and a pool refill
		// hours later that presented the ATTACH-time bytes was dropped by the
		// gateway without a word, then reported as "(timeout)".
		//
		// What the factory holds is the secret's NAME, the tenant override and
		// the DatabaseInstance -- never the token and never the client secret.
		// TokenCache answers while the token is good (spec 047 FR-012 keys it
		// by DatabaseInstance); past the refresh margin it re-reads the secret
		// and mints a new one. Interactive chains cannot mint from here (no
		// user, no terminal, a worker thread mid-query) and say so by name.
		//
		// A DatabaseInstance and not a ClientContext: the ATTACH context is gone
		// long before the pool stops refilling (issue #178's constraint), and
		// the DatabaseInstance owns the AttachedDatabase that owns this catalog.
		auto host = connection_info_->host;
		auto port = connection_info_->port;
		auto database = connection_info_->database;
		auto encrypt = connection_info_->use_encrypt;
		auto tds_packet_size = connection_info_->tds_packet_size;
		auto utf8_support = connection_info_->utf8_support;
		auto tls_options = connection_info_->GetTlsOptions();
		auto secret_name = connection_info_->azure_secret_name;
		auto tenant = connection_info_->azure_tenant_id;
		auto connect_timeout = connect_timeout_;
		DatabaseInstance *db = &GetDatabase();
		factory = [db, host, port, database, encrypt, app_name, tds_packet_size, utf8_support, tls_options, secret_name,
				   tenant, connect_timeout]() -> std::shared_ptr<tds::TdsConnection> {
			auto token_result = mssql::azure::AcquireToken(*db, secret_name, tenant, /*allow_interactive=*/false);
			if (!token_result.success) {
				throw ConnectionException("Azure AD token for secret '%s': %s", secret_name,
										  token_result.error_message);
			}
			auto fedauth = mssql::azure::BuildFedAuthData(token_result.access_token);
			auto conn = std::make_shared<tds::TdsConnection>();
			conn->SetRequestedPacketSize(tds_packet_size);
			conn->SetRequestUtf8Support(utf8_support);
			conn->SetTlsOptions(tls_options);
			if (!conn->Connect(host, port, connect_timeout->load())) {
				throw ConnectionException(
					"%s", MSSQLTranslateConnectionError(conn->GetLastError(), host, port, "", database));
			}
			if (!conn->AuthenticateWithFedAuth(database, fedauth.token_utf16le, encrypt, app_name)) {
				throw ConnectionException(
					"Azure AD authentication failed: %s",
					MSSQLTranslateConnectionError(conn->GetLastError(), host, port, "", database,
												  conn->GetLastErrorNumber(), conn->GetLastErrorState()));
			}
			return conn;
		};
		break;
	}
	case AuthMethod::MANUAL_TOKEN: {
		// A token handed to ATTACH directly. There is no secret to refresh it
		// from, so its own `exp` decides, and it decides BEFORE a socket is
		// opened: Azure SQL does not answer an expired FEDAUTH token with an
		// error, it drops the connection (spec 073 F7), so the server-side
		// message would be "failed to receive" after the login read ran out.
		auto host = connection_info_->host;
		auto port = connection_info_->port;
		auto database = connection_info_->database;
		auto encrypt = connection_info_->use_encrypt;
		auto token = fedauth_token_utf16le_;
		auto tds_packet_size = connection_info_->tds_packet_size;
		auto utf8_support = connection_info_->utf8_support;
		auto tls_options = connection_info_->GetTlsOptions();
		auto connect_timeout = connect_timeout_;
		int64_t exp = 0;
		{
			auto claims = mssql::azure::ParseJwtClaims(connection_info_->access_token);
			if (claims.valid && claims.exp > 0) {
				exp = claims.exp;
			}
		}
		factory = [host, port, database, encrypt, token, app_name, tds_packet_size, utf8_support, tls_options,
				   connect_timeout, exp]() -> std::shared_ptr<tds::TdsConnection> {
			if (exp > 0 && mssql::azure::IsTokenExpired(exp, /*margin_seconds=*/0)) {
				throw ConnectionException(
					"Azure AD access token supplied at ATTACH expired at %s; a fixed token cannot "
					"be refreshed -- DETACH and ATTACH with a new one",
					mssql::azure::FormatTimestamp(exp));
			}
			auto conn = std::make_shared<tds::TdsConnection>();
			conn->SetRequestedPacketSize(tds_packet_size);
			conn->SetRequestUtf8Support(utf8_support);
			conn->SetTlsOptions(tls_options);
			if (!conn->Connect(host, port, connect_timeout->load())) {
				throw ConnectionException(
					"%s", MSSQLTranslateConnectionError(conn->GetLastError(), host, port, "", database));
			}
			if (!conn->AuthenticateWithFedAuth(database, token, encrypt, app_name)) {
				throw ConnectionException(
					"Azure AD authentication failed: %s",
					MSSQLTranslateConnectionError(conn->GetLastError(), host, port, "", database,
												  conn->GetLastErrorNumber(), conn->GetLastErrorState()));
			}
			return conn;
		};
		break;
	}
	case AuthMethod::KRB5:
	case AuthMethod::WINSSPI: {
		// Integrated-auth path: build a fresh authenticator per connection so
		// gss_init_sec_context state is independent across pool refills and a
		// kinit-refreshed ticket is picked up on the next fill. (Spec 042.)
		MSSQLConnectionInfo info_copy = *connection_info_;
		auto connect_timeout = connect_timeout_;
		factory = [info_copy, app_name, connect_timeout]() -> std::shared_ptr<tds::TdsConnection> {
			auto conn = std::make_shared<tds::TdsConnection>();
			conn->SetRequestedPacketSize(info_copy.tds_packet_size);
			conn->SetRequestUtf8Support(info_copy.utf8_support);
			conn->SetTlsOptions(info_copy.GetTlsOptions());
			if (!conn->Connect(info_copy.host, info_copy.port, connect_timeout->load())) {
				throw ConnectionException("integrated-auth: %s",
										  MSSQLTranslateConnectionError(conn->GetLastError(), info_copy.host,
																		info_copy.port, "", info_copy.database));
			}
			// Spec 068 D3: a factory, not an instance. It is called once per
			// login attempt, so a routing hop gets a ticket for the ROUTED
			// host's SPN instead of a retry of the gateway's. The first call
			// receives this connection's original host/port, which is what the
			// pre-068 code built the authenticator from — so a non-routed login
			// is unchanged. `DeriveSpn` reads info.host/info.port, and honours
			// an explicit service_principal_name verbatim, so the override
			// survives hops with no extra handling here.
			//
			// `strategy_error` carries a construction failure out: the callable
			// cannot throw across the TDS layer. By reference, because this
			// callable is built and consumed inside this one synchronous call.
			string strategy_error;
			auto auth_factory = [&info_copy, &strategy_error](const std::string &host,
															  uint16_t port) -> std::shared_ptr<tds::IAuthenticator> {
				MSSQLConnectionInfo hop_info = info_copy;
				hop_info.host = host;
				hop_info.port = port;
				std::shared_ptr<tds::AuthenticationStrategy> strategy;
				try {
					strategy = tds::AuthStrategyFactory::Create(hop_info);
				} catch (const std::exception &e) {
					strategy_error = ErrorData(e).RawMessage();
					return nullptr;
				}
				if (!strategy) {
					strategy_error = "failed to construct integrated-auth strategy";
					return nullptr;
				}
				auto authenticator = strategy->GetAuthenticator();
				if (!authenticator) {
					strategy_error = "integrated-auth strategy did not provide an authenticator";
				}
				return authenticator;
			};
			if (!conn->AuthenticateIntegrated(info_copy.database, auth_factory, info_copy.use_encrypt, app_name,
											  info_copy.login7_max_packet)) {
				// Both messages: the connection's names WHICH target failed --
				// after a routing hop the routed server and its SPN, which
				// Kerberos.md documents as the one-round diagnosis -- and
				// `strategy_error` the GSSAPI/SSPI cause.
				string error = MSSQLTranslateConnectionError(conn->GetLastError(), info_copy.host, info_copy.port,
															 info_copy.user, info_copy.database,
															 conn->GetLastErrorNumber(), conn->GetLastErrorState());
				if (!strategy_error.empty()) {
					error += " (" + strategy_error + ")";
				}
				throw ConnectionException("integrated-auth: %s", error);
			}
			return conn;
		};
		break;
	}
	case AuthMethod::SQL:
	default: {
		// SQL Server username/password.
		auto host = connection_info_->host;
		auto port = connection_info_->port;
		auto username = connection_info_->user;
		auto password = connection_info_->password;
		auto database = connection_info_->database;
		auto encrypt = connection_info_->use_encrypt;
		auto tds_packet_size = connection_info_->tds_packet_size;
		auto utf8_support = connection_info_->utf8_support;
		auto tls_options = connection_info_->GetTlsOptions();
		auto connect_timeout = connect_timeout_;
		factory = [host, port, username, password, database, encrypt, app_name, tds_packet_size, utf8_support,
				   tls_options, connect_timeout]() -> std::shared_ptr<tds::TdsConnection> {
			auto conn = std::make_shared<tds::TdsConnection>();
			conn->SetRequestedPacketSize(tds_packet_size);
			conn->SetRequestUtf8Support(utf8_support);
			conn->SetTlsOptions(tls_options);
			// Throw, do not return nullptr: the pool keeps the reason and the
			// caller finally sees "Login failed for user ..." instead of
			// "(timeout)" (issue #302).
			if (!conn->Connect(host, port, connect_timeout->load())) {
				throw ConnectionException(
					"%s", MSSQLTranslateConnectionError(conn->GetLastError(), host, port, username, database));
			}
			if (!conn->Authenticate(username, password, database, encrypt, app_name)) {
				throw ConnectionException(
					"%s", MSSQLTranslateConnectionError(conn->GetLastError(), host, port, username, database,
														conn->GetLastErrorNumber(), conn->GetLastErrorState()));
			}
			return conn;
		};
		break;
	}
	}

	connection_pool_ = make_shared_ptr<tds::ConnectionPool>(context_name_, pool_config_, std::move(factory));

	// Issue #324, review of #386: the ATTACH validates through the pool. Its
	// first login is the check -- the factory's own, so the connection that
	// proved the credentials is the pool's first connection, and there is no
	// second login routine to keep in step with the factory.
	if (startup_.validate) {
		ValidateThroughPool();
	}
	// mssql_min_connections is opened here, the logins in parallel, before
	// anything below takes a connection -- so the collation query runs on a
	// warm one and a default ATTACH pays about one login's time for N. A
	// shortfall is not the ATTACH's: the credentials are proved, and the pool
	// opens the rest on demand; the ATTACH logs it.
	if (startup_.prewarm && pool_config_.min_connections > 0) {
		std::string failure;
		try {
			const size_t opened = connection_pool_->Prewarm(pool_config_.min_connections, &failure);
			(void)opened;
		} catch (std::exception &e) {
			failure = ErrorData(e).RawMessage();
		}
		prewarm_shortfall_ = failure;
	}

	// Skip metadata initialization when catalog integration is disabled
	// (mssql_scan/mssql_exec will still work via raw queries)
	if (!catalog_enabled_) {
		return;
	}

	// Query database collation (needed for column metadata)
	QueryDatabaseCollation();
}

void MSSQLCatalog::ValidateThroughPool() {
	// The first login under mssql_attach_validation_timeout, every later one
	// under mssql_connection_timeout.
	const int validation_timeout =
		startup_.validation_timeout_seconds > 0 ? startup_.validation_timeout_seconds : pool_config_.connection_timeout;
	connect_timeout_->store(validation_timeout);
	std::string why;
	auto connection = connection_pool_->Acquire(validation_timeout * 1000, &why);
	connect_timeout_->store(pool_config_.connection_timeout);
	if (!connection) {
		// The pool's own framing ("pool 'x' could not create a connection: ")
		// is dropped: here the connection is the ATTACH's, and the factory's
		// reason -- already classified -- is the message.
		const std::string marker = "could not create a connection: ";
		const auto at = why.find(marker);
		throw InvalidInputException("MSSQL connection validation failed: %s",
									at == std::string::npos ? why : why.substr(at + marker.size()));
	}
	connection_info_->utf8_support_acked = connection->UTF8SupportAcked() ? 1 : 0;
	// Under TLS a login can succeed while the data path fails, which only a
	// query shows.
	if (connection_info_->use_encrypt) {
		string error;
		try {
			auto result = MSSQLSimpleQuery::Execute(*connection, "SELECT 1");
			if (!result.success) {
				error = result.error_message;
			}
		} catch (std::exception &e) {
			error = ErrorData(e).RawMessage();
		}
		if (!error.empty()) {
			connection->Close();
			connection_pool_->Release(connection);
			throw InvalidInputException(
				"MSSQL connection validation failed: TLS connection established but validation query failed. The "
				"server may have network issues or TLS may be misconfigured. Details: %s",
				MSSQLTranslateConnectionError(error, connection_info_->host, connection_info_->port,
											  connection_info_->user, connection_info_->database));
		}
	}
	connection_pool_->Release(connection);
}

void MSSQLCatalog::QueryDatabaseCollation() {
	if (!connection_pool_) {
		return;
	}

	auto connection = connection_pool_->Acquire();
	if (!connection) {
		return;
	}

	try {
		std::string collation;
		int32_t code_page = 0;
		// The snapshot probe rides along only when the answer is used: `snapshot`
		// (refused at ATTACH when OFF) or `auto`, and not on Fabric or Synapse,
		// where neither consults it. Everyone else's ATTACH query carries no
		// sys.databases read, so a platform whose sys.databases lacks the column
		// or the row cannot fail an ATTACH that never asked about isolation.
		const auto &level = connection_info_->transaction_isolation;
		const bool probe_snapshot = (level == "snapshot" || level == "auto") && !connection_info_->IsFabricEndpoint() &&
									!connection_info_->IsSynapseEndpoint();
		auto read_int = [](const std::vector<std::string> &values, size_t i, int32_t fallback) -> int32_t {
			if (values.size() <= i || values[i].empty()) {
				return fallback;
			}
			try {
				return static_cast<int32_t>(std::stoi(values[i]));
			} catch (...) {
				return fallback;
			}
		};
		auto run = [&](const string &sql) {
			collation.clear();
			code_page = 0;
			snapshot_isolation_state_ = -1;
			engine_edition_ = -1;
			product_major_version_ = -1;
			bool properties_in_row = false;
			// Positions: the plain query has 0-1, the D0 query adds 2-3, the
			// snapshot column is 4. A query without a column leaves it unread.
			auto result =
				MSSQLSimpleQuery::ExecuteWithCallback(*connection, sql, [&](const std::vector<std::string> &values) {
					if (!values.empty()) {
						collation = values[0];
					}
					code_page = read_int(values, 1, 0);
					engine_edition_ = read_int(values, 2, -1);
					product_major_version_ = read_int(values, 3, -1);
					snapshot_isolation_state_ = read_int(values, 4, -1);
					properties_in_row = values.size() > 3;
					return true;  // one row; keep the stream drained
				});
			if (result.success && properties_in_row) {
				server_properties_read_ = true;
			}
			return result;
		};
		// A failed probe must not take the collation and code page with it (review
		// of #381), nor the server properties with the snapshot column: each
		// failure drops one step, and what is left unread stays unknown -- `auto`
		// then sends nothing, and rung 3 takes the INTERSECT form, which is
		// correct everywhere. A server error leaves the connection Idle for the
		// next step; a broken connection fails them all, as before.
		bool done = probe_snapshot && run(string(DATABASE_COLLATION_SQL) + SNAPSHOT_STATE_COLUMN).success;
		if (!done) {
			done = run(DATABASE_COLLATION_SQL).success;
		}
		if (!done) {
			run(DATABASE_COLLATION_PLAIN_SQL);
		}

		if (!collation.empty()) {
			database_collation_ = collation;
			database_code_page_ = code_page;

			// Update metadata cache with collation
			if (metadata_cache_) {
				metadata_cache_->SetDatabaseCollation(database_collation_, database_code_page_);
			}
		}
	} catch (...) {
		connection_pool_->Release(std::move(connection));
		throw;
	}

	connection_pool_->Release(std::move(connection));
}

//===----------------------------------------------------------------------===//
// Catalog Type
//===----------------------------------------------------------------------===//

string MSSQLCatalog::GetCatalogType() {
	return "mssql";
}

string MSSQLCatalog::TransactionIsolationStatement() const {
	const auto &level = connection_info_->transaction_isolation;
	// Fabric Warehouse enforces snapshot and ignores the SET; Synapse's only
	// settable level, READ UNCOMMITTED, is already its default, and anything else
	// is refused at ATTACH (CheckTransactionIsolation).
	if (level.empty() || connection_info_->IsFabricEndpoint() || connection_info_->IsSynapseEndpoint()) {
		return "";
	}
	if (level == "auto") {
		if (snapshot_isolation_state_ != 1) {
			return "";
		}
		return "SET TRANSACTION ISOLATION LEVEL SNAPSHOT";
	}
	auto words = level;
	for (auto &c : words) {
		c = c == '_' ? ' ' : static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
	}
	return "SET TRANSACTION ISOLATION LEVEL " + words;
}

string MSSQLCatalog::TransactionIsolationRestoreStatement() const {
	return TransactionIsolationStatement().empty() ? "" : "SET TRANSACTION ISOLATION LEVEL READ COMMITTED";
}

void MSSQLCatalog::CheckTransactionIsolation() const {
	const auto &level = connection_info_->transaction_isolation;
	if (connection_info_->IsSynapseEndpoint() && !level.empty() && level != "auto" && level != "read_uncommitted") {
		throw InvalidInputException(
			"MSSQL ATTACH error: transaction_isolation '%s' is not available on Azure Synapse, whose only settable "
			"level is READ UNCOMMITTED -- its default",
			level);
	}
	if (level == "snapshot" && snapshot_isolation_state_ == 0 && !connection_info_->IsFabricEndpoint()) {
		throw InvalidInputException(
			"MSSQL ATTACH error: transaction_isolation 'snapshot' needs ALLOW_SNAPSHOT_ISOLATION ON in database "
			"'%s', and it is OFF. ALTER DATABASE ... SET ALLOW_SNAPSHOT_ISOLATION ON, or use 'auto', which falls "
			"back to the server's default level where snapshot is not allowed",
			connection_info_->database);
	}
}

optional<Identifier> MSSQLCatalog::GetDefaultSchema() const {
	// See the header: a value (not nullopt, not an empty Identifier) is what says
	// "probe this schema for unqualified names".
	return Identifier(default_schema_);
}

//===----------------------------------------------------------------------===//
// Schema Operations
//===----------------------------------------------------------------------===//

MSSQLMetadataCache &MSSQLCatalog::SchemaListCache(ClientContext *context) {
	if (!context || context->transaction.IsAutoCommit()) {
		return *metadata_cache_;
	}
	// Issue #380: inside a transaction the list is loaded on the pinned
	// connection, which sees schemas the transaction created or dropped -- so a
	// load goes into the transaction's own cache, never the shared one. A shared
	// list already loaded is committed state and good to read, unless the
	// transaction changed "anything" (mssql_exec DDL), which may mean schemas.
	//
	// Ask non-creatingly first (review of db73be6): MSSQLTransaction::Get ->
	// MetaTransaction::GetTransaction CREATES this catalog's transaction, and a
	// transaction that has not touched MSSQL yet has no transaction-local view to
	// consult -- there is nothing to create it FOR. Creating it here would also
	// flip HasUsedAnyMSSQLCatalogInTransaction, so any schema lookup DuckDB makes
	// against an attached MSSQL catalog (search-path resolution, duckdb_schemas())
	// would refuse mssql_refresh_cache / mssql_preload_catalog for a
	// transaction that has taken no connection -- the
	// very case issue #380's refusal was narrowed to allow. The shortcut is
	// limited to an already-LOADED shared list: that is committed state and
	// answers with no connection. Anything else falls through and creates the
	// transaction exactly as before, so a LOAD still lands in its cache. (A
	// "did you mean" scan is out of reach: DuckDB creates the transaction for it
	// before asking -- see MSSQLSchemaEntry::GetSimilarEntry.)
	if (metadata_cache_->GetSchemasState() == CacheLoadState::LOADED &&
		!MetaTransaction::Get(*context).TryGetTransaction(GetAttached())) {
		return *metadata_cache_;
	}
	auto &metadata = MSSQLTransaction::Get(*context, *this).Metadata(*context);
	if (!metadata.IsAllChanged() && metadata_cache_->GetSchemasState() == CacheLoadState::LOADED) {
		return *metadata_cache_;
	}
	return metadata.Cache();
}

optional_ptr<SchemaCatalogEntry> MSSQLCatalog::LookupSchema(CatalogTransaction transaction,
															const EntryLookupInfo &schema_lookup,
															OnEntryNotFound if_not_found) {
	auto &name = schema_lookup.GetEntryName();

	// Ensure cache settings are loaded (sets TTL)
	if (transaction.context) {
		EnsureCacheLoaded(*transaction.context);
	}

	// Spec 079 § 0.1: DuckDB's remote-pushdown rewriter looks a table of a
	// two-part name (`db.t`) or of `USE db` up in the hard-coded schema `main`,
	// not in Catalog::GetDefaultSchema(). SQL Server has no `main`, so without
	// this the most common spelling would never be pushed. With
	// mssql_remote_pushdown on, `main` answers as this catalog's default schema
	// -- the ATTACH `default_schema` option (#322), `dbo` when it is unset, the
	// same one GetDefaultSchema() reports -- unless the server does have a
	// schema called `main`, which then wins.
	const bool main_alias = startup_.remote_pushdown && name == DEFAULT_SCHEMA;
	auto resolve = [&](MSSQLMetadataCache &schemas) -> string {
		return main_alias && !schemas.HasSchema(name) ? default_schema_ : name;
	};

	// Check schema filter — filtered-out schemas return not found (Spec 033).
	// For the `main` alias the filter judges the schema it resolves to, below.
	if (!main_alias && catalog_filter_.HasSchemaFilter() && !catalog_filter_.MatchesSchema(name)) {
		if (if_not_found == OnEntryNotFound::THROW_EXCEPTION) {
			throw CatalogException("Schema '%s' not found in MSSQL database", name);
		}
		return nullptr;
	}

	// T035 (FR-003/Bug 0.2): Check cache BEFORE acquiring connection to reduce connection usage
	// Fast path: If schemas are already loaded and schema exists in cache, skip connection acquisition
	// Through SchemaListCache, not metadata_cache_ directly (review of #382):
	// inside a transaction that ran DDL the extension cannot see through, the
	// shared list still describes committed state and would answer here for a
	// schema the transaction has since dropped. ScanSchemas already reads the
	// list this way, and the two must not disagree about which one is
	// authoritative. With no context, or in autocommit, this IS metadata_cache_.
	// Bound ONCE for both paths (review of 0e12914): two calls could return
	// different cache objects if the transaction's IsAllChanged state or the
	// shared list's load state changed in between, and the miss decision would
	// then be made against one cache and the load against another.
	auto &schema_list = SchemaListCache(transaction.context.get());
	auto visible = [&](const string &schema) {
		return schema_list.HasSchema(schema) &&
			   (!catalog_filter_.HasSchemaFilter() || catalog_filter_.MatchesSchema(schema));
	};
	if (schema_list.GetSchemasState() == CacheLoadState::LOADED && visible(resolve(schema_list))) {
		auto schema_sp = GetOrCreateSchemaEntryShared(resolve(schema_list));
		if (transaction.context) {
			MSSQLBindAnchors::For(*transaction.context, *this).AnchorSchema(schema_sp);
		}
		return schema_sp.get();
	}

	// T013-T014 (FR-003): Use ConnectionProvider for transaction-aware connection acquisition
	// This ensures schema lookups during INSERT in transaction use the pinned connection
	if (!connection_pool_) {
		throw InternalException("Connection pool not initialized");
	}

	std::shared_ptr<tds::TdsConnection> connection;
	if (transaction.context) {
		// Use ConnectionProvider for proper transaction handling
		connection = ConnectionProvider::GetConnection(*transaction.context, *this);
	} else {
		// Fallback to direct pool access if no context available
		std::string why;
		connection = connection_pool_->Acquire(-1, &why);
		if (!connection) {
			throw IOException("Failed to acquire connection for schema lookup: " + why);
		}
	}
	if (!connection) {
		throw IOException("Failed to acquire connection for schema lookup");
	}

	// Trigger lazy loading of schema list (ensure connection released on exception)
	try {
		schema_list.EnsureSchemasLoaded(*connection);
	} catch (...) {
		if (transaction.context) {
			ConnectionProvider::ReleaseConnection(*transaction.context, *this, std::move(connection));
		} else {
			connection_pool_->Release(std::move(connection));
		}
		throw;
	}

	// Release connection properly (no-op if pinned to transaction)
	if (transaction.context) {
		ConnectionProvider::ReleaseConnection(*transaction.context, *this, std::move(connection));
	} else {
		connection_pool_->Release(std::move(connection));
	}

	// Check if schema exists in cache
	const string resolved = resolve(schema_list);
	if (!visible(resolved)) {
		if (if_not_found == OnEntryNotFound::THROW_EXCEPTION) {
			throw CatalogException("Schema '%s' not found in MSSQL database", name);
		}
		return nullptr;
	}

	// Get or create schema entry
	auto schema_sp = GetOrCreateSchemaEntryShared(resolved);
	if (transaction.context) {
		MSSQLBindAnchors::For(*transaction.context, *this).AnchorSchema(schema_sp);
	}
	return schema_sp.get();
}

void MSSQLCatalog::ScanSchemas(ClientContext &context, std::function<void(SchemaCatalogEntry &)> callback) {
	// Ensure cache is loaded (sets TTL)
	EnsureCacheLoaded(context);

	// T036 (FR-003/Bug 0.2): Check cache BEFORE acquiring connection
	// Fast path: If schemas are already loaded, get names without acquiring connection
	vector<string> schema_names;
	auto &schema_list = SchemaListCache(&context);
	if (schema_list.TryGetCachedSchemaNames(schema_names)) {
		// Cache hit - iterate without connection.
		// Spec 052 (Option D): anchor each schema entry so it survives a
		// concurrent Invalidate between DuckDB walker phase 1 (collect) and
		// phase 2 (read). Same reasoning as MSSQLTableSet::Scan.
		for (const auto &name : schema_names) {
			auto schema_sp = GetOrCreateSchemaEntryShared(name);
			MSSQLBindAnchors::For(context, *this).AnchorSchema(schema_sp);
			callback(*schema_sp);
		}
		return;
	}

	// T015-T016 (FR-003): Use ConnectionProvider for transaction-aware connection acquisition
	if (!connection_pool_) {
		throw InternalException("Connection pool not initialized");
	}

	// Use ConnectionProvider for proper transaction handling
	auto connection = ConnectionProvider::GetConnection(context, *this);
	if (!connection) {
		throw IOException("Failed to acquire connection for schema scan");
	}

	try {
		schema_names = schema_list.GetSchemaNames(*connection);
	} catch (...) {
		ConnectionProvider::ReleaseConnection(context, *this, std::move(connection));
		throw;
	}

	// Release connection properly (no-op if pinned to transaction)
	ConnectionProvider::ReleaseConnection(context, *this, std::move(connection));

	for (const auto &name : schema_names) {
		auto schema_sp = GetOrCreateSchemaEntryShared(name);
		MSSQLBindAnchors::For(context, *this).AnchorSchema(schema_sp);
		callback(*schema_sp);
	}
}

shared_ptr<MSSQLSchemaEntry> MSSQLCatalog::GetOrCreateSchemaEntryShared(const string &schema_name) {
	std::lock_guard<std::mutex> lock(schema_mutex_);

	auto it = schema_entries_.find(schema_name);
	if (it != schema_entries_.end()) {
		return it->second;	// shared_ptr copy — refcount inc
	}

	// Spec 052: construct via make_shared_ptr — enable_shared_from_this on
	// MSSQLSchemaEntry requires shared_ptr ownership from the first store.
	// schema_mutex_ is held across find + emplace above, so no race is
	// possible here; the emplace simply publishes the freshly constructed
	// entry.
	auto entry = make_shared_ptr<MSSQLSchemaEntry>(*this, schema_name);
	auto insert_result = schema_entries_.emplace(schema_name, std::move(entry));
	return insert_result.first->second;
}

MSSQLSchemaEntry &MSSQLCatalog::GetOrCreateSchemaEntry(const string &schema_name) {
	// Reference-returning wrapper for internal call-sites that don't need to
	// anchor (DDL paths that use the entry briefly and discard).
	return *GetOrCreateSchemaEntryShared(schema_name);
}

optional_ptr<CatalogEntry> MSSQLCatalog::CreateSchema(CatalogTransaction transaction, CreateSchemaInfo &info) {
	CheckWriteAccess("CREATE SCHEMA");

	if (!transaction.HasContext()) {
		throw InternalException("Cannot execute CREATE SCHEMA without client context");
	}

	// Handle IF NOT EXISTS: check if schema already exists (Issue #54)
	if (info.on_conflict == OnCreateConflict::IGNORE_ON_CONFLICT) {
		EntryLookupInfo lookup(CatalogType::SCHEMA_ENTRY, QualifiedName(info.SchemaName()));
		auto existing = LookupSchema(transaction, lookup, OnEntryNotFound::RETURN_NULL);
		if (existing) {
			return existing.get();
		}
	}

	// Generate T-SQL for CREATE SCHEMA
	string tsql = MSSQLDDLTranslator::TranslateCreateSchema(info.SchemaName().GetIdentifierName());

	// Execute DDL on SQL Server
	ExecuteDDL(transaction.GetContext(), tsql);

	// Point invalidation: invalidate schema list so new schema is visible
	metadata_cache_->InvalidateAll();
	// The transaction's own schema list too, or a CREATE TABLE in the schema
	// just created is refused as "not found" (review of #382).
	NoteTransactionChange(transaction.GetContext());

	return &GetOrCreateSchemaEntry(info.SchemaName().GetIdentifierName());
}

void MSSQLCatalog::DropSchema(ClientContext &context, DropInfo &info) {
	CheckWriteAccess("DROP SCHEMA");

	// Handle IF EXISTS: check if schema exists before attempting DROP (Issue #54)
	if (info.if_not_found == OnEntryNotFound::RETURN_NULL) {
		CatalogTransaction cat_transaction = GetCatalogTransaction(context);
		EntryLookupInfo lookup(CatalogType::SCHEMA_ENTRY, QualifiedName(info.GetQualifiedName().Name()));
		auto existing = LookupSchema(cat_transaction, lookup, OnEntryNotFound::RETURN_NULL);
		if (!existing) {
			return;
		}
	}

	// Generate T-SQL for DROP SCHEMA
	string tsql = MSSQLDDLTranslator::TranslateDropSchema(info.GetQualifiedName().Name().GetIdentifierName());

	// Execute DDL on SQL Server
	ExecuteDDL(context, tsql);

	// Point invalidation: invalidate schema list
	metadata_cache_->InvalidateAll();
	NoteTransactionChange(context);

	// Spec 052 (Option D): just erase. Any binder that looked up this schema
	// before DROP SCHEMA fired is already anchored in its ClientContext's
	// MSSQLBindAnchors via the LookupSchema path; the schema entry stays
	// alive until that ClientContext's QueryEnd. DuckDB serializes DETACH
	// against active queries, so we don't need to worry about the catalog
	// dying mid-query.
	std::lock_guard<std::mutex> lock(schema_mutex_);
	schema_entries_.erase(info.GetQualifiedName().Name().GetIdentifierName());
}

//===----------------------------------------------------------------------===//
// Catalog Information
//===----------------------------------------------------------------------===//

DatabaseSize MSSQLCatalog::GetDatabaseSize(ClientContext &context) {
	DatabaseSize size;
	size.free_blocks = 0;
	size.total_blocks = 0;
	size.used_blocks = 0;
	size.wal_size = 0;
	size.block_size = 0;
	return size;
}

bool MSSQLCatalog::InMemory() {
	return false;  // This is a remote database
}

string MSSQLCatalog::GetDBPath() {
	// Return connection info as path representation
	return "mssql://" + connection_info_->host + ":" + std::to_string(connection_info_->port) + "/" +
		   connection_info_->database;
}

//===----------------------------------------------------------------------===//
// Detach Hook
//===----------------------------------------------------------------------===//

void MSSQLCatalog::OnDetach(ClientContext &context) {
	// T023 (FR-005): Invalidate cached Azure token on detach
	// This ensures re-attach will acquire a fresh token, not use a stale cached one.
	// Spec 047 T046b (FR-012): invalidate only this DatabaseInstance's namespace
	// so a sibling instance sharing the same secret name keeps its token.
	// PR #118 review M3: also evict tenant-suffixed variants
	// (`secret_name:tenant_a`, `secret_name:tenant_b`, ...) that the
	// interactive-auth path in AcquireToken builds — bare-name Invalidate
	// would otherwise leave those rows behind.
	if (connection_info_ && connection_info_->use_azure_auth && !connection_info_->azure_secret_name.empty()) {
		mssql::azure::TokenCache::Instance().InvalidateByPrefix(*context.db, connection_info_->azure_secret_name);
	}

	// Spec 047 T012+T020: pool teardown is implicit via ~MSSQLCatalog → unique_ptr
	// destruction; the MssqlPoolManager / MSSQLContextManager singletons that
	// used to require explicit RemovePool() / UnregisterContext() are gone.
	(void)context;
}

//===----------------------------------------------------------------------===//
// MSSQL-specific Accessors
//===----------------------------------------------------------------------===//

weak_ptr<tds::ConnectionPool> MSSQLCatalog::GetConnectionPoolHandle() const {
	return weak_ptr<tds::ConnectionPool>(connection_pool_);
}

tds::ConnectionPool &MSSQLCatalog::GetConnectionPool() {
	if (!connection_pool_) {
		throw IOException("MSSQL connection pool not initialized");
	}
	return *connection_pool_;
}

MSSQLMetadataCache &MSSQLCatalog::GetMetadataCache() {
	return *metadata_cache_;
}

MSSQLStatisticsProvider &MSSQLCatalog::GetStatisticsProvider() {
	return *statistics_provider_;
}

const string &MSSQLCatalog::GetDatabaseCollation() const {
	return database_collation_;
}

mssql::DmlCapabilities MSSQLCatalog::GetDmlCapabilities() const {
	auto platform = mssql::DmlPlatform::SqlServer;
	if (startup_.dml_platform == "fabric") {
		platform = mssql::DmlPlatform::Fabric;
	} else if (startup_.dml_platform == "synapse") {
		platform = mssql::DmlPlatform::Synapse;
	} else if (startup_.dml_platform == "sqlserver") {
		platform = mssql::DmlPlatform::SqlServer;
	} else if (connection_info_ && connection_info_->IsFabricEndpoint()) {
		platform = mssql::DmlPlatform::Fabric;
	} else if (connection_info_ && connection_info_->IsSynapseEndpoint()) {
		platform = mssql::DmlPlatform::Synapse;
	}
	return mssql::DmlCapabilities::Resolve(platform, engine_edition_.load(), product_major_version_.load());
}

void MSSQLCatalog::EnsureServerProperties(ClientContext &context, const char *verb) {
	if (server_properties_read_.load()) {
		return;
	}
	std::lock_guard<std::mutex> guard(server_properties_mutex_);
	if (server_properties_read_.load()) {
		return;
	}
	string error;
	auto connection = ConnectionProvider::GetConnection(context, *this);
	if (!connection) {
		error = "no connection";
	} else {
		int32_t edition = -1;
		int32_t major = -1;
		auto result = MSSQLSimpleQuery::ExecuteWithCallback(
			*connection,
			"SELECT CAST(SERVERPROPERTY('EngineEdition') AS INT) AS e, "
			"TRY_CAST(CAST(SERVERPROPERTY('ProductMajorVersion') AS NVARCHAR(16)) AS INT) AS v",
			[&](const std::vector<std::string> &values) {
				if (!values.empty() && !values[0].empty()) {
					edition = std::stoi(values[0]);
				}
				if (values.size() > 1 && !values[1].empty()) {
					major = std::stoi(values[1]);
				}
				return true;
			});
		ConnectionProvider::ReleaseConnection(context, *this, std::move(connection));
		if (result.success && edition >= 0) {
			engine_edition_ = edition;
			product_major_version_ = major;
			server_properties_read_ = true;
			return;
		}
		error = result.success ? "the server returned no EngineEdition" : result.error_message;
	}
	throw NotImplementedException(
		"MSSQL: %s through '%s' needs SERVERPROPERTY('EngineEdition') to tell Azure Synapse, whose keys are NOT "
		"ENFORCED, from SQL Server, and it could not be read (%s). Use mssql_exec() to run the statement on the server",
		verb, GetName().GetIdentifierName(), error);
}

MSSQLCatalog::Utf8Support MSSQLCatalog::UTF8SupportState() {
	const int8_t cached = utf8_support_acked_.load(std::memory_order_relaxed);
	if (cached >= 0) {
		return cached == 1 ? Utf8Support::Granted : Utf8Support::Declined;
	}
	// The ATTACH-time validation login already answered this on every non-lazy
	// attach, and it is carried on the connection info the same way
	// is_fabric_endpoint is.
	const int8_t from_attach = connection_info_ ? connection_info_->utf8_support_acked : -1;
	if (from_attach >= 0) {
		utf8_support_acked_.store(from_attach, std::memory_order_relaxed);
		return from_attach == 1 ? Utf8Support::Granted : Utf8Support::Declined;
	}

	// Only a lazy attach gets here: no login has happened yet at ATTACH time.
	// Borrow whatever the pool has rather than opening a connection for the
	// question. If it cannot hand one over, leave the answer unobserved rather
	// than caching a guess — the next caller retries.
	auto &pool = GetConnectionPool();
	auto conn = pool.Acquire();
	if (!conn) {
		return Utf8Support::Unknown;
	}
	const bool acked = conn->UTF8SupportAcked();
	pool.Release(conn);
	utf8_support_acked_.store(acked ? 1 : 0, std::memory_order_relaxed);
	return acked ? Utf8Support::Granted : Utf8Support::Declined;
}

string MSSQLCatalog::ResolveVarcharCollation(ClientContext &context, bool wants_varchar, bool target_is_temp) {
	if (!wants_varchar) {
		return string();
	}

	Value setting;
	string requested;
	if (context.TryGetCurrentSetting("mssql_utf8_collation", setting)) {
		requested = setting.IsNull() ? string() : setting.ToString();
	}
	if (requested.empty()) {
		// The documented way to ask for the pre-#225 behaviour deliberately.
		return string();
	}

	// A database default that is already UTF-8 (Fabric) is the case where
	// inheriting is right: imposing a Latin1 collation would also impose its
	// case- and accent-sensitivity on every later comparison against the column.
	//
	// Except for a TEMP table, which does not inherit it. A #temp lives in
	// tempdb and takes TEMPDB's collation — the server default, and typically
	// not UTF-8 even when the database is. Verified on a UTF-8 database: a temp
	// varchar column came back SQL_Latin1_General_CP1_CI_AS and 'Привет' landed
	// as '??????'. Naming the database's own collation puts the temp column back
	// in step with the permanent tables around it, and on Fabric — where every
	// string column is a varchar, so this is the whole of it — that name is one
	// of the two a warehouse accepts.
	if (StringUtil::EndsWith(StringUtil::Upper(GetDatabaseCollation()), "_UTF8")) {
		if (!target_is_temp) {
			return string();
		}
		// The name reaches T-SQL as a bare identifier. It came from the server,
		// but it is concatenated into DDL, so it is checked like any other.
		return mssql::codec::IsValidCollationName(GetDatabaseCollation()) ? GetDatabaseCollation() : requested;
	}

	// Unknown is treated as granted, NOT as declined. Declined means the server
	// has no UTF-8 collations at all and the DDL will fail with a clear message;
	// unknown means only that no connection could be borrowed to ask. Reading
	// either as "skip the collation" would turn a transient pool timeout into a
	// silently lossy table.
	if (UTF8SupportState() == Utf8Support::Declined) {
		throw NotImplementedException(
			"A VARCHAR column needs a UTF-8 collation, and this server did not grant the TDS UTF8SUPPORT feature "
			"(SQL Server 2019 introduced both). The column would take the database's code page and lose every "
			"character outside it on insert, silently. Use NVARCHAR instead, name a collation with "
			"MSSQL_VARCHAR(n, 'collation'), or set mssql_utf8_collation='' to accept that loss deliberately.");
	}
	return requested;
}

bool MSSQLCatalog::RequiresSingleByteText() const {
	return connection_info_ && connection_info_->is_fabric_endpoint;
}

string MSSQLCatalog::WireVarcharCollation(const string &ddl_collation) const {
	if (!ddl_collation.empty()) {
		return ddl_collation;
	}
	// The DDL said nothing because the database's own collation is already what
	// the column wants. The wire has no such default, so name it.
	const string &db_collation = GetDatabaseCollation();
	if (StringUtil::EndsWith(StringUtil::Upper(db_collation), "_UTF8") &&
		mssql::codec::IsValidCollationName(db_collation)) {
		return db_collation;
	}
	return string();
}

void MSSQLCatalog::ValidateStringTargets(const vector<LogicalType> &types) {
	if (!RequiresSingleByteText()) {
		return;
	}

	// Fabric Data Warehouse allows exactly these two, both UTF-8, and the choice
	// is fixed when the warehouse is created. Anything else is rejected by the
	// server with "...is not a valid collation", which is a worse place to find
	// out than here.
	static const char *const FABRIC_COLLATIONS[] = {"LATIN1_GENERAL_100_BIN2_UTF8",
													"LATIN1_GENERAL_100_CI_AS_KS_WS_SC_UTF8"};

	for (const auto &type : types) {
		mssql::codec::TargetStringType spec;
		if (!mssql::codec::TryGetTargetStringType(type, spec)) {
			continue;
		}
		if (spec.unicode) {
			throw NotImplementedException(
				"MSSQL_NVARCHAR is not available on Microsoft Fabric: a warehouse stores tables as Delta Parquet and "
				"has no UTF-16 type, so nvarchar columns cannot be created there. Use MSSQL_VARCHAR(n) — its "
				"collation is UTF-8, so it holds the same characters, counting BYTES rather than UTF-16 units.");
		}
		if (spec.collation.empty()) {
			continue;
		}
		const string upper = StringUtil::Upper(spec.collation);
		bool supported = false;
		for (const char *candidate : FABRIC_COLLATIONS) {
			if (upper == candidate) {
				supported = true;
				break;
			}
		}
		if (!supported) {
			throw NotImplementedException(
				"Collation '%s' is not available on Microsoft Fabric, which supports only "
				"Latin1_General_100_BIN2_UTF8 and Latin1_General_100_CI_AS_KS_WS_SC_UTF8 — both UTF-8, and fixed when "
				"the warehouse was created. Omit the collation to inherit the warehouse's own.",
				spec.collation);
		}
	}
}

void MSSQLCatalog::ValidateTableOptions(const MSSQLTableOptions &options) {
	if (!RequiresSingleByteText()) {
		return;
	}
	// Verified against a live warehouse: table_kind and clustered_index both come
	// back as "CREATE INDEX is not a supported statement type", data_compression
	// as "The DATA COMPRESSION keyword is not supported in the CREATE TABLE
	// statement". Delta Parquet has no indexes, and compresses itself.
	if (options.kind != MSSQLTableKind::HEAP) {
		throw NotImplementedException(
			"Microsoft Fabric stores tables as Delta Parquet and supports no indexes, so table_kind and "
			"clustered_index cannot be applied there. Omit them — a warehouse is already columnar.");
	}
	if (!options.data_compression.empty()) {
		throw NotImplementedException(
			"DATA_COMPRESSION is not available on Microsoft Fabric: a warehouse stores tables as Delta Parquet, "
			"which carries its own compression. Omit the option.");
	}
}

ErrorData MSSQLCatalog::SupportsCreateTable(BoundCreateTableInfo &info) {
	auto &base = info.Base().Cast<CreateTableInfo>();
	// PARTITIONED BY and SORTED BY stay rejected by the base implementation:
	// SQL Server expresses both, but through partition schemes and index keys
	// that this spec does not build. Only the WITH clause is claimed here.
	if (base.partition_keys.empty() && base.sort_keys.empty()) {
		return ErrorData();
	}
	return Catalog::SupportsCreateTable(info);
}

const MSSQLConnectionInfo &MSSQLCatalog::GetConnectionInfo() const {
	return *connection_info_;
}

const MSSQLCatalogFilter &MSSQLCatalog::GetCatalogFilter() const {
	return catalog_filter_;
}

const string &MSSQLCatalog::GetContextName() const {
	return context_name_;
}

idx_t MSSQLCatalog::GetConnectionLimit() const {
	return pool_config_.connection_limit;
}

//===----------------------------------------------------------------------===//
// Access Mode (READ_ONLY Support)
//===----------------------------------------------------------------------===//

bool MSSQLCatalog::IsReadOnly() const {
	return access_mode_ == AccessMode::READ_ONLY;
}

AccessMode MSSQLCatalog::GetAccessMode() const {
	return access_mode_;
}

bool MSSQLCatalog::IsCatalogEnabled() const {
	return catalog_enabled_;
}

void MSSQLCatalog::CheckWriteAccess(const char *operation_name) const {
	if (IsReadOnly()) {
		if (operation_name) {
			throw CatalogException("Cannot execute %s: MSSQL catalog '%s' is attached in read-only mode",
								   operation_name, context_name_);
		} else {
			throw CatalogException("Cannot modify MSSQL catalog '%s': attached in read-only mode", context_name_);
		}
	}
}

//===----------------------------------------------------------------------===//
// DDL Execution
//===----------------------------------------------------------------------===//

void MSSQLCatalog::ExecuteDDL(ClientContext &context, const string &tsql) {
	if (!connection_pool_) {
		throw IOException("MSSQL connection pool not initialized - cannot execute DDL");
	}

	// Catalog DDL runs on a pool connection and autocommits (spec 057) -- except
	// in a transaction on a pool of ONE connection (issue #419), as CTAS does
	// since #380: the transaction holds the only connection (any lookup it made
	// pinned it), and a second Acquire waited out mssql_acquire_timeout. There
	// it runs on the pinned connection, inside the transaction, and rolls back
	// with it; COMMIT / ROLLBACK forget the names it changed
	// (ForgetTransactionChanges), as for any change the transaction made.
	if (!context.transaction.IsAutoCommit() && GetConnectionLimit() <= 1) {
		auto connection = ConnectionProvider::GetConnection(context, *this);
		if (!connection) {
			throw IOException("Failed to get the transaction's connection for DDL execution");
		}
		SimpleQueryResult result;
		try {
			result = MSSQLSimpleQuery::Execute(*connection, tsql);
		} catch (...) {
			ConnectionProvider::ReleaseConnection(context, *this, connection);
			throw;
		}
		ConnectionProvider::ReleaseConnection(context, *this, connection);
		if (!result.success) {
			throw CatalogException("MSSQL DDL error: %s", result.DescribeError());
		}
		return;
	}

	std::string why;
	auto connection = connection_pool_->Acquire(-1, &why);
	if (!connection) {
		throw IOException("Failed to acquire connection for DDL execution: " + why);
	}

	try {
		auto result = MSSQLSimpleQuery::Execute(*connection, tsql);

		if (!result.success) {
			connection_pool_->Release(std::move(connection));
			throw CatalogException("MSSQL DDL error: %s", result.DescribeError());
		}
	} catch (...) {
		connection_pool_->Release(std::move(connection));
		throw;
	}

	connection_pool_->Release(std::move(connection));
}

}  // namespace duckdb
