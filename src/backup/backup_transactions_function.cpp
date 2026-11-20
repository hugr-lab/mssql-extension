// Spec 085 task 27 (PR 5, owner 2026-11-20): mssql_backup_transactions and
// mssql_backup_transaction_records, the log a backup carries (backup_transactions_function.hpp). [DuckDB]
//
// Both open the backup's headers and read its log tail (stage 5, OpenedBackup::Fuzzy, kept by the
// opened-file cache); a full backup's catalog too, or a differential's over its full (diff :=) -- stages
// 1-4 and its model, which name the table an allocation unit belongs to and hold the page copies D11's
// flags compare with. A log backup has no pages, and a differential given alone no catalog to read: their
// records name allocation units only, and no change has an effect.

#include "backup/backup_transactions_function.hpp"

#include "backup/backup_bytes.hpp"
#include "backup/backup_cd_record.hpp"
#include "backup/backup_function_common.hpp"
#include "backup/backup_fuzzy.hpp"
#include "backup/backup_log_tail.hpp"
#include "backup/backup_opened.hpp"
#include "backup/backup_record.hpp"
#include "backup/backup_record_stager.hpp"
#include "backup/backup_scan_internal.hpp"
#include "backup/backup_system_tables.hpp"
#include "duckdb/common/error_data.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/client_context.hpp"
#include "mssql_function_docs.hpp"

#include <algorithm>
#include <cstdio>
#include <map>
#include <unordered_map>

