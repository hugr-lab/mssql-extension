//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// dml/mssql_dml_function.cpp
//
// Spec 080 D1 / PR 3b: the count vehicle of a pushed DML.
//
//   SELECT * FROM mssql_dml(context, statement [, params [, declarations]],
//                           kind := 'update' | 'delete',
//                           target_schema := ..., target_table := ...)
//
// What it owes the statement it replaces:
// - at least once: the binder of a SELECT leaves the result streamable
//   (ResultEagerness::AUTO, bind_select.cpp): its producers wait for the
//   client to decide how to take the result, and whether the source has been
//   initialised by then is a race (with no spare engine thread nothing runs).
//   The bind sets FORCED, which the planner copies from the global binder
//   state after binding, so the statement runs whole whatever the client
//   reads, as a native DML does;
// - exactly once: one InitGlobal, guarded by a latch; a source initialised
//   again (a recursive CTE's iteration) is refused rather than written twice.
//   Each EXECUTE of a PREPARE binds again (SupportStatementCache() false), so
//   each has its own latch;
// - UPDATE / DELETE only: a statement that may change the schema
//   (mssql::SqlMayChangeSchema) is refused -- it would bypass the cache rules
//   mssql_exec applies;
// - the write rules: CheckWriteAccess, and RegisterDBModify, through which
//   DuckDB applies a read-only database and its one-writable-database rule;
// - the result: one BIGINT named Count, the server's count (the last DONE that
//   carries one -- measured: an AFTER trigger's counts arrive before the
//   statement's, by batch and by RPC alike, and SET carries none);
// - atomic in autocommit (MSSQLStatementConnection's server transaction), on
//   the pinned connection inside a transaction;
// - the planner's row count after a DELETE, as the shipped path keeps it.
//===----------------------------------------------------------------------===//

#include "dml/mssql_dml_function.hpp"

#include <atomic>
#include <cstdio>

#include "catalog/mssql_catalog.hpp"
#include "catalog/mssql_table_entry.hpp"
#include "connection/mssql_connection_provider.hpp"
#include "connection/mssql_settings.hpp"
#include "dml/mssql_statement_connection.hpp"
#include "duckdb/common/enums/database_modification_type.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/planner/binder.hpp"
#include "mssql_counters.hpp"
#include "mssql_function_docs.hpp"
#include "query/mssql_ddl_detect.hpp"
#include "query/mssql_simple_query.hpp"
#include "query/mssql_sql_params.hpp"
#include "query/tds_info_log.hpp"

namespace duckdb {

namespace mssql {

void CountDml(DmlPath path) {
	static std::atomic<uint64_t> pushed{0};
	static std::atomic<uint64_t> staged{0};
	static std::atomic<uint64_t> values_join{0};
	const char *name = "";
	switch (path) {
	case DmlPath::PUSHED:
		++pushed;
		name = "pushed";
		break;
	case DmlPath::STAGED:
		++staged;
		name = "staged";
		break;
	case DmlPath::VALUES_JOIN:
		++values_join;
		name = "values_join";
		break;
	}
	if (CountersEnabled()) {
		fprintf(stderr, "[MSSQL COUNTERS] dml: %s (pushed=%llu staged=%llu values_join=%llu)\n", name,
				(unsigned long long)pushed.load(), (unsigned long long)staged.load(),
				(unsigned long long)values_join.load());
	}
}

}  // namespace mssql

namespace {

enum class DmlKind : uint8_t { UPDATE, DELETE };

struct MSSQLDmlBindData : public TableFunctionData {
	string context_name;
	string statement;
	tds::Request request;
	DmlKind kind = DmlKind::UPDATE;
	string schema_name;
	string table_name;
	//! Shared by the copies of this bind data (FunctionData::Copy), so a copy
	//! cannot reset it.
	shared_ptr<std::atomic<bool>> executed = make_shared_ptr<std::atomic<bool>>(false);

