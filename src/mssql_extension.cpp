#include "mssql_extension.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/vector_operations/generic_executor.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {

// Extension version string
static const char *GetMssqlExtensionVersion() {
#ifdef EXT_VERSION_MSSQL
	return EXT_VERSION_MSSQL;
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
	// Register mssql_version() scalar function for load verification
	auto mssql_version_func = ScalarFunction("mssql_version", {},  // No arguments
											 LogicalType::VARCHAR, MssqlVersionFunction);
	loader.RegisterFunction(mssql_version_func);
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
