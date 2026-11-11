// Spec 085 task 9: mssql_backup_info(path) -- one row per backup set on a media set: RESTORE
// HEADERONLY + LABELONLY, with FILELISTONLY as a nested list (spec 085 § Functions). [DuckDB]
//
// PR 1 reads the headers only (stage 1): the columns whose source is a later stage are NULL until it
// lands -- page_count (the allocation pages, PR 2), consistent / open_transactions /
// committed_after_checkpoint (D11's log tail, PR 5), and each file's backup_size_bytes (the page
// index). The database's version, level and collation come from the trailer SCIN.

#include "backup/backup_info_function.hpp"

#include "backup/backup_bytes.hpp"
#include "backup/backup_collation.hpp"
#include "backup/backup_function_common.hpp"
#include "backup/backup_opened.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/client_context.hpp"
#include "mssql_function_docs.hpp"

namespace duckdb {
namespace mssql {
namespace backup {

namespace {

struct Column {
	const char *name;
	LogicalType type;
};

LogicalType FileType() {
	child_list_t<LogicalType> fields;
	fields.emplace_back("logical_name", LogicalType::VARCHAR);
	fields.emplace_back("physical_name", LogicalType::VARCHAR);
	fields.emplace_back("type", LogicalType::VARCHAR);
	fields.emplace_back("file_id", LogicalType::INTEGER);
	fields.emplace_back("filegroup_id", LogicalType::INTEGER);
	fields.emplace_back("filegroup_name", LogicalType::VARCHAR);
	fields.emplace_back("size_bytes", LogicalType::BIGINT);
	fields.emplace_back("max_size_bytes", LogicalType::BIGINT);
	fields.emplace_back("create_lsn", LogicalType::DECIMAL(25, 0));
	fields.emplace_back("unique_id", LogicalType::UUID);
	fields.emplace_back("backup_size_bytes", LogicalType::BIGINT);
	return LogicalType::STRUCT(std::move(fields));
}

std::vector<Column> Columns() {
	const auto LSN = LogicalType::DECIMAL(25, 0);
	return {{"position", LogicalType::INTEGER},
			{"backup_type", LogicalType::VARCHAR},
			{"is_copy_only", LogicalType::BOOLEAN},
			{"backup_set_guid", LogicalType::UUID},
			{"first_lsn", LSN},
			{"last_lsn", LSN},
			{"checkpoint_lsn", LSN},
			{"database_backup_lsn", LSN},
			{"differential_base_lsn", LSN},
			{"differential_base_guid", LogicalType::UUID},
			{"database_name", LogicalType::VARCHAR},
			{"server_name", LogicalType::VARCHAR},
			{"database_version", LogicalType::INTEGER},
			{"compatibility_level", LogicalType::INTEGER},
			{"collation_name", LogicalType::VARCHAR},
			{"family_guid", LogicalType::UUID},
			{"recovery_fork_id", LogicalType::UUID},
			{"first_recovery_fork_id", LogicalType::UUID},
			{"fork_point_lsn", LSN},
			{"binding_id", LogicalType::UUID},
			{"backup_name", LogicalType::VARCHAR},
			{"description", LogicalType::VARCHAR},
			{"user_name", LogicalType::VARCHAR},
			{"machine_name", LogicalType::VARCHAR},
			{"software_version", LogicalType::VARCHAR},
			{"database_creation_date", LogicalType::TIMESTAMP},
			{"backup_start_date", LogicalType::TIMESTAMP},
			{"backup_finish_date", LogicalType::TIMESTAMP},
			{"media_set_guid", LogicalType::UUID},
			{"family_count", LogicalType::INTEGER},
			{"family_sequence_number", LogicalType::INTEGER},
			{"compressed", LogicalType::BOOLEAN},
			{"compression_algorithm", LogicalType::VARCHAR},
			{"encrypted", LogicalType::BOOLEAN},
			{"encryptor_thumbprint", LogicalType::BLOB},
			{"has_backup_checksums", LogicalType::BOOLEAN},
			{"backup_size", LogicalType::BIGINT},
			{"compressed_size", LogicalType::BIGINT},
			{"page_count", LogicalType::BIGINT},
			{"log_tail_bytes", LogicalType::BIGINT},
			{"consistent", LogicalType::BOOLEAN},
			{"open_transactions", LogicalType::INTEGER},
			{"committed_after_checkpoint", LogicalType::INTEGER},
			{"recovery_model", LogicalType::VARCHAR},
			{"files", LogicalType::LIST(FileType())}};
}

Value LsnValue(const Lsn &lsn) {
	// HEADERONLY's form: vlf, then block in 10 digits, then slot in 5 (< 10^25).
	const hugeint_t value =
		hugeint_t(static_cast<int64_t>(lsn.vlf)) * hugeint_t(static_cast<int64_t>(1000000000000000LL)) +
		hugeint_t(static_cast<int64_t>(lsn.block)) * hugeint_t(static_cast<int64_t>(100000)) +
		hugeint_t(static_cast<int64_t>(lsn.slot));
	return Value::DECIMAL(value, 25, 0);
}

Value OptionalLsn(const Lsn &lsn) {
	return lsn.IsZero() ? Value(LogicalType::DECIMAL(25, 0)) : LsnValue(lsn);
}

Value GuidValue(const Guid &guid) {
	return guid.IsZero() ? Value(LogicalType::UUID) : Value::UUID(guid.ToString());
}

Value DateValue(const MtfDate &d, const char *what) {
	if (d.IsZero()) {
		return Value(LogicalType::TIMESTAMP);
	}
	if (!Date::IsValid(d.year, d.month, d.day) || d.hour > 23 || d.minute > 59 || d.second > 59) {
		throw FormatError(std::string("the backup set's ") + what + " is not a date: " + d.ToString());
	}
	return Value::TIMESTAMP(d.year, d.month, d.day, d.hour, d.minute, d.second, 0);
}

//! The collation the trailer's TDS form names (backup_collation), NULL when none or ambiguous.
Value CollationValue(uint32_t info, uint8_t sort_id) {
	const std::string name = CollationNameOfTds(info, sort_id);
	return name.empty() ? Value(LogicalType::VARCHAR) : Value(name);
}

Value Text(const std::string &s) {
	return s.empty() ? Value(LogicalType::VARCHAR) : Value(s);
}

Value FileValue(const DatabaseFile &f) {
	child_list_t<Value> fields;
	fields.emplace_back("logical_name", Value(f.logical_name));
	fields.emplace_back("physical_name", Value(f.physical_name));
	fields.emplace_back("type", Value(f.type == 'L' ? "log" : "data"));
	fields.emplace_back("file_id", Value::INTEGER(static_cast<int32_t>(f.file_id)));
	fields.emplace_back("filegroup_id", Value::INTEGER(static_cast<int32_t>(f.filegroup_id)));
	fields.emplace_back("filegroup_name", Text(f.filegroup_name));
	fields.emplace_back("size_bytes", Value::BIGINT(static_cast<int64_t>(f.size_bytes)));
	fields.emplace_back("max_size_bytes", Value::BIGINT(static_cast<int64_t>(f.max_size_bytes)));
	fields.emplace_back("create_lsn", LsnValue(f.create_lsn));
	fields.emplace_back("unique_id", GuidValue(f.unique_id));
	fields.emplace_back("backup_size_bytes", Value(LogicalType::BIGINT));
	return Value::STRUCT(std::move(fields));
}

//! The row of the backup set the files of `opened` hold.
std::vector<Value> InfoRow(const OpenedHeaders &opened) {
	const auto &set = opened.Set();
	const auto &first = *opened.families.front();
	uint64_t backup_size = 0;
	uint64_t log_tail = 0;
	bool checksums = false;
	for (const auto &f : opened.families) {
		backup_size += f->set.backup_size;
		log_tail += f->set.log_tail_bytes;
		checksums = checksums || f->checksums;
	}
	// FILELISTONLY's list: the files as the backup began when the set records them (MSCI), else the
	// trailer's, a file marked removed left out as FILELISTONLY leaves it (files fixture, 2022).
	std::vector<Value> files;
	for (const auto &f : set.start_files.empty() ? set.files : set.start_files) {
		if (!f.marked_removed) {
			files.push_back(FileValue(f));
		}
	}
	const bool single = opened.families.size() == 1;
	const Value no_int(LogicalType::INTEGER);
	std::vector<Value> row;
	row.push_back(Value::INTEGER(set.position));
	row.push_back(Value(BackupKindName(set.kind)));
	row.push_back(Value::BOOLEAN(set.is_copy_only));
	row.push_back(GuidValue(set.backup_set_guid));
	row.push_back(LsnValue(set.first_lsn));
	row.push_back(LsnValue(set.last_lsn));
	row.push_back(LsnValue(set.checkpoint_lsn));
	row.push_back(LsnValue(set.database_backup_lsn));
	const bool differential = set.kind == BackupKind::Differential;
	row.push_back(differential ? LsnValue(set.differential_base_lsn) : Value(LogicalType::DECIMAL(25, 0)));
	row.push_back(differential ? GuidValue(set.differential_base_guid) : Value(LogicalType::UUID));
	row.push_back(Text(set.database_name));
	row.push_back(Text(set.server_name));
	// The database's version, compatibility level and collation, from the trailer SCIN (the boot page
	// cross-checks them in task 11); 0 there is no value.
	row.push_back(set.database_version ? Value::INTEGER(set.database_version) : no_int);
	row.push_back(set.compatibility_level ? Value::INTEGER(set.compatibility_level) : no_int);
	row.push_back(CollationValue(set.collation_info, set.collation_sort_id));
	row.push_back(GuidValue(set.family_guid));
	row.push_back(GuidValue(set.recovery_fork_id));
	row.push_back(GuidValue(set.FirstRecoveryForkId()));
	row.push_back(set.HasForkPoint() ? LsnValue(set.fork_point_lsn) : Value(LogicalType::DECIMAL(25, 0)));
	row.push_back(GuidValue(set.binding_id));
	row.push_back(Text(set.backup_name));
	row.push_back(Text(set.description));
	row.push_back(Text(set.user_name));
	row.push_back(Text(set.machine_name));
	// The server's version as the trailer records it (17.0.4075), else the SSET's major.minor.
	row.push_back(set.server_version
					  ? Value(std::to_string(set.server_version >> 24) + "." +
							  std::to_string((set.server_version >> 16) & 0xFF) + "." +
							  std::to_string(set.server_version & 0xFFFF))
					  : Value(std::to_string(set.software_major) + "." + std::to_string(set.software_minor)));
	row.push_back(DateValue(set.database_creation_date, "database_creation_date"));
	row.push_back(DateValue(set.backup_start_date, "backup_start_date"));
	row.push_back(DateValue(set.backup_finish_date, "backup_finish_date"));
	// A compressed family's media fields come from its container's header (task 9).
	row.push_back(GuidValue(set.media_set_guid));
	row.push_back(Value::INTEGER(static_cast<int32_t>(set.family_count)));
	// One family's number when one family was given; a whole set has no one number.
	row.push_back(single ? Value::INTEGER(static_cast<int32_t>(set.family_sequence_number)) : no_int);
	row.push_back(Value::BOOLEAN(opened.Compressed()));
	row.push_back(opened.Compressed() ? Value(CompressionAlgorithmName(first.container_header.algorithm))
									  : Value(LogicalType::VARCHAR));
	row.push_back(Value::BOOLEAN(false));  // an encrypted backup is refused when opened (spec 087)
	row.push_back(Value(LogicalType::BLOB));
	row.push_back(Value::BOOLEAN(checksums));
	// The BackupSize the trailer records (checked against the layout's when the files were opened).
	row.push_back(
		Value::BIGINT(static_cast<int64_t>(set.recorded_backup_size ? set.recorded_backup_size : backup_size)));
	// HEADERONLY's CompressedBackupSize, as the trailer records it (SCIN +344): the set's value, alike in
	// every family (checked when the families were opened); NULL on a SCIN too short to hold it.
	row.push_back(set.has_compressed_backup_size ? Value::BIGINT(static_cast<int64_t>(set.compressed_backup_size))
												 : Value(LogicalType::BIGINT));
	row.push_back(Value(LogicalType::BIGINT));	// page_count: the allocation pages (PR 2)
	row.push_back(Value::BIGINT(static_cast<int64_t>(log_tail)));
	row.push_back(Value(LogicalType::BOOLEAN));	 // consistent: D11 (PR 5)
	row.push_back(no_int);						 // open_transactions: D11
	row.push_back(no_int);						 // committed_after_checkpoint: D11
	row.push_back(Value(RecoveryModelName(set.recovery_model)));
	row.push_back(Value::LIST(FileType(), std::move(files)));
	return row;
}

struct BackupInfoBindData : public TableFunctionData {
	vector<std::string> paths;
	std::vector<Value> row;
	bool reread = false;

