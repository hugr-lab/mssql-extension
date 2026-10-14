#include "pushdown/mssql_merge_shape.hpp"

namespace duckdb {
namespace mssql {

const char *MergeWhenSql(MergeActionCondition when) {
	switch (when) {
	case MergeActionCondition::WHEN_MATCHED:
		return "WHEN MATCHED";
	case MergeActionCondition::WHEN_NOT_MATCHED_BY_TARGET:
		return "WHEN NOT MATCHED BY TARGET";
	default:
		return "WHEN NOT MATCHED BY SOURCE";
	}
}

bool CheckMergeShape(const std::map<MergeActionCondition, std::vector<MergeActionType>> &actions,
					 std::map<MergeActionCondition, std::vector<idx_t>> &kept, std::string &why) {
	kept.clear();
	bool any = false;
	// T-SQL's clause order, so a refusal names the first clause the statement
	// would write that T-SQL has no form for.
	const MergeActionCondition order[] = {MergeActionCondition::WHEN_MATCHED,
										  MergeActionCondition::WHEN_NOT_MATCHED_BY_TARGET,
										  MergeActionCondition::WHEN_NOT_MATCHED_BY_SOURCE};
	for (auto when : order) {
		auto found = actions.find(when);
		if (found == actions.end()) {
			continue;
		}
		const auto &types = found->second;
		const std::string when_sql = MergeWhenSql(when);
		std::vector<idx_t> indexes;
		for (idx_t i = 0; i < types.size(); i++) {
			const auto type = types[i];
			if (type == MergeActionType::MERGE_DO_NOTHING) {
				if (i + 1 != types.size()) {
					// Rows it holds back would reach a later clause on the server.
					why = "DO NOTHING before another " + when_sql + " clause";
					return false;
				}
				continue;
			}
			if (type == MergeActionType::MERGE_ERROR) {
				why = "an ERROR action in a MERGE";
				return false;
			}
			indexes.push_back(i);
		}
		if (when == MergeActionCondition::WHEN_NOT_MATCHED_BY_TARGET) {
			if (indexes.size() > 1) {
				why = "more than one WHEN NOT MATCHED BY TARGET clause";
				return false;
			}
			if (!indexes.empty() && types[indexes[0]] != MergeActionType::MERGE_INSERT) {
				why = "WHEN NOT MATCHED BY TARGET without INSERT";
				return false;
			}
		} else {
			if (indexes.size() > 2) {
				why = "more than two " + when_sql + " clauses";
				return false;
			}
			if (indexes.size() == 2 && types[indexes[0]] == types[indexes[1]]) {
				why = "two " + when_sql + " clauses of one action";
				return false;
			}
			for (auto i : indexes) {
				if (types[i] == MergeActionType::MERGE_INSERT) {
					why = "INSERT in " + when_sql;
					return false;
				}
			}
		}
		if (!indexes.empty()) {
			any = true;
			kept[when] = std::move(indexes);
		}
	}
	if (!any) {
		why = "a MERGE with no action";
		return false;
	}
	return true;
}

}  // namespace mssql
}  // namespace duckdb
