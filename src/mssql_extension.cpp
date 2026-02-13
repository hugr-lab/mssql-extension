#include "mssql_extension.hpp"
#include "azure/azure_test_function.hpp"
#include "catalog/mssql_preload_catalog.hpp"
#include "catalog/mssql_refresh_function.hpp"
#include "connection/mssql_diagnostic.hpp"
#include "connection/mssql_settings.hpp"
#include "copy/copy_function.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/vector_operations/generic_executor.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "mssql_functions.hpp"
#include "mssql_secret.hpp"
#include "mssql_storage.hpp"

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

	// 10. Register utility functions (mssql_version)
	auto mssql_version_func = ScalarFunction("mssql_version", {},  // No arguments
											 LogicalType::VARCHAR, MssqlVersionFunction);
	loader.RegisterFunction(mssql_version_func);

	// 11. Register Azure authentication test function
	mssql::azure::RegisterAzureTestFunction(loader);
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
