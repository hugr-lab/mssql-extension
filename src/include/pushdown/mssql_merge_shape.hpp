//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// pushdown/mssql_merge_shape.hpp
//
// Spec 080 D4: the WHEN clauses of a MERGE that T-SQL has a form for. Shared
// by the pushed MERGE (NodeWriter::WriteMerge, over the parsed node) and the
// staged one (PR 4b, over the bound actions), so the two refuse alike.
//===----------------------------------------------------------------------===//

#pragma once

#include <map>
#include <string>
#include <vector>
#include "duckdb/common/enums/merge_action_type.hpp"

namespace duckdb {
namespace mssql {

//! DuckDB takes any number of WHEN clauses of each kind and applies the first
//! whose condition holds. T-SQL takes:
//! - at most two WHEN MATCHED (one UPDATE and one DELETE, the first
//!   conditional);
//! - one WHEN NOT MATCHED [BY TARGET], an INSERT;
//! - at most two WHEN NOT MATCHED BY SOURCE, as WHEN MATCHED.
//! Within that shape the first-match rule is the same. A last DO NOTHING is
//! dropped: no later clause could take its rows. One before another clause of
//! its kind is refused, as is ERROR. DuckDB's parser already refuses a clause
//! after an unconditional one of its kind.
//!
//! `actions` holds each WHEN kind's action types in the statement's order.
//! On success `kept` holds, per WHEN kind, the indexes of the actions to
//! write, in order; a kind left with none is absent. On a refusal, `why`
//! names the first offending clause in T-SQL's clause order (WHEN MATCHED,
//! NOT MATCHED BY TARGET, NOT MATCHED BY SOURCE) and `kept` is unspecified.
bool CheckMergeShape(const std::map<MergeActionCondition, std::vector<MergeActionType>> &actions,
					 std::map<MergeActionCondition, std::vector<idx_t>> &kept, std::string &why);

//! "WHEN MATCHED", "WHEN NOT MATCHED BY TARGET", "WHEN NOT MATCHED BY SOURCE".
const char *MergeWhenSql(MergeActionCondition when);

}  // namespace mssql
}  // namespace duckdb
