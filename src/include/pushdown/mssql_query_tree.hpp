#pragma once

#include <functional>

#include "duckdb/parser/query_node.hpp"
#include "duckdb/parser/tableref.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"
#include "pushdown/mssql_sql_writer.hpp"

namespace duckdb {
namespace mssql {

//===----------------------------------------------------------------------===//
// Spec 079 PR E1: the parsed-query walks behind MSSQLCatalog's remote
// pushdown -- the names the rewriter strips, CTE scope, and the shapes a node
// handed back to the binder must keep.
//===----------------------------------------------------------------------===//

// Every query node nested in a node and every base table it names, visited as
// the rewriter's StripCatalogName visits them: CTE bodies and key targets, the
// FROM tree (joins and their conditions, subqueries, table function
// arguments, VALUES rows), the select list, WHERE, GROUP BY, HAVING, QUALIFY,
// the ORDER BY / LIMIT / OFFSET / DISTINCT ON modifiers, a set operation's
// children, and the subqueries inside any of those expressions. A nested node
// is handed to `on_node` by its slot, so it can be replaced; the walk does not
// descend into it (the callback does, if it wants).
using NestedNodeVisitor = std::function<void(unique_ptr<QueryNode> &slot)>;
using BaseTableVisitor = std::function<void(BaseTableRef &ref)>;

void VisitNode(QueryNode &node, const NestedNodeVisitor &on_node, const BaseTableVisitor &on_table);

void NoteOriginalTableName(const BaseTableRef &ref);
const QualifiedName &WrittenName(const BaseTableRef &ref);
bool WrittenUnqualified(const BaseTableRef &ref);
const SQLWriter::QualificationProbe &WrittenUnqualifiedProbe();
void CollectPushdownTables(const QueryNode &node, vector<const BaseTableRef *> &refs, const vector<string> &scope = {});
void RestoreTableNames(QueryNode &node);
unique_ptr<QueryNode> SelectStarFrom(unique_ptr<TableRef> ref);
vector<string> ExtendScope(const QueryNode &node, vector<string> scope);
bool NamesScopeTable(QueryNode &node, const vector<string> &scope);
bool MayRepeatOutputNames(const QueryNode &node);
bool KeepsChildSelectLists(const QueryNode &node);
bool IsSetOperationChild(const QueryNode &node, const unique_ptr<QueryNode> &slot);

}  // namespace mssql
}  // namespace duckdb
