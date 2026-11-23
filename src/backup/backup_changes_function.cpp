//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// backup/backup_changes_function.cpp
//
// Spec 085 task 24c (PR 4c-3, DQ-47): mssql_backup_schema_changes(source [, start_version :=,
// end_version :=, diff :=]) -- the schema changes from one state of a history to another, as an importer
// applies them before the rows.
//===----------------------------------------------------------------------===//

#include "backup/backup_changes_function.hpp"

#include "backup/backup_function_common.hpp"
#include "backup/backup_opened.hpp"
#include "backup/backup_schema_changes.hpp"
#include "backup/backup_snapshots_function.hpp"
#include "mssql_function_docs.hpp"

#include "duckdb/function/table_function.hpp"

namespace duckdb {
namespace mssql {
namespace backup {

namespace {

constexpr const char *FUNCTION = "mssql_backup_schema_changes";

std::vector<std::pair<const char *, LogicalType>> Columns() {
	return {{"start_version", LogicalType::BIGINT},
			{"end_version", LogicalType::BIGINT},
			{"change_type", LogicalType::VARCHAR},
			{"object_type", LogicalType::VARCHAR},
			{"type", LogicalType::VARCHAR},
			{"is_ms_shipped", LogicalType::BOOLEAN},
			{"object_id", LogicalType::INTEGER},
			{"old_object_id", LogicalType::INTEGER},
			{"schema_name", LogicalType::VARCHAR},
			{"name", LogicalType::VARCHAR},
			{"old_schema_name", LogicalType::VARCHAR},
			{"old_name", LogicalType::VARCHAR},
			{"column_id", LogicalType::INTEGER},
			{"column_name", LogicalType::VARCHAR},
			{"old_column_name", LogicalType::VARCHAR},
			{"sql_type", LogicalType::VARCHAR},
			{"old_sql_type", LogicalType::VARCHAR},
			{"is_nullable", LogicalType::BOOLEAN},
			{"old_is_nullable", LogicalType::BOOLEAN},
			{"definition", LogicalType::VARCHAR},
			{"old_definition", LogicalType::VARCHAR},
			{"partitions", LogicalType::LIST(LogicalType::INTEGER)},
			{"reason", LogicalType::VARCHAR}};
}

//! A bound of the range: a version's number (an integer, a whole decimal, or a string that reads as one,
//! as DuckLake's METADATA_PARAMETERS sends it -- ResolveVersion checks it is whole), or a time (the last
//! version finished by it): a TIMESTAMP, a DATE, or a string that reads as a time.
BackupAt BoundOf(const Value &value) {
	BackupAt at;
	if (value.IsNull()) {
		return at;
	}
	at.value = value;
	const auto id = value.type().id();
	const bool numeric = value.type().IsNumeric();
	const bool reads_as_number =
		id == LogicalTypeId::VARCHAR && value.DefaultTryCastAs(LogicalType::DOUBLE, nullptr, true).has_value();
	at.unit = numeric || reads_as_number ? "version" : "timestamp";
	return at;
}

BackupAt Version(uint64_t number) {
	BackupAt at;
	at.unit = "version";
	at.value = Value::BIGINT(static_cast<int64_t>(number));
	return at;
}

struct ChangesBindData : public TableFunctionData {
	vector<std::string> paths;
	vector<std::string> diff;
	Value start, end;

