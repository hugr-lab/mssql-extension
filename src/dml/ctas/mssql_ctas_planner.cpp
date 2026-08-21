#include "dml/ctas/mssql_ctas_planner.hpp"
#include "catalog/mssql_catalog.hpp"
#include "catalog/mssql_ddl_translator.hpp"
#include "codec/target_string_type.hpp"
#include "dml/ctas/mssql_physical_ctas.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"

#include <cstdio>
#include <cstdlib>

namespace duckdb {
namespace mssql {

//===----------------------------------------------------------------------===//
// Debug Logging (T045)
//===----------------------------------------------------------------------===//

static int GetCTASPlannerDebugLevel() {
	const char *env = std::getenv("MSSQL_DEBUG");
	return env ? std::atoi(env) : 0;
}

#define CTAS_PLANNER_DEBUG_LOG(lvl, fmt, ...)                         \
	do {                                                              \
		if (GetCTASPlannerDebugLevel() >= lvl)                        \
			fprintf(stderr, "[MSSQL CTAS] " fmt "\n", ##__VA_ARGS__); \
	} while (0)

//===----------------------------------------------------------------------===//
// CTASPlanner::Plan
//===----------------------------------------------------------------------===//

PhysicalOperator &CTASPlanner::Plan(ClientContext &context, PhysicalPlanGenerator &planner, MSSQLCatalog &catalog,
									LogicalCreateTable &op, PhysicalOperator &child_plan) {
	// Load CTAS configuration from settings
	CTASConfig config = CTASConfig::Load(context);

	// Issue #225: a VARCHAR target only round-trips non-ASCII if its collation is
	// a UTF-8 one. Without that, SQL Server converts on INSERT to the database's
	// code page and replaces anything outside it with '?' — no error, no warning,
	// and nothing downstream can tell, because '?' is valid UTF-8.
	//
	// Spec 060: an explicit MSSQL_VARCHAR(n) cast asks for exactly the same
	// single-byte column that mssql_ctas_text_type='VARCHAR' asks for globally,
	// so it needs the same collation. A column that named its own needs neither,
	// and the rule itself lives on the catalog so CREATE TABLE, CTAS and COPY
	// cannot drift apart on it.
	// Fabric has no NVARCHAR at all, so the setting cannot be honoured there and
	// nvarchar(max) — the default everywhere else — fails outright. Forcing
	// VARCHAR is what makes CTAS work on a warehouse; its collation is UTF-8, so
	// nothing is lost but the unit the length counts in.
	if (catalog.RequiresSingleByteText()) {
		config.text_type = CTASTextType::VARCHAR;
	}
	catalog.ValidateStringTargets(child_plan.types);

	// Spec 060 D5: the WITH clause of THIS statement, over the session defaults —
	// the same order MSSQLSchemaEntry::CreateTable applies for a plain CREATE
	// TABLE. Only that path called ApplyWithClause, so
	// `CREATE TABLE t WITH (table_kind = 'columnstore') AS SELECT ...` was
	// accepted and then dropped on the floor: the table came out a heap, and even
	// `table_kind = 'sideways'` passed without a word, while the same clause on a
	// plain CREATE TABLE errors. Every option went the same way — clustered_index
	// and data_compression too.
	config.table_options.ApplyWithClause(op.info->Base().options);
	catalog.ValidateTableOptions(config.table_options);

	bool wants_varchar = config.text_type == CTASTextType::VARCHAR;
	if (!wants_varchar) {
		for (const auto &type : child_plan.types) {
			if (codec::NeedsVarcharCollation(type)) {
				wants_varchar = true;
				break;
			}
		}
	}
	config.varchar_collation = catalog.ResolveVarcharCollation(context, wants_varchar);
	if (wants_varchar) {
		config.wire_varchar_collation = catalog.WireVarcharCollation(config.varchar_collation);
	}
	if (!config.varchar_collation.empty()) {
		CTAS_PLANNER_DEBUG_LOG(1, "VARCHAR target: collating as %s", config.varchar_collation.c_str());
	}

	// Extract target table information
	CTASTarget target = ExtractTarget(op, catalog);

	// Map columns from child plan to SQL Server types
	vector<CTASColumnDef> columns = MapColumns(op, child_plan, config);

	// Validate at least one column
	if (columns.empty()) {
		throw InvalidInputException("CTAS requires at least one column from the source query.");
	}

	// Result types: CTAS returns BIGINT row count
	vector<LogicalType> result_types;
	result_types.push_back(LogicalType::BIGINT);

	// Create the physical operator
	auto &physical_ctas =
		planner.Make<MSSQLPhysicalCreateTableAs>(std::move(result_types), op.estimated_cardinality, catalog,
												 std::move(target), std::move(columns), std::move(config));

	// Add child operator (the SELECT query)
	physical_ctas.children.push_back(child_plan);

	return physical_ctas;
}

//===----------------------------------------------------------------------===//
// CTASPlanner::ExtractTarget
//===----------------------------------------------------------------------===//

CTASTarget CTASPlanner::ExtractTarget(const LogicalCreateTable &op, MSSQLCatalog &catalog) {
	CTASTarget target;

	// Get catalog name from the MSSQL catalog
	target.catalog_name = catalog.GetName().GetIdentifierName();

	// Extract schema and table from CreateTableInfo via BoundCreateTableInfo
	// The schema is in op.schema (SchemaCatalogEntry&)
	target.schema_name = op.schema.name.GetIdentifierName();

	// Get table name and on_conflict from the base CreateTableInfo
	auto &base_info = op.info->Base();
	target.table_name = base_info.GetTableName().GetIdentifierName();

	// Check for OR REPLACE (from on_conflict behavior)
	target.on_conflict = base_info.on_conflict;
	target.or_replace = (base_info.on_conflict == OnCreateConflict::REPLACE_ON_CONFLICT);

	// Check for IF NOT EXISTS (Issue #44)
	target.if_not_exists = (base_info.on_conflict == OnCreateConflict::IGNORE_ON_CONFLICT);

	return target;
}

//===----------------------------------------------------------------------===//
// CTASPlanner::MapColumns
//===----------------------------------------------------------------------===//

vector<CTASColumnDef> CTASPlanner::MapColumns(const LogicalCreateTable &op, PhysicalOperator &child_plan,
											  const CTASConfig &config) {
	vector<CTASColumnDef> columns;

	// Get the output types from the child plan
	const auto &child_types = child_plan.types;

	// Get base CreateTableInfo for column information
	auto &base_info = op.info->Base();

	// The columns in CreateTableInfo should match child_types count
	if (base_info.columns.LogicalColumnCount() != child_types.size()) {
		throw InternalException("CTAS column count mismatch: expected %llu, got %llu",
								(unsigned long long)child_types.size(),
								(unsigned long long)base_info.columns.LogicalColumnCount());
	}

	idx_t col_idx = 0;
	for (auto &col : base_info.columns.Logical()) {
		CTASColumnDef col_def;

		// Column name
		col_def.name = col.GetName().GetIdentifierName();

		// DuckDB type from child plan. Spec 060: a plain VARCHAR picks up the
		// session's default target type here, once, so the DDL text, the INSERT
		// BULK declaration and the encoder's length guard all read the same
		// annotation rather than each learning the policy separately. A column
		// that states its own type is left alone.
		col_def.duckdb_type =
			codec::ApplyDefaultStringType(child_types[col_idx], config.text_type != CTASTextType::VARCHAR,
										  config.default_string_length, config.varchar_collation);

		// Map to SQL Server type using CTAS-specific mapper (FR-012, FR-013)
		try {
			col_def.mssql_type = MSSQLDDLTranslator::MapLogicalTypeToCTAS(col_def.duckdb_type, config);
		} catch (NotImplementedException &e) {
			// Enhance error message with column name (FR-012)
			throw NotImplementedException("CTAS failed for column '%s': %s", col_def.name, e.what());
		}

		// Nullability - assume nullable unless source is NOT NULL
		// For CTAS, we typically allow NULL for all columns since we can't
		// determine NOT NULL from the source query plan
		col_def.nullable = true;

		columns.push_back(std::move(col_def));
		col_idx++;
	}

	return columns;
}

}  // namespace mssql
}  // namespace duckdb
