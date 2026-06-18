// mssql_refresh_function.cpp
// Provides the mssql_refresh_cache() scalar function for manual metadata cache refresh

#include "catalog/mssql_refresh_function.hpp"
#include "catalog/mssql_catalog.hpp"
#include "mssql_compat.hpp"
#include "mssql_storage.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/vector.hpp"
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
// mssql_invalidate_cache - lazy point invalidation (catalog / schema / table)
//===----------------------------------------------------------------------===//

// Resolve an attached MSSQL catalog by name, throwing a clear error otherwise.
static MSSQLCatalog &ResolveMSSQLCatalog(ClientContext &context, const string &catalog_name, const char *fn) {
	if (catalog_name.empty()) {
		throw InvalidInputException("%s: catalog name is required", fn);
	}
	try {
		auto &raw_catalog = Catalog::GetCatalog(context, catalog_name);
		if (raw_catalog.GetCatalogType() != "mssql") {
			throw InvalidInputException("%s: catalog '%s' is not an MSSQL catalog (type: %s)", fn, catalog_name,
										raw_catalog.GetCatalogType());
		}
		return raw_catalog.Cast<MSSQLCatalog>();
	} catch (const InvalidInputException &) {
		throw;
	} catch (const std::exception &) {
		throw InvalidInputException(
			"%s: catalog '%s' not found. "
			"Attach a database first with: ATTACH '' AS %s (TYPE mssql, SECRET ...)",
			fn, catalog_name, catalog_name);
	}
}

// mssql_invalidate_cache(catalog [, schema [, table]]) -> BOOLEAN
// Lazy invalidation at the requested granularity (no eager reload):
//   1 arg  -> whole catalog          (InvalidateMetadataCache)
//   2 args -> one schema             (InvalidateSchemaTableSet)
//   3 args -> one table              (InvalidateTableEntry; keeps other tables' columns)
static void MSSQLInvalidateCacheExecute(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &client_context = state.GetContext();
	const idx_t col_count = args.ColumnCount();

	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto result_data = FlatVector::GetData<bool>(result);
	for (idx_t row = 0; row < args.size(); row++) {
		string catalog_name = args.GetValue(0, row).ToString();
		auto &catalog = ResolveMSSQLCatalog(client_context, catalog_name, "mssql_invalidate_cache");

		if (col_count >= 3 && !args.GetValue(2, row).IsNull() && !args.GetValue(1, row).IsNull()) {
			catalog.InvalidateTableEntry(args.GetValue(1, row).ToString(), args.GetValue(2, row).ToString());
		} else if (col_count >= 2 && !args.GetValue(1, row).IsNull()) {
			catalog.InvalidateSchemaTableSet(args.GetValue(1, row).ToString());
		} else {
			catalog.InvalidateMetadataCache();
		}
		result_data[row] = true;
	}
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

	// mssql_invalidate_cache(catalog [, schema [, table]]) -> BOOLEAN
	ScalarFunctionSet invalidate("mssql_invalidate_cache");
	for (auto &arg_types :
		 {vector<LogicalType>{LogicalType::VARCHAR}, vector<LogicalType>{LogicalType::VARCHAR, LogicalType::VARCHAR},
		  vector<LogicalType>{LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR}}) {
		ScalarFunction overload("mssql_invalidate_cache", arg_types, LogicalType::BOOLEAN, MSSQLInvalidateCacheExecute);
		overload.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
		invalidate.AddFunction(overload);
	}
	loader.RegisterFunction(invalidate);
}

}  // namespace duckdb
