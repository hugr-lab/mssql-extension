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
