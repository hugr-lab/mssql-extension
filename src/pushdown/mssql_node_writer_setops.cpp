#include "pushdown/mssql_node_writer.hpp"

#include "duckdb/common/string_util.hpp"

namespace duckdb {
namespace mssql {
namespace node_writer {

bool NodeWriter::SetOperationKeyword(const SetOperationNode &node, const char *&keyword) {
	switch (node.setop_type) {
	case SetOperationType::UNION:
		keyword = node.setop_all ? " UNION ALL " : " UNION ";
		break;
	case SetOperationType::EXCEPT:
		keyword = " EXCEPT ";
		break;
	case SetOperationType::INTERSECT:
		keyword = " INTERSECT ";
		break;
	default:
		return Veto("UNION BY NAME");
	}
	if (node.setop_all && node.setop_type != SetOperationType::UNION) {
		// T-SQL has no EXCEPT ALL / INTERSECT ALL.
		return Veto("EXCEPT ALL / INTERSECT ALL");
	}
	return true;
}

bool NodeWriter::WriteSetOperationMembers(const SetOperationNode &node, const char *keyword,
										  std::vector<WrittenQuery> &child_outs,
										  std::vector<unique_ptr<NodeWriter>> &children, std::string &sql) {
	for (idx_t c = 0; c < node.children.size(); c++) {
		auto &child_node = *node.children[c];
		bool ordered = false;
		bool limited = false;
		for (auto &modifier : child_node.modifiers) {
			ordered = ordered || modifier->type == ResultModifierType::ORDER_MODIFIER;
			limited = limited || modifier->type == ResultModifierType::LIMIT_MODIFIER;
		}
		if (ordered && !limited) {
			// 1033 there, as in a derived table; DuckDB promises no order.
			return Veto("an ORDER BY without LIMIT in a set operation's member");
		}
		// Its children are aliased past nothing of this node: a correlated
		// column is the enclosing node's.
		children.push_back(make_uniq<NodeWriter>(*this, child_outs[c], Correlated()));
		if (!children.back()->WriteQueryNode(child_node)) {
			out_.refers_outside = out_.refers_outside || child_outs[c].refers_outside;
			return false;
		}
		std::string part = child_outs[c].statement;
		if (ordered || limited) {
			// TOP / ORDER BY / OFFSET inside a set operation's member: in a
			// derived table of its own, which T-SQL takes.
			part = "SELECT * FROM (" + part + ") AS [q" + std::to_string(alias_base_ + c + 1) + "]";
		} else if (child_node.type == QueryNodeType::SET_OPERATION_NODE) {
			part = "(" + part + ")";
		}
		sql += (c == 0 ? "" : keyword) + part;
	}
	return true;
}

//! One DuckDB type, known, on every side: the server's promotion (varchar(10) with varchar(20), int with
//! bigint) is not DuckDB's.
static bool SetOperationTypesAgree(const WrittenQuery &mine, const WrittenQuery &first, idx_t i) {
	return mine.column_types[i] == first.column_types[i] && mine.cast_types[i] == first.cast_types[i] &&
		   !(mine.column_types[i].id() == LogicalTypeId::INVALID && mine.cast_types[i].id() == LogicalTypeId::INVALID);
}

//! One server shape too: time(3) and time(7) are both TIME here, a varchar(5) and a varchar(50) both
//! VARCHAR with native types off -- the first member's metadata would misdescribe the rest.
static bool SameServerShape(const MSSQLColumnInfo &mine, const MSSQLColumnInfo &theirs) {
	return StringUtil::CIEquals(mine.sql_type_name, theirs.sql_type_name) && mine.max_length == theirs.max_length &&
		   mine.precision == theirs.precision && mine.scale == theirs.scale;
}

bool NodeWriter::CheckSetOperationServerTypes(const NodeWriter &child, const OutputColumn &output,
											  const NodeWriter &first, const OutputColumn &reference) {
	const bool column = output.column_index != DConstants::INVALID_INDEX;
	const bool ref_column = reference.column_index != DConstants::INVALID_INDEX;
	const auto kind = column ? KindOf(child.FlatColumn(output.column_index)) : output.value.kind;
	if (kind == ComparableKind::String) {
		// One collation, or the server's 468 / a different comparison.
		if (!column || !output.plain_read || !ref_column || !reference.plain_read ||
			child.FlatColumn(output.column_index).collation_name !=
				first.FlatColumn(reference.column_index).collation_name ||
			!StringUtil::CIEquals(child.FlatColumn(output.column_index).sql_type_name,
								  first.FlatColumn(reference.column_index).sql_type_name)) {
			return Veto("set operation string columns that are not columns of one type and collation");
		}
	}
	// A type the server converts by its own rules (hierarchyid read as its text, sql_variant, ...:
	// comparison_differs) unifies with another member there by the server's precedence, which
	// DuckDB's type does not show: an nvarchar member beside a hierarchyid one is converted to
	// hierarchyid on the server. Every member is then a plain column of that one server type,
	// whichever comes first (review of #593: the String check above saw only one order).
	const bool mine_differs = column && child.FlatColumn(output.column_index).comparison_differs;
	const bool theirs_differs = ref_column && first.FlatColumn(reference.column_index).comparison_differs;
	if ((mine_differs || theirs_differs) &&
		(!column || !ref_column || !output.plain_read || !reference.plain_read ||
		 !StringUtil::CIEquals(child.FlatColumn(output.column_index).sql_type_name,
							   first.FlatColumn(reference.column_index).sql_type_name))) {
		return Veto(
			"set operation columns of different server types, one converted by the server's "
			"own rules");
	}
	return true;
}

bool NodeWriter::CheckSetOperationMember(const NodeWriter &child, const WrittenQuery &child_out,
										 const NodeWriter &first, const WrittenQuery &first_out, idx_t i, bool compares,
										 SetOperationColumn &state) {
	if (child.Outputs().size() != first.Outputs().size()) {
		return Veto("set operation children of different widths");
	}
	auto &output = child.Outputs()[i];
	auto &reference = first.Outputs()[i];
	if (!SetOperationTypesAgree(child_out, first_out, i)) {
		return Veto("set operation columns of different types");
	}
	const bool column = output.column_index != DConstants::INVALID_INDEX;
	state.plain = state.plain && column && output.plain_read;
	if (state.plain) {
		state.plain = SameServerShape(child.FlatColumn(output.column_index), first.FlatColumn(reference.column_index));
	}
	if (!CheckSetOperationServerTypes(child, output, first, reference)) {
		return false;
	}
	if (compares) {
		if (column ? !IsGroupable(child.FlatColumn(output.column_index))
				   : output.value.kind == ComparableKind::None || output.value.approximate || output.value.division) {
			return Veto("a set operation compares " + output.name);
		}
	}
	if (column) {
		state.nullable = state.nullable || child.FlatColumn(output.column_index).is_nullable ||
						 child.divergent_of_[output.column_index];
		state.divergent = state.divergent || child.divergent_of_[output.column_index];
	} else {
		state.nullable = true;
		state.divergent = state.divergent || output.value.division || output.value.approximate;
	}
	return true;
}

void NodeWriter::AddSetOperationOutput(const NodeWriter &first, const WrittenQuery &first_out, idx_t i,
									   const SetOperationColumn &state) {
	auto &reference = first.Outputs()[i];
	const bool cast_back = first_out.cast_types[i].id() != LogicalTypeId::INVALID;
	OutputColumn entry;
	entry.name = reference.name;
	if (state.plain) {
		auto column = first.FlatColumn(reference.column_index);
		column.name = reference.name;  // the set operation's column, named by its output on both sides
		column.duckdb_name = reference.name;
		column.is_nullable = state.nullable;
		entry.column_index = columns_.size();
		entry.plain_read = true;
		columns_.push_back(std::move(column));
		// Parallel to columns_ as elsewhere; relation_of_ stays empty, as a
		// set operation has no relation (FindColumn answers 0 for it).
		types_.push_back(first_out.column_types[i]);
		cast_of_.push_back(first_out.cast_types[i]);
		already_read_.push_back(true);
		divergent_of_.push_back(state.divergent);
	} else {
		entry.column_index = DConstants::INVALID_INDEX;
		if (reference.column_index != DConstants::INVALID_INDEX) {
			// Columns of different server shapes: a value, opaque outside.
			auto &column = first.FlatColumn(reference.column_index);
			entry.value.type = column.duckdb_type;
			entry.value.kind = ComparableKind::None;
		} else {
			entry.value = reference.value;
		}
		entry.value.column = nullptr;
		entry.value.constant = nullptr;
		entry.value.orderable = false;
		entry.value.not_null = false;
		entry.value.division = state.divergent;
		entry.value.approximate = state.divergent;
		entry.value.result_type = cast_back ? first_out.cast_types[i] : first_out.column_types[i];
		entry.value.cast_result = cast_back;
	}
	outputs_.push_back(std::move(entry));
	out_.column_types.push_back(first_out.column_types[i]);
	out_.cast_types.push_back(first_out.cast_types[i]);
	out_.column_names.push_back(first_out.column_names[i]);
}

void NodeWriter::MergeMemberFacts(const std::vector<WrittenQuery> &child_outs) {
	for (auto &child_out : child_outs) {
		out_.picks_rows = out_.picks_rows || child_out.picks_rows;
		out_.gain_uncertain = out_.gain_uncertain || child_out.gain_uncertain;
		out_.value_divergence = out_.value_divergence || child_out.value_divergence;
		out_.largest_input_rows = MaxValue(out_.largest_input_rows, child_out.largest_input_rows);
		out_.input_size_unknown = out_.input_size_unknown || child_out.input_size_unknown;
		out_.total_input_rows += child_out.total_input_rows;
		out_.total_size_unknown = out_.total_size_unknown || child_out.total_size_unknown;
	}
}

bool NodeWriter::WriteSetOperation(const SetOperationNode &node) {
	const char *keyword;
	if (!SetOperationKeyword(node, keyword)) {
		return false;
	}
	// UNION, EXCEPT and INTERSECT compare rows: a set of values, as DISTINCT's.
	const bool compares = !(node.setop_type == SetOperationType::UNION && node.setop_all);
	if (node.children.size() < 2) {
		return Veto("a set operation of one child");
	}
	std::vector<WrittenQuery> child_outs(node.children.size());
	std::vector<unique_ptr<NodeWriter>> children;
	std::string sql;
	if (!WriteSetOperationMembers(node, keyword, child_outs, children, sql)) {
		return false;
	}
	auto &first = *children[0];
	const auto width = first.Outputs().size();
	for (idx_t i = 0; i < width; i++) {
		SetOperationColumn state;
		for (idx_t c = 0; c < children.size(); c++) {
			if (!CheckSetOperationMember(*children[c], child_outs[c], first, child_outs[0], i, compares, state)) {
				return false;
			}
		}
		AddSetOperationOutput(first, child_outs[0], i, state);
	}
	MergeMemberFacts(child_outs);
	out_.statement = sql;
	return true;
}

}  // namespace node_writer
}  // namespace mssql
}  // namespace duckdb
