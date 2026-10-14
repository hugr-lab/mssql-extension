//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// dml/merge/mssql_merge_rewrite.hpp
//
// Spec 080 D4 (PR 4b): a MERGE into a catalog table runs on the server, always,
// or is refused by name. One whose source is on the same server is pushed
// whole by the remote-pushdown rewriter before binding (PR 4a) and never
// becomes a LogicalMergeInto. Every other one reaches the optimizer as one,
// and RewriteMerge takes it apart before DuckDB's own optimizers run:
//
//   LogicalMergeInto                      MSSQLLogicalStagedMerge
//     LogicalProjection   (actions)  =>     LogicalProjection (#src columns)
//       join (source, target)                 <the source, untouched>
//
// - the join and the target's scan go; the ON condition is the join's
//   conditions and the filters the binder pushed onto the target's side (the
//   ones it pushed onto the source's side stay in DuckDB, which filters the
//   rows that reach #src: equivalent for every join type that pushes them);
// - every action expression is inlined through the binder's projection, then
//   split: each largest part over the source alone (any DuckDB function)
//   becomes a #src column DuckDB computes, constants are folded, and what
//   touches the target is written in T-SQL through FilterEncoder with a
//   ColumnResolver for two relations, `[t].[col]` and `[s].[cN]`;
// - a #src column is typed through the CTAS mapping, except one compared
//   directly with a target column: that one is the target column's own type
//   and collation (#358: a datetime against a datetime; error 468 otherwise),
//   strings and binaries widened to MAX so the fill never truncates a value
//   into a false match;
// - a target column the ON condition reads must survive the round trip
//   (IsRoundTripExactForKey), or an unmatched row would be inserted twice.
//
// Anything the server has no form for is refused by name, nothing sent.
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/planner/logical_operator.hpp"

namespace duckdb {

class ClientContext;
class Optimizer;

namespace mssql {

//! `op` is a LogicalMergeInto into a table of an MSSQL catalog: replace it with
//! a MSSQLLogicalStagedMerge, or throw NotImplementedException naming why the
//! MERGE cannot run on the server.
void RewriteMerge(ClientContext &context, Optimizer &optimizer, unique_ptr<LogicalOperator> &op);

}  // namespace mssql
}  // namespace duckdb
