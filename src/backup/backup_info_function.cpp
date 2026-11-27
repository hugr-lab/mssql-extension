// Spec 085 task 9: mssql_backup_info(path) -- one row per backup set on a media set: RESTORE
// HEADERONLY + LABELONLY, with FILELISTONLY as a nested list (spec 085 § Functions). [DuckDB]
//
// PR 1 reads the headers only (stage 1): the columns whose source is a later stage are NULL until it
// lands -- page_count (the allocation pages, PR 2) and each file's backup_size_bytes (the page index).
// consistent / open_transactions / committed_after_checkpoint / rolled_back_after_checkpoint are D11's database level
// (task 27): the log tail read (stage 5, kept by the opened-file cache); NULL for a log backup, whose records are not
// a tail. The headers are read at bind, the tail at init and only when one of those four is selected (review of
// #591: a DESCRIBE, or a header question, read the whole tail -- over s3://, downloaded it). The database's
// version, level and collation come from the trailer SCIN.

#include "backup/backup_info_function.hpp"

#include "backup/backup_bytes.hpp"
#include "backup/backup_collation.hpp"
#include "backup/backup_function_common.hpp"
#include "backup/backup_key_secret.hpp"
#include "backup/backup_opened.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/logging/logger.hpp"
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
	fields.emplace_back("is_present", LogicalType::BOOLEAN);
	fields.emplace_back("tde_thumbprint", LogicalType::BLOB);  // FILELISTONLY's TDEThumbprint (spec 087)
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
			{"key_algorithm", LogicalType::VARCHAR},
			{"tde", LogicalType::BOOLEAN},
			{"tde_thumbprint", LogicalType::BLOB},
			{"has_backup_checksums", LogicalType::BOOLEAN},
			{"backup_size", LogicalType::BIGINT},
			{"compressed_size", LogicalType::BIGINT},
			{"page_count", LogicalType::BIGINT},
			{"log_tail_bytes", LogicalType::BIGINT},
			{"consistent", LogicalType::BOOLEAN},
			{"open_transactions", LogicalType::INTEGER},
			{"committed_after_checkpoint", LogicalType::INTEGER},
			{"rolled_back_after_checkpoint", LogicalType::INTEGER},
			{"recovery_model", LogicalType::VARCHAR},
			{"files", LogicalType::LIST(FileType())}};
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

Value FileValue(const DatabaseFile &f, bool present) {
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
	fields.emplace_back("is_present", Value::BOOLEAN(present));
	fields.emplace_back("tde_thumbprint", f.tde ? Value::BLOB(f.tde_thumbprint.data(), f.tde_thumbprint.size())
												: Value(LogicalType::BLOB));
	return Value::STRUCT(std::move(fields));
}

//! The place of `name` among Columns().
idx_t ColumnIndex(const char *name) {
	const auto columns = Columns();
	for (idx_t i = 0; i < columns.size(); i++) {
		if (std::string(columns[i].name) == name) {
			return i;
		}
	}
	throw InternalException("mssql_backup_info: no column %s", name);
}

//! D11's four columns (consistent, open_transactions, committed_after_checkpoint,
//! rolled_back_after_checkpoint), in Columns()'s order from the first: what the tail is read for.
constexpr idx_t D11_COLUMNS = 4;

//! D11's columns from `tail`, at their places in `row`; NULL with none.
void PutTail(std::vector<Value> &row, const FuzzySummary *tail) {
	const idx_t at = ColumnIndex("consistent");
	const Value no_int(LogicalType::INTEGER);
	row[at] = tail ? Value::BOOLEAN(tail->Consistent()) : Value(LogicalType::BOOLEAN);
	row[at + 1] = tail ? Value::INTEGER(static_cast<int32_t>(tail->open_transactions)) : no_int;
	row[at + 2] = tail ? Value::INTEGER(static_cast<int32_t>(tail->committed_after_checkpoint)) : no_int;
	row[at + 3] = tail ? Value::INTEGER(static_cast<int32_t>(tail->rolled_back_after_checkpoint)) : no_int;
}

//! HEADERONLY's CompressionAlgorithm: NULL for a backup with no container, and for one in a container whose blocks
//! are stored (an encrypted backup WITHOUT COMPRESSION: RESTORE says Compressed 1, CompressionAlgorithm NULL; measured
//! on 2025, spec 087's matrix).
static Value AlgorithmValue(bool compressed, CompressionAlgorithm algorithm) {
	if (!compressed || algorithm == CompressionAlgorithm::None) {
		return Value(LogicalType::VARCHAR);
	}
	return Value(CompressionAlgorithmName(algorithm));
}

//! The row of the backup set the files of `opened` hold, D11's columns NULL (PutTail).
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
			// FILELISTONLY's IsPresent: the trailer's flag (the start's list may predate it).
			bool present = f.present;
			for (const auto &t : set.files) {
				if (t.file_id == f.file_id && t.type == f.type) {
					present = t.present;
				}
			}
			// FILELISTONLY's TDEThumbprint: the trailer's (the start's list may predate a rotation).
			DatabaseFile shown = f;
			for (const auto &t : set.files) {
				if (t.file_id == f.file_id && t.type == f.type) {
					shown.tde = t.tde;
					shown.tde_thumbprint = t.tde_thumbprint;
				}
			}
			files.push_back(FileValue(shown, present));
		}
	}
	const bool single = opened.families.size() == 1;
	const Value no_int(LogicalType::INTEGER);
	std::vector<Value> row;
	row.push_back(Value::INTEGER(set.position));
	row.push_back(Value(BackupTypeName(set)));
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
	row.push_back(AlgorithmValue(opened.Compressed(), first.container_header.algorithm));
	// Spec 087: an encrypted backup (its key unwrapped by the secret D3 selected) and its encryptor's
	// thumbprint, as HEADERONLY's EncryptorThumbprint.
	const bool encrypted = first.container && first.container_header.encrypted;
	row.push_back(Value::BOOLEAN(encrypted));
	row.push_back(encrypted ? Value::BLOB(first.container_header.encryptor_thumbprint.data(),
										  first.container_header.encryptor_thumbprint.size())
							: Value(LogicalType::BLOB));
	// The backup key's algorithm, from the key D3's secret unwrapped (HEADERONLY's KeyAlgorithm: aes_128 …).
	row.push_back(encrypted && first.keys.backup ? Value(KeyAlgorithmName(first.keys.backup->algorithm))
												 : Value(LogicalType::VARCHAR));
	// A TDE database's backup, and the certificate that wraps its DEK (the container's header, or the data
	// files' SFIN: D10).
	Thumbprint dek_certificate;
	const bool tde = opened.Tde(&dek_certificate);
	row.push_back(Value::BOOLEAN(tde));
	row.push_back(tde ? Value::BLOB(dek_certificate.data(), dek_certificate.size()) : Value(LogicalType::BLOB));
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
	for (idx_t i = 0; i < D11_COLUMNS; i++) {
		row.push_back(Value());	 // D11's: NULL of its type below, the tail's at init (PutTail)
	}
	row.push_back(Value(RecoveryModelName(set.recovery_model)));
	row.push_back(Value::LIST(FileType(), std::move(files)));
	PutTail(row, nullptr);
	return row;
}

//! Spec 087 D10 (owner, 2026-11-27): the row of a backup whose key no secret holds -- what its first file's
//! container header says in clear (compressed, the algorithm, encrypted, the encryptor's thumbprint, TDE and
//! the DEK's certificate), every column the decrypted stream holds NULL. `position` is the set asked for.
std::vector<Value> ClearRow(const ClearFileHeader &file) {
	const auto columns = Columns();
	std::vector<Value> row;
	for (const auto &c : columns) {
		row.push_back(Value(c.type));
	}
	const auto &h = file.header;
	row[ColumnIndex("position")] = Value::INTEGER(1);
	// HEADERONLY's Compressed is the container's (1 for an encrypted backup stored in one, as with the key:
	// OpenedBackup::Compressed), the algorithm its blocks'.
	row[ColumnIndex("compressed")] = Value::BOOLEAN(file.container);
	row[ColumnIndex("compression_algorithm")] = AlgorithmValue(file.container, h.algorithm);
	row[ColumnIndex("encrypted")] = Value::BOOLEAN(h.encrypted);
	if (h.encrypted) {
		row[ColumnIndex("encryptor_thumbprint")] =
			Value::BLOB(h.encryptor_thumbprint.data(), h.encryptor_thumbprint.size());
	}
	row[ColumnIndex("tde")] = Value::BOOLEAN(h.tde);
	if (h.tde) {
		row[ColumnIndex("tde_thumbprint")] = Value::BLOB(h.tde_thumbprint.data(), h.tde_thumbprint.size());
	}
	return row;
}

//! D10: the certificates (thumbprints, in hex) whose key `files` need and no secret holds; empty when every
//! one is held or none is needed (a plain file, an unencrypted container, an uncompressed TDE backup).
std::vector<std::string> MissingKeys(ClientContext &context, const std::vector<ClearFileHeader> &files) {
	std::vector<std::string> missing;
	for (const auto &f : files) {
		if (!f.container) {
			continue;
		}
		const auto &h = f.header;
		if (h.encrypted && !HasBackupKeySecret(context, f.path, h.encryptor_thumbprint)) {
			missing.push_back(ThumbprintHex(h.encryptor_thumbprint) + " (the backup's key, for \"" + f.path + "\")");
		}
		if (h.tde && !HasBackupKeySecret(context, f.path, h.tde_thumbprint)) {
			missing.push_back(ThumbprintHex(h.tde_thumbprint) + " (the TDE database's DEK, for \"" + f.path + "\")");
		}
	}
	return missing;
}

struct BackupInfoBindData : public TableFunctionData {
	vector<std::string> paths;
	BackupAt at;  // task 24b
	std::vector<Value> row;
	bool reread = false;
	//! A data backup's: its tail holds D11's columns. Its set's GUID, to tell its files from others at init.
	bool has_tail = false;
	std::string set_guid;

	unique_ptr<FunctionData> Copy() const override {
		auto copy = make_uniq<BackupInfoBindData>();
		copy->paths = paths;
		copy->at = at;
		copy->row = row;
		copy->reread = reread;
		copy->has_tail = has_tail;
		copy->set_guid = set_guid;
		return std::move(copy);
	}
	bool Equals(const FunctionData &other_p) const override {
		const auto &other = other_p.Cast<BackupInfoBindData>();
		return paths == other.paths && at.Same(other.at);
	}
	//! The row is read at bind, and a prepared statement keeps it: a backup is opened to be read, and
	//! a file rewritten under an open backup is invalid anyway while the rewrite runs. `reread := true`
	//! binds again on every EXECUTE (a device overwritten nightly, read by a statement prepared once). An
	//! attached name's version is pinned again at every EXECUTE too, so it follows a refresh (review of #581).
	bool SupportStatementCache() const override {
		return !reread && at.key.empty();
	}
};

struct BackupInfoState : public GlobalTableFunctionState {
	std::vector<Value> row;
	std::vector<column_t> column_ids;  // the columns asked for (projection pushdown)
	bool done = false;
};

