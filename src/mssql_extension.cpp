#include "mssql_extension.hpp"
#include "azure/azure_test_function.hpp"
#include "catalog/mssql_preload_catalog.hpp"
#include "catalog/mssql_refresh_function.hpp"
#include "connection/mssql_diagnostic.hpp"
#include "connection/mssql_settings.hpp"
#include "copy/copy_function.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/extension_type_info.hpp"
#include "duckdb/function/cast/default_casts.hpp"
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
// Bound to a VARCHAR carrying the length in ExtensionTypeInfo, WITH an alias —
// and the alias is not optional. Without it the type looks like a plain VARCHAR
// to function binding (which is nicer: upper(), ||, = and LIKE keep working),
// but it then crashes the binder outright: `CREATE TABLE t (a MSSQL_NVARCHAR(50));
// INSERT INTO t VALUES ('x')` segfaults on a stack overflow, in plain DuckDB with
// no catalog involved. DuckDB supports extension types only in their aliased
// form. The cost is that operations on the type need a cast back, which is
// acceptable because this type exists to DECLARE a target column: it is written
// last, feeding the sink — `upper(cust)::MSSQL_NVARCHAR(50)` — and nothing is
// applied to it afterwards. Measured both ways; see spec 060 § 2a.
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
	type.SetAlias("MSSQL_NVARCHAR");
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

	// Spec 060: parameterised target types (see the note above).
	{
		auto nvarchar_type = LogicalType(LogicalTypeId::VARCHAR);
		nvarchar_type.SetAlias("MSSQL_NVARCHAR");
		loader.RegisterType("MSSQL_NVARCHAR", nvarchar_type, BindMssqlNVarchar);
		// The alias is required (it is what keeps the binder from recursing), but
		// on its own it makes the type opaque to every VARCHAR function. Both
		// directions are registered as implicit no-op casts at cost 0: the values
		// ARE varchars, so overload resolution should reach upper(), ||, LIKE and
		// the rest without the user writing a cast back.
		loader.RegisterCastFunction(nvarchar_type, LogicalType::VARCHAR, BoundCastInfo(DefaultCasts::NopCast), 0);
		loader.RegisterCastFunction(LogicalType::VARCHAR, nvarchar_type, BoundCastInfo(DefaultCasts::NopCast), 0);
	}

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
