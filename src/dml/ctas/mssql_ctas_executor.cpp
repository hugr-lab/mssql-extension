#include "dml/ctas/mssql_ctas_executor.hpp"
#include "catalog/mssql_catalog.hpp"
#include "catalog/mssql_ddl_translator.hpp"
#include "dml/insert/mssql_insert_executor.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "query/mssql_simple_query.hpp"
#include "tds/tds_connection.hpp"
#include "tds/tds_connection_pool.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace duckdb {
namespace mssql {

//===----------------------------------------------------------------------===//
// Debug Logging Helpers
//===----------------------------------------------------------------------===//

static int GetDebugLevel() {
	const char *env = std::getenv("MSSQL_DEBUG");
	if (!env) {
		env = std::getenv("MSSQL_DML_DEBUG");
	}
	if (!env) {
		return 0;
	}
	return std::atoi(env);
}

static void DebugLog(int level, const char *format, ...) {
	if (GetDebugLevel() < level) {
		return;
	}
	va_list args;
	va_start(args, format);
	fprintf(stderr, "[MSSQL CTAS] ");
	vfprintf(stderr, format, args);
	fprintf(stderr, "\n");
	va_end(args);
}

//===----------------------------------------------------------------------===//
// CTASExecutionState::Initialize
//===----------------------------------------------------------------------===//

void CTASExecutionState::Initialize(MSSQLCatalog &catalog_ref, CTASTarget target_p, vector<CTASColumnDef> columns_p,
									CTASConfig config_p) {
	catalog = &catalog_ref;
	target = std::move(target_p);
	columns = std::move(columns_p);
	config = std::move(config_p);
	phase = CTASPhase::PENDING;
	start_time = std::chrono::steady_clock::now();

	// Generate DDL SQL
	ddl_sql = MSSQLDDLTranslator::TranslateCreateTableFromSchema(target.schema_name, target.table_name, columns);
	ddl_bytes = ddl_sql.size();

	DebugLog(1, "Initialized CTAS for %s (DDL: %llu bytes, %llu columns)", target.GetQualifiedName().c_str(),
			 (unsigned long long)ddl_bytes, (unsigned long long)columns.size());
}

//===----------------------------------------------------------------------===//
// CTASExecutionState::ExecuteDDL
//===----------------------------------------------------------------------===//

void CTASExecutionState::ExecuteDDL(ClientContext &context) {
	phase = CTASPhase::DDL_EXECUTING;
	auto ddl_start = std::chrono::steady_clock::now();

	DebugLog(2, "Executing DDL: %s", ddl_sql.c_str());

	try {
		// Execute CREATE TABLE using catalog's DDL execution method
		catalog->ExecuteDDL(context, ddl_sql);

		auto ddl_end = std::chrono::steady_clock::now();
		ddl_time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(ddl_end - ddl_start).count();

		DebugLog(1, "DDL completed in %lld ms", ddl_time_ms);

		phase = CTASPhase::DDL_DONE;

		// Initialize insert executor for the DML phase
		// Build MSSQLInsertTarget from our CTASColumnDef vector
		// Store in member variable so it remains valid for insert_executor's lifetime
		insert_target.catalog_name = target.catalog_name;
		insert_target.schema_name = target.schema_name;
		insert_target.table_name = target.table_name;
		insert_target.has_identity_column = false;
		insert_target.identity_column_index = 0;
		insert_target.columns.clear();
		insert_target.insert_column_indices.clear();

		for (idx_t i = 0; i < columns.size(); i++) {
			MSSQLInsertColumn col;
			col.name = columns[i].name;
			col.duckdb_type = columns[i].duckdb_type;
			col.mssql_type = columns[i].mssql_type;
			col.is_identity = false;
			col.is_nullable = columns[i].nullable;
			col.has_default = false;
			col.collation = "";
			col.precision = 0;
			col.scale = 0;
			insert_target.columns.push_back(std::move(col));
			insert_target.insert_column_indices.push_back(i);
		}

		// Build MSSQLInsertConfig from CTASConfig
		// Store in member variable so it remains valid for insert_executor's lifetime
		insert_config.batch_size = config.batch_size;
		insert_config.max_rows_per_statement = config.max_rows_per_statement;
		insert_config.max_sql_bytes = config.max_sql_bytes;
		insert_config.use_returning_output = false;	 // CTAS never uses RETURNING

		insert_executor = make_uniq<MSSQLInsertExecutor>(context, insert_target, insert_config);

		phase = CTASPhase::INSERT_EXECUTING;

	} catch (std::exception &e) {
		error_message = e.what();
		phase = CTASPhase::FAILED;
		throw;
	}
}

//===----------------------------------------------------------------------===//
// CTASExecutionState::ExecuteDrop
//===----------------------------------------------------------------------===//

void CTASExecutionState::ExecuteDrop(ClientContext &context) {
	string drop_sql = MSSQLDDLTranslator::TranslateDropTable(target.schema_name, target.table_name);

	DebugLog(2, "Executing DROP for OR REPLACE: %s", drop_sql.c_str());

	try {
		catalog->ExecuteDDL(context, drop_sql);
		DebugLog(1, "DROP TABLE completed for OR REPLACE");
	} catch (std::exception &e) {
		// DROP failed - rethrow with context
		throw InvalidInputException("CTAS OR REPLACE failed: could not drop existing table '%s': %s",
									target.GetQualifiedName(), e.what());
	}
}

//===----------------------------------------------------------------------===//
// CTASExecutionState::TableExists
//===----------------------------------------------------------------------===//

bool CTASExecutionState::TableExists(ClientContext &context) {
	string check_sql =
		StringUtil::Format("SELECT 1 FROM INFORMATION_SCHEMA.TABLES WHERE TABLE_SCHEMA = '%s' AND TABLE_NAME = '%s'",
						   MSSQLDDLTranslator::EscapeStringLiteral(target.schema_name),
						   MSSQLDDLTranslator::EscapeStringLiteral(target.table_name));

	auto &pool = catalog->GetConnectionPool();
	auto conn = pool.Acquire();
	if (!conn) {
		throw IOException("Failed to acquire connection to check table existence");
	}

	try {
		auto result = MSSQLSimpleQuery::Execute(*conn, check_sql);
		pool.Release(std::move(conn));
		return result.HasRows();
	} catch (...) {
		pool.Release(std::move(conn));
		throw;
	}
}

//===----------------------------------------------------------------------===//
// CTASExecutionState::SchemaExists
//===----------------------------------------------------------------------===//

bool CTASExecutionState::SchemaExists(ClientContext &context) {
	string check_sql = StringUtil::Format("SELECT 1 FROM INFORMATION_SCHEMA.SCHEMATA WHERE SCHEMA_NAME = '%s'",
										  MSSQLDDLTranslator::EscapeStringLiteral(target.schema_name));

	auto &pool = catalog->GetConnectionPool();
	auto conn = pool.Acquire();
	if (!conn) {
		throw IOException("Failed to acquire connection to check schema existence");
	}

	try {
		auto result = MSSQLSimpleQuery::Execute(*conn, check_sql);
		pool.Release(std::move(conn));
		return result.HasRows();
	} catch (...) {
		pool.Release(std::move(conn));
		throw;
	}
}

//===----------------------------------------------------------------------===//
// CTASExecutionState::FlushInserts
//===----------------------------------------------------------------------===//

void CTASExecutionState::FlushInserts(ClientContext &context) {
	if (!insert_executor) {
		return;
	}

	auto insert_start = std::chrono::steady_clock::now();

	try {
		insert_executor->Finalize();

		auto insert_end = std::chrono::steady_clock::now();
		insert_time_ms += std::chrono::duration_cast<std::chrono::milliseconds>(insert_end - insert_start).count();

		DebugLog(1, "INSERT finalized: %llu rows in %lld ms", (unsigned long long)rows_inserted, insert_time_ms);

	} catch (std::exception &e) {
		error_message = e.what();
		throw;
	}
}

//===----------------------------------------------------------------------===//
// CTASExecutionState::AttemptCleanup
//===----------------------------------------------------------------------===//

void CTASExecutionState::AttemptCleanup(ClientContext &context) {
	if (phase == CTASPhase::PENDING || phase == CTASPhase::DDL_EXECUTING) {
		// Table was never created, nothing to clean up
		return;
	}

	DebugLog(1, "Attempting cleanup DROP TABLE due to failure");

	string drop_sql = MSSQLDDLTranslator::TranslateDropTable(target.schema_name, target.table_name);

	try {
		catalog->ExecuteDDL(context, drop_sql);
		DebugLog(1, "Cleanup DROP TABLE succeeded");
	} catch (std::exception &e) {
		cleanup_error = e.what();
		DebugLog(1, "Cleanup DROP TABLE failed: %s", cleanup_error.c_str());
	}
}

//===----------------------------------------------------------------------===//
// CTASExecutionState::InvalidateCache
//===----------------------------------------------------------------------===//

void CTASExecutionState::InvalidateCache() {
	if (catalog) {
		catalog->InvalidateMetadataCache();
		DebugLog(2, "Catalog cache invalidated");
	}
}

//===----------------------------------------------------------------------===//
// CTASExecutionState::LogMetrics
//===----------------------------------------------------------------------===//

void CTASExecutionState::LogMetrics() const {
	if (GetDebugLevel() < 1) {
		return;
	}

	auto now = std::chrono::steady_clock::now();
	auto total_time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - start_time).count();

	fprintf(stderr, "[MSSQL CTAS] Metrics:\n");
	fprintf(stderr, "  Target: %s\n", target.GetQualifiedName().c_str());
	fprintf(stderr, "  OR REPLACE: %s\n", target.or_replace ? "yes" : "no");
	fprintf(stderr, "  DDL bytes: %llu\n", (unsigned long long)ddl_bytes);
	fprintf(stderr, "  DDL time: %lld ms\n", (long long)ddl_time_ms);
	fprintf(stderr, "  Rows produced: %llu\n", (unsigned long long)rows_produced);
	fprintf(stderr, "  Rows inserted: %llu\n", (unsigned long long)rows_inserted);
	fprintf(stderr, "  INSERT time: %lld ms\n", (long long)insert_time_ms);
	fprintf(stderr, "  Total time: %lld ms\n", (long long)total_time_ms);
	fprintf(stderr, "  Phase: %s\n", GetPhaseName(phase).c_str());
	if (!error_message.empty()) {
		fprintf(stderr, "  Error: %s\n", error_message.c_str());
	}
	if (!cleanup_error.empty()) {
		fprintf(stderr, "  Cleanup error: %s\n", cleanup_error.c_str());
	}
}

//===----------------------------------------------------------------------===//
// CTASExecutionState::GetPhaseName
//===----------------------------------------------------------------------===//

