#include "mssql_extension.hpp"
#include "azure/azure_test_function.hpp"
#include "catalog/mssql_preload_catalog.hpp"
#include "catalog/mssql_refresh_function.hpp"
#include "connection/mssql_diagnostic.hpp"
#include "connection/mssql_settings.hpp"
#include "copy/copy_function.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/extension_type_info.hpp"
#include "duckdb/common/vector_operations/generic_executor.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/optimizer/optimizer_extension.hpp"
#include "mssql_functions.hpp"
#include "mssql_secret.hpp"
#include "mssql_storage.hpp"
#include "table_scan/mssql_optimizer.hpp"
#include "tds/auth/krb5_test_function.hpp"
#include "tds/auth/winsspi_test_function.hpp"

namespace duckdb {

// Extension version string
static const char *GetMssqlExtensionVersion() {
#ifdef MSSQL_VERSION
	return MSSQL_VERSION;
#else
	return "unknown";
#endif
}

// Placeholder scalar function to verify extension loads correctly
static void MssqlVersionFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto version = GetMssqlExtensionVersion();
	result.SetVectorType(VectorType::CONSTANT_VECTOR);
	ConstantVector::GetData<string_t>(result)[0] = StringVector::AddString(result, version);
}

// Internal function to register extension functionality
//===----------------------------------------------------------------------===//
// Spec 060 — parameterised target types
//
// MSSQL_NVARCHAR(n) states the type a CREATE/CTAS/COPY target column should get.
// DuckDB drops the VARCHAR length modifier at CREATE TABLE and Parquet carries
// no string length, so the bound cannot be plumbed from a source — it has to be
// stated, and this is the only mechanism available on the CTAS path, which has
// no options syntax.
//
// Bound to a plain VARCHAR carrying the length in ExtensionTypeInfo, and
// deliberately WITHOUT an alias: an aliased VARCHAR is a distinct type for
// binding, and every operation on it — upper(), ||, =, LIKE, IS NULL,
// count(DISTINCT) — fails with "No function matches". Extension info alone is
// invisible to overload resolution and still reaches DDL generation, which is
// exactly the pair of properties needed. Measured both ways; see spec 060 § 2a.
//===----------------------------------------------------------------------===//

static LogicalType BindMssqlNVarchar(BindLogicalTypeInput &input) {
	auto &modifiers = input.modifiers;
	if (modifiers.size() != 1) {
		throw BinderException("MSSQL_NVARCHAR takes exactly one modifier: MSSQL_NVARCHAR(n)");
	}
	auto &val = modifiers[0].GetValue();
	if (val.IsNull() || val.type() != LogicalType::INTEGER) {
		throw BinderException("MSSQL_NVARCHAR(n): n must be a non-NULL integer");
	}
	auto length = val.GetValue<int32_t>();
	if (length < 1 || length > 4000) {
		throw BinderException("MSSQL_NVARCHAR(n): n must be between 1 and 4000 (use VARCHAR for MAX)");
	}
	auto type = LogicalType(LogicalTypeId::VARCHAR);
	auto info = make_uniq<ExtensionTypeInfo>();
	info->modifiers.emplace_back(Value::INTEGER(length));
	type.SetExtensionInfo(std::move(info));
	return type;
}

static void LoadInternal(ExtensionLoader &loader) {
	// 1. Register secrets
	RegisterMSSQLSecretType(loader);

	// 2. Register storage extension (ATTACH TYPE mssql)
	RegisterMSSQLStorageExtension(loader);

	// 3. Register table functions
	RegisterMSSQLFunctions(loader);

	// 4. Register mssql_exec scalar function
	RegisterMSSQLExecFunction(loader);

	// 5. Register connection pool settings
	RegisterMSSQLSettings(loader);

	// 6. Register diagnostic functions (mssql_open, mssql_close, mssql_ping, mssql_pool_stats)
	RegisterMSSQLDiagnosticFunctions(loader);

	// 7. Register mssql_refresh_cache function
	RegisterMSSQLRefreshCacheFunction(loader);

	// 8. Register mssql_preload_catalog function
	RegisterMSSQLPreloadCatalogFunction(loader);

	// 9. Register COPY functions (bcp format)
	RegisterMSSQLCopyFunctions(loader);

	// Spec 060: parameterised target types (DDL-facing; see the note above).
	loader.RegisterType("MSSQL_NVARCHAR", LogicalType::VARCHAR, BindMssqlNVarchar);

	// 10. Register utility functions (mssql_version)
	auto mssql_version_func = ScalarFunction("mssql_version", {},  // No arguments
											 LogicalType::VARCHAR, MssqlVersionFunction);
	loader.RegisterFunction(mssql_version_func);

	// 11. Register Azure authentication test function
	mssql::azure::RegisterAzureTestFunction(loader);

	// 12. Register Kerberos authentication test function (spec 042).
	//     Always registered; on builds without MSSQL_ENABLE_KRB5 it returns a
	//     clear "compiled without Kerberos support" message instead of being absent.
	mssql::krb5::RegisterKrb5TestFunction(loader);
	mssql::winsspi::RegisterWinSspiTestFunction(loader);

	// 13. Register optimizer extension for ORDER BY pushdown (Spec 039)
	auto &db = loader.GetDatabaseInstance();
	auto &config = DBConfig::GetConfig(db);
	OptimizerExtension mssql_optimizer;
	mssql_optimizer.optimize_function = MSSQLOptimizer::Optimize;
	OptimizerExtension::Register(config, std::move(mssql_optimizer));
}

// Extension class methods
void MssqlExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

std::string MssqlExtension::Name() {
	return "mssql";
}

std::string MssqlExtension::Version() const {
	return GetMssqlExtensionVersion();
}

}  // namespace duckdb

extern "C" {

// Use the new DUCKDB_CPP_EXTENSION_ENTRY macro for loadable extension entry point
DUCKDB_CPP_EXTENSION_ENTRY(mssql, loader) {
	duckdb::LoadInternal(loader);
}
}
