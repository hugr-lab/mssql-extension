#include "catalog/mssql_table_options.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"

namespace duckdb {

static string QuoteName(const string &name) {
	string escaped;
	escaped.reserve(name.size() + 2);
	for (const char c : name) {
		escaped += c;
		if (c == ']') {
			escaped += ']';
		}
	}
	return "[" + escaped + "]";
}

static MSSQLTableKind ParseTableKind(const string &value) {
	const string upper = StringUtil::Upper(value);
	if (upper == "HEAP") {
		return MSSQLTableKind::HEAP;
	}
	if (upper == "COLUMNSTORE" || upper == "CLUSTERED_COLUMNSTORE" || upper == "CCI") {
		return MSSQLTableKind::COLUMNSTORE;
	}
	if (upper == "CLUSTERED") {
		return MSSQLTableKind::CLUSTERED;
	}
	throw InvalidInputException("MSSQL: unknown table_kind '%s'; expected HEAP, COLUMNSTORE or CLUSTERED", value);
}

MSSQLTableOptions MSSQLTableOptions::FromSettings(ClientContext &context) {
	MSSQLTableOptions options;
	Value setting;
	if (context.TryGetCurrentSetting("mssql_default_table_kind", setting) && !setting.IsNull()) {
		options.kind = ParseTableKind(setting.ToString());
	}
	return options;
}

void MSSQLTableOptions::ApplyOption(const string &name, const string &value) {
	const string lower = StringUtil::Lower(name);
	if (lower == "table_kind") {
		kind = ParseTableKind(value);
		return;
	}
	if (lower == "clustered_index") {
		// A key list, so the kind is implied — writing both would be a way to
		// state two different things.
		kind = MSSQLTableKind::CLUSTERED;
		clustered_keys.clear();
		for (auto &key : StringUtil::Split(value, ',')) {
			string trimmed = key;
			StringUtil::Trim(trimmed);
			if (!trimmed.empty()) {
				clustered_keys.push_back(trimmed);
			}
		}
		if (clustered_keys.empty()) {
			throw InvalidInputException("MSSQL: clustered_index needs at least one column name");
		}
		return;
	}
	if (lower == "data_compression") {
		const string upper = StringUtil::Upper(value);
		if (upper != "PAGE" && upper != "ROW" && upper != "NONE") {
			throw InvalidInputException("MSSQL: data_compression must be PAGE, ROW or NONE, got '%s'", value);
		}
		data_compression = upper == "NONE" ? string() : upper;
		return;
	}
	throw InvalidInputException(
		"MSSQL: unknown table option '%s'. Supported: table_kind, clustered_index, data_compression", name);
}

void MSSQLTableOptions::ApplyWithClause(const case_insensitive_map_t<unique_ptr<ParsedExpression>> &options) {
	for (auto &entry : options) {
		if (!entry.second || entry.second->GetExpressionClass() != ExpressionClass::CONSTANT) {
			throw InvalidInputException("MSSQL: table option '%s' must be a constant", entry.first);
		}
		auto &constant = entry.second->Cast<ConstantExpression>();
		if (constant.value.IsNull()) {
			throw InvalidInputException("MSSQL: table option '%s' must not be NULL", entry.first);
		}
		ApplyOption(entry.first, constant.value.ToString());
	}
}

string MSSQLTableOptions::CreateTableSuffix() const {
	if (data_compression.empty()) {
		return string();
	}
	return " WITH (DATA_COMPRESSION = " + data_compression + ")";
}

string MSSQLTableOptions::PostCreateStatement(const string &schema_name, const string &table_name) const {
	// A temp table target carries an empty schema — it lives in tempdb under the
	// session, and "[].[#t]" is not a name SQL Server accepts.
	const string qualified =
		schema_name.empty() ? QuoteName(table_name) : QuoteName(schema_name) + "." + QuoteName(table_name);

	switch (kind) {
	case MSSQLTableKind::COLUMNSTORE:
		// No DATA_COMPRESSION clause here on purpose: PAGE and ROW are
		// rowstore-only and SQL Server rejects them on a columnstore index. The
		// CREATE TABLE suffix is where they belong, and columnstore brings its
		// own compression regardless. The index name only has to be unique
		// within the table.
		return "CREATE CLUSTERED COLUMNSTORE INDEX " + QuoteName("CCI_" + table_name) + " ON " + qualified;
	case MSSQLTableKind::CLUSTERED: {
		string keys;
		for (idx_t i = 0; i < clustered_keys.size(); i++) {
			if (i > 0) {
				keys += ", ";
			}
			keys += QuoteName(clustered_keys[i]);
		}
		string sql =
			"CREATE CLUSTERED INDEX " + QuoteName("CIX_" + table_name) + " ON " + qualified + " (" + keys + ")";
		if (!data_compression.empty()) {
			sql += " WITH (DATA_COMPRESSION = " + data_compression + ")";
		}
		return sql;
	}
	case MSSQLTableKind::HEAP:
	default:
		return string();
	}
}

}  // namespace duckdb
