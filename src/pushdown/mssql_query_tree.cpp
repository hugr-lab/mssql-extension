#include "pushdown/mssql_query_tree.hpp"

#include <unordered_map>

#include "duckdb/common/exception.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/star_expression.hpp"
#include "duckdb/parser/expression/subquery_expression.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/query_node/set_operation_node.hpp"
#include "duckdb/parser/tableref/expressionlistref.hpp"
#include "duckdb/parser/tableref/joinref.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"

namespace duckdb {
namespace mssql {

// The table names as the query wrote them, noted by SupportsPushdown(TableRef)
// before the rewriter's strip. FinishPushdown strips the SAME objects in place
// and hands them to RemoteExecute, so a node handed back to the binder gets
// every name back exactly -- a reconstruction from the stripped tree cannot:
// `db.t` is stripped to a bare `t`, which a CTE of that name would then
// capture, or the search path bind elsewhere (review of PR E1).
namespace {
struct OriginalTableNames {
	std::unordered_map<const BaseTableRef *, QualifiedName> names;
	//! The generation before the last overflow: a statement noted across it
	//! keeps its names (review of E1 -- a lost note made `db.t` look bare).
	std::unordered_map<const BaseTableRef *, QualifiedName> previous;

	//! The note for `ref`, if it is this ref's: notes are keyed by address,
	//! and one left by an earlier statement (pushed whole, never restored)
	//! can sit at an address a new ref reuses. The strip removes only the
	//! catalog / schema, so a note whose table name differs is another ref's
	//! (review of #399) -- not found, rather than a silent bind elsewhere.
	const QualifiedName *Find(const BaseTableRef &ref) const {
		for (auto map : {&names, &previous}) {
			auto entry = map->find(&ref);
			if (entry != map->end()) {
				return StringUtil::CIEquals(entry->second.Name().GetIdentifierName(), ref.Table().GetIdentifierName())
						   ? &entry->second
						   : nullptr;
			}
		}
		return nullptr;
	}
	void Erase(const BaseTableRef &ref) {
		names.erase(&ref);
		previous.erase(&ref);
	}
};
OriginalTableNames &TableNames() {
	thread_local OriginalTableNames names;
	return names;
}
}  // namespace

void NoteOriginalTableName(const BaseTableRef &ref) {
	auto &table_names = TableNames();
	if (table_names.names.size() > 4096) {
		// Entries of statements that were not handed back. Two generations: a
		// statement whose notes straddle the overflow keeps them.
		table_names.previous = std::move(table_names.names);
		table_names.names.clear();
	}
	table_names.names[&ref] = ref.GetQualifiedName();
}

static void VisitExpression(ParsedExpression &expr, const NestedNodeVisitor &on_node) {
	if (expr.GetExpressionClass() == ExpressionClass::SUBQUERY) {
		auto &subquery = expr.Cast<SubqueryExpression>();
		if (subquery.GetChildMutable()) {
			VisitExpression(*subquery.GetChildMutable(), on_node);
		}
		on_node(subquery.SubqueryMutable()->node);
		return;
	}
	ParsedExpressionIterator::EnumerateChildren(
		expr, [&](unique_ptr<ParsedExpression> &child) { VisitExpression(*child, on_node); });
}

static void VisitTableRef(TableRef &ref, const NestedNodeVisitor &on_node, const BaseTableVisitor &on_table) {
	switch (ref.type) {
	case TableReferenceType::BASE_TABLE:
		on_table(ref.Cast<BaseTableRef>());
		break;
	case TableReferenceType::JOIN: {
		auto &join = ref.Cast<JoinRef>();
		VisitTableRef(*join.left, on_node, on_table);
		VisitTableRef(*join.right, on_node, on_table);
		if (join.condition) {
			VisitExpression(*join.condition, on_node);
		}
		break;
	}
	case TableReferenceType::SUBQUERY:
		on_node(ref.Cast<SubqueryRef>().subquery->node);
		break;
	case TableReferenceType::TABLE_FUNCTION:
		if (ref.Cast<TableFunctionRef>().function) {
			VisitExpression(*ref.Cast<TableFunctionRef>().function, on_node);
		}
		break;
	case TableReferenceType::EXPRESSION_LIST:
		for (auto &row : ref.Cast<ExpressionListRef>().values) {
			for (auto &value : row) {
				VisitExpression(*value, on_node);
			}
		}
		break;
	default:
		break;
	}
}

void VisitNode(QueryNode &node, const NestedNodeVisitor &on_node, const BaseTableVisitor &on_table) {
	for (auto &cte : node.cte_map.map) {
		if (cte.second->query_node) {
			on_node(cte.second->query_node);
		}
		for (auto &key : cte.second->key_targets) {
			VisitExpression(*key, on_node);
		}
	}
	// A RECURSIVE_CTE_NODE is not walked: the writer refuses it, so it is
	// never pushed nor handed back with parts (checked live).
	if (node.type == QueryNodeType::SET_OPERATION_NODE) {
		for (auto &child : node.Cast<SetOperationNode>().children) {
			on_node(child);
		}
	} else if (node.type == QueryNodeType::SELECT_NODE) {
		auto &select = node.Cast<SelectNode>();
		if (select.from_table) {
			VisitTableRef(*select.from_table, on_node, on_table);
		}
		for (auto &item : select.select_list) {
			VisitExpression(*item, on_node);
		}
		for (auto &key : select.groups.group_expressions) {
			VisitExpression(*key, on_node);
		}
		for (auto *clause : {&select.where_clause, &select.having, &select.qualify}) {
			if (*clause) {
				VisitExpression(**clause, on_node);
			}
		}
	}
	for (auto &modifier : node.modifiers) {
		switch (modifier->type) {
		case ResultModifierType::ORDER_MODIFIER:
			for (auto &order : modifier->Cast<OrderModifier>().orders) {
				VisitExpression(*order.expression, on_node);
			}
			break;
		case ResultModifierType::LIMIT_MODIFIER: {
			auto &limit = modifier->Cast<LimitModifier>();
			for (auto *count : {&limit.limit, &limit.offset}) {
				if (*count) {
					VisitExpression(**count, on_node);
				}
			}
			break;
		}
		case ResultModifierType::DISTINCT_MODIFIER:
			for (auto &target : modifier->Cast<DistinctModifier>().distinct_on_targets) {
				VisitExpression(*target, on_node);
			}
			break;
		default:
			break;
		}
	}
}

// Whether the query wrote `ref` with no catalog and no schema, from the name
// SupportsPushdown(TableRef) noted before the rewriter's strip (`db.t`
// arrives as a bare `t`); a name it did not note is taken as it stands.
const QualifiedName &WrittenName(const BaseTableRef &ref) {
	auto original = TableNames().Find(ref);
	return original ? *original : ref.GetQualifiedName();
}

bool WrittenUnqualified(const BaseTableRef &ref) {
	auto &name = WrittenName(ref);
	return name.Catalog().empty() && name.Schema().empty();
}

const mssql::SQLWriter::QualificationProbe &WrittenUnqualifiedProbe() {
	static const mssql::SQLWriter::QualificationProbe probe = WrittenUnqualified;
	return probe;
}

// The base tables a node names, for the writer's resolver -- not a reference
// to a CTE in scope, which the writer inlines (PR E1). The scope ignores the
// order CTEs are defined in: a body naming a later sibling leaves that name
// out here, and the writer, which does track the order, then finds neither a
// CTE nor a resolved table -- a veto.
void CollectPushdownTables(const QueryNode &node, vector<const BaseTableRef *> &refs, const vector<string> &scope) {
	auto inner = ExtendScope(node, scope);
	VisitNode(
		const_cast<QueryNode &>(node), [&](unique_ptr<QueryNode> &slot) { CollectPushdownTables(*slot, refs, inner); },
		[&](BaseTableRef &ref) {
			if (WrittenUnqualified(ref)) {
				for (auto &cte : inner) {
					if (StringUtil::CIEquals(cte, ref.Table().GetIdentifierName())) {
						return;
					}
				}
			}
			refs.push_back(&ref);
		});
}

// Puts back every table name the strip took the catalog off, from the names
// SupportsPushdown(TableRef) noted. A name it did not note would have to be
// guessed, so the statement is refused rather than bound to what might be
// another table.
void RestoreTableNames(QueryNode &node) {
	VisitNode(
		node, [](unique_ptr<QueryNode> &slot) { RestoreTableNames(*slot); },
		[](BaseTableRef &ref) {
			auto &table_names = TableNames();
			auto original = table_names.Find(ref);
			if (!original) {
				throw BinderException(
					"mssql: remote pushdown lost the name of table %s; run the statement with "
					"mssql_remote_pushdown = false, and please report it",
					ref.ToString());
			}
			ref.SetQualifiedName(*original);
			table_names.Erase(ref);
		});
}

unique_ptr<QueryNode> SelectStarFrom(unique_ptr<TableRef> ref) {
	auto select = make_uniq<SelectNode>();
	select->select_list.push_back(make_uniq<StarExpression>());
	select->from_table = std::move(ref);
	return std::move(select);
}

// The CTE names visible inside a node: its own and every enclosing node's.
vector<string> ExtendScope(const QueryNode &node, vector<string> scope) {
	for (auto &cte : node.cte_map.map) {
		scope.push_back(cte.first.GetIdentifierName());
	}
	return scope;
}

// Whether `node` names, anywhere inside, an unqualified table that a CTE in
// `scope` defines. Such a name is the CTE, not the table the writer would
// resolve it to (review of PR E1: `WITH o AS (SELECT … FROM db.o WHERE …)
// SELECT … FROM (SELECT … FROM o …)` read dbo.o and lost the CTE's filter).
// The rewriter refuses any nested node under a CTE-holding parent for the
// same reason (its FinishPushdown FIXME); here only the ones that name one.
bool NamesScopeTable(QueryNode &node, const vector<string> &scope) {
	// The node's own CTEs are its to name (PR E1 inlines them); an enclosing
	// scope's CTE of the same name is shadowed by them.
	vector<string> inner;
	for (auto &name : scope) {
		bool shadowed = false;
		for (auto &cte : node.cte_map.map) {
			shadowed = shadowed || StringUtil::CIEquals(cte.first.GetIdentifierName(), name);
		}
		if (!shadowed) {
			inner.push_back(name);
		}
	}
	if (inner.empty()) {
		return false;
	}
	bool found = false;
	VisitNode(
		node, [&](unique_ptr<QueryNode> &slot) { found = found || NamesScopeTable(*slot, inner); },
		[&](BaseTableRef &ref) {
			// By the written name: a stripped `db.t` is a table, not a CTE `t`.
			if (!WrittenUnqualified(ref)) {
				return;
			}
			for (auto &cte : inner) {
				found = found || StringUtil::CIEquals(cte, ref.Table().GetIdentifierName());
			}
		});
	return found;
}

// Whether a node handed back could carry two result columns of one name: the
// binder deduplicates a subquery's names (`n, n` comes back `n, n_1`), and a
// node handed back is a subquery. Unknowable before binding for a `*` over a
// join, so that is refused too; a node the writer renders refuses duplicates
// itself.
bool MayRepeatOutputNames(const QueryNode &node) {
	if (node.type == QueryNodeType::SET_OPERATION_NODE) {
		auto &children = node.Cast<SetOperationNode>().children;
		return !children.empty() && MayRepeatOutputNames(*children[0]);
	}
	if (node.type != QueryNodeType::SELECT_NODE) {
		return true;
	}
	auto &select = node.Cast<SelectNode>();
	vector<string> names;
	for (auto &item : select.select_list) {
		if (item->GetExpressionClass() == ExpressionClass::STAR) {
			if ((select.from_table && select.from_table->type == TableReferenceType::JOIN) ||
				select.select_list.size() > 1) {
				return true;
			}
			continue;
		}
		string name = !item->GetAlias().empty() ? item->GetAlias().GetIdentifierName()
					  : item->GetExpressionClass() == ExpressionClass::COLUMN_REF
						  ? item->Cast<ColumnRefExpression>().GetColumnName().GetIdentifierName()
						  : item->GetName().GetIdentifierName();
		for (auto &existing : names) {
			if (StringUtil::CIEquals(existing, name)) {
				return true;
			}
		}
		names.push_back(std::move(name));
	}
	return false;
}

// The result names a query node's rows carry, as DuckDB names them: an
// alias, else a bare column's name; false when one is not knowable before
// binding (a `*`, an expression's text). A set operation's are its first
// child's.
static bool ResultNames(const QueryNode &node, vector<string> &out) {
	if (node.type == QueryNodeType::SET_OPERATION_NODE) {
		auto &children = node.Cast<SetOperationNode>().children;
		return !children.empty() && ResultNames(*children[0], out);
	}
	if (node.type != QueryNodeType::SELECT_NODE) {
		return false;
	}
	for (auto &item : node.Cast<SelectNode>().select_list) {
		if (!item->GetAlias().empty()) {
			out.push_back(item->GetAlias().GetIdentifierName());
		} else if (item->GetExpressionClass() == ExpressionClass::COLUMN_REF) {
			out.push_back(item->Cast<ColumnRefExpression>().GetColumnName().GetIdentifierName());
		} else {
			return false;
		}
	}
	return true;
}

// Whether a set operation's children must keep their select lists: DuckDB
// binds the set operation's ORDER BY / DISTINCT ON key against the children's
// own select-list expressions -- `... UNION ALL ... ORDER BY g + 1`, or a bare
// `g` the children select under other names -- which `SELECT * FROM
// <vehicle>` no longer has (review of E1: a binder error where DuckDB returns
// rows). A position, or a bare name that is one of the result names, binds by
// those, which the vehicle keeps. The rewriter itself skips every child under
// any modifier.
bool KeepsChildSelectLists(const QueryNode &node) {
	if (node.type != QueryNodeType::SET_OPERATION_NODE) {
		return false;
	}
	vector<string> names;
	const bool known = ResultNames(node, names);
	auto by_result = [&](const ParsedExpression &key) {
		if (key.GetExpressionClass() == ExpressionClass::CONSTANT) {
			return true;
		}
		if (!known || key.GetExpressionClass() != ExpressionClass::COLUMN_REF ||
			key.Cast<ColumnRefExpression>().ColumnNames().size() != 1) {
			return false;
		}
		for (auto &name : names) {
			if (StringUtil::CIEquals(name, key.Cast<ColumnRefExpression>().GetColumnName().GetIdentifierName())) {
				return true;
			}
		}
		return false;
	};
	for (auto &modifier : node.modifiers) {
		if (modifier->type == ResultModifierType::ORDER_MODIFIER) {
			for (auto &order : modifier->Cast<OrderModifier>().orders) {
				if (!by_result(*order.expression)) {
					return true;
				}
			}
		} else if (modifier->type == ResultModifierType::DISTINCT_MODIFIER) {
			for (auto &target : modifier->Cast<DistinctModifier>().distinct_on_targets) {
				if (!by_result(*target)) {
					return true;
				}
			}
		}
	}
	return false;
}

// Whether `slot` is one of `node`'s set-operation children.
bool IsSetOperationChild(const QueryNode &node, const unique_ptr<QueryNode> &slot) {
	if (node.type != QueryNodeType::SET_OPERATION_NODE) {
		return false;
	}
	for (auto &child : node.Cast<SetOperationNode>().children) {
		if (&child == &slot) {
			return true;
		}
	}
	return false;
}

}  // namespace mssql
}  // namespace duckdb
