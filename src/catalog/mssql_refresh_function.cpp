// mssql_refresh_function.cpp
// Provides the mssql_refresh_cache() scalar function for manual metadata cache refresh

#include "catalog/mssql_refresh_function.hpp"
#include "catalog/mssql_catalog.hpp"
#include "mssql_storage.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/vector_operations/unary_executor.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"

namespace duckdb {

//===----------------------------------------------------------------------===//
// Bind Function - Validates arguments at compile time
//===----------------------------------------------------------------------===//

static unique_ptr<FunctionData> MSSQLRefreshCacheBind(ClientContext &context, ScalarFunction &bound_function,
													  vector<unique_ptr<Expression>> &arguments) {
	// First argument is the catalog name (must be constant)
	if (arguments[0]->HasParameter()) {
		throw InvalidInputException("mssql_refresh_cache: catalog_name must be a constant, not a parameter");
	}

	// Extract the catalog name if it's a constant
	string catalog_name;
	if (arguments[0]->IsFoldable()) {
		auto catalog_val = ExpressionExecutor::EvaluateScalar(context, *arguments[0]);

		// Check for NULL
		if (catalog_val.IsNull()) {
			throw InvalidInputException("mssql_refresh_cache: catalog name is required (got NULL)");
		}

		catalog_name = catalog_val.ToString();

		// Check for empty string
		if (catalog_name.empty()) {
			throw InvalidInputException("mssql_refresh_cache: catalog name is required (got empty string)");
		}

		// Validate the catalog exists via MSSQLContextManager
		auto &manager = MSSQLContextManager::Get(*context.db);
		if (!manager.HasContext(catalog_name)) {
			throw BinderException(
				"mssql_refresh_cache: catalog '%s' not found. "
				"Attach a database first with: ATTACH '' AS %s (TYPE mssql, SECRET ...)",
				catalog_name, catalog_name);
		}

		// Verify it's an MSSQL catalog
		auto ctx = manager.GetContext(catalog_name);
		if (!ctx || !ctx->attached_db) {
			throw BinderException("mssql_refresh_cache: catalog '%s' has no attached database", catalog_name);
		}

		// Check catalog type
		auto &catalog = ctx->attached_db->GetCatalog();
		if (catalog.GetCatalogType() != "mssql") {
			throw BinderException("mssql_refresh_cache: catalog '%s' is not an MSSQL catalog (type: %s)", catalog_name,
								  catalog.GetCatalogType());
		}
	}

	return make_uniq<MSSQLRefreshCacheBindData>(catalog_name);
}

//===----------------------------------------------------------------------===//
// Execute Function - Performs the actual cache refresh
//===----------------------------------------------------------------------===//

static void MSSQLRefreshCacheExecute(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &bind_data = state.expr.Cast<BoundFunctionExpression>().bind_info->Cast<MSSQLRefreshCacheBindData>();

	auto &catalog_names = args.data[0];

	UnaryExecutor::Execute<string_t, bool>(catalog_names, result, args.size(), [&](string_t catalog_str) -> bool {
		// Get the catalog name from bind data or runtime argument
		string catalog_name = bind_data.catalog_name;
		if (catalog_name.empty()) {
			catalog_name = catalog_str.GetString();
		}

		// Get the client context
		auto &client_context = state.GetContext();

		// Get the MSSQL context
		auto &manager = MSSQLContextManager::Get(*client_context.db);
		auto ctx = manager.GetContext(catalog_name);
		if (!ctx) {
			throw InvalidInputException(
				"mssql_refresh_cache: catalog '%s' not found. "
				"Attach a database first with: ATTACH '' AS %s (TYPE mssql, SECRET ...)",
				catalog_name, catalog_name);
		}

		if (!ctx->attached_db) {
			throw InvalidInputException("mssql_refresh_cache: catalog '%s' has no attached database", catalog_name);
		}

		// Get the MSSQL catalog
		auto &catalog = ctx->attached_db->GetCatalog().Cast<MSSQLCatalog>();

		// Perform full cache refresh (invalidates and reloads all metadata)
		catalog.RefreshCache(client_context);

		// Return true to indicate success
		return true;
	});
}

//===----------------------------------------------------------------------===//
// Registration
//===----------------------------------------------------------------------===//

void RegisterMSSQLRefreshCacheFunction(ExtensionLoader &loader) {
	// mssql_refresh_cache(catalog_name VARCHAR) -> BOOLEAN
	ScalarFunction func("mssql_refresh_cache", {LogicalType::VARCHAR}, LogicalType::BOOLEAN, MSSQLRefreshCacheExecute,
						MSSQLRefreshCacheBind);
	func.null_handling = FunctionNullHandling::SPECIAL_HANDLING;
	loader.RegisterFunction(func);
}

}  // namespace duckdb
