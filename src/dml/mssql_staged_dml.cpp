#include "dml/mssql_staged_dml.hpp"

#include <algorithm>

#include "catalog/mssql_catalog.hpp"
#include "connection/mssql_connection_provider.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/uuid.hpp"
#include "duckdb/main/client_context.hpp"
#include "query/mssql_identifier.hpp"
#include "query/mssql_simple_query.hpp"

namespace duckdb {

//! mssql_query_timeout in ms for MSSQLSimpleQuery (0 = none), clamped to int.
static int QueryTimeoutMs(const MSSQLStagedDmlTarget &target) {
	const int64_t ms = static_cast<int64_t>(target.query_timeout_seconds) * 1000;
	return ms > NumericLimits<int32_t>::Maximum() ? NumericLimits<int32_t>::Maximum() : static_cast<int>(ms);
}

static string KeyName(idx_t i) {
	return "k" + std::to_string(i);
}

static string NewValueName(idx_t i) {
	return "n" + std::to_string(i);
}

//! rowversion / timestamp: a stage column of that type would refuse the value
//! INSERT BULK sends, so the stage holds it as the binary(8) it is.
static bool IsRowVersion(const MSSQLColumnInfo &col) {
	const auto t = StringUtil::Lower(col.sql_type_name);
	return t == "timestamp" || t == "rowversion";
}

//! A column as the stage's SELECT INTO copies it.
static string StageColumnSource(const MSSQLColumnInfo &col) {
	const auto ref = "t." + mssql::QuoteIdentifier(col.name);
	return IsRowVersion(col) ? "CAST(" + ref + " AS binary(8))" : ref;
}

string MSSQLStagedDml::CreateStageSql(const MSSQLStagedDmlTarget &target, const string &stage_name) {
	// SELECT INTO copies each column's type, length and collation exactly, so
	// the stage compares like the target. The UNION ALL drops the IDENTITY
	// property SELECT INTO would otherwise carry over (INSERT BULK would then
	// refuse explicit values); computed columns become plain ones.
	string list;
	for (idx_t i = 0; i < target.key_columns.size(); i++) {
		list += (list.empty() ? "" : ", ") + StageColumnSource(target.key_columns[i]) + " AS " +
				mssql::QuoteIdentifier(KeyName(i));
	}
	for (idx_t i = 0; i < target.set_columns.size(); i++) {
		list += ", " + StageColumnSource(target.set_columns[i]) + " AS " + mssql::QuoteIdentifier(NewValueName(i));
	}
	const auto from = " FROM " + mssql::QuoteIdentifier(target.schema_name) + "." +
					  mssql::QuoteIdentifier(target.table_name) + " AS t WHERE 1 = 0";
	return "SELECT " + list + " INTO " + mssql::QuoteIdentifier(stage_name) + from + " UNION ALL SELECT " + list + from;
}

static bool IsCharacterString(const MSSQLColumnInfo &col) {
	const auto t = StringUtil::Lower(col.sql_type_name);
	return t == "char" || t == "varchar" || t == "nchar" || t == "nvarchar" || t == "sysname";
}

string MSSQLStagedDml::JoinStatementSql(const MSSQLStagedDmlTarget &target, const string &stage_name) {
	// A stage row must match exactly the rows equal to it in every byte. A
	// string compares under its collation -- case, accents, trailing spaces --
	// so 'Ab' and 'ab' (on a _CI_ collation), or 'a' and 'a ', would each match
	// the other's stage row: an UPDATE could write one row's new value into the
	// other, and a DELETE whose pushed LIKE told them apart would remove both.
	// A string column is therefore compared as its bytes (stage and target share
	// type and collation, so the encodings agree), with the collation `=` kept
	// beside it on a NOT NULL column so an index on it can still seek.
	//
	// NOT NULL columns compare with `=`; the nullable ones with IS NOT DISTINCT
	// FROM where the server has it, else together through one
	// EXISTS (… INTERSECT …), which treats NULL as equal to NULL everywhere.
	string on;
	string intersect_target;
	string intersect_stage;
	auto add = [&](const string &term) { on += (on.empty() ? "" : " AND ") + term; };
	for (idx_t i = 0; i < target.key_columns.size(); i++) {
		const auto &col = target.key_columns[i];
		const auto t = "t." + mssql::QuoteIdentifier(col.name);
		const auto s = "s." + mssql::QuoteIdentifier(KeyName(i));
		string t_value = t;
		string s_value = s;
		if (IsRowVersion(col)) {
			t_value = "CAST(" + t + " AS binary(8))";
		} else if (IsCharacterString(col)) {
			t_value = "CAST(" + t + " AS varbinary(max))";
			s_value = "CAST(" + s + " AS varbinary(max))";
		}
		if (!col.is_nullable) {
			if (IsCharacterString(col)) {
				add(t + " = " + s);
			}
			add(t_value + " = " + s_value);
		} else if (target.null_safe_operator) {
			add(t_value + " IS NOT DISTINCT FROM " + s_value);
		} else {
			intersect_target += (intersect_target.empty() ? "" : ", ") + t_value;
			intersect_stage += (intersect_stage.empty() ? "" : ", ") + s_value;
		}
	}
	if (!intersect_target.empty()) {
		add("EXISTS (SELECT " + intersect_target + " INTERSECT SELECT " + intersect_stage + ")");
	}
	const auto table = mssql::QuoteIdentifier(target.schema_name) + "." + mssql::QuoteIdentifier(target.table_name);
	const auto join = " FROM " + table + " AS t INNER JOIN " + mssql::QuoteIdentifier(stage_name) + " AS s ON " + on;
	if (target.kind == MSSQLStagedDmlKind::DELETE) {
		return "DELETE t" + join;
	}
	string set;
	for (idx_t i = 0; i < target.set_columns.size(); i++) {
		set += (set.empty() ? "" : ", ") + string("t.") + mssql::QuoteIdentifier(target.set_columns[i].name) + " = s." +
			   mssql::QuoteIdentifier(NewValueName(i));
	}
	return "UPDATE t SET " + set + join;
}

MSSQLStagedDml::MSSQLStagedDml(ClientContext &context, MSSQLStagedDmlTarget target)
	: target_(std::move(target)),
	  catalog_(Catalog::GetCatalog(context, Identifier(target_.catalog_name)).Cast<MSSQLCatalog>()) {
	auto uuid = UUID::ToString(UUID::GenerateRandomUUID());
	uuid.erase(std::remove(uuid.begin(), uuid.end(), '-'), uuid.end());
	stage_name_ = "#stage_" + uuid;
}

MSSQLStagedDml::~MSSQLStagedDml() = default;

void MSSQLStagedDml::FailAndThrow(ClientContext &context, const string &message) {
	session_.Abandon();
	stmt_conn_.Fail(context, catalog_);
	connection_.reset();
	throw IOException("%s", message);
}

void MSSQLStagedDml::Start(ClientContext &context, DataChunk &first_chunk) {
	const char *verb = target_.kind == MSSQLStagedDmlKind::UPDATE ? "UPDATE" : "DELETE";
	connection_ = stmt_conn_.Acquire(context, catalog_);
	{
		auto pinned_lock = stmt_conn_.LockPinned(context, catalog_);
		MSSQLStatementConnection::RequireIdle(*connection_, verb);
		const auto create = CreateStageSql(target_, stage_name_);
		auto result = MSSQLSimpleQuery::Execute(*connection_, create, QueryTimeoutMs(target_));
		if (!result.success) {
			FailAndThrow(context, StringUtil::Format("MSSQL %s on '%s.%s': creating the stage failed: %s", verb,
													 target_.schema_name, target_.table_name, result.error_message));
		}
	}

	// The fill: every staged column from its chunk position. The key is the
	// last key_columns.size() columns (the hidden key columns the binder
	// appended, mssql_keyless_key.hpp); a SET value sits where the binder put it.
	const idx_t key_count = target_.key_columns.size();
	if (first_chunk.ColumnCount() < key_count) {
		throw InternalException("MSSQL %s: chunk has %llu columns, the key %llu", verb,
								(unsigned long long)first_chunk.ColumnCount(), (unsigned long long)key_count);
	}
	const idx_t key_start = first_chunk.ColumnCount() - key_count;
	auto add_column = [&](const MSSQLColumnInfo &col, const string &name, idx_t chunk_index) {
		if (IsRowVersion(col)) {
			bcp_columns_.push_back(mssql::BCPColumnMetadata::FromServerColumn(name, "binary", 8, 0, 0, true, string()));
		} else {
			bcp_columns_.push_back(mssql::BCPColumnMetadata::FromServerColumn(name, col.sql_type_name, col.max_length,
																			  col.precision, col.scale, col.is_nullable,
																			  col.collation_name));
		}
		column_mapping_.push_back(static_cast<int32_t>(chunk_index));
	};
	for (idx_t i = 0; i < key_count; i++) {
		add_column(target_.key_columns[i], KeyName(i), key_start + i);
	}
	for (idx_t i = 0; i < target_.set_columns.size(); i++) {
		add_column(target_.set_columns[i], NewValueName(i), target_.set_chunk_index[i]);
	}
	bcp_target_ = mssql::BCPCopyTarget(target_.catalog_name, string(), stage_name_);
	// KEEP_NULLS: a NULL is a value of the key, not a request for a default.
	insert_bulk_sql_ = mssql::BuildInsertBulkSql(bcp_target_, bcp_columns_, false, target_.flush_rows,
												 mssql::InsertBulkHints::StatementSemantics());

	session_params_.pool = &catalog_.GetConnectionPool();
	session_params_.pool_handle = catalog_.GetConnectionPoolHandle();
	session_params_.insert_bulk_sql = &insert_bulk_sql_;
	session_params_.target = &bcp_target_;
	session_params_.columns = &bcp_columns_;
	session_params_.column_mapping = &column_mapping_;
	session_params_.flush_rows = target_.flush_rows;
	session_params_.reset_on_release = ConnectionProvider::ShouldResetOnRelease(context);
	// The statement's server transaction already brackets the fill.
	session_params_.own_transaction = false;
	// The connection is the statement's (MSSQLStatementConnection), not the
	// session's: adopted as "pinned", the session never releases it -- Finish /
	// the error paths drop the reference, closing it on an error -- and the
	// statement's Commit / Fail decide what happens to it.
	session_.Adopt(connection_, session_params_, true);
}

void MSSQLStagedDml::Execute(ClientContext &context, DataChunk &chunk) {
	if (finalized_) {
		throw InternalException("MSSQLStagedDml::Execute called after Finalize");
	}
	if (chunk.size() == 0) {
		return;
	}
	if (!connection_) {
		Start(context, chunk);
	}
	auto pinned_lock = stmt_conn_.LockPinned(context, catalog_);
	try {
		session_.Write(chunk);
	} catch (std::exception &ex) {
		ErrorData error(ex);
		FailAndThrow(context, StringUtil::Format("MSSQL %s on '%s.%s': filling the stage failed after %llu rows: %s",
												 target_.kind == MSSQLStagedDmlKind::UPDATE ? "UPDATE" : "DELETE",
												 target_.schema_name, target_.table_name,
												 (unsigned long long)rows_staged_, error.RawMessage()));
	}
	rows_staged_ += chunk.size();
}

idx_t MSSQLStagedDml::Finalize(ClientContext &context) {
	if (finalized_) {
		return 0;
	}
	finalized_ = true;
	if (!connection_) {
		// Nothing selected: no connection was taken, nothing to send.
		return 0;
	}
	const char *verb = target_.kind == MSSQLStagedDmlKind::UPDATE ? "UPDATE" : "DELETE";
	idx_t rows = 0;
	{
		auto pinned_lock = stmt_conn_.LockPinned(context, catalog_);
		try {
			session_.CloseStream();
		} catch (std::exception &ex) {
			ErrorData error(ex);
			FailAndThrow(context, StringUtil::Format("MSSQL %s on '%s.%s': filling the stage failed: %s", verb,
													 target_.schema_name, target_.table_name, error.RawMessage()));
		}
		session_.Release();
		const auto sql = JoinStatementSql(target_, stage_name_);
		auto result = MSSQLSimpleQuery::Execute(*connection_, sql, QueryTimeoutMs(target_));
		if (!result.success) {
			FailAndThrow(context, StringUtil::Format("MSSQL %s on '%s.%s' failed: %s", verb, target_.schema_name,
													 target_.table_name, result.error_message));
		}
		rows = static_cast<idx_t>(result.rows_affected);
		auto dropped = MSSQLSimpleQuery::Execute(*connection_, "DROP TABLE " + mssql::QuoteIdentifier(stage_name_),
												 QueryTimeoutMs(target_));
		if (!dropped.success) {
			FailAndThrow(context, StringUtil::Format("MSSQL %s on '%s.%s': dropping the stage failed: %s", verb,
													 target_.schema_name, target_.table_name, dropped.error_message));
		}
	}
	connection_.reset();
	stmt_conn_.Commit(context, catalog_);
	return rows;
}

}  // namespace duckdb