string CTASExecutionState::GetPhaseName(CTASPhase phase) {
	switch (phase) {
	case CTASPhase::PENDING:
		return "PENDING";
	case CTASPhase::DDL_EXECUTING:
		return "DDL_EXECUTING";
	case CTASPhase::DDL_DONE:
		return "DDL_DONE";
	case CTASPhase::INSERT_EXECUTING:
		return "INSERT_EXECUTING";
	case CTASPhase::COMPLETE:
		return "COMPLETE";
	case CTASPhase::FAILED:
		return "FAILED";
	default:
		return "UNKNOWN";
	}
}

//===----------------------------------------------------------------------===//
// CTASObservability::Log
//===----------------------------------------------------------------------===//

void CTASObservability::Log(int level) const {
	if (GetDebugLevel() < level) {
		return;
	}

	fprintf(stderr, "[MSSQL CTAS] Observability:\n");
	fprintf(stderr, "  Target: %s\n", target_table.c_str());
	fprintf(stderr, "  OR REPLACE: %s\n", or_replace ? "yes" : "no");
	fprintf(stderr, "  DDL bytes: %llu\n", (unsigned long long)ddl_bytes);
	fprintf(stderr, "  DDL time: %lld ms\n", (long long)ddl_time_ms);
	fprintf(stderr, "  Rows produced: %llu\n", (unsigned long long)rows_produced);
	fprintf(stderr, "  Rows inserted: %llu\n", (unsigned long long)rows_inserted);
	fprintf(stderr, "  Batches: %llu\n", (unsigned long long)batches_executed);
	fprintf(stderr, "  INSERT time: %lld ms\n", (long long)insert_time_ms);
	fprintf(stderr, "  Success: %s\n", success ? "yes" : "no");
	if (!success) {
		fprintf(stderr, "  Failure phase: %s\n", failure_phase.c_str());
		fprintf(stderr, "  Error: %s\n", error_message.c_str());
	}
}

}  // namespace mssql
}  // namespace duckdb