	unique_ptr<FunctionData> Copy() const override {
		auto copy = make_uniq<BackupInfoBindData>();
		copy->paths = paths;
		copy->row = row;
		copy->reread = reread;
		return std::move(copy);
	}
	bool Equals(const FunctionData &other_p) const override {
		return paths == other_p.Cast<BackupInfoBindData>().paths;
	}
	//! The row is read at bind, and a prepared statement keeps it: a backup is opened to be read, and
	//! a file rewritten under an open backup is invalid anyway while the rewrite runs. `reread := true`
	//! binds again on every EXECUTE (a device overwritten nightly, read by a statement prepared once).
	bool SupportStatementCache() const override {
		return !reread;
	}
};

struct BackupInfoState : public GlobalTableFunctionState {
	bool done = false;
};

unique_ptr<FunctionData> Bind(ClientContext &context, TableFunctionBindInput &input, vector<LogicalType> &return_types,
							  vector<Identifier> &names) {
	auto data = make_uniq<BackupInfoBindData>();
	data->paths = PathsOf(input.inputs[0], "mssql_backup_info");
	for (const auto &kv : input.named_parameters) {
		if (kv.first == "reread") {
			data->reread = !kv.second.IsNull() && kv.second.GetValue<bool>();
		}
		if (kv.first == "position") {
			if (kv.second.IsNull()) {
				throw InvalidInputException("mssql_backup_info: position is NULL (the first backup set is 1)");
			}
			if (kv.second.GetValue<int32_t>() != 1) {
				throw InvalidInputException(
					"mssql_backup_info: position %s: reading a backup set other than the first (an appended set) is "
					"not supported yet",
					kv.second.ToString());
			}
		}
	}
	for (const auto &column : Columns()) {
		names.emplace_back(column.name);
		return_types.push_back(column.type);
	}
	try {
		auto opened = OpenBackupHeaders(context, std::vector<std::string>(data->paths.begin(), data->paths.end()));
		data->row = InfoRow(*opened);
	} catch (FormatError &e) {
		throw InvalidInputException("mssql_backup_info: %s", e.what());
	}
	return std::move(data);
}

unique_ptr<GlobalTableFunctionState> Init(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<BackupInfoState>();
}

void Execute(ClientContext &, TableFunctionInput &input, DataChunk &output) {
	auto &state = input.global_state->Cast<BackupInfoState>();
	if (state.done) {
		output.SetChildCardinality(0);
		return;
	}
	const auto &row = input.bind_data->Cast<BackupInfoBindData>().row;
	for (idx_t c = 0; c < row.size(); c++) {
		output.data[c].SetValue(0, row[c]);
	}
	output.SetChildCardinality(1);
	state.done = true;
}

}  // namespace

void RegisterBackupInfoFunction(ExtensionLoader &loader) {
	TableFunctionSet set("mssql_backup_info");
	const vector<LogicalType> arguments{LogicalType(LogicalType::VARCHAR), LogicalType::LIST(LogicalType::VARCHAR)};
	for (const auto &argument : arguments) {
		TableFunction f("mssql_backup_info", {argument}, Execute, Bind, Init);
		f.GetSignature().AddKeywordOnly("position", LogicalType::INTEGER, Value::INTEGER(1));
		f.GetSignature().AddKeywordOnly("reread", LogicalType::BOOLEAN, Value::BOOLEAN(false));
		set.AddFunction(f);
	}
	mssql::RegisterDocumentedFunction(
		loader, std::move(set),
		{{},
		 "Describes the backup set in a SQL Server backup file (.bak) -- what RESTORE HEADERONLY, LABELONLY and "
		 "FILELISTONLY report: the backup type, LSNs, dates, database and server, recovery fork, media set, "
		 "compression and the database files -- read from the file alone, through DuckDB's file system (local, "
		 "s3://, https://, ...). The path is one file, a glob, or a list: every family (stripe) of one media set.",
		 {"SELECT * FROM mssql_backup_info('/backups/sales_full.bak')",
		  "SELECT backup_type, first_lsn, last_lsn FROM mssql_backup_info(['/backups/s1.bak', '/backups/s2.bak'])"},
		 {"backup"}});
}

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb
