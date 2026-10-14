#include "dml/merge/mssql_merge_rewrite.hpp"

#include <algorithm>

#include "catalog/mssql_catalog.hpp"
#include "catalog/mssql_ddl_translator.hpp"
#include "catalog/mssql_rowid_key_choice.hpp"
#include "catalog/mssql_table_entry.hpp"
#include "connection/mssql_settings.hpp"
#include "copy/bcp_config.hpp"
#include "copy/target_resolver.hpp"
#include "dml/ctas/mssql_ctas_config.hpp"
#include "dml/merge/mssql_logical_staged_merge.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/uuid.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/column_binding_map.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/operator/logical_any_join.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_merge_into.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "pushdown/mssql_merge_shape.hpp"
#include "query/mssql_identifier.hpp"
#include "query/mssql_sql_params.hpp"
#include "table_scan/filter_encoder.hpp"

namespace duckdb {
namespace mssql {

namespace {

//! What an expression reads. SERVER: the target's columns or the #src columns
//! already staged -- what the statement writes in T-SQL.
enum : uint8_t { REACH_NONE = 0, REACH_SOURCE = 1, REACH_SERVER = 2, REACH_OTHER = 4 };

//! Where an expression stands in the MERGE. A WHEN NOT MATCHED BY SOURCE
//! clause has no source row, so nothing in it can be computed per row.
enum class MergeScope : uint8_t { ON, MATCHED, BY_TARGET, BY_SOURCE };

bool IsStringType(const LogicalType &type) {
	return type.id() == LogicalTypeId::VARCHAR;
}

//! One column of #src.
struct SrcColumn {
	//! What DuckDB computes, over the source's bindings.
	unique_ptr<Expression> expr;
	LogicalType type;
	//! The target column it is declared as (compared with it directly).
	optional_idx like_target;
	//! What the resolver reports for [s].[cN]: the declared column, for a
	//! like-target one; none for a CTAS-typed one (a constant compared with it
	//! is declared from its own type, and #426's datetime rules see no
	//! datetime column in it).
	unique_ptr<MSSQLColumnInfo> info;
};

//! How a like-target #src column is declared: the target column's own type,
//! a string or binary widened to MAX in its collation (INSERT BULK would
//! otherwise cut a longer source value to the declared length and match its
//! prefix), rowversion as the binary(8) it is. `cast` empty: the column as it
//! is. A char / varchar under a code-page collation is staged as nvarchar(max)
//! in that collation: as varchar the source string would be converted to the
//! code page first, a character outside it to a best-fit letter or '?', and
//! 'Łukasz' would match 'Lukasz' (#361's case, on the write side). Compared
//! with the nvarchar, the column is widened to Unicode instead, as a DuckDB
//! comparison of the two strings would see them.
struct LikeTargetType {
	string cast;
	string type_name;
	int16_t max_length;
};

LikeTargetType LikeTargetOf(const MSSQLColumnInfo &col) {
	const auto t = StringUtil::Lower(col.sql_type_name);
	if ((t == "char" || t == "varchar") && col.is_utf8) {
		return {"varchar(max)", "varchar", -1};
	}
	if (t == "char" || t == "varchar") {
		return {"nvarchar(max)", "nvarchar", -1};
	}
	if (t == "nchar" || t == "nvarchar" || t == "sysname") {
		return {"nvarchar(max)", "nvarchar", -1};
	}
	if (t == "binary" || t == "varbinary") {
		return {"varbinary(max)", "varbinary", -1};
	}
	if (t == "timestamp" || t == "rowversion") {
		return {"binary(8)", "binary", 8};
	}
	return {string(), col.sql_type_name, col.max_length};
}

string SrcName(idx_t n) {
	return "c" + std::to_string(n);
}

class MergeColumnResolver : public ColumnResolver {
public:
	MergeColumnResolver(TableIndex target_index, const vector<ColumnIndex> &target_ids,
						const vector<MSSQLColumnInfo> &columns, TableIndex src_index, const vector<SrcColumn> &src)
		: target_index_(target_index), target_ids_(target_ids), columns_(columns), src_index_(src_index), src_(src) {}