unique_ptr<FunctionData> Bind(ClientContext &context, TableFunctionBindInput &input, vector<LogicalType> &return_types,
							  vector<Identifier> &names) {
	auto data = make_uniq<BackupInfoBindData>();
	data->paths = PathsOf(input.inputs[0], "mssql_backup_info");
	data->at = BackupAtOf(input.named_parameters, "mssql_backup_info");
	if (data->paths.size() == 1) {
		// Over an attached backup, the version pinned now by its files (task 24b, owner 2026-11-19).
		PinAttachedVersion(context, data->paths.front(), data->at);
	}
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
		// D10: a backup whose key no secret holds is described by what it says in clear, with a WARNING; an
		// attached name and a version are opened as before (their keys were selected when they were read).
		if (!data->at.IsSet() &&
			!(data->paths.size() == 1 && FindAttachedBackup(context, data->paths.front(), data->at))) {
			const auto clear = ReadClearHeaders(context, data->paths);
			const auto missing = MissingKeys(context, clear);
			if (!missing.empty()) {
				std::string list;
				for (const auto &m : missing) {
					list += (list.empty() ? "" : ", ") + m;
				}
				DUCKDB_LOG_WARNING(
					context,
					"mssql_backup_info: \"%s\": no mssql_backup_key secret whose scope matches holds the "
					"key of the certificate %s: the columns its header gives in clear are shown, every "
					"other one NULL; CREATE SECRET (TYPE mssql_backup_key, CERTIFICATE ..., PRIVATE_KEY "
					"..., PASSWORD ..., SCOPE ...) with it reads the rest",
					clear.front().path, list);
				data->row = ClearRow(clear.front());
				return std::move(data);
			}
		}
		// The headers are what the opened-file cache holds first (D9): a later call on the same files
		// takes them, and so does a catalog call after this one.
		auto opened = OpenBackup(context, std::vector<std::string>(data->paths.begin(), data->paths.end()),
								 std::vector<std::string>(), data->at);
		// D11's database level from the log tail (stage 5): a data backup's only, read at init.
		data->has_tail = opened.backup->Headers().Set().kind != BackupKind::Log;
		data->set_guid = opened.backup->Headers().Set().backup_set_guid.ToString();
		data->row = InfoRow(opened.backup->Headers());
	} catch (FormatError &e) {
		throw InvalidInputException("mssql_backup_info: %s", e.what());
	}
	return std::move(data);
}

unique_ptr<GlobalTableFunctionState> Init(ClientContext &context, TableFunctionInitInput &input) {
	const auto &data = input.bind_data->Cast<BackupInfoBindData>();
	auto state = make_uniq<BackupInfoState>();
	state->row = data.row;
	state->column_ids = input.column_ids;
	const idx_t first = ColumnIndex("consistent");
	bool asked = false;
	for (auto id : state->column_ids) {
		asked = asked || (id >= first && id < first + D11_COLUMNS);
	}
	if (!asked || !data.has_tail) {
		return std::move(state);
	}
	try {
		// The files opened again for the statement (the opened-file cache's entry, when it holds them, with
		// its tail): the bind's row stands for them only while they hold the same backup set.
		auto opened = OpenBackup(context, std::vector<std::string>(data.paths.begin(), data.paths.end()),
								 std::vector<std::string>(), data.at);
		if (opened.backup->Headers().Set().backup_set_guid.ToString() != data.set_guid) {
			throw InvalidInputException(
				"mssql_backup_info: the files hold another backup set (%s) than when the statement was bound (%s): "
				"rewritten since; reread := true binds again on every EXECUTE",
				opened.backup->Headers().Set().backup_set_guid.ToString(), data.set_guid);
		}
		BindFiles bound(opened.binding);
		PutTail(state->row, TailOrWarn(context, opened, "mssql_backup_info"));
	} catch (FormatError &e) {
		throw InvalidInputException("mssql_backup_info: %s", e.what());
	}
	return std::move(state);
}

void Execute(ClientContext &, TableFunctionInput &input, DataChunk &output) {
	auto &state = input.global_state->Cast<BackupInfoState>();
	if (state.done) {
		output.SetChildCardinality(0);
		return;
	}
	for (idx_t c = 0; c < state.column_ids.size(); c++) {
		const column_t id = state.column_ids[c];
		if (id < state.row.size()) {
			output.data[c].SetValue(0, state.row[id]);
		}
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
		f.projection_pushdown = true;  // D11's columns cost the log tail's read (Init)
		f.GetSignature().AddKeywordOnly("position", LogicalType::INTEGER, Value::INTEGER(1));
		f.GetSignature().AddKeywordOnly("reread", LogicalType::BOOLEAN, Value::BOOLEAN(false));
		AddVersionParameters(f);
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