namespace duckdb {
namespace mssql {
namespace backup {

namespace {

const char *const TRANSACTIONS = "mssql_backup_transactions";
const char *const RECORDS = "mssql_backup_transaction_records";

//! fn_dblog's names of the operations and contexts the reader has met (checked against fn_dump_dblog on
//! the committed samples and the load / fuzzy fixtures); another is spelled by its code.
std::string OperationName(uint8_t op) {
	switch (op) {
	case 0x01:
		return "LOP_FORMAT_PAGE";
	case 0x02:
		return "LOP_INSERT_ROWS";
	case 0x03:
		return "LOP_DELETE_ROWS";
	case 0x04:
		return "LOP_MODIFY_ROW";
	case 0x05:
		return "LOP_MODIFY_HEADER";
	case 0x06:
		return "LOP_MODIFY_COLUMNS";
	case 0x07:
		return "LOP_SET_BITS";
	case 0x0a:
		return "LOP_SET_FREE_SPACE";
	case 0x0b:
		return "LOP_DELETE_SPLIT";
	case 0x0d:
		return "LOP_EXPUNGE_ROWS";
	case 0x10:
		return "LOP_FILE_HDR_MODIFY";
	case 0x12:
		return "LOP_COUNT_DELTA";
	case 0x13:
		return "LOP_ROOT_CHANGE";
	case 0x16:
		return "LOP_INSYSXACT";
	case 0x80:
		return "LOP_BEGIN_XACT";
	case 0x81:
		return "LOP_COMMIT_XACT";
	case 0x82:
		return "LOP_ABORT_XACT";
	case 0x83:
		return "LOP_PREP_XACT";
	case 0x8c:
		return "LOP_HOBT_DELTA";
	case 0x8d:
		return "LOP_LOCK_XACT";
	case 0x8f:
		return "LOP_MIGRATE_LOCKS";
	case 0x96:
		return "LOP_BEGIN_CKPT";
	case 0x98:
		return "LOP_XACT_CKPT";
	case 0x99:
		return "LOP_END_CKPT";
	case 0xb0:
		return "LOP_CREATE_ALLOCCHAIN";
	default: {
		char buf[16];
		snprintf(buf, sizeof(buf), "LOP_0x%02X", op);
		return buf;
	}
	}
}

std::string ContextName(uint8_t context) {
	switch (context) {
	case 0x00:
		return "LCX_NULL";
	case 0x01:
		return "LCX_HEAP";
	case 0x02:
		return "LCX_CLUSTERED";
	case 0x03:
		return "LCX_INDEX_LEAF";
	case 0x04:
		return "LCX_INDEX_INTERIOR";
	case 0x05:
		return "LCX_TEXT_MIX";
	case 0x06:
		return "LCX_TEXT_TREE";
	case 0x08:
		return "LCX_GAM";
	case 0x0a:
		return "LCX_IAM";
	case 0x0b:
		return "LCX_PFS";
	case 0x11:
		return "LCX_FILE_HEADER";
	case 0x12:
		return "LCX_SCHEMA_VERSION";
	case 0x13:
		return "LCX_MARK_AS_GHOST";
	case 0x17:
		return "LCX_BOOT_PAGE_CKPT";
	case 0x18:
		return "LCX_DIFF_MAP";
	default: {
		char buf[16];
		snprintf(buf, sizeof(buf), "LCX_0x%02X", context);
		return buf;
	}
	}
}

Value OptionalLsn(const Lsn &lsn) {
	return lsn.IsZero() ? Value(LogicalType::DECIMAL(25, 0)) : LsnValue(lsn);
}

Value Blob(const LogElement &e) {
	return Value::BLOB(const_data_ptr_cast(e.data), e.length);
}

//! Whose an allocation unit is: a table's (or an indexed view's stored rows'), by index and partition.
struct UnitOwner {
	const CatalogTable *table = nullptr;
	int32_t index_id = 0;
	uint8_t type = 0;  // 1 in-row, 2 LOB, 3 row-overflow
	const CatalogPartition *partition = nullptr;
};

std::unordered_map<uint64_t, UnitOwner> OwnersOf(const CatalogModel &model) {
	std::unordered_map<uint64_t, UnitOwner> owners;
	for (const auto *tables : {&model.tables, &model.indexed_views}) {
		for (const auto &t : *tables) {
			for (const auto &u : t.units) {
				UnitOwner owner;
				owner.table = &t;
				owner.index_id = u.index_id;
				owner.type = u.unit.type;
				for (const auto &p : t.partitions) {
					if (p.rowset_id == u.unit.owner_id) {
						owner.partition = &p;
					}
				}
				owners[u.unit.auid] = owner;
			}
		}
	}
	return owners;
}

//! A table's row images decoded as its scan decodes its rows: the image laid on a page of its own and
//! staged by RecordStager into the scan's columns. Not an image whose values are stored off row (their
//! pages are the backup's, not the record's moment), nor a ROW-compressed one (spec 086's CD form): those
//! keep their bytes, and say why.
class RowImageDecoder {
public:
	explicit RowImageDecoder(const CatalogTable &table) : table_(table) {
		const ScanColumns columns = ScanColumnsOf(table);
		names_ = columns.names;
		stager_.reset(new RecordStager(table, columns.column_ids));
		chunk_.Initialize(Allocator::DefaultAllocator(), stager_->Types());
		page_.assign(PAGE_SIZE, 0);
	}

