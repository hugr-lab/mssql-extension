//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// backup/backup_snapshots_function.cpp
//
// Spec 085 task 24b (PR 4c-2, DQ-48): mssql_backup_snapshots -- one row per version of a backup history:
// an attached backup's (its catalog's history), or the files' (each one's header read, nothing else).
//===----------------------------------------------------------------------===//

#include "backup/backup_snapshots_function.hpp"

#include "backup/backup_function_common.hpp"
#include "backup/backup_opened.hpp"
#include "catalog/backup/mssql_backup_catalog.hpp"
#include "mssql_function_docs.hpp"

#include "duckdb/common/types/date.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/database_manager.hpp"

namespace duckdb {
namespace mssql {
namespace backup {

namespace {

constexpr const char *FUNCTION = "mssql_backup_snapshots";

struct Column {
	const char *name;
	LogicalType type;
};

std::vector<Column> Columns() {
	return {{"snapshot_id", LogicalType::BIGINT},
			{"snapshot_time", LogicalType::TIMESTAMP},
			{"backup_type", LogicalType::VARCHAR},
			{"is_copy_only", LogicalType::BOOLEAN},
			{"backup_start_date", LogicalType::TIMESTAMP},
			{"checkpoint_lsn", LogicalType::DECIMAL(25, 0)},
			{"backup_set_guid", LogicalType::UUID},
			{"differential_base_guid", LogicalType::UUID},
			{"recovery_fork_id", LogicalType::UUID},
			{"history_break", LogicalType::BOOLEAN},
			{"is_default", LogicalType::BOOLEAN},
			{"database_name", LogicalType::VARCHAR},
			{"full_files", LogicalType::LIST(LogicalType::VARCHAR)},
			{"diff_files", LogicalType::LIST(LogicalType::VARCHAR)}};
}

Value Timestamp(const MtfDate &d) {
	if (d.IsZero() || !Date::IsValid(d.year, d.month, d.day) || d.hour > 23 || d.minute > 59 || d.second > 59) {
		return Value(LogicalType::TIMESTAMP);
	}
	return Value::TIMESTAMP(d.year, d.month, d.day, d.hour, d.minute, d.second, 0);
}

Value Uuid(const Guid &guid) {
	return guid.IsZero() ? Value(LogicalType::UUID) : Value::UUID(guid.ToString());
}

Value Files(const std::vector<std::string> &paths) {
	vector<Value> values;
	for (const auto &p : paths) {
		values.emplace_back(p);
	}
	return Value::LIST(LogicalType::VARCHAR, std::move(values));
}

struct SnapshotsBindData : public TableFunctionData {
	vector<vector<Value>> rows;

	unique_ptr<FunctionData> Copy() const override {
		auto copy = make_uniq<SnapshotsBindData>();
		copy->rows = rows;
		return std::move(copy);
	}
	bool Equals(const FunctionData &other) const override {
		return rows == other.Cast<SnapshotsBindData>().rows;
	}
	//! Every EXECUTE binds again: an ATTACH OR REPLACE, or a file added, changes the versions.
	bool SupportStatementCache() const override {
		return false;
	}
};

struct SnapshotsState : public GlobalTableFunctionState {
	idx_t next = 0;
};

//! The history of the catalog attached as `name` (TYPE mssql_backup), or none.
std::shared_ptr<const BackupHistory> AttachedHistory(ClientContext &context, const std::string &name,
													 uint64_t &default_version) {
	auto db = DatabaseManager::Get(context).GetDatabase(Identifier(name));
	if (!db || db->GetCatalog().GetCatalogType() != MSSQLBackupCatalog::TYPE) {
		return nullptr;
	}
	const auto versions = db->GetCatalog().Cast<MSSQLBackupCatalog>().CurrentVersions();
	default_version = versions->default_version;
	return versions->history;
}

unique_ptr<FunctionData> Bind(ClientContext &context, TableFunctionBindInput &input, vector<LogicalType> &return_types,
							  vector<Identifier> &names) {
	auto data = make_uniq<SnapshotsBindData>();
	auto paths = PathsOf(input.inputs[0], FUNCTION);
	for (const auto &column : Columns()) {
		names.emplace_back(column.name);
		return_types.push_back(column.type);
	}
	uint64_t default_version = 0;
	std::shared_ptr<const BackupHistory> history;
	if (paths.size() == 1) {
		history = AttachedHistory(context, paths.front(), default_version);
	}
	if (!history) {
		try {
			auto read = std::make_shared<BackupHistory>(ReadBackupHistory(context, paths));
			default_version = read->versions.back().number;
			history = std::move(read);
		} catch (FormatError &e) {
			throw InvalidInputException("%s: %s", FUNCTION, e.what());
		}
	}
	for (const auto &v : history->versions) {
		const auto &full = history->backups[v.full];
		const auto &last = history->Last(v).set;
		vector<Value> row;
		row.push_back(Value::BIGINT(static_cast<int64_t>(v.number)));
		row.push_back(Timestamp(last.backup_finish_date));
		row.push_back(Value(v.IsDifferential() ? "differential" : "full"));
		row.push_back(Value::BOOLEAN(last.is_copy_only));
		row.push_back(Timestamp(last.backup_start_date));
		row.push_back(last.checkpoint_lsn.IsZero() ? Value(LogicalType::DECIMAL(25, 0))
												   : LsnValue(last.checkpoint_lsn));
		row.push_back(Uuid(last.backup_set_guid));
		row.push_back(v.IsDifferential() ? Uuid(last.differential_base_guid) : Value(LogicalType::UUID));
		row.push_back(Uuid(last.recovery_fork_id));
		row.push_back(Value::BOOLEAN(v.history_break));
		row.push_back(Value::BOOLEAN(v.number == default_version));
		row.push_back(last.database_name.empty() ? Value(LogicalType::VARCHAR) : Value(last.database_name));
		row.push_back(Files(full.paths));
		row.push_back(v.IsDifferential() ? Files(history->backups[v.diff].paths)
										 : Value(LogicalType::LIST(LogicalType::VARCHAR)));
		data->rows.push_back(std::move(row));
	}
	return std::move(data);
}

unique_ptr<GlobalTableFunctionState> Init(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<SnapshotsState>();
}

void Execute(ClientContext &, TableFunctionInput &input, DataChunk &output) {
	auto &state = input.global_state->Cast<SnapshotsState>();
	const auto &rows = input.bind_data->Cast<SnapshotsBindData>().rows;
	idx_t count = 0;
	while (state.next < rows.size() && count < STANDARD_VECTOR_SIZE) {
		const auto &row = rows[state.next++];
		for (idx_t c = 0; c < row.size(); c++) {
			output.data[c].SetValue(count, row[c]);
		}
		count++;
	}
	output.SetChildCardinality(count);
}

// mssql_backup_refresh(name): the attached history read again, at execution (not at bind: an EXPLAIN or a
// PREPARE refreshes nothing).
struct RefreshBindData : public TableFunctionData {
	std::string name;
};

struct RefreshState : public GlobalTableFunctionState {
	bool done = false;
};

unique_ptr<FunctionData> RefreshBind(ClientContext &, TableFunctionBindInput &input, vector<LogicalType> &return_types,
									 vector<Identifier> &names) {
	if (input.inputs[0].IsNull()) {
		throw InvalidInputException("mssql_backup_refresh: the name is NULL");
	}
	auto data = make_uniq<RefreshBindData>();
	data->name = input.inputs[0].ToString();
	names = {Identifier("versions_before"), Identifier("versions")};
	return_types = {LogicalType::BIGINT, LogicalType::BIGINT};
	return std::move(data);
}

unique_ptr<GlobalTableFunctionState> RefreshInit(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<RefreshState>();
}

void RefreshExecute(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	auto &state = input.global_state->Cast<RefreshState>();
	if (state.done) {
		output.SetChildCardinality(0);
		return;
	}
	state.done = true;
	const auto &name = input.bind_data->Cast<RefreshBindData>().name;
	auto db = DatabaseManager::Get(context).GetDatabase(Identifier(name));
	if (!db || db->GetCatalog().GetCatalogType() != MSSQLBackupCatalog::TYPE) {
		throw InvalidInputException(
			"mssql_backup_refresh: %s is not a backup attached as a catalog "
			"(ATTACH … (TYPE mssql_backup, history true))",
			name);
	}
	const auto counts = db->GetCatalog().Cast<MSSQLBackupCatalog>().Refresh(context);
	output.data[0].SetValue(0, Value::BIGINT(static_cast<int64_t>(counts.first)));
	output.data[1].SetValue(0, Value::BIGINT(static_cast<int64_t>(counts.second)));
	output.SetChildCardinality(1);
}

}  // namespace

void RegisterBackupSnapshotsFunction(ExtensionLoader &loader) {
	{
		TableFunctionSet refresh("mssql_backup_refresh");
		refresh.AddFunction(
			TableFunction("mssql_backup_refresh", {LogicalType::VARCHAR}, RefreshExecute, RefreshBind, RefreshInit));
		mssql::RegisterDocumentedFunction(
			loader, std::move(refresh),
			{{},
			 "Reads again the files of a backup history attached with ATTACH … (TYPE mssql_backup, history true): "
			 "the glob expanded again, the headers of new or changed files read (the others kept), and the "
			 "versions renumbered; a version already read keeps its catalog. Returns the number of versions "
			 "before and after.",
			 {"CALL mssql_backup_refresh('bk')"},
			 {"backup"}});
	}
	TableFunctionSet set(FUNCTION);
	const vector<LogicalType> arguments{LogicalType(LogicalType::VARCHAR), LogicalType::LIST(LogicalType::VARCHAR)};
	for (const auto &argument : arguments) {
		set.AddFunction(TableFunction(FUNCTION, {argument}, Execute, Bind, Init));
	}
	mssql::RegisterDocumentedFunction(
		loader, std::move(set),
		{{},
		 "Lists the versions of a history of SQL Server backups, as ducklake_snapshots lists a lake's: each full "
		 "backup, and each differential over a full among the files, a state of the database at its finish, "
		 "numbered in that order. Takes the name of a backup attached with ATTACH … (TYPE mssql_backup) -- whose "
		 "AT (VERSION => n) / AT (TIMESTAMP => t) read these versions -- or the files (a glob or a list), each "
		 "read for its header only. Log backups are left out.",
		 {"SELECT snapshot_id, snapshot_time, backup_type FROM mssql_backup_snapshots('bk')",
		  "SELECT * FROM mssql_backup_snapshots('s3://backups/sales/*.bak')"},
		 {"backup"}});
}

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb
