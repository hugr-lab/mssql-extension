#include "catalog/mssql_catalog.hpp"

#include "catalog/mssql_catalog_debug.hpp"
#include "catalog/mssql_table_entry.hpp"
#include "connection/mssql_settings.hpp"
#include "duckdb/catalog/catalog_search_path.hpp"
#include "duckdb/main/client_data.hpp"
#include "duckdb/parser/expression/cast_expression.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"
#include "pushdown/mssql_pushdown_resolution.hpp"
#include "pushdown/mssql_query_tree.hpp"
#include "pushdown/mssql_sql_writer.hpp"

namespace duckdb {

using mssql::CollectPushdownTables;
using mssql::ExtendScope;
using mssql::IsSetOperationChild;
using mssql::KeepsChildSelectLists;
using mssql::MayRepeatOutputNames;
using mssql::NamesScopeTable;
using mssql::NoteOriginalTableName;
using mssql::RestoreTableNames;
using mssql::SelectStarFrom;
using mssql::VisitNode;
using mssql::WrittenName;
using mssql::WrittenUnqualified;
using mssql::WrittenUnqualifiedProbe;

// The schema a pushed statement's base table is in. No schema means the
// default schema, where the rewriter's own lookup of `main` was answered
// (LookupSchema) -- never "any schema": after the strip `db.t` and `db.s2.t`
// would otherwise both match the newest note named `t`. Before the rewriter
// strips the catalog (the dry run), `db.t` parses as schema.name with no
// catalog, so a "schema" equal to this catalog's name is the catalog; after the
// strip (RemoteExecute), and whenever a catalog is written too (`db.db.t`), a
// schema is a real one.
string MSSQLCatalog::PushdownSchemaOf(const BaseTableRef &ref, bool stripped) const {
	const auto &name = ref.GetQualifiedName();
	auto schema = name.Schema().GetIdentifierName();
	if (schema.empty()) {
		return default_schema_;
	}
	if (!stripped && name.Catalog().empty() && StringUtil::CIEquals(schema, GetName().GetIdentifierName())) {
		return default_schema_;
	}
	return schema;
}

// An unqualified `t` binds through the session's search path (`USE db.sales`),
// but the rewriter looks it up in schema `main`, which LookupSchema answers
// with the default schema -- so pushing it could read `dbo.t` where the binder
// reads `sales.t` (measured before this check). Pushed only when every search
// path entry of this catalog is its default schema.
bool MSSQLCatalog::SearchPathIsDefaultSchema(ClientContext &context) const {
	for (auto &entry : ClientData::Get(context).catalog_search_path->Get()) {
		if (!StringUtil::CIEquals(entry.GetCatalog().GetIdentifierName(), GetName().GetIdentifierName())) {
			continue;
		}
		const auto &schema = entry.GetSchema().GetIdentifierName();
		if (!schema.empty() && schema != default_schema_ && schema != DEFAULT_SCHEMA) {
			return false;
		}
	}
	return true;
}

// A name the query wrote without a schema: `t`, and `db.t` too -- DuckDB's
// binder reads both through the catalog's search-path entry (`USE db.sales`
// makes `db.t` sales.t; measured in the E1 review, where `db.t` read dbo.t).
bool MSSQLCatalog::WrittenWithoutSchema(const BaseTableRef &ref) const {
	auto &name = WrittenName(ref);
	return name.Schema().empty() || (name.Catalog().empty() && StringUtil::CIEquals(name.Schema().GetIdentifierName(),
																					GetName().GetIdentifierName()));
}

// The table a pushed base-table reference names. With a context (RemoteExecute)
// it is looked up through this catalog like any binder lookup, so the kept run
// never depends on what the thread noted; without one (the SupportsPushdown
// hooks get none) it is the entry the rewriter's own lookup just noted.
MSSQLCatalog::PushdownTable MSSQLCatalog::ResolvePushdownTable(const BaseTableRef &ref,
															   optional_ptr<ClientContext> context) {
	PushdownTable result;
	const auto schema = PushdownSchemaOf(ref, context != nullptr);
	const auto &name = ref.Table().GetIdentifierName();
	if (context) {
		// As below: a name without a schema is this catalog's default schema
		// only when the search path says so. Asked by the name the query wrote
		// -- the rewriter has stripped the catalog by now.
		if (WrittenWithoutSchema(ref) && !SearchPathIsDefaultSchema(*context)) {
			return result;
		}
		auto entry = GetEntry(*context, CatalogType::TABLE_ENTRY, Identifier(schema), Identifier(name),
							  OnEntryNotFound::RETURN_NULL);
		if (!entry && startup_.remote_pushdown && schema == DEFAULT_SCHEMA) {
			entry = GetEntry(*context, CatalogType::TABLE_ENTRY, Identifier(default_schema_), Identifier(name),
							 OnEntryNotFound::RETURN_NULL);
		}
		if (entry) {
			result.entry = &entry->Cast<MSSQLTableEntry>();
			result.context = context.get();
		}
		return result;
	}
	auto resolved = mssql::FindResolvedTable(*this, schema, name);
	if (!resolved && schema == DEFAULT_SCHEMA) {
		// `db.main.t`: the rewriter's lookup of `main` was answered with the
		// default schema (LookupSchema), so that is what was noted.
		resolved = mssql::FindResolvedTable(*this, default_schema_, name);
	}
	if (resolved && WrittenWithoutSchema(ref) && !SearchPathIsDefaultSchema(*resolved.context)) {
		return result;
	}
	if (resolved) {
		result.entry = resolved.entry.get();
		result.context = resolved.context.get();
		result.keep_entry = resolved.entry;
		result.keep_context = resolved.context;
	}
	return result;
}

bool MSSQLCatalog::Supports(RemoteCapability capability) const {
	switch (capability) {
	case RemoteCapability::IS_REMOTE:
	case RemoteCapability::EXECUTE_QUERY_NODE:
		return startup_.remote_pushdown;
	default:
		return false;
	}
}

// Spec 079 D1: the expression overload is asked only about constructs that
// reference no catalog (a constant, a function of constants). They are judged
// where they are used -- the node writer knows the column a constant is
// compared with and declares it from that column -- so here they pass, and
// the node's dry run below is the one answer.
bool MSSQLCatalog::SupportsPushdown(const ParsedExpression &expression) {
	MSSQL_CATALOG_DEBUG_LOG(2, "SupportsPushdown(expression %s)", expression.ToString().c_str());
	return true;
}

// A base table this catalog resolved on this thread (the rewriter's own
// lookup just did it), or a join -- whose tables the rewriter has already
// asked about, and which the node's dry run renders or refuses as a whole --
// or a subquery (PR E1: its node is asked on its own, and RemoteExecute can
// push it as a part of a node that does not go whole).
bool MSSQLCatalog::SupportsPushdown(const TableRef &ref) {
	if (ref.type == TableReferenceType::JOIN || ref.type == TableReferenceType::SUBQUERY) {
		// The node's dry run renders or refuses it as a whole (PR E1).
		MSSQL_CATALOG_DEBUG_LOG(2, "SupportsPushdown(table ref %s): a join / subquery", ref.ToString().c_str());
		return true;
	}
	if (ref.type == TableReferenceType::BASE_TABLE) {
		NoteOriginalTableName(ref.Cast<BaseTableRef>());
		auto resolved = ResolvePushdownTable(ref.Cast<BaseTableRef>(), nullptr);
		auto entry = resolved.entry;
		MSSQL_CATALOG_DEBUG_LOG(
			2, "SupportsPushdown(table ref %s): resolved %s", ref.ToString().c_str(),
			entry ? (entry->schema.name.GetIdentifierName() + "." + entry->name.GetIdentifierName()).c_str()
				  : "(nothing)");
		// An unqualified name no noted table answers: a CTE's (the rewriter asks
		// about a CTE reference when its body is this catalog's), or a table
		// the search path finds in a schema other than the default one. The
		// writer refuses both, and a node handed back binds the name as the
		// query wrote it (RestoreTableNames).
		const auto &qualified = ref.Cast<BaseTableRef>().GetQualifiedName();
		return entry != nullptr || (qualified.Catalog().empty() && qualified.Schema().empty());
	}
	MSSQL_CATALOG_DEBUG_LOG(2, "SupportsPushdown(table ref %s): not a base table", ref.ToString().c_str());
	return false;
}

bool MSSQLCatalog::WritePushdown(const QueryNode &node, mssql::WrittenQuery &out, string &why,
								 optional_ptr<ClientContext> context) {
	if (node.type != QueryNodeType::SELECT_NODE && node.type != QueryNodeType::SET_OPERATION_NODE) {
		why = "not a SELECT";
		return false;
	}
	// Every base table the node names -- its FROM's, a derived table's, a
	// subquery expression's (PR E1) -- resolved before the writer runs and
	// held until it is done. What the writer has no form for it refuses.
	vector<const BaseTableRef *> refs;
	CollectPushdownTables(node, refs);
	if (refs.empty()) {
		why = "no FROM";
		return false;
	}
	vector<PushdownTable> resolved;
	// The types each entry reports, not recomputed: a SET of
	// mssql_catalog_native_types since the entry was built must not make the
	// pushed column differ from the catalog's.
	vector<vector<LogicalType>> types(refs.size());
	for (idx_t i = 0; i < refs.size(); i++) {
		resolved.push_back(ResolvePushdownTable(*refs[i], context));
		if (!resolved.back().entry) {
			why = "table " + refs[i]->Table().GetIdentifierName() + " did not resolve in this catalog";
			// An unqualified name: a CTE of a node around this one, asked about on
			// its own (the rewriter asks about every nested node; PR E1).
			out.refers_outside = WrittenUnqualified(*refs[i]);
			return false;
		}
		for (auto &column : resolved.back().entry->GetColumns().Logical()) {
			types[i].push_back(column.Type());
		}
	}
	// The session's settings are the context's: RemoteExecute's own, or, for
	// the context-free hooks, the one that resolved the tables.
	auto options = mssql::SQLWriterOptions::FromContext(*resolved[0].context);
	mssql::SQLWriter writer(options, [&](const BaseTableRef &ref, mssql::WriterTable &table) {
		for (idx_t i = 0; i < refs.size(); i++) {
			if (refs[i] != &ref) {
				continue;
			}
			auto &entry = *resolved[i].entry;
			table.schema = entry.schema.name.GetIdentifierName();
			table.name = entry.name.GetIdentifierName();
			table.columns = &entry.GetMSSQLColumns();
			table.types = &types[i];
			// The count the planner gets (GetStorageInfo's fast path): the
			// statistics cache, else the one loaded with the entry -- no round
			// trip either way. Both can be stale; the planner's estimate is too.
			idx_t cached = 0;
			table.approx_rows = GetStatisticsProvider().TryGetCachedRowCount(
									table.schema, table.name, LoadStatisticsCacheTTL(*resolved[i].context), cached,
									/*exempt_catalog_sourced=*/true)
									? cached
									: entry.GetApproxRowCount();
			table.size_known = entry.GetObjectType() != MSSQLObjectType::VIEW;
			auto key = entry.LoadedPrimaryKeyInfo();
			if (key && key->exists) {
				for (auto &column : key->columns) {
					table.unique_key.push_back(column.name);
				}
			}
			return true;
		}
		return false;
	});
	writer.SetWrittenUnqualified(WrittenUnqualified);
	return writer.Write(node, out, why);
}

// The dry run (D1): renderability only. Whether the node gains anything over
// the catalog scan is RemoteExecute's question, not this one: the rewriter
// asks this hook about every NESTED node too (a subquery, a CTE body, a set
// operation's child) and a refusal there poisons the whole statement, so a
// plain `SELECT … WHERE` inside a subquery must answer yes (PR E1).
bool MSSQLCatalog::SupportsPushdown(const QueryNode &node) {
	mssql::WrittenQuery written;
	string why;
	if (WritePushdown(node, written, why)) {
		MSSQL_CATALOG_DEBUG_LOG(1, "SupportsPushdown(query node %s): %s", node.ToString().c_str(),
								written.statement.c_str());
		return true;
	}
	if (written.refers_outside &&
		(node.type == QueryNodeType::SELECT_NODE || node.type == QueryNodeType::SET_OPERATION_NODE) &&
		!MayRepeatOutputNames(node)) {
		// A correlated subquery, asked about on its own: a no would keep the
		// statement around it from being pushed whole. RemoteExecute renders it
		// with that statement, or hands it back.
		MSSQL_CATALOG_DEBUG_LOG(1, "SupportsPushdown(query node %s): refers outside (%s)", node.ToString().c_str(),
								why.c_str());
		return true;
	}
	// Not as a whole -- but RemoteExecute can still push its nested parts, of
	// a query (never of an INSERT / UPDATE / DELETE / MERGE, which it would
	// turn into a SELECT).
	if ((node.type == QueryNodeType::SELECT_NODE || node.type == QueryNodeType::SET_OPERATION_NODE) &&
		!MayRepeatOutputNames(node) && HasPushablePart(node, {})) {
		MSSQL_CATALOG_DEBUG_LOG(1, "SupportsPushdown(query node %s): parts (%s)", node.ToString().c_str(), why.c_str());
		return true;
	}
	MSSQL_CATALOG_DEBUG_LOG(1, "SupportsPushdown(query node %s): no: %s", node.ToString().c_str(), why.c_str());
	return false;
}

// A node that renders AND gains over the scan, written; else false. Never one
// that names a CTE of an enclosing scope.
bool MSSQLCatalog::WritePushablePart(const QueryNode &node, mssql::WrittenQuery &written,
									 optional_ptr<ClientContext> context, const vector<string> &scope, bool nested) {
	string why;
	// A set operation goes nested only (the owner's call, PR E1): at a
	// statement's top its children go as parts and DuckDB combines them.
	if (!(node.type == QueryNodeType::SELECT_NODE || (nested && node.type == QueryNodeType::SET_OPERATION_NODE)) ||
		!mssql::SQLWriter::PushesMoreThanScan(node, &WrittenUnqualifiedProbe()) ||
		NamesScopeTable(const_cast<QueryNode &>(node), scope) || !WritePushdown(node, written, why, context)) {
		return false;
	}
	if (nested && written.value_divergence) {
		// A part's results feed DuckDB's own computation above it (a filter,
		// a join): a division's NULL-for-inf or a float aggregate's last bits
		// would change rows there, not just values (review of PR E1).
		return false;
	}
	// A join of uncertain gain (PR E1). Trusted without a check inside a
	// transaction or on a pool of one connection (the owner's call: nothing
	// there to spend on asking), and when every table it joins is small by the
	// cached count; else the scan path keeps it. The dry run (no context)
	// answers optimistically -- RemoteExecute decides.
	if (context && written.gain_uncertain && GetConnectionLimit() > 1 && context->transaction.IsAutoCommit()) {
		const auto threshold = LoadPushdownJoinRowsThreshold(*context);
		if (threshold > 0 && (written.input_size_unknown || written.largest_input_rows >= idx_t(threshold))) {
			MSSQL_CATALOG_DEBUG_LOG(1, "RemoteExecute: a join of uncertain gain over %llu rows stays with the scan",
									(unsigned long long)written.largest_input_rows);
			return false;
		}
	}
	// A floor the user set (PR E2): small reads stay with the scans. Not in a
	// transaction or on a pool of one (trusted, as above), nor over a view.
	if (context && GetConnectionLimit() > 1 && context->transaction.IsAutoCommit()) {
		const auto min_rows = LoadPushdownMinRows(*context);
		if (min_rows > 0 && !written.total_size_unknown && written.total_input_rows < idx_t(min_rows)) {
			MSSQL_CATALOG_DEBUG_LOG(1, "RemoteExecute: %llu rows in all stay with the scans (mssql_pushdown_min_rows)",
									(unsigned long long)written.total_input_rows);
			return false;
		}
	}
	return true;
}

// Whether a node nested in `node` would be pushed (the dry run). `keep`: the
// node is a set operation whose children must keep their select lists (an
// enclosing set operation binds its key against them, through this one).
bool MSSQLCatalog::HasPushablePart(const QueryNode &node, const vector<string> &scope, bool keep) {
	auto inner = ExtendScope(node, scope);
	keep = keep || KeepsChildSelectLists(node);
	bool found = false;
	VisitNode(
		const_cast<QueryNode &>(node),
		[&](unique_ptr<QueryNode> &slot) {
			const bool kept = keep && IsSetOperationChild(node, slot);
			mssql::WrittenQuery written;
			found = found || (!kept && WritePushablePart(*slot, written, nullptr, inner, true)) ||
					HasPushablePart(*slot, inner, kept);
		},
		[](BaseTableRef &) {});
	return found;
}

// The PR E1 decomposition: every nested node that renders and gains is
// replaced by `SELECT * FROM <its vehicle>`; one that does not is searched in
// turn. What is left runs in DuckDB -- the outer node's EXCLUDE, a window, a
// local join -- over as many vehicles as the statement has parts.
void MSSQLCatalog::PushNestedParts(ClientContext &context, QueryNode &node, const vector<string> &scope, bool keep) {
	auto inner = ExtendScope(node, scope);
	keep = keep || KeepsChildSelectLists(node);
	VisitNode(
		node,
		[&](unique_ptr<QueryNode> &slot) {
			const bool kept = keep && IsSetOperationChild(node, slot);
			mssql::WrittenQuery written;
			if (!kept && WritePushablePart(*slot, written, context, inner, true)) {
				slot = SelectStarFrom(VehicleFor(written));
				return;
			}
			PushNestedParts(context, *slot, inner, kept);
		},
		[](BaseTableRef &) {});
}

unique_ptr<TableRef> MSSQLCatalog::RemoteExecute(ClientContext &context, unique_ptr<QueryNode> node) {
	mssql::WrittenQuery written;
	if (WritePushablePart(*node, written, context, {}, false)) {
		return VehicleFor(written);
	}
	// The node as a whole does not go: it renders but gains nothing over the
	// catalog scan, or it holds something the writer has no form for. It is
	// handed back as itself, a subquery the binder plans -- through the scan
	// path, which (unlike a pushed node) still takes the filters from ABOVE it
	// (measured: an outer `id = 2` reaches the scan's WHERE through a UNION) --
	// with every nested part that renders and gains pushed on its own.
	PushNestedParts(context, *node, {});
	RestoreTableNames(*node);
	MSSQL_CATALOG_DEBUG_LOG(1, "RemoteExecute: handed back, parts pushed: %s", node->ToString().c_str());
	auto statement = make_uniq<SelectStatement>();
	statement->node = std::move(node);
	return make_uniq<SubqueryRef>(std::move(statement));
}

// D3: the pushed node becomes a call of `mssql_scan_params` (or `mssql_scan`
// when it carries no parameter) -- the describe at bind, the run at init, the
// transaction's pinned connection, and EXPLAIN shows the statement. Nothing
// touches the server here. Spec 081: when the writer knows the type of every
// result column, the call is the `_unsafe` form with that shape as `columns`,
// and the bind asks the server nothing either; the stream is held at init to
// the same rule the describe is held to through `column_types`.
unique_ptr<TableRef> MSSQLCatalog::VehicleFor(mssql::WrittenQuery &written) {
	bool shape_known = !written.column_types.empty();
	for (auto &type : written.column_types) {
		shape_known = shape_known && type.id() != LogicalTypeId::INVALID;
	}
	vector<unique_ptr<ParsedExpression>> arguments;
	arguments.push_back(ConstantExpression::String(GetName().GetIdentifierName()));
	arguments.push_back(ConstantExpression::String(written.statement));
	string function = "mssql_scan";
	if (!written.params.empty()) {
		function = "mssql_scan_params";
		child_list_t<Value> values;
		for (auto &param : written.params) {
			values.emplace_back(Identifier(param.name), param.value);
		}
		arguments.push_back(ConstantExpression::FromValue(Value::STRUCT(std::move(values))));
		arguments.push_back(ConstantExpression::String(written.Declarations()));
	}
	// A table function's binder reads a named parameter from the argument's
	// alias (`name => value`), not from FunctionArgument's name.
	if (shape_known) {
		function += "_unsafe";
		child_list_t<Value> columns;
		for (idx_t i = 0; i < written.column_types.size(); i++) {
			columns.emplace_back(Identifier(written.column_names[i]),
								 Value(mssql::ColumnTypeName(written.column_types[i])));
		}
		auto shape = ConstantExpression::FromValue(Value::STRUCT(std::move(columns)));
		shape->SetAlias(Identifier("columns"));
		arguments.push_back(std::move(shape));
	} else {
		vector<Value> types;
		for (auto &type : written.column_types) {
			types.emplace_back(mssql::ColumnTypeName(type));
		}
		auto column_types = ConstantExpression::FromValue(Value::LIST(LogicalType::VARCHAR, std::move(types)));
		column_types->SetAlias(Identifier("column_types"));
		arguments.push_back(std::move(column_types));
	}
	auto ref = make_uniq<TableFunctionRef>();
	ref->function = make_uniq<FunctionExpression>(
		QualifiedName(Identifier(SYSTEM_CATALOG), Identifier(DEFAULT_SCHEMA), Identifier(function)),
		std::move(arguments));
	mssql::CountRemotePushdown();
	MSSQL_CATALOG_DEBUG_LOG(1, "RemoteExecute: %s", written.statement.c_str());
	bool casts = false;
	for (auto &type : written.cast_types) {
		casts = casts || type.id() != LogicalTypeId::INVALID;
	}
	if (!casts) {
		return std::move(ref);
	}
	// A result column no wire type decodes into -- SUM over integers, HUGEINT
	// in DuckDB, decimal(38,0) on the server -- is cast after the read, in a
	// projection over the call: `SELECT c1, CAST(c2 AS HUGEINT) AS c2 FROM …`.
	auto select = make_uniq<SelectNode>();
	for (idx_t i = 0; i < written.column_names.size(); i++) {
		const Identifier name(written.column_names[i]);
		unique_ptr<ParsedExpression> column = make_uniq<ColumnRefExpression>(name);
		if (written.cast_types[i].id() != LogicalTypeId::INVALID) {
			column = make_uniq<CastExpression>(written.cast_types[i], std::move(column));
		}
		column->SetAlias(name);
		select->select_list.push_back(std::move(column));
	}
	select->from_table = std::move(ref);
	auto statement = make_uniq<SelectStatement>();
	statement->node = std::move(select);
	return make_uniq<SubqueryRef>(std::move(statement));
}

// No statement-level pushdown in spec 079: DDL and DML are spec 080's.
bool MSSQLCatalog::SupportsPushdown(const SQLStatement &) {
	return false;
}

}  // namespace duckdb
