// mssql_preload_catalog.cpp
// Provides the mssql_preload_catalog() scalar function for bulk metadata loading

#include "catalog/mssql_preload_catalog.hpp"
#include "catalog/mssql_catalog.hpp"
#include "mssql_storage.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/vector_operations/unary_executor.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"

namespace duckdb {

//===----------------------------------------------------------------------===//
// Bind Function - Validates arguments at compile time
//===----------------------------------------------------------------------===//

static unique_ptr<FunctionData> MSSQLPreloadCatalogBind(ClientContext &context, ScalarFunction &bound_function,
														vector<unique_ptr<Expression>> &arguments) {
	// First argument is the catalog name (must be constant)
	if (arguments[0]->HasParameter()) {
		throw InvalidInputException("mssql_preload_catalog: catalog_name must be a constant, not a parameter");
	}

	string catalog_name;
	string schema_name;

	if (arguments[0]->IsFoldable()) {
		auto catalog_val = ExpressionExecutor::EvaluateScalar(context, *arguments[0]);
		if (catalog_val.IsNull()) {
			throw InvalidInputException("mssql_preload_catalog: catalog name is required (got NULL)");
		}
		catalog_name = catalog_val.ToString();
		if (catalog_name.empty()) {
			throw InvalidInputException("mssql_preload_catalog: catalog name is required (got empty string)");
		}

		// Validate the catalog exists
		auto &manager = MSSQLContextManager::Get(*context.db);
		if (!manager.HasContext(catalog_name)) {
			throw BinderException(
				"mssql_preload_catalog: catalog '%s' not found. "
				"Attach a database first with: ATTACH '' AS %s (TYPE mssql, SECRET ...)",
				catalog_name, catalog_name);
		}

		auto ctx = manager.GetContext(catalog_name);
		if (!ctx || !ctx->attached_db) {
			throw BinderException("mssql_preload_catalog: catalog '%s' has no attached database", catalog_name);
		}

		auto &catalog = ctx->attached_db->GetCatalog();
		if (catalog.GetCatalogType() != "mssql") {
			throw BinderException("mssql_preload_catalog: catalog '%s' is not an MSSQL catalog (type: %s)",
								  catalog_name, catalog.GetCatalogType());
		}
	}

	// Optional second argument: schema_name
	if (arguments.size() > 1 && arguments[1]->IsFoldable()) {
		auto schema_val = ExpressionExecutor::EvaluateScalar(context, *arguments[1]);
		if (!schema_val.IsNull()) {
			schema_name = schema_val.ToString();
		}
	}

	return make_uniq<MSSQLPreloadCatalogBindData>(catalog_name, schema_name);
}

//===----------------------------------------------------------------------===//
// Execute Function - Performs the bulk catalog preload
//===----------------------------------------------------------------------===//

static void MSSQLPreloadCatalogExecute(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &bind_data = state.expr.Cast<BoundFunctionExpression>().bind_info->Cast<MSSQLPreloadCatalogBindData>();

	auto &catalog_names = args.data[0];

	UnaryExecutor::Execute<string_t, string_t>(catalog_names, result, args.size(), [&](string_t catalog_str) -> string_t {
		string catalog_name = bind_data.catalog_name;
		if (catalog_name.empty()) {
			catalog_name = catalog_str.GetString();
		}

		auto &client_context = state.GetContext();

		// Get the MSSQL context
		auto &manager = MSSQLContextManager::Get(*client_context.db);
		auto ctx = manager.GetContext(catalog_name);
		if (!ctx) {
			throw InvalidInputException(
				"mssql_preload_catalog: catalog '%s' not found. "
				"Attach a database first with: ATTACH '' AS %s (TYPE mssql, SECRET ...)",
				catalog_name, catalog_name);
		}

		if (!ctx->attached_db) {
			throw InvalidInputException("mssql_preload_catalog: catalog '%s' has no attached database", catalog_name);
		}

		// Get the MSSQL catalog
		auto &catalog = ctx->attached_db->GetCatalog().Cast<MSSQLCatalog>();
		auto &cache = catalog.GetMetadataCache();
		auto &pool = catalog.GetConnectionPool();

		// Ensure cache settings are loaded
		catalog.EnsureCacheLoaded(client_context);

		// Acquire connection
		auto connection = pool.Acquire();
		if (!connection) {
			throw IOException("mssql_preload_catalog: failed to acquire connection");
		}

		// Execute bulk preload
		idx_t schema_count = 0;
		idx_t table_count = 0;
		idx_t column_count = 0;
		cache.BulkLoadAll(*connection, bind_data.schema_name, schema_count, table_count, column_count);

		pool.Release(std::move(connection));

		// Build status message
		string status;
		if (bind_data.schema_name.empty()) {
			status = StringUtil::Format("Preloaded %llu schemas, %llu tables, %llu columns",
										schema_count, table_count, column_count);
		} else {
			status = StringUtil::Format("Preloaded schema '%s': %llu tables, %llu columns",
										bind_data.schema_name, table_count, column_count);
		}

		return StringVector::AddString(result, status);
	});
}

//===----------------------------------------------------------------------===//
// Registration
//===----------------------------------------------------------------------===//

void RegisterMSSQLPreloadCatalogFunction(ExtensionLoader &loader) {
	// mssql_preload_catalog(catalog_name VARCHAR [, schema_name VARCHAR]) -> VARCHAR
	ScalarFunction func("mssql_preload_catalog", {LogicalType::VARCHAR}, LogicalType::VARCHAR,
						MSSQLPreloadCatalogExecute, MSSQLPreloadCatalogBind);
	func.varargs = LogicalType::VARCHAR;  // Optional schema_name
	func.null_handling = FunctionNullHandling::SPECIAL_HANDLING;
	loader.RegisterFunction(func);
}

}  // namespace duckdb