	unique_ptr<FunctionData> Copy() const override {
		auto copy = make_uniq<ChangesBindData>();
		copy->paths = paths;
		copy->diff = diff;
		copy->start = start;
		copy->end = end;
		return std::move(copy);
	}
	bool Equals(const FunctionData &other_p) const override {
		const auto &other = other_p.Cast<ChangesBindData>();
		// NULL-safe: a bound is NULL by default, and Value's == is not defined on NULL.
		auto same = [](const Value &a, const Value &b) {
			return a.IsNull() == b.IsNull() && (a.IsNull() || (a.type() == b.type() && a == b));
		};
		return paths == other.paths && diff == other.diff && same(start, other.start) && same(end, other.end);
	}
	//! Every EXECUTE binds again: the files, and a history's versions, are read at init.
	bool SupportStatementCache() const override {
		return false;
	}
};

struct ChangesState : public GlobalTableFunctionState {
	vector<vector<Value>> rows;
	idx_t next = 0;
};

unique_ptr<FunctionData> Bind(ClientContext &, TableFunctionBindInput &input, vector<LogicalType> &return_types,
							  vector<Identifier> &names) {
	auto data = make_uniq<ChangesBindData>();
	data->paths = PathsOf(input.inputs[0], FUNCTION);
	data->diff = DiffPathsOf(input.named_parameters, FUNCTION);
	for (const auto &kv : input.named_parameters) {
		if (kv.first == "start_version") {
			data->start = kv.second;
		} else if (kv.first == "end_version") {
			data->end = kv.second;
		}
	}
	if (!data->diff.empty() && (!data->start.IsNull() || !data->end.IsNull())) {
		throw InvalidInputException(
			"%s: diff := names its two states (the full, and the full with the "
			"differential): start_version / end_version do not go beside it",
			FUNCTION);
	}
	for (const auto &column : Columns()) {
		names.emplace_back(column.first);
		return_types.push_back(column.second);
	}
	return std::move(data);
}

Value Text(const std::string &s) {
	return s.empty() ? Value(LogicalType::VARCHAR) : Value(s);
}

Value Id(int32_t id) {
	return id == 0 ? Value(LogicalType::INTEGER) : Value::INTEGER(id);
}

Value Nullable(int8_t value) {
	return value < 0 ? Value(LogicalType::BOOLEAN) : Value::BOOLEAN(value != 0);
}

unique_ptr<GlobalTableFunctionState> Init(ClientContext &context, TableFunctionInitInput &input) {
	const auto &data = input.bind_data->Cast<ChangesBindData>();
	auto state = make_uniq<ChangesState>();
	const std::vector<std::string> paths(data.paths.begin(), data.paths.end());
	try {
		auto states = OpenTwoStates(context, paths, std::vector<std::string>(data.diff.begin(), data.diff.end()),
									data.start, data.end, FUNCTION, false);
		if (states.same) {
			return std::move(state);  // one state: no change (DQ-47: an empty set, not an error)
		}
		const auto &old_catalog = states.old_catalog;
		const auto &new_catalog = states.new_catalog;
		const Value &start_number = states.start;
		const Value &end_number = states.end;
		const bool history_break = states.history_break;
		// Off-row definitions within MAX_CALL_OFF_ROW_BYTES a call, as mssql_backup_views reads them (review of
		// #549): past it a definition is NULL with the reason.
		uint64_t off_row_bytes = 0;
		const ViewDefinition definition = [&](bool old_state, const CatalogView &view) {
			const bool off_row = view.definition.state == ModuleDefinition::State::Deferred;
			if (off_row && off_row_bytes >= MAX_CALL_OFF_ROW_BYTES) {
				throw FormatError("its definition is stored off row, and the call's off-row definitions pass " +
								  std::to_string(MAX_CALL_OFF_ROW_BYTES) + " bytes in all");
			}
			const auto &catalog = old_state ? *old_catalog : *new_catalog;
			BindFiles bound(catalog.Binding());
			auto text =
				ReadModuleDefinition(*catalog.pages->store, view.definition, "the definition of view " + view.name);
			if (off_row) {
				off_row_bytes += text.size();
			}
			return text;
		};
		for (const auto &c : CompareCatalogs(old_catalog->Model(), new_catalog->Model(), history_break, definition,
											 states.old_whole.get(), states.new_whole.get())) {
			vector<Value> row;
			row.push_back(start_number);
			row.push_back(end_number);
			row.emplace_back(c.change_type);
			row.emplace_back(c.object_type);
			row.emplace_back(c.type);
			row.push_back(Value::BOOLEAN(c.is_ms_shipped));
			row.push_back(Id(c.object_id));
			row.push_back(Id(c.old_object_id));
			row.push_back(Text(c.schema_name));
			row.push_back(Text(c.name));
			row.push_back(Text(c.old_schema_name));
			row.push_back(Text(c.old_name));
			row.push_back(Id(c.column_id));
			row.push_back(Text(c.column_name));
			row.push_back(Text(c.old_column_name));
			row.push_back(Text(c.sql_type));
			row.push_back(Text(c.old_sql_type));
			row.push_back(Nullable(c.is_nullable));
			row.push_back(Nullable(c.old_is_nullable));
			row.push_back(c.has_definition ? Value(c.definition) : Value(LogicalType::VARCHAR));
			row.push_back(c.has_old_definition ? Value(c.old_definition) : Value(LogicalType::VARCHAR));
			if (c.partitions.empty()) {
				row.push_back(Value(LogicalType::LIST(LogicalType::INTEGER)));
			} else {
				vector<Value> numbers;
				for (const auto n : c.partitions) {
					numbers.push_back(Value::INTEGER(n));
				}
				row.push_back(Value::LIST(LogicalType::INTEGER, std::move(numbers)));
			}
			row.push_back(Text(c.reason));
			state->rows.push_back(std::move(row));
		}
	} catch (FormatError &e) {
		throw InvalidInputException("%s: %s", FUNCTION, e.what());
	}
	return std::move(state);
}

void Execute(ClientContext &, TableFunctionInput &input, DataChunk &output) {
	auto &state = input.global_state->Cast<ChangesState>();
	idx_t count = 0;
	while (state.next < state.rows.size() && count < STANDARD_VECTOR_SIZE) {
		const auto &row = state.rows[state.next++];
		for (idx_t c = 0; c < row.size(); c++) {
			output.data[c].SetValue(count, row[c]);
		}
		count++;
	}
	output.SetChildCardinality(count);
}

}  // namespace

TwoStates OpenTwoStates(ClientContext &context, const std::vector<std::string> &paths,
						const std::vector<std::string> &diff_paths, const Value &start_value, const Value &end_value,
						const char *function, bool open_same) {
	TwoStates states;
	if (!diff_paths.empty()) {
		// The full, and the full with the differential.
		states.old_catalog = OpenBackupCatalog(context, paths);
		states.new_catalog = OpenBackupCatalog(context, paths, true, CatalogScope(), diff_paths);
	} else {
		// The history read once: its versions are opened by their files, checked against this read
		// (review: three reads of every header, which could disagree).
		uint64_t default_version = 0;
		bool attached = false;
		const auto history = HistoryOf(context, paths, default_version, &attached);
		const std::string what = std::string(function) + ": " + paths.front();
		const BackupAt end_at = BoundOf(end_value);
		const uint64_t end =
			end_at.IsSet() ? ResolveVersion(*history, &context, end_at, what + ", end_version") : default_version;
		uint64_t start = 0;
		const BackupAt start_at = BoundOf(start_value);
		if (start_at.IsSet()) {
			start = ResolveVersion(*history, &context, start_at, what + ", start_version");
		} else {
			// No start: the end state's full alone (DQ-47), version `end` itself when it is a full.
			const auto &e = history->versions[end - 1];
			for (const auto &v : history->versions) {
				if (v.full == e.full && !v.IsDifferential()) {
					start = v.number;
				}
			}
		}
		if (start > end) {
			throw InvalidInputException("%s: start_version %llu is after end_version %llu", function,
										(unsigned long long)start, (unsigned long long)end);
		}
		states.start = Value::BIGINT(static_cast<int64_t>(start));
		states.end = Value::BIGINT(static_cast<int64_t>(end));
		for (uint64_t v = start + 1; v <= end; v++) {
			states.history_break = states.history_break || history->versions[v - 1].history_break;
		}
		auto open = [&](uint64_t number) {
			if (attached) {
				// By its key in the history read above, not its number: a refresh meanwhile renumbers them.
				BackupAt at = Version(number);
				at.key = AttachedStateKey(*history, number);
				const auto backup = FindAttachedBackup(context, paths.front(), at);
				std::shared_ptr<OpenedCatalog> catalog(OpenAttachedCatalog(context, *backup).release());
				// Every object's model: what rowset owners are found in, between two states only.
				if (backup->objects && start != end) {
					(number == start ? states.old_whole : states.new_whole) =
						WholeModel(*catalog, "attached backup " + backup->name);
				}
				return catalog;
			}
			const auto &v = history->versions[number - 1];
			const auto &full = history->backups[v.full].paths;
			std::vector<std::string> diff;
			if (v.IsDifferential()) {
				diff = history->backups[v.diff].paths;
			}
			std::shared_ptr<OpenedCatalog> catalog(
				OpenBackupCatalog(context, full, true, CatalogScope(), diff).release());
			CheckVersionFiles(*catalog->opened.backup, *history, v, "the backups \"" + paths.front() + "\"", false);
			return catalog;
		};
		if (start == end) {
			// One state: no change, but table_changes names its columns by it (opened from this read, not
			// its number read again).
			states.same = true;
			if (open_same) {
				states.new_catalog = open(end);
			}
			return states;
		}
		states.old_catalog = open(start);
		states.new_catalog = open(end);
	}
	// What the older state holds as of: its last backup's checkpoint (a page written since is past it);
	// and whether the newer state is a differential over the older state's own full.
	const auto &old_backup = *states.old_catalog->opened.backup;
	const auto &new_backup = *states.new_catalog->opened.backup;
	states.old_checkpoint = old_backup.Headers().Set().checkpoint_lsn;
	const auto &old_full = old_backup.Base() ? *old_backup.Base() : old_backup;
	states.same_full = new_backup.Base() && new_backup.Base()->Headers().Set().backup_set_guid.bytes ==
												old_full.Headers().Set().backup_set_guid.bytes;
	return states;
}

void RegisterBackupChangesFunctions(ExtensionLoader &loader) {
	TableFunctionSet set(FUNCTION);
	const vector<LogicalType> arguments{LogicalType(LogicalType::VARCHAR), LogicalType::LIST(LogicalType::VARCHAR)};
	for (const auto &argument : arguments) {
		TableFunction f(FUNCTION, {argument}, Execute, Bind, Init);
		f.GetSignature().AddKeywordOnly("start_version", LogicalType::ANY, Value());
		f.GetSignature().AddKeywordOnly("end_version", LogicalType::ANY, Value());
		AddDiffParameter(f);
		set.AddFunction(f);
	}
	mssql::RegisterDocumentedFunction(
		loader, std::move(set),
		{{},
		 "The schema changes between two states of a SQL Server backup history, as an importer applies them "
		 "before the rows: tables created, dropped, renamed, truncated, a partition switched out, or to "
		 "reload (recreated under a new object id, a partition switched in, the database restored between the "
		 "two), columns added, dropped, renamed, retyped or "
		 "of another nullability, views created, dropped, renamed, redefined (their T-SQL old and new), indexed "
		 "or unindexed -- compared by object id and column id, so a rename is a rename. Every table is compared, "
		 "the system (type S) and internal (IT) ones too: type and is_ms_shipped tell them from the user's. "
		 "The states are versions "
		 "of an attached history or of the files (start_version / end_version: a number, or a time; by default "
		 "the default version, and its full alone; NULL for diff :=), or a full and the differential diff := names. "
		 "One state: no rows.",
		 {"SELECT change_type, name, column_name FROM mssql_backup_schema_changes('bk', start_version := 3, "
		  "end_version := 7)",
		  "FROM mssql_backup_schema_changes('/backups/sales_full.bak', diff := '/backups/sales_diff.bak')"},
		 {"backup"}});
}

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb
