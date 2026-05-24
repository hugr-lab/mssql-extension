// mssql_refresh_function.cpp
// Provides the mssql_refresh_cache() scalar function for manual metadata cache refresh

#include "catalog/mssql_refresh_function.hpp"
#include "catalog/mssql_catalog.hpp"
#include "mssql_compat.hpp"
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

MSSQL_BIND_SCALAR_SIG(MSSQLRefreshCacheBind) {
	MSSQL_BIND_SCALAR_PROLOGUE
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

		// Validate the catalog exists (Spec 047: per-catalog ownership)
		try {
			auto &catalog = Catalog::GetCatalog(context, catalog_name);
			if (catalog.GetCatalogType() != "mssql") {
				throw BinderException("mssql_refresh_cache: catalog '%s' is not an MSSQL catalog (type: %s)",
									  catalog_name, catalog.GetCatalogType());
			}
		} catch (const BinderException &) {
			throw;
		} catch (const std::exception &) {
			throw BinderException(
				"mssql_refresh_cache: catalog '%s' not found. "
				"Attach a database first with: ATTACH '' AS %s (TYPE mssql, SECRET ...)",
				catalog_name, catalog_name);
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

		// Get the MSSQL catalog (Spec 047: per-catalog ownership)
		MSSQLCatalog *catalog_ptr = nullptr;
		try {
			auto &raw_catalog = Catalog::GetCatalog(client_context, catalog_name);
			if (raw_catalog.GetCatalogType() != "mssql") {
				throw InvalidInputException("mssql_refresh_cache: catalog '%s' is not an MSSQL catalog (type: %s)",
											catalog_name, raw_catalog.GetCatalogType());
			}
			catalog_ptr = &raw_catalog.Cast<MSSQLCatalog>();
		} catch (const InvalidInputException &) {
			throw;
		} catch (const std::exception &) {
			throw InvalidInputException(
				"mssql_refresh_cache: catalog '%s' not found. "
				"Attach a database first with: ATTACH '' AS %s (TYPE mssql, SECRET ...)",
				catalog_name, catalog_name);
		}
		auto &catalog = *catalog_ptr;

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
	func.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	loader.RegisterFunction(func);
}

}  // namespace duckdb