	//! The image's columns as a STRUCT, or false with `reason` set.
	bool Decode(const RowsetLayout &layout, const LogElement &image, Value &out, std::string &reason) {
		if (layout.compression != 0) {
			reason = "a compressed record (ROW or PAGE): its image is not decoded here yet";
			return false;
		}
		constexpr size_t AT = 96;  // a page header's length: where a page's first record starts
		if (image.length < 4 || AT + image.length + 2 > PAGE_SIZE) {
			reason = "an image of " + std::to_string(image.length) + " bytes is not a record";
			return false;
		}
		try {
			std::fill(page_.begin(), page_.end(), 0);
			std::copy(image.data, image.data + image.length, page_.begin() + AT);
			const RecordView view =
				ReadRecord(page_.data(), PAGE_SIZE, PAGE_SIZE - 2, static_cast<uint16_t>(AT), table_.name.c_str());
			if (view.Type() != RECORD_PRIMARY && view.Type() != RECORD_FORWARDED) {
				reason = "a record of type " + std::to_string(view.Type()) + ", not a row";
				return false;
			}
			// The table's own variable columns only: a forwarded record's last one is its back pointer, complex too.
			const size_t var_count = std::min<size_t>(RowVarCount(view, layout.table), view.var_count);
			for (size_t i = 0; i < var_count; i++) {
				const uint16_t end = static_cast<uint16_t>(view.var_ends[2 * i] | (view.var_ends[2 * i + 1] << 8));
				if (end & 0x8000) {
					reason = "a value stored off row: its pages are the backup's, not the record's";
					return false;
				}
			}
			if (layout_ != &layout) {
				stager_->UseLayout(layout);
				layout_ = &layout;
			}
			chunk_.Reset();
			stager_->BeginChunk(chunk_);
			const RowRecord row = RowRecord::Fixed(view, RowVarCount(view, layout.table));
			stager_->PrepareRow(row);
			stager_->Append(row);
			stager_->FinalizeChunk(chunk_);
			child_list_t<Value> values;
			for (idx_t c = 0; c < names_.size(); c++) {
				values.emplace_back(names_[c], chunk_.GetValue(c, 0));
			}
			out = Value::STRUCT(std::move(values));
			return true;
		} catch (std::exception &e) {
			// A stager left mid-row is begun again by the next image's BeginChunk.
			ErrorData error(e);
			reason = error.RawMessage();
			return false;
		}
	}

private:
	const CatalogTable &table_;
	std::vector<std::string> names_;
	std::unique_ptr<RecordStager> stager_;
	const RowsetLayout *layout_ = nullptr;
	DataChunk chunk_;
	std::vector<uint8_t> page_;
};

struct LogBindData : public TableFunctionData {
	vector<std::string> paths;
	vector<std::string> diff;
	BackupAt at;
	bool records = false;

	unique_ptr<FunctionData> Copy() const override {
		auto copy = make_uniq<LogBindData>();
		copy->paths = paths;
		copy->diff = diff;
		copy->at = at;
		copy->records = records;
		return std::move(copy);
	}
	bool Equals(const FunctionData &other_p) const override {
		const auto &other = other_p.Cast<LogBindData>();
		return paths == other.paths && diff == other.diff && at.Same(other.at) && records == other.records;
	}
	//! As the listings: an attached name's version is pinned again by every EXECUTE.
	bool SupportStatementCache() const override {
		return at.key.empty();
	}
};

struct LogState : public GlobalTableFunctionState {
	OpenedForStatement opened;				 // a log backup's (no catalog)
	std::unique_ptr<OpenedCatalog> catalog;	 // a data backup's
	const OpenedHeaders *headers = nullptr;	 // either's
	const FuzzySummary *tail = nullptr;
	std::unordered_map<uint64_t, UnitOwner> owners;
	// mssql_backup_transactions: the rows, built at init.
	std::vector<std::vector<Value>> rows;
	size_t next = 0;
	// mssql_backup_transaction_records: the tail read one record at a time.
	std::unique_ptr<LogTailReader> reader;
	std::unique_ptr<LogTailCursor> cursor;
	std::map<const CatalogTable *, std::unique_ptr<RowImageDecoder>> decoders;

	const FileBinding &Binding() const {
		return catalog ? catalog->Binding() : opened.binding;
	}
	const PageStore *Pages() const {
		return catalog ? catalog->pages->store.get() : nullptr;
	}
};

LogicalType TableStructType() {
	child_list_t<LogicalType> f;
	f.emplace_back("schema_name", LogicalType::VARCHAR);
	f.emplace_back("table_name", LogicalType::VARCHAR);
	f.emplace_back("index_id", LogicalType::INTEGER);
	f.emplace_back("allocation_unit", LogicalType::BIGINT);
	f.emplace_back("pages", LogicalType::INTEGER);
	f.emplace_back("effect", LogicalType::VARCHAR);
	return LogicalType::STRUCT(std::move(f));
}

struct Column {
	const char *name;
	LogicalType type;
};

std::vector<Column> TransactionColumns() {
	return {{"transaction_id", LogicalType::VARCHAR},
			{"name", LogicalType::VARCHAR},
			{"status", LogicalType::VARCHAR},
			{"begin_lsn", LogicalType::DECIMAL(25, 0)},
			{"end_lsn", LogicalType::DECIMAL(25, 0)},
			{"records", LogicalType::INTEGER},
			{"row_operations", LogicalType::INTEGER},
			{"after_checkpoint", LogicalType::BOOLEAN},
			{"tables", LogicalType::LIST(TableStructType())}};
}

std::vector<Column> RecordColumns() {
	return {{"lsn", LogicalType::DECIMAL(25, 0)},	  {"previous_lsn", LogicalType::DECIMAL(25, 0)},
			{"operation", LogicalType::VARCHAR},	  {"context", LogicalType::VARCHAR},
			{"transaction_id", LogicalType::VARCHAR}, {"transaction_status", LogicalType::VARCHAR},
			{"page", LogicalType::VARCHAR},			  {"slot", LogicalType::INTEGER},
			{"allocation_unit", LogicalType::BIGINT}, {"schema_name", LogicalType::VARCHAR},
			{"table_name", LogicalType::VARCHAR},	  {"index_id", LogicalType::INTEGER},
			{"contents", LogicalType::VARIANT()}};
}

template <bool RECORDS_FUNCTION>
unique_ptr<FunctionData> Bind(ClientContext &context, TableFunctionBindInput &input, vector<LogicalType> &return_types,
							  vector<Identifier> &names) {
	const char *function = RECORDS_FUNCTION ? RECORDS : TRANSACTIONS;
	auto data = make_uniq<LogBindData>();
	data->records = RECORDS_FUNCTION;
	data->paths = PathsOf(input.inputs[0], function);
	data->diff = DiffPathsOf(input.named_parameters, function);
	data->at = BackupAtOf(input.named_parameters, function);
	if (data->paths.size() == 1) {
		PinAttachedVersion(context, data->paths.front(), data->at);
	}
	for (const auto &column : RECORDS_FUNCTION ? RecordColumns() : TransactionColumns()) {
		names.emplace_back(column.name);
		return_types.push_back(column.type);
	}
	return std::move(data);
}

//! Per transaction, per allocation unit: the pages it changed and whether a change had D11's effect (the
//! copy holds an open transaction's change: dirty; it lacks a finished one's: stale).
struct UnitEffect {
	uint32_t pages = 0;
	bool effect = false;
};

std::map<uint64_t, std::map<uint64_t, UnitEffect>> EffectsOf(const FuzzySummary &tail, const PageStore *pages) {
	std::map<uint64_t, std::map<uint64_t, UnitEffect>> effects;	 // by transaction, then unit
	Page copy;
	for (const auto &unit : tail.units) {
		for (int open = 0; open < 2; open++) {
			const auto &by_page = open ? unit.second.open : unit.second.finished;
			for (const auto &page : by_page) {
				PageState state = PageState::Pending;
				if (pages) {
					try {
						state = pages->Fetch(static_cast<uint16_t>(page.first >> 32), static_cast<uint32_t>(page.first),
											 copy);
					} catch (FormatError &) {
						state = PageState::Pending;
					}
				}
				for (const auto &c : page.second) {
					auto &e = effects[c.transaction][unit.first];
					e.pages++;
					if (!pages) {
						continue;  // a log backup: no copy to compare with
					}
					// TableFuzziness's rules (backup_fuzzy.cpp), by transaction.
					if (open) {
						e.effect = e.effect || (state == PageState::Present	  ? c.oldest <= copy.header.lsn
												: state == PageState::NotLive ? false
																			  : true);
					} else {
						e.effect = e.effect || (state == PageState::Present	  ? !(c.newest <= copy.header.lsn)
												: state == PageState::NotLive ? !(c.newest <= tail.checkpoint)
																			  : true);
					}
				}
			}
		}
	}
	return effects;
}

unique_ptr<GlobalTableFunctionState> Init(ClientContext &context, TableFunctionInitInput &input) {
	const auto &data = input.bind_data->Cast<LogBindData>();
	const char *function = data.records ? RECORDS : TRANSACTIONS;
	auto state = make_uniq<LogState>();
	const std::vector<std::string> paths(data.paths.begin(), data.paths.end());
	const std::vector<std::string> diff(data.diff.begin(), data.diff.end());
	try {
		state->opened = OpenBackup(context, paths, diff, data.at);
		// A log backup has no pages, and a differential given alone none to read its catalog through (its full
		// is not there): their records are read without names or effects.
		const BackupKind kind = state->opened.backup->Headers().Set().kind;
		// A differential over its full -- diff :=, an attached one, a history's version -- is an overlay.
		if (kind == BackupKind::Full || (kind == BackupKind::Differential && state->opened.backup->Base())) {
			state->catalog = OpenBackupCatalog(context, paths, true, CatalogScope(), diff, data.at);
			state->headers = state->catalog->headers;
			state->owners = OwnersOf(state->catalog->Model());
		} else {
			state->headers = &state->opened.backup->Headers();
		}
		BindFiles bound(state->Binding());
		state->tail = &StatementTail(context, state->catalog ? state->catalog->opened : state->opened);
		if (data.records) {
			const auto &family = *state->headers->families.front();
			state->reader.reset(new LogTailReader(family.Stream(), family.layout,
												  LogBlockHeaderBytes(state->headers->Set().software_major)));
			state->cursor.reset(new LogTailCursor(*state->reader));
			return std::move(state);
		}
		// One row per transaction, in the order they began (those with no BEGIN last, by id).
		std::vector<std::pair<uint64_t, const FuzzySummary::Transaction *>> order;
		for (const auto &t : state->tail->transactions) {
			order.emplace_back(t.first, &t.second);
		}
		std::sort(order.begin(), order.end(),
				  [](const std::pair<uint64_t, const FuzzySummary::Transaction *> &a,
					 const std::pair<uint64_t, const FuzzySummary::Transaction *> &b) {
					  const bool a_begun = !a.second->begin.IsZero(), b_begun = !b.second->begin.IsZero();
					  if (a_begun != b_begun) {
						  return a_begun;
					  }
					  if (a_begun && !(a.second->begin <= b.second->begin && b.second->begin <= a.second->begin)) {
						  return a.second->begin <= b.second->begin;
					  }
					  return a.first < b.first;
				  });
		const auto effects = EffectsOf(*state->tail, state->Pages());
		for (const auto &entry : order) {
			const auto &t = *entry.second;
			vector<Value> tables;
			auto found = effects.find(entry.first);
			if (found != effects.end()) {
				for (const auto &unit : found->second) {
					auto owner = state->owners.find(unit.first);
					const bool known = owner != state->owners.end();
					const char *effect = nullptr;
					if (unit.second.effect) {
						effect = t.state == FuzzySummary::State::Open ? "dirty" : "stale";
					}
					child_list_t<Value> f;
					f.emplace_back("schema_name",
								   known ? Value(owner->second.table->schema_name) : Value(LogicalType::VARCHAR));
					f.emplace_back("table_name",
								   known ? Value(owner->second.table->name) : Value(LogicalType::VARCHAR));
					f.emplace_back("index_id",
								   known ? Value::INTEGER(owner->second.index_id) : Value(LogicalType::INTEGER));
					f.emplace_back("allocation_unit", Value::BIGINT(static_cast<int64_t>(unit.first)));
					f.emplace_back("pages", Value::INTEGER(static_cast<int32_t>(unit.second.pages)));
					f.emplace_back("effect", effect ? Value(effect) : Value(LogicalType::VARCHAR));
					tables.push_back(Value::STRUCT(std::move(f)));
				}
			}
			std::vector<Value> row;
			row.push_back(Value(TransactionIdText(entry.first)));
			row.push_back(t.name.empty() ? Value(LogicalType::VARCHAR) : Value(t.name));
			row.push_back(Value(TransactionStateName(t.state)));
			row.push_back(OptionalLsn(t.begin));
			row.push_back(OptionalLsn(t.end));
			row.push_back(Value::INTEGER(static_cast<int32_t>(t.records)));
			row.push_back(Value::INTEGER(static_cast<int32_t>(t.row_operations)));
			row.push_back(Value::BOOLEAN(t.after_checkpoint));
			row.push_back(Value::LIST(TableStructType(), std::move(tables)));
			state->rows.push_back(std::move(row));
		}
	} catch (FormatError &e) {
		throw InvalidInputException("%s: %s", function, e.what());
	}
	return std::move(state);
}

void TransactionsExecute(ClientContext &, TableFunctionInput &input, DataChunk &output) {
	auto &state = input.global_state->Cast<LogState>();
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

//! A row image's value: its columns where the table's decoder takes it, else its bytes; `reason` set
//! for the record when one does not decode.
Value RowValue(LogState &state, const UnitOwner *owner, const LogRecord &r, const LogElement &image,
			   std::string &reason) {
	if (image.length == 0) {
		return Value(LogicalType::BLOB);  // no image logged (a delete that logs none)
	}
	if (!owner || !owner->table || !owner->partition || owner->index_id > 1 || owner->type != 1 ||
		!owner->table->decodable || (r.context != 0x01 && r.context != 0x02 && r.context != 0x13)) {
		return Blob(image);	 // not a base table's row (an index's, a LOB fragment, an unknown unit)
	}
	auto &decoder = state.decoders[owner->table];
	if (!decoder) {
		decoder.reset(new RowImageDecoder(*owner->table));
	}
	Value out;
	std::string why;
	if (decoder->Decode(owner->table->Layout(*owner->partition), image, out, why)) {
		return out;
	}
	reason = why;
	return Blob(image);
}

//! What a record holds, by its operation (E3§6): a row operation's row images and byte ranges, a
//! transaction's name, else its elements as bytes.
Value Contents(LogState &state, const UnitOwner *owner, const LogRecord &r, const std::vector<LogElement> &e) {
	child_list_t<Value> f;
	std::string reason;
	auto u16 = [&](size_t at) -> Value {
		return at + 2 <= r.fixed_length ? Value::INTEGER(static_cast<int32_t>(r.bytes[at] | (r.bytes[at + 1] << 8)))
										: Value(LogicalType::INTEGER);
	};
	if (r.op == log_op::INSERT_ROWS && !e.empty()) {
		// The row images: all elements but the last two (a split moves many rows in one record), one at least.
		const size_t n = e.size() > 2 ? e.size() - 2 : 1;
		vector<Value> rows;
		for (size_t i = 0; i < n; i++) {
			rows.push_back(RowValue(state, owner, r, e[i], reason));
		}
		bool structs = false, others = false;
		for (const auto &v : rows) {
			(v.type().id() == LogicalTypeId::STRUCT ? structs : others) = true;
		}
		if (!reason.empty() || (structs && others)) {
			// One list holds one type: every image as its bytes when one does not decode (or is not logged).
			if (reason.empty()) {
				reason = "an image not logged beside images decoded";
			}
			rows.clear();
			for (size_t i = 0; i < n; i++) {
				rows.push_back(Blob(e[i]));
			}
		}
		f.emplace_back("rows",
					   structs && !others ? Value::LIST(rows) : Value::LIST(LogicalType::BLOB, std::move(rows)));
	} else if (r.op == log_op::DELETE_ROWS && !e.empty()) {
		f.emplace_back("row", RowValue(state, owner, r, e[0], reason));	 // the before-image
	} else if (r.op == log_op::MODIFY_ROW && e.size() >= 2) {
		f.emplace_back("offset", u16(56));
		f.emplace_back("size", u16(58));
		f.emplace_back("before", Blob(e[0]));
		f.emplace_back("after", Blob(e[1]));
	} else if (r.op == log_op::MODIFY_COLUMNS && e.size() >= 4) {
		// Element 0 the ranges' offsets (pairs), 1 their lengths, from 4 on each range's before and after.
		vector<Value> ranges;
		for (size_t i = 0; 4 + 2 * i + 1 < e.size(); i++) {
			child_list_t<Value> range;
			range.emplace_back(
				"offset", 4 * i + 2 <= e[0].length
							  ? Value::INTEGER(static_cast<int32_t>(e[0].data[4 * i] | (e[0].data[4 * i + 1] << 8)))
							  : Value(LogicalType::INTEGER));
			range.emplace_back("before", Blob(e[4 + 2 * i]));
			range.emplace_back("after", Blob(e[5 + 2 * i]));
			ranges.push_back(Value::STRUCT(std::move(range)));
		}
		child_list_t<LogicalType> range_type;
		range_type.emplace_back("offset", LogicalType::INTEGER);
		range_type.emplace_back("before", LogicalType::BLOB);
		range_type.emplace_back("after", LogicalType::BLOB);
		f.emplace_back("ranges", Value::LIST(LogicalType::STRUCT(std::move(range_type)), std::move(ranges)));
	} else if (r.op == log_op::BEGIN_XACT && !e.empty()) {
		f.emplace_back("name", Value(Utf16LeToUtf8(e[0].data, e[0].length)));
	} else {
		vector<Value> elements;
		for (const auto &x : e) {
			elements.push_back(Blob(x));
		}
		f.emplace_back("elements", Value::LIST(LogicalType::BLOB, std::move(elements)));
	}
	if (!reason.empty()) {
		f.emplace_back("undecoded", Value(reason));
	}
	return Value::STRUCT(std::move(f)).DefaultCastAs(LogicalType::VARIANT());
}

void RecordsExecute(ClientContext &, TableFunctionInput &input, DataChunk &output) {
	auto &state = input.global_state->Cast<LogState>();
	BindFiles bound(state.Binding());
	const auto &tail = *state.tail;
	idx_t count = 0;
	LogRecord r;
	try {
		while (count < STANDARD_VECTOR_SIZE && state.cursor->Next(r)) {
			if (!tail.last.IsZero() && tail.last <= r.lsn) {
				continue;  // LastLSN is the next record after the backup's (HEADERONLY): not the backup's
			}
			auto t = r.transaction ? tail.transactions.find(r.transaction) : tail.transactions.end();
			auto owner = r.has_allocation_unit ? state.owners.find(r.allocation_unit) : state.owners.end();
			const UnitOwner *o = owner == state.owners.end() ? nullptr : &owner->second;
			const std::string where = "the log record " + r.lsn.ToDecimal();
			std::vector<Value> row;
			row.push_back(LsnValue(r.lsn));
			row.push_back(OptionalLsn(r.previous));
			row.push_back(Value(OperationName(r.op)));
			row.push_back(Value(ContextName(r.context)));
			row.push_back(r.transaction ? Value(TransactionIdText(r.transaction)) : Value(LogicalType::VARCHAR));
			row.push_back(t != tail.transactions.end() ? Value(TransactionStateName(t->second.state))
													   : Value(LogicalType::VARCHAR));
			row.push_back(r.has_page ? Value(std::to_string(r.file) + ":" + std::to_string(r.page))
									 : Value(LogicalType::VARCHAR));
			row.push_back(r.has_page ? Value::INTEGER(r.slot) : Value(LogicalType::INTEGER));
			row.push_back(r.has_allocation_unit ? Value::BIGINT(static_cast<int64_t>(r.allocation_unit))
												: Value(LogicalType::BIGINT));
			row.push_back(o ? Value(o->table->schema_name) : Value(LogicalType::VARCHAR));
			row.push_back(o ? Value(o->table->name) : Value(LogicalType::VARCHAR));
			row.push_back(o ? Value::INTEGER(o->index_id) : Value(LogicalType::INTEGER));
			Value contents;
			try {
				contents = Contents(state, o, r, RowLogElements(r, where));
			} catch (FormatError &e) {
				// Contents of another layout than row-log elements: the bytes after the fixed part, and why.
				child_list_t<Value> f;
				const size_t fixed = std::min<size_t>(r.fixed_length, r.length);
				f.emplace_back("bytes", Value::BLOB(const_data_ptr_cast(r.bytes + fixed), r.length - fixed));
				f.emplace_back("undecoded", Value(e.what()));
				contents = Value::STRUCT(std::move(f)).DefaultCastAs(LogicalType::VARIANT());
			}
			row.push_back(contents);
			for (idx_t c = 0; c < row.size(); c++) {
				output.data[c].SetValue(count, row[c]);
			}
			count++;
		}
	} catch (FormatError &e) {
		throw InvalidInputException("%s: %s", RECORDS, e.what());
	}
	output.SetChildCardinality(count);
}

}  // namespace

void RegisterBackupTransactionsFunctions(ExtensionLoader &loader) {
	const vector<LogicalType> arguments{LogicalType(LogicalType::VARCHAR), LogicalType::LIST(LogicalType::VARCHAR)};
	{
		TableFunctionSet set(TRANSACTIONS);
		for (const auto &argument : arguments) {
			TableFunction f(TRANSACTIONS, {argument}, TransactionsExecute, Bind<false>, Init);
			AddDiffParameter(f);
			AddVersionParameters(f);
			set.AddFunction(f);
		}
		mssql::RegisterDocumentedFunction(
			loader, std::move(set),
			{{},
			 "Lists the transactions in the log a SQL Server backup carries, with no SQL Server: a data backup's "
			 "tail (FirstLSN to LastLSN) or a log backup's whole log. Each with its state at the backup's end "
			 "(open, committed, rolled back, or no begin in the log: the system's work), its records, and the "
			 "tables it changed with what its changes did to the backup's rows: dirty (an open transaction's "
			 "change the page copies hold) or stale (a finished transaction's change they lack) -- why "
			 "mssql_backup_tables reports a table as it does.",
			 {"SELECT transaction_id, name, status, after_checkpoint FROM "
			  "mssql_backup_transactions('/backups/busy.bak')",
			  "SELECT t.status, u.table_name, u.effect FROM mssql_backup_transactions('bk') t, unnest(t.tables) "
			  "AS x(u) WHERE u.effect IS NOT NULL"},
			 {"backup"}});
	}
	{
		TableFunctionSet set(RECORDS);
		for (const auto &argument : arguments) {
			TableFunction f(RECORDS, {argument}, RecordsExecute, Bind<true>, Init);
			AddDiffParameter(f);
			AddVersionParameters(f);
			set.AddFunction(f);
		}
		mssql::RegisterDocumentedFunction(
			loader, std::move(set),
			{{},
			 "Lists the records of the log a SQL Server backup carries, as fn_dump_dblog does, with no SQL Server: "
			 "each record's LSN, operation, context, transaction (and its state at the backup's end), page, slot "
			 "and allocation unit, the table it belongs to, and its contents as a VARIANT -- an inserted or "
			 "deleted row's columns decoded as mssql_backup_scan reads them, a modified row's byte ranges, a "
			 "transaction's name, else the record's elements as bytes.",
			 {"SELECT lsn, operation, table_name, contents FROM mssql_backup_transaction_records('/backups/sales.trn')",
			  "SELECT contents.rows FROM mssql_backup_transaction_records('bk') WHERE operation = 'LOP_INSERT_ROWS'"},
			 {"backup"}});
	}
}

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb
