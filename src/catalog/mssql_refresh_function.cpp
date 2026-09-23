// mssql_refresh_function.cpp
// Provides the mssql_refresh_cache() scalar function for manual metadata cache refresh

#include "catalog/mssql_refresh_function.hpp"
#include "catalog/mssql_catalog.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "mssql_function_docs.hpp"
#include "mssql_storage.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/vector_operations/unary_executor.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"

namespace duckdb {

//===----------------------------------------------------------------------===//
// Bind Function - Validates arguments at compile time
//===----------------------------------------------------------------------===//

static duckdb::unique_ptr<duckdb::FunctionData> MSSQLRefreshCacheBind(duckdb::BindScalarFunctionInput &input) {
	auto &context = input.GetClientContext();
	auto &arguments = input.GetArguments();
	(void)context;
	(void)arguments;
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
			auto &catalog = Catalog::GetCatalog(context, Identifier(catalog_name));
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
	// Refused inside an explicit transaction (issue #380): a forced load there
	// either blocks on the transaction's own uncommitted DDL (a pool connection
	// waits on its schema lock until the metadata timeout) or, on the pinned
	// connection, would publish the transaction's uncommitted view into the cache
	// every other connection reads. Invalidation stays allowed.
	if (!state.GetContext().transaction.IsAutoCommit()) {
		throw InvalidInputException(
			"mssql_refresh_cache cannot run inside a transaction: it would load the catalog's "
			"metadata while the transaction may hold uncommitted changes. Use "
			"mssql_invalidate_cache() inside the transaction, or refresh after COMMIT");
	}
	auto &bind_data = state.expr.Cast<BoundFunctionExpression>().BindInfo()->Cast<MSSQLRefreshCacheBindData>();

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
			auto &raw_catalog = Catalog::GetCatalog(client_context, Identifier(catalog_name));
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
		auto &raw_catalog = Catalog::GetCatalog(context, Identifier(catalog_name));
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

// mssql_invalidate_cache(context [, schema [, table]]) -> BOOLEAN
// Lazy invalidation at the requested granularity (no eager reload):
//   1 arg  -> whole catalog          (InvalidateMetadataCache)
//   2 args -> one schema             (InvalidateSchemaTableSet)
//   3 args -> one table              (InvalidateTableEntry; keeps other tables' columns)
static void MSSQLInvalidateCacheExecute(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &client_context = state.GetContext();
	const idx_t col_count = args.ColumnCount();

	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto result_data = FlatVector::GetDataMutable<bool>(result);
	for (idx_t row = 0; row < args.size(); row++) {
		string catalog_name = args.GetValue(0, row).ToString();
		auto &catalog = ResolveMSSQLCatalog(client_context, catalog_name, "mssql_invalidate_cache");

		// Inside an explicit transaction the invalidation is also the
		// transaction's own (issue #380): it stops trusting the shared cache for
		// the name, and its end invalidates it again.
		if (col_count >= 3 && !args.GetValue(2, row).IsNull() && !args.GetValue(1, row).IsNull()) {
			catalog.InvalidateTableEntry(args.GetValue(1, row).ToString(), args.GetValue(2, row).ToString());
			catalog.NoteTransactionChange(client_context, args.GetValue(1, row).ToString(),
										  args.GetValue(2, row).ToString());
		} else if (col_count >= 2 && !args.GetValue(1, row).IsNull()) {
			catalog.InvalidateSchemaTableSet(args.GetValue(1, row).ToString());
			catalog.NoteTransactionChange(client_context, args.GetValue(1, row).ToString());
		} else {
			catalog.InvalidateMetadataCache();
			catalog.NoteTransactionChange(client_context);
		}
		result_data[row] = true;
	}
}

//===----------------------------------------------------------------------===//
// Registration
//===----------------------------------------------------------------------===//

void RegisterMSSQLRefreshCacheFunction(ExtensionLoader &loader) {
	// Both functions mutate the metadata cache: VOLATILE keeps the optimizer
	// from constant-folding the call at plan time (which executed the side
	// effect during optimization and tripped ExpressionExecutor's
	// CONSTANT_VECTOR assertion in debug builds — issue #178 finding D1).

	// mssql_refresh_cache(context VARCHAR) -> BOOLEAN
	ScalarFunction func("mssql_refresh_cache", {LogicalType::VARCHAR}, LogicalType::BOOLEAN, MSSQLRefreshCacheExecute,
						MSSQLRefreshCacheBind);
	func.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	func.SetVolatile();
	func.SetFallible();
	mssql::RegisterDocumentedFunction(loader, {func},
									  {{"context"},
									   "Reloads the whole metadata cache -- schemas, tables, columns -- of an attached "
									   "SQL Server database from the server, now.",
									   {"mssql_refresh_cache('db')"},
									   {"catalog"}});

	// mssql_invalidate_cache(context [, schema [, table]]) -> BOOLEAN
	vector<ScalarFunction> invalidate;
	for (auto &arg_types :
		 {vector<LogicalType>{LogicalType::VARCHAR}, vector<LogicalType>{LogicalType::VARCHAR, LogicalType::VARCHAR},
		  vector<LogicalType>{LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR}}) {
		ScalarFunction overload("mssql_invalidate_cache", arg_types, LogicalType::BOOLEAN, MSSQLInvalidateCacheExecute);
		overload.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
		overload.SetVolatile();
		overload.SetFallible();
		invalidate.push_back(overload);
	}
	mssql::RegisterDocumentedFunction(
		loader, std::move(invalidate),
		{{"context", "schema", "table"},
		 "Marks cached metadata of an attached SQL Server database as stale -- the whole catalog, one schema, or one "
		 "table -- so it is reloaded from the server on next use; everything else keeps its cache.",
		 {"mssql_invalidate_cache('db', 'dbo', 'orders')"},
		 {"catalog"}});
}

}  // namespace duckdb