	bool Resolve(const BoundColumnRefExpression &expr, std::string &sql,
				 const MSSQLColumnInfo *&column) const override {
		const auto &binding = expr.Binding();
		if (binding.table_index == target_index_) {
			const auto position = binding.column_index.GetIndex();
			if (position >= target_ids_.size() || target_ids_[position].IsVirtualColumn()) {
				return false;
			}
			const auto index = target_ids_[position].GetPrimaryIndex();
			if (index >= columns_.size()) {
				return false;
			}
			column = &columns_[index];
			sql = "[t]." + QuoteIdentifier(column->name);
			return true;
		}
		if (binding.table_index == src_index_) {
			const auto n = binding.column_index.GetIndex();
			if (n >= src_.size()) {
				return false;
			}
			column = src_[n].info.get();
			sql = "[s]." + QuoteIdentifier(SrcName(n));
			return true;
		}
		return false;
	}

private:
	TableIndex target_index_;
	const vector<ColumnIndex> &target_ids_;
	const vector<MSSQLColumnInfo> &columns_;
	TableIndex src_index_;
	const vector<SrcColumn> &src_;
};

class MergeRewriter {
public:
	MergeRewriter(ClientContext &context, Optimizer &optimizer, LogicalMergeInto &merge)
		: context_(context),
		  optimizer_(optimizer),
		  merge_(merge),
		  table_(merge.table.Cast<MSSQLTableEntry>()),
		  catalog_(merge.table.ParentCatalog().Cast<MSSQLCatalog>()),
		  columns_(table_.GetMSSQLColumns()),
		  schema_name_(table_.ParentSchema().name.GetIdentifierName()),
		  table_name_(table_.name.GetIdentifierName()),
		  no_ids_(),
		  no_names_(),
		  no_types_(),
		  ctx_(no_ids_, no_names_, no_types_) {}

	unique_ptr<LogicalOperator> Rewrite();

private:
	[[noreturn]] void Refuse(const string &why) const {
		throw NotImplementedException("MSSQL: MERGE INTO '%s.%s' is not supported: %s. Nothing was sent.", schema_name_,
									  table_name_, why);
	}

	void CheckPlatform();
	void TakeJoinApart(vector<unique_ptr<Expression>> &on);
	optional_idx TargetColumn(const ColumnBinding &binding) const;
	optional_idx TargetColumnOf(const Expression &expr) const;
	uint8_t Classify(const Expression &expr) const;
	void ReplaceProjectionRefs(unique_ptr<Expression> &expr) const;
	unique_ptr<Expression> Inline(const Expression &action_expr) const;
	void Split(unique_ptr<Expression> &expr, MergeScope scope);
	void FoldOrStage(unique_ptr<Expression> &expr, MergeScope scope);
	void Stage(unique_ptr<Expression> &expr, optional_idx like_target);
	bool LikeTargetFits(idx_t column, const LogicalType &source_type) const;
	void CheckOnColumns(const Expression &conjunct) const;
	string ConditionSql(const Expression &condition, MergeScope scope, const string &what);
	void StripAssignmentCast(unique_ptr<Expression> &expr, const LogicalType &column_type) const;
	string ValueSql(const Expression &value, MergeScope scope, const string &what, const LogicalType &column_type);
	string Clause(MergeActionCondition when, const BoundMergeIntoAction &action);
	void TypeSrcColumns(MSSQLStagedMergeTarget &target, string &select_list);

	ClientContext &context_;
	Optimizer &optimizer_;
	LogicalMergeInto &merge_;
	MSSQLTableEntry &table_;
	MSSQLCatalog &catalog_;
	const vector<MSSQLColumnInfo> &columns_;
	string schema_name_;
	string table_name_;

	LogicalProjection *projection_ = nullptr;
	LogicalOperator *join_ = nullptr;
	idx_t source_child_ = 0;
	TableIndex target_index_;
	vector<ColumnIndex> target_ids_;
	column_binding_set_t source_bindings_;
	TableIndex src_index_;
	vector<SrcColumn> src_;

