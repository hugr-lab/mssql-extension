#include "pushdown/mssql_sql_writer.hpp"
#include "pushdown/mssql_node_writer.hpp"

#include "catalog/mssql_column_info.hpp"
#include "codec/target_string_type.hpp"
#include "connection/mssql_settings.hpp"
#include "mssql_counters.hpp"
#include "pushdown/mssql_expression_vocabulary.hpp"
#include "pushdown/mssql_order_term.hpp"
#include "query/mssql_identifier.hpp"
#include "query/mssql_sql_params.hpp"

#include <atomic>
#include <cstdio>
#include <deque>

#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/decimal.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/parser/expression/between_expression.hpp"
#include "duckdb/parser/expression/case_expression.hpp"
#include "duckdb/parser/expression/cast_expression.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/comparison_expression.hpp"
#include "duckdb/parser/expression/conjunction_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/operator_expression.hpp"
#include "duckdb/parser/expression/star_expression.hpp"
#include "duckdb/parser/expression/subquery_expression.hpp"
#include "duckdb/parser/expression/type_expression.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/query_node/set_operation_node.hpp"
#include "duckdb/parser/result_modifier.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"
#include "duckdb/parser/tableref/joinref.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"

namespace duckdb {
namespace mssql {

using node_writer::ContainsAggregate;
using node_writer::HasSubqueryExpression;
using node_writer::NodeWriter;

std::string WrittenQuery::Declarations() const {
	std::string out;
	for (auto &param : params) {
		if (!out.empty()) {
			out += ", ";
		}
		out += "@" + param.name + " " + param.declaration;
	}
	return out;
}

SQLWriterOptions SQLWriterOptions::FromContext(ClientContext &context) {
	SQLWriterOptions options;
	options.convert_varchar_max = LoadConvertVarcharMax(context);
	options.parameterize = LoadScanParameterizeFilters(context);
	Value ieee;
	if (context.TryGetCurrentSetting("ieee_floating_point_ops", ieee) && !ieee.IsNull()) {
		options.ieee_floating_point_ops = ieee.GetValue<bool>();
	}
	options.division_by_zero_errors = LoadErrorOnDivisionByZero(context);
	Value scalar_errors;
	if (context.TryGetCurrentSetting("scalar_subquery_error_on_multiple_rows", scalar_errors) &&
		!scalar_errors.IsNull()) {
		options.scalar_subquery_errors = scalar_errors.GetValue<bool>();
	}
	auto &config = DBConfig::GetConfig(context);
	options.default_order = config.ResolveOrder(context, OrderType::ORDER_DEFAULT);
	options.default_null_order_asc =
		config.ResolveNullOrder(context, OrderType::ASCENDING, OrderByNullType::ORDER_DEFAULT);
	options.default_null_order_desc =
		config.ResolveNullOrder(context, OrderType::DESCENDING, OrderByNullType::ORDER_DEFAULT);
	return options;
}

SQLWriter::SQLWriter(SQLWriterOptions options, TableResolver resolver)
	: options_(options), resolver_(std::move(resolver)) {}

bool SQLWriter::Write(const QueryNode &node, WrittenQuery &out, std::string &why) {
	out = WrittenQuery();
	NodeWriter writer(options_, resolver_, out, why);
	writer.SetWrittenUnqualified(&written_unqualified_);
	if (!writer.WriteQueryNode(node)) {
		return false;
	}
	writer.TakeParams(out.params);
	return true;
}

std::string ColumnTypeName(const LogicalType &type) {
	if (type.id() == LogicalTypeId::INVALID) {
		// A computed column: '' leaves the vehicle the type the server describes.
		return std::string();
	}
	codec::TargetStringType spec;
	if (!codec::TryGetTargetStringType(type, spec)) {
		return type.ToString();
	}
	std::string name = spec.unicode ? "MSSQL_NVARCHAR(" : "MSSQL_VARCHAR(";
	name += codec::IsMaxLength(spec.length) ? std::string("'MAX'") : std::to_string(spec.length);
	if (!spec.collation.empty()) {
		name += ", '" + StringUtil::Replace(spec.collation, "'", "''") + "'";
	}
	return name + ")";
}

void CountRemotePushdown() {
	static std::atomic<uint64_t> statements{0};
	const auto total = ++statements;
	if (CountersEnabled()) {
		fprintf(stderr, "[MSSQL COUNTERS] remote_pushdown: statements=%llu\n", (unsigned long long)total);
	}
}

bool SQLWriter::PushesMoreThanScan(const QueryNode &node, const QualificationProbe *probe) {
	for (auto &modifier : node.modifiers) {
		// LIMIT 0: DuckDB plans an empty result and asks the server nothing,
		// where a vehicle would cost a describe and an execution. DuckLake's
		// attach probes every inlined-data table with one in a UNION ALL, and a
		// thousand of them were seconds pushed (#406 bench).
		if (modifier->type == ResultModifierType::LIMIT_MODIFIER) {
			auto &limit = modifier->Cast<LimitModifier>();
			if (limit.limit_type == LimitValueType::ROW_COUNT && limit.limit &&
				limit.limit->GetExpressionClass() == ExpressionClass::CONSTANT) {
				int64_t rows;
				const auto &literal = limit.limit->Cast<ConstantExpression>().GetLiteral();
				if (literal.kind == LiteralKind::INTEGER && literal.TryGetInt64(rows) && rows == 0) {
					return false;
				}
			}
		}
	}
	for (auto &modifier : node.modifiers) {
		if (modifier->type == ResultModifierType::ORDER_MODIFIER ||
			modifier->type == ResultModifierType::LIMIT_MODIFIER ||
			modifier->type == ResultModifierType::DISTINCT_MODIFIER) {
			return true;
		}
	}
	if (node.type == QueryNodeType::SET_OPERATION_NODE) {
		// UNION / EXCEPT / INTERSECT compare rows there, not here; a UNION ALL
		// gains what its children do (DuckDB pushes a filter into each).
		auto &setop = node.Cast<SetOperationNode>();
		if (!(setop.setop_type == SetOperationType::UNION && setop.setop_all)) {
			return true;
		}
		for (auto &child : setop.children) {
			if (PushesMoreThanScan(*child, probe)) {
				return true;
			}
		}
		return false;
	}
	if (node.type != QueryNodeType::SELECT_NODE) {
		return false;
	}
	auto &select = node.Cast<SelectNode>();
	// QUALIFY drops rows after a window the scan path computes here (PR E2).
	if (select.qualify) {
		return true;
	}
	// A filter over a derived table's window (`WHERE rn = 1` around
	// row_number()): the scan path cannot push it into the scan.
	if (select.where_clause && select.from_table && select.from_table->type == TableReferenceType::SUBQUERY) {
		auto &inner = *select.from_table->Cast<SubqueryRef>().subquery->node;
		if (inner.type == QueryNodeType::SELECT_NODE) {
			for (auto &item : inner.Cast<SelectNode>().select_list) {
				if (item->IsWindow()) {
					return true;
				}
			}
		}
	}
	// A subquery in FROM that gains (its aggregate, its TOP) gains for the
	// node around it too: pushed whole, rather than handed back with the part.
	std::function<bool(const TableRef &)> from_gains = [&](const TableRef &ref) -> bool {
		if (ref.type == TableReferenceType::SUBQUERY) {
			return PushesMoreThanScan(*ref.Cast<SubqueryRef>().subquery->node, probe);
		}
		if (ref.type == TableReferenceType::BASE_TABLE) {
			// A reference to one of the node's CTEs: its body's gain.
			auto &base = ref.Cast<BaseTableRef>();
			auto &name = base.GetQualifiedName();
			const bool unqualified = probe && *probe ? (*probe)(base) : name.Catalog().empty() && name.Schema().empty();
			for (auto &cte : select.cte_map.map) {
				if (unqualified &&
					StringUtil::CIEquals(cte.first.GetIdentifierName(), name.Name().GetIdentifierName()) &&
					cte.second->query_node) {
					return PushesMoreThanScan(*cte.second->query_node, probe);
				}
			}
		}
		if (ref.type == TableReferenceType::JOIN) {
			return from_gains(*ref.Cast<JoinRef>().left) || from_gains(*ref.Cast<JoinRef>().right);
		}
		return false;
	};
	if (select.from_table && from_gains(*select.from_table)) {
		return true;
	}
	// A subquery in an expression: the server filters (IN / EXISTS) or looks
	// up (a scalar subquery), where the scan path would read both tables and
	// join them here.
	if (HasSubqueryExpression(select)) {
		return true;
	}
	// A join with conditions: the server matches the rows, where the scans
	// would each send their whole (filtered) table. A CROSS JOIN anywhere in
	// the chain -- a comma join included -- can send the product, more rather
	// than less, so it is a gain only with one of the others below.
	bool joined = false;
	bool crossed = false;
	for (auto ref = select.from_table.get(); ref && ref->type == TableReferenceType::JOIN;
		 ref = ref->Cast<JoinRef>().left.get()) {
		joined = true;
		crossed = crossed || ref->Cast<JoinRef>().ref_type == JoinRefType::CROSS;
	}
	if (joined && !crossed) {
		return true;
	}
	if (!select.groups.group_expressions.empty() || !select.groups.grouping_sets.empty() || select.having ||
		select.aggregate_handling != AggregateHandling::STANDARD_HANDLING) {
		return true;
	}
	for (auto &item : select.select_list) {
		if (ContainsAggregate(*item)) {
			return true;
		}
	}
	return false;
}

}  // namespace mssql
}  // namespace duckdb