	unique_ptr<FunctionData> Copy() const override {
		auto copy = make_uniq<MSSQLDmlBindData>();
		copy->context_name = context_name;
		copy->statement = statement;
		copy->request = request;
		copy->kind = kind;
		copy->schema_name = schema_name;
		copy->table_name = table_name;
		copy->executed = executed;
		return std::move(copy);
	}
	bool Equals(const FunctionData &) const override {
		return false;
	}
	//! A prepared statement binds again for every EXECUTE: a cached plan would
	//! carry a latch already spent.
	bool SupportStatementCache() const override {
		return false;
	}
};

struct MSSQLDmlGlobalState : public GlobalTableFunctionState {
	int64_t count = 0;
	bool returned = false;
	idx_t MaxThreads() const override {
		return 1;
	}
};

MSSQLCatalog &DmlCatalog(ClientContext &context, const string &context_name) {
	optional_ptr<Catalog> catalog;
	try {
		catalog = &Catalog::GetCatalog(context, Identifier(context_name));
	} catch (const std::exception &) {
		catalog = nullptr;
	}
	if (!catalog || catalog->GetCatalogType() != "mssql") {
		throw InvalidInputException(
			"mssql_dml: Unknown context '%s'. Attach a database first with: ATTACH '' AS %s (TYPE mssql, SECRET ...)",
			context_name, context_name);
	}
	return catalog->Cast<MSSQLCatalog>();
}

string NamedString(const TableFunctionBindInput &input, const char *name) {
	auto entry = input.named_parameters.find(name);
	if (entry == input.named_parameters.end() || entry->second.IsNull()) {
		return string();
	}
	return entry->second.GetValue<string>();
}

unique_ptr<FunctionData> MSSQLDmlBind(ClientContext &context, TableFunctionBindInput &input,
									  vector<LogicalType> &return_types, vector<Identifier> &names) {
	if (input.inputs.size() < 2 || input.inputs.size() > 4) {
		throw InvalidInputException(
			"mssql_dml requires a context name and a statement, with an optional STRUCT of parameters and "
			"declaration list, and kind := / target_schema := / target_table :=");
	}
	auto bind_data = make_uniq<MSSQLDmlBindData>();
	bind_data->context_name = input.inputs[0].GetValue<string>();
	bind_data->statement = input.inputs[1].GetValue<string>();
	const auto kind = StringUtil::Lower(NamedString(input, "kind"));
	if (kind == "update") {
		bind_data->kind = DmlKind::UPDATE;
	} else if (kind == "delete") {
		bind_data->kind = DmlKind::DELETE;
	} else {
		throw InvalidInputException("mssql_dml: kind := must be 'update' or 'delete', got '%s'", kind);
	}
	bind_data->schema_name = NamedString(input, "target_schema");
	bind_data->table_name = NamedString(input, "target_table");
	if (bind_data->schema_name.empty() || bind_data->table_name.empty()) {
		throw InvalidInputException("mssql_dml: target_schema := and target_table := name the table it writes");
	}

	if (mssql::SqlMayChangeSchema(bind_data->statement)) {
		throw InvalidInputException(
			"mssql_dml runs an UPDATE or DELETE; use mssql_exec() for a statement that may "
			"change the schema");
	}

	auto &catalog = DmlCatalog(context, bind_data->context_name);
	catalog.CheckWriteAccess(kind == "update" ? "UPDATE" : "DELETE");
	// Without the binder there is no way to make the statement run whole or to
	// register the write with DuckDB: refused rather than run streamable and
	// past the read-only and one-writable-database rules. A plan copy rebinds
	// with no binder (LogicalGet::Deserialize); NotImplementedException is what
	// CTE inlining takes as "keep the CTE materialised" (cte_inlining.cpp), so
	// the statement then runs once.
	if (!input.binder) {
		throw NotImplementedException("mssql_dml: cannot be bound here (no binder to register the write with)");
	}
	auto &properties = input.binder->GetStatementProperties();
	properties.result_eagerness = ResultEagerness::FORCED;
	properties.RegisterDBModify(catalog, context,
								bind_data->kind == DmlKind::UPDATE ? DatabaseModificationType::UPDATE_DATA
																   : DatabaseModificationType::DELETE_DATA);

	if (input.inputs.size() >= 3) {
		string declarations_override;
		if (input.inputs.size() == 4 && !input.inputs[3].IsNull()) {
			declarations_override = input.inputs[3].GetValue<string>();
		}
		auto params = mssql::BuildSqlParams(input.inputs[2], declarations_override);
		bind_data->request = params.ExecuteSqlRequest(bind_data->statement);
	} else {
		bind_data->request = tds::Request(bind_data->statement);
	}

	return_types.push_back(LogicalType::BIGINT);
	names.emplace_back("Count");
	return std::move(bind_data);
}

int QueryTimeoutMs(ClientContext &context) {
	const int64_t seconds = LoadQueryTimeout(context);
	if (seconds <= 0) {
		return 0;
	}
	const int64_t ms = seconds * 1000;
	return ms > NumericLimits<int32_t>::Maximum() ? NumericLimits<int32_t>::Maximum() : static_cast<int>(ms);
}

unique_ptr<GlobalTableFunctionState> MSSQLDmlInitGlobal(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<MSSQLDmlBindData>();
	// Not an InternalException, which would invalidate the database: user SQL
	// can re-initialise a source (a recursive CTE's iteration).
	if (bind_data.executed->exchange(true)) {
		throw NotImplementedException(
			"mssql_dml: its source was initialised a second time, which would write twice "
			"(a recursive CTE?): %s",
			bind_data.statement);
	}
	auto &catalog = DmlCatalog(context, bind_data.context_name);
	const char *verb = bind_data.kind == DmlKind::UPDATE ? "UPDATE" : "DELETE";
	auto state = make_uniq<MSSQLDmlGlobalState>();

	// As the shipped DELETE does, the planner's estimate follows an autocommit
	// DELETE (inside a transaction the shared entry keeps committed state).
	// Looked up BEFORE the write: after it, a cold entry would load the count
	// that already excludes the deleted rows, and a failed lookup would report
	// a committed DELETE as failed. Best effort -- the estimate is a hint.
	optional_ptr<MSSQLTableEntry> deleted_from;
	if (bind_data.kind == DmlKind::DELETE && !ConnectionProvider::IsInTransaction(context, catalog)) {
		try {
			auto entry = catalog.GetEntry(context, CatalogType::TABLE_ENTRY, Identifier(bind_data.schema_name),
										  Identifier(bind_data.table_name), OnEntryNotFound::RETURN_NULL);
			if (entry) {
				deleted_from = &entry->Cast<MSSQLTableEntry>();
			}
		} catch (const std::exception &) {
			deleted_from = nullptr;
		}
	}

	MSSQLStatementConnection statement;
	auto connection = statement.Acquire(context, catalog);
	{
		auto pinned_lock = statement.LockPinned(context, catalog);
		MSSQLStatementConnection::RequireIdle(*connection, verb);
		auto result = MSSQLSimpleQuery::Execute(*connection, bind_data.request, QueryTimeoutMs(context));
		for (const auto &info : result.info_messages) {
			LogTdsInfo(context, info);
		}
		if (!result.success) {
			const bool pinned = statement.IsPinned();
			connection.reset();
			statement.Fail(context, catalog);
			throw IOException("MSSQL %s on '%s.%s' failed: %s %s", verb, bind_data.schema_name, bind_data.table_name,
							  result.DescribeError(),
							  pinned ? "(the open transaction must be rolled back)"
									 : "(nothing was written: the statement's server transaction was rolled back)");
		}
		state->count = result.rows_affected;
	}
	connection.reset();
	statement.Commit(context, catalog);
	mssql::CountDml(mssql::DmlPath::PUSHED);
	if (deleted_from) {
		deleted_from->NoteRowsDeleted(static_cast<idx_t>(state->count));
	}
	return std::move(state);
}

void MSSQLDmlFunction(ClientContext &, TableFunctionInput &input, DataChunk &output) {
	auto &state = input.global_state->Cast<MSSQLDmlGlobalState>();
	if (state.returned) {
		output.SetChildCardinality(0);
		return;
	}
	state.returned = true;
	FlatVector::GetDataMutable<int64_t>(output.data[0])[0] = state.count;
	output.SetChildCardinality(1);
}

unique_ptr<NodeStatistics> MSSQLDmlCardinality(ClientContext &, const FunctionData *) {
	return make_uniq<NodeStatistics>(1, 1);
}

}  // namespace

void RegisterMSSQLDmlFunction(ExtensionLoader &loader) {
	TableFunctionSet set("mssql_dml");
	for (idx_t arguments = 2; arguments <= 4; arguments++) {
		vector<LogicalType> types{LogicalType::VARCHAR, LogicalType::VARCHAR};
		if (arguments >= 3) {
			types.push_back(LogicalType::ANY);
		}
		if (arguments == 4) {
			types.push_back(LogicalType::VARCHAR);
		}
		TableFunction f("mssql_dml", types, MSSQLDmlFunction, MSSQLDmlBind, MSSQLDmlInitGlobal);
		f.GetSignature().AddKeywordOnly("kind", LogicalType::VARCHAR, Value(LogicalType::VARCHAR));
		f.GetSignature().AddKeywordOnly("target_schema", LogicalType::VARCHAR, Value(LogicalType::VARCHAR));
		f.GetSignature().AddKeywordOnly("target_table", LogicalType::VARCHAR, Value(LogicalType::VARCHAR));
		f.cardinality = MSSQLDmlCardinality;
		// `SELECT * FROM mssql_dml(...)` reports a changed-row count, as the
		// native DML it replaces does (bind_select.cpp's passthrough rule).
		f.call_return_type = StatementReturnType::CHANGED_ROWS;
		// Not a plan the common-subplan optimizer may fold: two equal calls are
		// two writes.
		f.verify_serialization = false;
		set.AddFunction(f);
	}
	mssql::RegisterDocumentedFunction(
		loader, std::move(set),
		{{},
		 "Internal: the vehicle a pushed UPDATE / DELETE runs through (spec 080). Runs the T-SQL statement once on "
		 "the attached database -- atomically in autocommit, on the transaction's connection inside one -- and "
		 "returns its row count as Count. The remote-pushdown rewriter's target, not an API: its arguments carry no "
		 "compatibility promise.",
		 {"SELECT * FROM mssql_dml('db', 'UPDATE [dbo].[t] SET [v] = 1 WHERE [id] = @p1', {'p1': 5}, '@p1 int', "
		  "kind := 'update', target_schema := 'dbo', target_table := 't')"},
		 {"dml"}});
}

}  // namespace duckdb