	vector<column_t> no_ids_;
	vector<string> no_names_;
	vector<LogicalType> no_types_;
	SqlParamSet params_;
	ExpressionEncodeContext ctx_;
	unique_ptr<MergeColumnResolver> resolver_;
};

void MergeRewriter::CheckPlatform() {
	catalog_.CheckWriteAccess("MERGE");
	if (merge_.return_chunk) {
		Refuse("RETURNING");
	}
	catalog_.EnsureServerProperties(context_, "MERGE");
	const auto caps = catalog_.GetDmlCapabilities();
	if (caps.IsSynapse()) {
		Refuse("Synapse runs MERGE in preview only");
	}
	if (!caps.merge || !caps.stage_bulk) {
		Refuse("this platform has no MERGE or no bulk load into a session #temp table");
	}
}

//! The binder's plan: LogicalMergeInto -> LogicalProjection -> join, source on
//! the left and target on the right -- swapped when a RIGHT join (WHEN NOT
//! MATCHED BY SOURCE alone) became a LEFT one. Collects the ON condition: the
//! join's conditions and the filters on the target's side.
void MergeRewriter::TakeJoinApart(vector<unique_ptr<Expression>> &on) {
	if (merge_.children.size() != 1 || merge_.children[0]->type != LogicalOperatorType::LOGICAL_PROJECTION) {
		Refuse("its plan is not the binder's projection over a join");
	}
	projection_ = &merge_.children[0]->Cast<LogicalProjection>();
	if (projection_->children.size() != 1) {
		Refuse("its plan is not the binder's projection over a join");
	}
	join_ = projection_->children[0].get();
	JoinType join_type;
	switch (join_->type) {
	case LogicalOperatorType::LOGICAL_COMPARISON_JOIN: {
		auto &join = join_->Cast<LogicalComparisonJoin>();
		join_type = join.join_type;
		for (auto &condition : join.conditions) {
			if (condition.IsComparison()) {
				on.push_back(BoundComparisonExpression::Create(condition.GetComparisonType(), condition.GetLHS().Copy(),
															   condition.GetRHS().Copy()));
			} else {
				on.push_back(condition.GetJoinExpression().Copy());
			}
		}
		break;
	}
	case LogicalOperatorType::LOGICAL_ANY_JOIN: {
		auto &join = join_->Cast<LogicalAnyJoin>();
		join_type = join.join_type;
		on.push_back(join.condition->Copy());
		break;
	}
	default:
		// A subquery in the ON condition or in an action is planned into the
		// join's tree (an uncorrelated scalar one as a cross product). An ON
		// that went to the sides whole leaves an ANY join on TRUE.
		Refuse("a subquery in the MERGE");
	}
	// The binder joins source and target INNER / LEFT / RIGHT / FULL; any
	// other join here is one a subquery in an action was planned into.
	if ((join_type != JoinType::INNER && join_type != JoinType::LEFT && join_type != JoinType::RIGHT &&
		 join_type != JoinType::OUTER) ||
		join_->children.size() != 2) {
		Refuse("a subquery in the MERGE");
	}
	const bool by_source = merge_.actions.count(MergeActionCondition::WHEN_NOT_MATCHED_BY_SOURCE) > 0;
	const bool by_target = merge_.actions.count(MergeActionCondition::WHEN_NOT_MATCHED_BY_TARGET) > 0;
	// bind_merge_into: CreatePlan can turn a RIGHT join into a LEFT one, with
	// the children reversed.
	const bool inverted = by_source && !by_target && join_type == JoinType::LEFT;
	source_child_ = inverted ? 1 : 0;
	const idx_t target_child = 1 - source_child_;

	LogicalOperator *node = join_->children[target_child].get();
	while (node->type == LogicalOperatorType::LOGICAL_FILTER) {
		for (auto &filter : node->expressions) {
			on.push_back(filter->Copy());
		}
		if (node->children.size() != 1) {
			Refuse("its target is not a plain scan of the table");
		}
		node = node->children[0].get();
	}
	if (node->type != LogicalOperatorType::LOGICAL_GET) {
		Refuse("its target is not a plain scan of the table");
	}
	auto &get = node->Cast<LogicalGet>();
	if (get.GetTable().get() != &merge_.table) {
		Refuse("its target is not a plain scan of the table");
	}
	target_index_ = get.table_index;
	target_ids_ = get.GetColumnIds();
	for (auto &binding : join_->children[source_child_]->GetColumnBindings()) {
		source_bindings_.insert(binding);
	}
}

//! The table column a target binding reads, or invalid for a virtual one.
optional_idx MergeRewriter::TargetColumn(const ColumnBinding &binding) const {
	const auto position = binding.column_index.GetIndex();
	if (position >= target_ids_.size() || target_ids_[position].IsVirtualColumn()) {
		return optional_idx();
	}
	const auto index = target_ids_[position].GetPrimaryIndex();
	return index < columns_.size() ? optional_idx(index) : optional_idx();
}

//! The target column `expr` IS, through casts between string types (the
//! no-op DuckDB puts over an MSSQL_VARCHAR(n) column to reach the VARCHAR
//! operators).
optional_idx MergeRewriter::TargetColumnOf(const Expression &expr_in) const {
	const Expression *expr = &expr_in;
	while (expr->GetExpressionClass() == ExpressionClass::BOUND_FUNCTION && BoundCastExpression::IsCast(*expr)) {
		auto &child = BoundCastExpression::Child(expr->Cast<BoundFunctionExpression>());
		if (!IsStringType(child.GetReturnType()) || !IsStringType(expr->GetReturnType())) {
			return optional_idx();
		}
		expr = &child;
	}
	if (expr->GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
		return optional_idx();
	}
	const auto &binding = expr->Cast<BoundColumnRefExpression>().Binding();
	if (binding.table_index != target_index_) {
		return optional_idx();
	}
	return TargetColumn(binding);
}

uint8_t MergeRewriter::Classify(const Expression &expr) const {
	uint8_t reach = REACH_NONE;
	ExpressionIterator::VisitExpression<BoundColumnRefExpression>(expr, [&](const BoundColumnRefExpression &ref) {
		const auto &binding = ref.Binding();
		if (binding.table_index == target_index_ || binding.table_index == src_index_) {
			reach |= REACH_SERVER;
		} else if (source_bindings_.count(binding)) {
			reach |= REACH_SOURCE;
		} else {
			reach |= REACH_OTHER;
		}
	});
	return reach;
}

//! An action expression refers to the binder's projection; put the
//! projection's expressions in its place, so it reads the join's sides.
void MergeRewriter::ReplaceProjectionRefs(unique_ptr<Expression> &expr) const {
	if (expr->GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
		const auto &binding = expr->Cast<BoundColumnRefExpression>().Binding();
		if (binding.table_index != projection_->table_index) {
			return;
		}
		const auto k = binding.column_index.GetIndex();
		if (k >= merge_.row_id_start || k >= projection_->expressions.size() ||
			(merge_.source_marker.IsValid() && k == merge_.source_marker.GetIndex())) {
			Refuse("an expression reads a row id");
		}
		expr = projection_->expressions[k]->Copy();
		return;
	}
	ExpressionIterator::EnumerateChildren(*expr, [&](unique_ptr<Expression> &child) { ReplaceProjectionRefs(child); });
}

unique_ptr<Expression> MergeRewriter::Inline(const Expression &action_expr) const {
	auto expr = action_expr.Copy();
	ReplaceProjectionRefs(expr);
	if (expr->HasSubquery()) {
		Refuse("a subquery in the MERGE");
	}
	if (Classify(*expr) & REACH_OTHER) {
		Refuse("an expression reads a column of neither the target nor the source");
	}
	return expr;
}

bool MergeRewriter::LikeTargetFits(idx_t column, const LogicalType &source_type) const {
	const auto &info = columns_[column];
	if (info.is_geometry ||
		IsRoundTripExactForKey(info.sql_type_name, info.scale, info.is_cast_required) == KeyFidelity::NotRead) {
		return false;
	}
	const auto &column_type = table_.GetColumns().GetColumn(LogicalIndex(column)).Type();
	if (IsStringType(column_type)) {
		return IsStringType(source_type);
	}
	return source_type == column_type;
}

//! Split `expr` between DuckDB and the server: each largest part over the
//! source alone becomes a #src column, each part over no column is folded (the
//! binder leaves `CAST(15 AS INTEGER)` beside an integer column), the rest
//! stays to be written in T-SQL.
void MergeRewriter::Split(unique_ptr<Expression> &expr, MergeScope scope) {
	const auto reach = Classify(*expr);
	if (reach == REACH_NONE) {
		FoldOrStage(expr, scope);
		return;
	}
	if (reach == REACH_SOURCE) {
		Stage(expr, optional_idx());
		return;
	}
	if ((reach & REACH_SOURCE) && BoundComparisonExpression::IsComparison(*expr)) {
		// A source value compared with a target column directly is declared
		// as that column, so the server compares like with like.
		auto &children = expr->Cast<BoundFunctionExpression>().GetChildrenMutable();
		if (children.size() == 2) {
			for (idx_t side = 0; side < 2; side++) {
				auto &mine = children[side];
				if (Classify(*mine) != REACH_SOURCE) {
					continue;
				}
				const auto column = TargetColumnOf(*children[1 - side]);
				if (column.IsValid() && LikeTargetFits(column.GetIndex(), mine->GetReturnType())) {
					Stage(mine, column);
				}
			}
		}
	}
	ExpressionIterator::EnumerateChildren(*expr, [&](unique_ptr<Expression> &child) { Split(child, scope); });
}

void MergeRewriter::FoldOrStage(unique_ptr<Expression> &expr, MergeScope scope) {
	if (expr->GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
		return;
	}
	if (!expr->IsVolatile() && expr->IsFoldable()) {
		duckdb::Value value;
		if (ExpressionExecutor::TryEvaluateScalar(context_, *expr, value)) {
			expr = make_uniq<BoundConstantExpression>(std::move(value));
			return;
		}
	}
	// Volatile, a parameter, or a constant DuckDB fails to fold: DuckDB
	// computes it per source row.
	if (scope == MergeScope::BY_SOURCE) {
		Refuse(
			"a WHEN NOT MATCHED BY SOURCE clause holds a value DuckDB computes per row (a volatile function or "
			"a parameter), and that clause has no source row");
	}
	Stage(expr, optional_idx());
}

void MergeRewriter::Stage(unique_ptr<Expression> &expr, optional_idx like_target) {
	const auto type = expr->GetReturnType();
	auto same_target = [&](const optional_idx &other) {
		return other.IsValid() == like_target.IsValid() &&
			   (!other.IsValid() || other.GetIndex() == like_target.GetIndex());
	};
	// One #src column per distinct expression -- but never for a volatile one:
	// `uuid(), uuid()` are two values, as DuckDB computes them.
	idx_t n = 0;
	for (; n < src_.size() && !expr->IsVolatile(); n++) {
		if (same_target(src_[n].like_target) && src_[n].type == type && src_[n].expr->Equals(*expr)) {
			break;
		}
	}
	if (expr->IsVolatile()) {
		n = src_.size();
	}
	if (n == src_.size()) {
		SrcColumn column;
		column.type = type;
		column.like_target = like_target;
		if (like_target.IsValid()) {
			const auto declared = LikeTargetOf(columns_[like_target.GetIndex()]);
			column.info = make_uniq<MSSQLColumnInfo>(columns_[like_target.GetIndex()]);
			column.info->name = SrcName(n);
			column.info->sql_type_name = declared.type_name;
			column.info->max_length = declared.max_length;
			column.info->is_nullable = true;
			column.info->is_identity = false;
		}
		column.expr = std::move(expr);
		src_.push_back(std::move(column));
	}
	expr = make_uniq<BoundColumnRefExpression>(type, ColumnBinding(src_index_, ProjectionIndex(n)));
}

//! A target column the ON condition compares with the source must read back as
//! stored: a value that does not would leave its row unmatched, and a WHEN NOT
//! MATCHED INSERT would duplicate it.
void MergeRewriter::CheckOnColumns(const Expression &conjunct) const {
	if (!(Classify(conjunct) & REACH_SOURCE)) {
		return;
	}
	ExpressionIterator::VisitExpression<BoundColumnRefExpression>(conjunct, [&](const BoundColumnRefExpression &ref) {
		if (ref.Binding().table_index != target_index_) {
			return;
		}
		const auto column = TargetColumn(ref.Binding());
		if (!column.IsValid()) {
			Refuse("the ON condition reads a row id");
		}
		const auto &info = columns_[column.GetIndex()];
		switch (IsRoundTripExactForKey(info.sql_type_name, info.scale, info.is_cast_required)) {
		case KeyFidelity::Exact:
			return;
		case KeyFidelity::NotRoundTrip:
			Refuse("the ON condition compares column '" + info.name + "' of type " +
				   SpelledKeyType(info.sql_type_name, info.scale) +
				   ", whose value does not survive the round trip through DuckDB");
		case KeyFidelity::NotRead:
			Refuse("the ON condition compares column '" + info.name + "' of type " + info.sql_type_name +
				   ", which the catalog does not read as the server stores it");
		}
	});
}

string MergeRewriter::ConditionSql(const Expression &condition, MergeScope scope, const string &what) {
	auto expr = Inline(condition);
	const auto text = expr->ToString();
	Split(expr, scope);
	const auto result = FilterEncoder::EncodeSearchCondition(*expr, ctx_);
	if (!result.supported) {
		Refuse(what + " cannot be written in T-SQL: " + text);
	}
	return result.sql;
}

//! The binder casts a value to its column's type; the server's assignment does
//! the same conversion, raising on overflow as DuckDB's cast does. So for a
//! value that reads the target (one DuckDB computes whole keeps its cast) the
//! cast is dropped between integers and between strings, which FilterEncoder
//! would otherwise refuse as value-changing.
void MergeRewriter::StripAssignmentCast(unique_ptr<Expression> &expr, const LogicalType &column_type) const {
	if (!(Classify(*expr) & REACH_SERVER)) {
		return;
	}
	while (expr->GetExpressionClass() == ExpressionClass::BOUND_FUNCTION && BoundCastExpression::IsCast(*expr) &&
		   expr->GetReturnType() == column_type) {
		auto &child = BoundCastExpression::ChildMutable(expr->Cast<BoundFunctionExpression>());
		const auto &from = child->GetReturnType();
		const bool integers = from.IsIntegral() && column_type.IsIntegral();
		const bool strings = IsStringType(from) && IsStringType(column_type);
		if (!integers && !strings) {
			return;
		}
		auto stripped = std::move(child);
		expr = std::move(stripped);
	}
}

string MergeRewriter::ValueSql(const Expression &value, MergeScope scope, const string &what,
							   const LogicalType &column_type) {
	auto expr = Inline(value);
	const auto text = expr->ToString();
	StripAssignmentCast(expr, column_type);
	Split(expr, scope);
	const auto result = FilterEncoder::EncodeExpression(*expr, ctx_);
	if (!result.supported) {
		Refuse(what + " cannot be written in T-SQL: " + text);
	}
	return result.sql;
}

string MergeRewriter::Clause(MergeActionCondition when, const BoundMergeIntoAction &action) {
	const auto scope = when == MergeActionCondition::WHEN_MATCHED				  ? MergeScope::MATCHED
					   : when == MergeActionCondition::WHEN_NOT_MATCHED_BY_TARGET ? MergeScope::BY_TARGET
																				  : MergeScope::BY_SOURCE;
	const string when_sql = MergeWhenSql(when);
	string sql = " " + when_sql;
	if (action.condition) {
		sql += " AND " + ConditionSql(*action.condition, scope, "the condition of a " + when_sql + " clause");
	}
	switch (action.action_type) {
	case MergeActionType::MERGE_DELETE:
		return sql + " THEN DELETE";
	case MergeActionType::MERGE_UPDATE: {
		string sets;
		for (idx_t i = 0; i < action.columns.size(); i++) {
			const auto index = action.columns[i].index;
			const auto &col = columns_[index];
			if (col.is_identity) {
				Refuse("an UPDATE of the identity column '" + col.name + "'");
			}
			sets += (sets.empty() ? "" : ", ") + QuoteIdentifier(col.name) + " = " +
					ValueSql(*action.expressions[i], scope, "the value SET for column '" + col.name + "'",
							 merge_.table.GetColumns().GetColumn(LogicalIndex(index)).Type());
		}
		return sql + " THEN UPDATE SET " + sets;
	}
	case MergeActionType::MERGE_INSERT: {
		// DuckDB binds the action full width, in storage order: a column the
		// INSERT did not name carries a copy of its bound default, and stays out
		// of the list so the server applies its identity / DEFAULT / computed
		// value, as for a plain INSERT.
		string names;
		string values;
		idx_t expr_idx = 0;
		for (auto &column : merge_.table.GetColumns().Physical()) {
			const auto storage_idx = column.StorageOid();
			auto &expr = *action.expressions[expr_idx++];
			bool unnamed;
			if (!action.column_index_map.empty()) {
				unnamed = action.column_index_map[column.Physical()] == DConstants::INVALID_INDEX;
			} else {
				unnamed = storage_idx < merge_.bound_defaults.size() && merge_.bound_defaults[storage_idx] &&
						  expr.GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF &&
						  expr.Equals(*merge_.bound_defaults[storage_idx]);
			}
			if (unnamed) {
				continue;
			}
			const auto &col = columns_[storage_idx];
			if (col.is_identity) {
				Refuse("an INSERT naming the identity column '" + col.name +
					   "' (a MERGE runs without IDENTITY_INSERT)");
			}
			names += (names.empty() ? "" : ", ") + QuoteIdentifier(col.name);
			values += (values.empty() ? "" : ", ") +
					  ValueSql(expr, scope, "the value INSERTed into column '" + col.name + "'", column.Type());
		}
		if (names.empty()) {
			return sql + " THEN INSERT DEFAULT VALUES";
		}
		return sql + " THEN INSERT (" + names + ") VALUES (" + values + ")";
	}
	default:
		Refuse("a MERGE action T-SQL has no form for");
	}
}

//! Declare every #src column: a like-target one as its target column, any
//! other through
//! the CTAS mapping. Strings that are not like-target are plain VARCHAR
//! there -- nvarchar(max), or varchar(max) on a platform without nvarchar --
//! in the database's collation rather than tempdb's, so a comparison with a
//! target column does not raise 468; the server converts a value into the
//! target column's type where it is written.
void MergeRewriter::TypeSrcColumns(MSSQLStagedMergeTarget &target, string &select_list) {
	CTASConfig config = CTASConfig::Load(context_);
	const bool single_byte = catalog_.RequiresSingleByteText();
	config.text_type = single_byte ? CTASTextType::VARCHAR : CTASTextType::NVARCHAR;
	config.default_string_length = 0;
	config.varchar_collation = catalog_.ResolveVarcharCollation(context_, single_byte, true);
	config.wire_varchar_collation = single_byte ? catalog_.WireVarcharCollation(config.varchar_collation) : string();

	for (idx_t n = 0; n < src_.size(); n++) {
		auto &column = src_[n];
		const auto name = SrcName(n);
		string item;
		if (column.like_target.IsValid()) {
			const auto &col = columns_[column.like_target.GetIndex()];
			const auto declared = LikeTargetOf(col);
			const auto ref = "t." + QuoteIdentifier(col.name);
			if (declared.cast.empty()) {
				item = ref;
			} else {
				item = "CAST(" + ref + " AS " + declared.cast + ")";
				if (!col.collation_name.empty()) {
					item += " COLLATE " + col.collation_name;
				}
			}
			target.src_columns.push_back(BCPColumnMetadata::FromServerColumn(
				name, declared.type_name, declared.max_length, col.precision, col.scale, true, col.collation_name));
			target.src_columns.back().reported_name = col.name;
		} else {
			const auto type = IsStringType(column.type) ? LogicalType::VARCHAR : column.type;
			string declaration;
			try {
				declaration = MSSQLDDLTranslator::MapLogicalTypeToCTAS(type, config);
			} catch (std::exception &) {
				Refuse("a value of type " + column.type.ToString() + " cannot be staged in #src");
			}
			string collate;
			const auto lower = StringUtil::Lower(declaration);
			const auto at = lower.find(" collate ");
			if (at != string::npos) {
				collate = declaration.substr(at + 9);
				declaration = declaration.substr(0, at);
			} else if (IsStringType(type)) {
				collate = "DATABASE_DEFAULT";
			}
			item = "CAST(NULL AS " + declaration + ")";
			if (!collate.empty()) {
				item += " COLLATE " + collate;
			}
			vector<BCPColumnMetadata> metadata;
			try {
				metadata =
					TargetResolver::GenerateColumnMetadata({type}, {name}, config.wire_varchar_collation, single_byte);
			} catch (std::exception &) {
				Refuse("a value of type " + column.type.ToString() + " cannot be staged in #src");
			}
			target.src_columns.push_back(std::move(metadata[0]));
		}
		select_list += (n ? ", " : "") + item + " AS " + QuoteIdentifier(name);
	}
}

unique_ptr<LogicalOperator> MergeRewriter::Rewrite() {
	CheckPlatform();
	vector<unique_ptr<Expression>> on;
	TakeJoinApart(on);
	src_index_ = optimizer_.binder.GenerateTableIndex();

	resolver_ = make_uniq<MergeColumnResolver>(target_index_, target_ids_, columns_, src_index_, src_);
	ctx_.resolver = resolver_.get();
	ctx_.params = &params_;
	ctx_.division_by_zero_errors = LoadErrorOnDivisionByZero(context_);
	ctx_.widening_casts = true;

	// The clauses T-SQL has a form for, in its order.
	std::map<MergeActionCondition, std::vector<MergeActionType>> shape;
	for (auto &entry : merge_.actions) {
		auto &list = shape[entry.first];
		for (auto &action : entry.second) {
			list.push_back(action->action_type);
		}
	}
	std::map<MergeActionCondition, std::vector<idx_t>> kept;
	string why;
	if (!CheckMergeShape(shape, kept, why)) {
		Refuse(why);
	}

	string on_sql;
	for (auto &conjunct : on) {
		const auto text = conjunct->ToString();
		if (conjunct->HasSubquery()) {
			Refuse("a subquery in the ON condition");
		}
		if (Classify(*conjunct) & REACH_OTHER) {
			Refuse("the ON condition reads a column of neither the target nor the source");
		}
		CheckOnColumns(*conjunct);
		Split(conjunct, MergeScope::ON);
		const auto result = FilterEncoder::EncodeSearchCondition(*conjunct, ctx_);
		if (!result.supported) {
			Refuse("the ON condition cannot be written in T-SQL: " + text);
		}
		on_sql += (on_sql.empty() ? "" : " AND ") + result.sql;
	}
	if (on_sql.empty()) {
		on_sql = "1 = 1";
	}

	string clauses;
	const MergeActionCondition order[] = {MergeActionCondition::WHEN_MATCHED,
										  MergeActionCondition::WHEN_NOT_MATCHED_BY_TARGET,
										  MergeActionCondition::WHEN_NOT_MATCHED_BY_SOURCE};
	for (auto when : order) {
		auto entry = kept.find(when);
		if (entry == kept.end()) {
			continue;
		}
		auto &actions = merge_.actions.find(when)->second;
		for (auto i : entry->second) {
			clauses += Clause(when, *actions[i]);
		}
	}

	// #src needs a column even when nothing reads the source: its rows are
	// what the MERGE matches.
	if (src_.empty()) {
		unique_ptr<Expression> marker = make_uniq<BoundConstantExpression>(duckdb::Value::BOOLEAN(true));
		Stage(marker, optional_idx());
	}

	MSSQLStagedMergeTarget target;
	target.catalog_name = catalog_.GetName().GetIdentifierName();
	target.schema_name = schema_name_;
	target.table_name = table_name_;
	auto uuid = UUID::ToString(UUID::GenerateRandomUUID());
	uuid.erase(std::remove(uuid.begin(), uuid.end(), '-'), uuid.end());
	target.src_name = "#src_" + uuid;
	string select_list;
	TypeSrcColumns(target, select_list);
	const auto qualified_table = QuoteIdentifier(schema_name_) + "." + QuoteIdentifier(table_name_);
	// SELECT INTO copies a like-target column's type, length and collation; the
	// outer join and the UNION ALL make every column nullable and drop the
	// IDENTITY a bare column would carry over (measured: through the join alone
	// an identity column came out NOT NULL IDENTITY); WHERE 1 = 0 keeps the
	// outer row out.
	const auto from = " FROM (SELECT 1 AS d) AS d LEFT JOIN " + qualified_table + " AS t ON 1 = 0 WHERE 1 = 0";
	target.create_sql = "SELECT " + select_list + " INTO " + QuoteIdentifier(target.src_name) + from +
						" UNION ALL SELECT " + select_list + from;
	target.merge_sql = "MERGE " + qualified_table + " AS [t] USING " + QuoteIdentifier(target.src_name) +
					   " AS [s] ON " + on_sql + clauses + ";";
	target.merge_request =
		params_.params.empty() ? tds::Request(target.merge_sql) : params_.ExecuteSqlRequest(target.merge_sql);
	target.runs_on_empty_source = kept.count(MergeActionCondition::WHEN_NOT_MATCHED_BY_SOURCE) > 0;
	target.flush_rows = LoadBCPCopyConfig(context_).flush_rows;
	target.query_timeout_seconds = LoadQueryTimeout(context_);

	// The source, untouched, under a projection that computes the #src columns.
	vector<unique_ptr<Expression>> computed;
	vector<unique_ptr<Expression>> references;
	for (idx_t n = 0; n < src_.size(); n++) {
		references.push_back(
			make_uniq<BoundColumnRefExpression>(src_[n].type, ColumnBinding(src_index_, ProjectionIndex(n))));
		computed.push_back(std::move(src_[n].expr));
	}
	auto src_projection = make_uniq<LogicalProjection>(src_index_, std::move(computed));
	src_projection->children.push_back(std::move(join_->children[source_child_]));
	auto staged =
		make_uniq<MSSQLLogicalStagedMerge>(merge_.table, merge_.table_index, std::move(target), std::move(references));
	staged->children.push_back(std::move(src_projection));
	return std::move(staged);
}

}  // namespace

void RewriteMerge(ClientContext &context, Optimizer &optimizer, unique_ptr<LogicalOperator> &op) {
	MergeRewriter rewriter(context, optimizer, op->Cast<LogicalMergeInto>());
	op = rewriter.Rewrite();
}

}  // namespace mssql
}  // namespace duckdb
