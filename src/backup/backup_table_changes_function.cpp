//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// backup/backup_table_changes_function.cpp
//
// Spec 085 task 24d (PR 4c-3, DQ-47): mssql_backup_table_changes / _insertions / _deletions -- a table's
// row changes between two states of a backup history, read as mssql_backup_scan reads the table
// (backup_scan_internal.hpp). [DuckDB]
//===----------------------------------------------------------------------===//

#include "backup/backup_change_collector.hpp"
#include "backup/backup_scan_function.hpp"
#include "backup/backup_scan_internal.hpp"

#include "backup/backup_changes_function.hpp"
#include "backup/backup_function_common.hpp"
#include "backup/backup_hash.hpp"
#include "backup/backup_opened.hpp"
#include "backup/backup_record_stager.hpp"
#include "backup/backup_reservation.hpp"
#include "backup/backup_schema_changes.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/radix_partitioning.hpp"
#include "duckdb/common/serializer/deserializer.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/common/types/column/partitioned_column_data.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/logging/logger.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/storage/buffer_manager.hpp"
#include "duckdb/storage/statistics/node_statistics.hpp"
#include "mssql_function_docs.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace duckdb {
namespace mssql {
namespace backup {

namespace {

//===----------------------------------------------------------------------===//
// Row changes (spec 085 task 24d, DQ-47): mssql_backup_table_changes / _insertions / _deletions
//===----------------------------------------------------------------------===//

const char *ChangeName(ChangeKind kind) {
	switch (kind) {
	case ChangeKind::Insert:
		return "insert";
	case ChangeKind::Delete:
		return "delete";
	case ChangeKind::Preimage:
		return "update_preimage";
	default:
		return "update_postimage";
	}
}

//! The name a listing is called by, for its errors.
const char *ChangesFunction(ChangeListing listing);

//! The call's state: the collection's units, claimed by DuckDB's tasks one at a time (as the scan's).
struct TableChangesState : public GlobalTableFunctionState {
	std::unique_ptr<ChangeSide> old_side, new_side;
	std::unique_ptr<CollectMemory> memory;
	std::unique_ptr<ChangeCollector> collector;
	std::atomic<size_t> next_unit{0};
	idx_t max_threads = 1;
	//! D17, as the scan's: a chunk's staging reserved for the call that stages it, by side (the newer, the
	//! older -- whose values are staged and then cast: twice), against half of memory_limit.
	std::unique_ptr<ReservationGate> gate;
	uint64_t chunk_bytes[2] = {0, 0};
	uint64_t chunk_allowance[2] = {0, 0};
	uint64_t virtual_row = 0;

	idx_t MaxThreads() const override {
		return max_threads;
	}
};

//! A task's state: the changes of the unit it claimed, returned a chunk at a time (`batch` its index; the
//! pairing's past every unit).
struct TableChangesLocal : public LocalTableFunctionState {
	std::shared_ptr<CollectTask> task;	// the collector's buffers (NewTask)
	std::vector<ChangeItem> queue;		// by side, partition, page, slot
	size_t pos = 0;
	idx_t batch = 0;
	bool entered = false, done = false;	 // claimed units (Enter), and found none left (Leave)
	bool pairing = false;				 // the last to leave: it pairs what every unit left
	bool placed = false;				 // its rows paired into partitions by place (PlaceAll)
	idx_t place_next = 0;				 // the lists of them returned (each its own batch index)
	uint64_t held_items = 0;			 // the bytes of the partition it returns (TakePlaced)
	std::unique_ptr<RecordStager> new_stager, old_stager;
	LobPageFetch new_fetch, old_fetch;
	DataChunk old_values;  // the older state's columns, before their cast to the newer state's types
};

unique_ptr<FunctionData> TableChangesBind(ClientContext &context, TableFunctionBindInput &input,
										  vector<LogicalType> &return_types, vector<Identifier> &names,
										  ChangeListing listing) {
	auto data = make_uniq<TableChangesBindData>();
	data->listing = listing;
	data->function = ChangesFunction(listing);
	data->paths = PathsOf(input.inputs[0], data->function);
	data->diff = DiffPathsOf(input.named_parameters, data->function);
	if (input.inputs[1].IsNull() || input.inputs[2].IsNull()) {
		throw InvalidInputException("%s: the schema and the table must not be NULL", data->function);
	}
	data->schema = input.inputs[1].ToString();
	data->table = input.inputs[2].ToString();
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
			data->function);
	}
	try {
		data->states = OpenTwoStates(context, std::vector<std::string>(data->paths.begin(), data->paths.end()),
									 std::vector<std::string>(data->diff.begin(), data->diff.end()), data->start,
									 data->end, data->function, true);
		// The newer state's table names the columns, whatever the states (one state: no rows; OpenTwoStates
		// opened it).
		const CatalogModel &new_model = data->states.new_catalog->Model();
		data->new_table = new_model.FindTable(data->schema, data->table);
		if (!data->new_table) {
			throw InvalidInputException("%s: the newer state has no table %s.%s (mssql_backup_tables lists them)",
										data->function, data->schema, data->table);
		}
		if (!data->new_table->decodable) {
			throw InvalidInputException("%s: %s.%s is not read: %s", data->function, data->schema, data->table,
										data->new_table->reason);
		}
		if (!data->states.same) {
			// The older state's table: by object id (a rename is a rename), by name across a break.
			const CatalogModel &old_model = data->states.old_catalog->Model();
			for (const auto &t : old_model.tables) {
				if (data->states.history_break
						? (t.schema_name == data->new_table->schema_name && t.name == data->new_table->name)
						: t.object_id == data->new_table->object_id) {
					data->old_table = &t;
				}
			}
			// A reload (DQ-47): the table recreated, a partition switched in, a break -- its whole content.
			bool reload = !data->old_table || data->states.history_break;
			if (!reload) {
				// Against the whole models: a partition switched in is a rowset another table owned, one an
				// ATTACH's filters leave out too.
				const ViewDefinition none = [](bool, const CatalogView &) { return std::string(); };
				for (const auto &c : CompareCatalogs(old_model, new_model, false, none, data->states.old_whole.get(),
													 data->states.new_whole.get())) {
					reload = reload || (c.change_type == "table_reload" && c.object_id == data->new_table->object_id);
				}
			}
			data->all_inserts = reload;
			if (!reload && !data->old_table->decodable) {
				throw InvalidInputException("%s: %s.%s is not read in the older state: %s", data->function,
											data->schema, data->table, data->old_table->reason);
			}
		}
	} catch (FormatError &e) {
		throw InvalidInputException("%s: %s", data->function, e.what());
	}
	data->columns = ScanColumnsOf(*data->new_table);
	if (data->old_table && !data->all_inserts) {
		const ScanColumns old_columns = ScanColumnsOf(*data->old_table);
		std::set<int32_t> readable(old_columns.column_ids.begin(), old_columns.column_ids.end());
		for (idx_t i = 0; i < data->columns.column_ids.size(); i++) {
			if (readable.count(data->columns.column_ids[i])) {
				data->old_staged.push_back(data->columns.column_ids[i]);
				data->old_outputs.push_back(i);
			}
		}
	}
	names.emplace_back("change_type");
	return_types.push_back(LogicalType::VARCHAR);
	for (idx_t i = 0; i < data->columns.names.size(); i++) {
		names.emplace_back(data->columns.names[i]);
		return_types.push_back(data->columns.types[i]);
	}
	names.emplace_back("_mssql_rid");
	return_types.push_back(LogicalType::BLOB);
	return std::move(data);
}

unique_ptr<FunctionData> TableChangesAllBind(ClientContext &context, TableFunctionBindInput &input,
											 vector<LogicalType> &return_types, vector<Identifier> &names) {
	return TableChangesBind(context, input, return_types, names, ChangeListing::All);
}
unique_ptr<FunctionData> TableInsertionsBind(ClientContext &context, TableFunctionBindInput &input,
											 vector<LogicalType> &return_types, vector<Identifier> &names) {
	return TableChangesBind(context, input, return_types, names, ChangeListing::Insertions);
}
unique_ptr<FunctionData> TableDeletionsBind(ClientContext &context, TableFunctionBindInput &input,
											vector<LogicalType> &return_types, vector<Identifier> &names) {
	return TableChangesBind(context, input, return_types, names, ChangeListing::Deletions);
}

unique_ptr<GlobalTableFunctionState> TableChangesInit(ClientContext &context, TableFunctionInitInput &input) {
	const auto &data = input.bind_data->Cast<TableChangesBindData>();
	auto state = make_uniq<TableChangesState>();
	if (data.states.same) {
		return std::move(state);
	}
	try {
		BindFiles bound_new(data.states.new_catalog->Binding());
		BindFiles bound_old(data.states.old_catalog->Binding());
		state->new_side = make_uniq<ChangeSide>(MakeSide(data.states.new_catalog, *data.new_table));
		if (data.old_table && !data.all_inserts) {
			state->old_side = make_uniq<ChangeSide>(MakeSide(data.states.old_catalog, *data.old_table));
		}
		state->memory = make_uniq<CollectMemory>(BufferManager::GetBufferManager(context), data.function);
		Value hash;
		const bool sha256 = context.TryGetCurrentSetting("mssql_backup_change_hash", hash) && !hash.IsNull() &&
							StringUtil::CIEquals(hash.ToString(), "sha256");
		state->collector =
			make_uniq<ChangeCollector>(context, data, *state->new_side, state->old_side.get(), *state->memory, sha256);
	} catch (FormatError &e) {
		throw InvalidInputException("%s: %s", data.function, e.what());
	}
	auto &buffers = BufferManager::GetBufferManager(context);
	// DuckDB's `threads`, but no more tasks than a quarter of memory_limit holds the buffers of: each task adds
	// the rows it leaves unpaired to every partition of them through a chunk it keeps (a block of their values
	// and one of their strings each), and the smaller memory_limit, the more partitions (measured: 16 tasks of
	// 64 partitions under 256 MB were out of memory at once; DuckDB's own operators bound their threads alike).
	const uint64_t per_task = state->collector->LonePartitions() * 2 * buffers.GetBlockAllocSize();
	idx_t tasks = std::max<idx_t>(1, context.db->NumberOfThreads());
	if (per_task > 0) {
		tasks = std::max<idx_t>(1, std::min<idx_t>(tasks, buffers.GetMaxMemory() / 4 / per_task));
	}
	state->max_threads = tasks;
	// A chunk's reservation, as the scan's (Init): the stager's fixed part and twice a row's variable
	// values plus a rid, for a vector of the widest rows, or CHUNK_RESERVATION_BYTES with the chunk
	// closing on what it staged; never past the budget unless one row is.
	const uint64_t budget = buffers.GetMaxMemory() / 2;
	state->virtual_row = sizeof(std::string) + 1 + 1700 + 4;
	auto size_side = [&](int side, const CatalogTable &table, const std::vector<int32_t> &staged, uint64_t copies) {
		RecordStager probe(table, staged);
		const uint64_t fixed = probe.FixedStagingBytes();
		const uint64_t row = 2 * probe.VarRowBytes() + state->virtual_row;
		const uint64_t vector = fixed + STANDARD_VECTOR_SIZE * row;
		uint64_t chunk = std::min(vector, std::max(CHUNK_RESERVATION_BYTES, fixed + row));
		chunk = std::min(chunk, std::max(budget / copies, fixed + row));
		state->chunk_allowance[side] = chunk < vector ? chunk - fixed : 0;
		state->chunk_bytes[side] = copies * chunk;
	};
	size_side(0, *data.new_table, data.columns.column_ids, 1);
	if (data.old_table && !data.all_inserts) {
		size_side(1, *data.old_table, data.old_staged, 2);
	}
	BufferManager *manager = &buffers;
	state->gate = make_uniq<ReservationGate>(
		budget,
		[manager](uint64_t bytes, std::string &why) {
			try {
				manager->ReserveMemory(bytes);
				return true;
			} catch (OutOfMemoryException &e) {
				why = e.what();
				return false;
			}
		},
		[manager](uint64_t bytes) { manager->FreeReservedMemory(bytes); });
	return std::move(state);
}

//! The changes of `listing`'s kinds, in the order a chunk takes them.
void KeepListed(const TableChangesBindData &data, std::vector<ChangeItem> &items) {
	if (data.listing != ChangeListing::All) {
		const bool insertions = data.listing == ChangeListing::Insertions;
		items.erase(std::remove_if(items.begin(), items.end(),
								   [insertions](const ChangeItem &item) {
									   return (item.kind == ChangeKind::Insert || item.kind == ChangeKind::Postimage) !=
											  insertions;
								   }),
					items.end());
	}
	std::sort(items.begin(), items.end(), [](const ChangeItem &a, const ChangeItem &b) {
		return std::make_tuple(a.old_side, a.partition, a.page.file, a.page.page, a.slot) <
			   std::make_tuple(b.old_side, b.partition, b.page.file, b.page.page, b.slot);
	});
}

//! The next changes to return, into local.queue: the rest of its unit, then a newly claimed unit's, then
//! (for the last task to find none left) the pairing's, paired into partitions by place and returned one at a time.
//! False when none are left.
bool NextChanges(ClientContext &context, const TableChangesBindData &data, TableChangesState &global,
				 TableChangesLocal &local) {
	while (local.pos >= local.queue.size()) {
		local.queue.clear();
		local.pos = 0;
		if (local.pairing) {
			global.collector->ReleaseItems(local.held_items);
			local.held_items = 0;
			if (!local.placed) {
				global.collector->PlaceAll(context);
				local.placed = true;
			}
			if (context.IsInterrupted()) {
				throw InterruptException();
			}
			if (!global.collector->TakePlaced(context, local.queue, local.held_items)) {
				local.pairing = false;
				return false;
			}
			local.batch = global.collector->Units() + local.place_next;
			local.place_next++;
			KeepListed(data, local.queue);
			continue;
		}
		if (local.done) {
			return false;
		}
		if (!local.entered) {
			global.collector->Enter();
			local.entered = true;
		}
		const size_t unit = global.next_unit.fetch_add(1);
		if (unit >= global.collector->Units()) {
			local.done = true;
			local.pairing = global.collector->Leave(*local.task);
			continue;
		}
		if (context.IsInterrupted()) {
			throw InterruptException();
		}
		global.collector->CollectUnit(*local.task, unit, local.queue);
		local.batch = unit;
		global.collector->HandIn(*local.task);
		KeepListed(data, local.queue);
	}
	return true;
}

OperatorPartitionData TableChangesPartitionData(ClientContext &, TableFunctionGetPartitionInput &input) {
	if (input.partition_info.RequiresPartitionColumns()) {
		throw InternalException("%s: partition columns are not supported", CHANGES_FUNCTION);
	}
	return OperatorPartitionData(input.local_state->Cast<TableChangesLocal>().batch);
}

unique_ptr<LocalTableFunctionState> TableChangesInitLocal(ExecutionContext &execution, TableFunctionInitInput &input,
														  GlobalTableFunctionState *global_state) {
	const auto &data = input.bind_data->Cast<TableChangesBindData>();
	auto local = make_uniq<TableChangesLocal>();
	if (data.states.same) {
		return std::move(local);
	}
	ClientContext *context = &execution.client;
	auto fetch_for = [context](const OpenedCatalog &catalog) {
		const PageStore *store = catalog.pages->store.get();
		auto fetched = std::make_shared<uint32_t>(0);
		return LobPageFetch([store, context, fetched](const PagePointer &at, Page &out) {
			if ((++*fetched & 255) == 0 && context->IsInterrupted()) {
				throw InterruptException();
			}
			store->ReadLive(at.file, at.page, out, "a fragment of a value stored off row");
		});
	};
	const auto &global = global_state->Cast<TableChangesState>();
	local->task = global.collector->NewTask();
	local->new_stager = make_uniq<RecordStager>(*data.new_table, data.columns.column_ids);
	local->new_stager->LimitBytes(global.chunk_allowance[0], global.virtual_row);
	local->new_fetch = fetch_for(*data.states.new_catalog);
	if (data.old_table && !data.all_inserts) {
		local->old_stager = make_uniq<RecordStager>(*data.old_table, data.old_staged);
		local->old_stager->LimitBytes(global.chunk_allowance[1], global.virtual_row);
		local->old_fetch = fetch_for(*data.states.old_catalog);
		local->old_values.Initialize(Allocator::DefaultAllocator(), local->old_stager->Types());
	}
	return std::move(local);
}

void TableChangesExecute(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	const auto &data = input.bind_data->Cast<TableChangesBindData>();
	auto &global = input.global_state->Cast<TableChangesState>();
	auto &local = input.local_state->Cast<TableChangesLocal>();
	if (data.states.same) {
		output.SetChildCardinality(0);
		return;
	}
	try {
		BindFiles bound_new(data.states.new_catalog->Binding());
		BindFiles bound_old(data.states.old_catalog->Binding());
		// A chunk's items: of one state and one partition (one layout), STANDARD_VECTOR_SIZE at most.
		if (!NextChanges(context, data, global, local)) {
			output.SetChildCardinality(0);
			return;
		}
		const size_t begin = local.pos;
		size_t end = begin;
		while (end < local.queue.size() && end - begin < STANDARD_VECTOR_SIZE &&
			   local.queue[end].old_side == local.queue[begin].old_side &&
			   local.queue[end].partition == local.queue[begin].partition) {
			end++;
		}
		const ChangeItem &first = local.queue[begin];
		const bool old_side = first.old_side;
		// The chunk's staging reserved for this call only (D17, as the scan's Execute).
		const uint64_t chunk_bytes = global.chunk_bytes[old_side ? 1 : 0];
		CallReservations held(*global.gate, [&context]() { return context.IsInterrupted(); });
		held.Acquire(chunk_bytes, chunk_bytes);
		const ChangeSide &side = old_side ? *global.old_side : *global.new_side;
		const PartitionCursor &cursor = side.cursors[first.partition];
		RecordStager &stager = old_side ? *local.old_stager : *local.new_stager;
		const RowsetLayout &layout = side.data.entry->Layout(*cursor.partition);
		stager.UseLayout(layout);
		stager.UseOffRow(old_side ? &local.old_fetch : &local.new_fetch, cursor.off_row);
		std::vector<Vector *> targets;
		if (old_side) {
			local.old_values.Reset();
			for (idx_t c = 0; c < local.old_values.ColumnCount(); c++) {
				targets.push_back(&local.old_values.data[c]);
			}
		} else {
			for (idx_t c = 0; c < data.columns.types.size(); c++) {
				targets.push_back(&output.data[1 + c]);
			}
		}
		stager.BeginChunk(targets);
		const PageStore &store = *side.data.catalog->pages->store;
		const std::string name = side.data.schema + "." + side.data.table;
		Page page;
		bool have = false;
		std::vector<bool> needed;
		stager.MarkNeeded(needed);
		for (size_t k : cursor.key) {
			needed[k] = true;
		}
		if (cursor.has_uniquifier) {
			needed[cursor.uniquifier] = true;
		}
		CdRecord cd;
		CdRow cd_row;
		std::vector<std::string> rids;
		std::vector<ChangeKind> kinds;
		size_t i = begin;
		for (; i < end && !stager.Full(); i++) {
			const ChangeItem &item = local.queue[i];
			if (!have || !(page.header.page_id == item.page)) {
				if (!ReadUnitDataPage(store, *cursor.unit, item.page, name, page)) {
					throw FormatError("backup: page " + item.page.ToString() + " of " + name +
									  " is no longer a data page of the table on its second read");
				}
				have = true;
			}
			RowLocation at;
			at.page = page.header.page_id;
			at.slot = item.slot;
			RowRecord row;
			RecordView fixed;
			if (layout.compression == 1) {
				const size_t pos = PAGE_SIZE - 2 - 2 * static_cast<size_t>(item.slot);
				const uint16_t offset = static_cast<uint16_t>(page.bytes[pos] | (page.bytes[pos + 1] << 8));
				ReadCdRecord(page.bytes.data(), PAGE_SIZE, PAGE_SIZE - 2u * page.header.slot_count, offset,
							 side.data.table.c_str(), cd);
				if (cd.kind == CdRecord::Kind::Forwarded) {
					cd.BackPointer(at.page.page, at.page.file, at.slot);
				}
				cd_row.Read(cd, layout, &needed);
				row = RowRecord::Compressed(cd, cd_row);
			} else {
				fixed =
					ReadSlot(page.bytes.data(), PAGE_SIZE, page.header.slot_count, item.slot, side.data.table.c_str());
				if (fixed.Type() == RECORD_FORWARDED) {
					at = ForwardedFrom(fixed, layout.table);
				}
				row = RowRecord::Fixed(fixed, RowVarCount(fixed, layout.table));
			}
			// A row's values stored off row reserved before it is staged (D17): a refusal with rows staged
			// closes the chunk before this row; with none, the chunk's reservation is taken again with it.
			const OffRowNeed need = stager.PrepareRow(row);
			// Named when refused: the row by its place, what it holds off row, what was asked.
			auto refused = [&](uint64_t bytes, const ReservationRefused &e) {
				return OutOfMemoryException(
					"%s: staging the row of %s.%s on page %s slot %u, whose values stored "
					"off row (%s) take %s of the %s reserved: %s",
					data.function, data.schema, data.table, item.page.ToString(), static_cast<unsigned>(item.slot),
					stager.DescribePrepared(), StringUtil::BytesToHumanReadableString(need.Total()),
					StringUtil::BytesToHumanReadableString(bytes), e.what());
			};
			bool fits = need.Total() == 0;
			if (!fits) {
				try {
					fits = held.TryAcquire(need.Total(), need.Ours());
				} catch (ReservationRefused &e) {
					throw refused(need.Total(), e);
				}
			}
			if (!fits) {
				if (stager.Count() > 0) {
					break;
				}
				held.ReleaseAll();
				try {
					held.Acquire(chunk_bytes + need.Total(), chunk_bytes + need.Ours());
				} catch (ReservationRefused &e) {
					throw refused(chunk_bytes + need.Total(), e);
				}
			}
			stager.Append(row);
			rids.push_back(RowId(side.data, cursor, row, at));
			kinds.push_back(item.kind);
		}
		local.pos = i;
		const idx_t count = stager.Count();
		stager.FinalizeChunk();
		if (old_side) {
			// The older state's values on the newer state's columns: by column id, cast to the newer types;
			// a column the older state has not is NULL.
			std::vector<bool> filled(data.columns.types.size(), false);
			for (idx_t c = 0; c < data.old_outputs.size(); c++) {
				Vector &target = output.data[1 + data.old_outputs[c]];
				// An older value the newer type cannot take (the column's type changed) is NULL, said once a
				// chunk; DuckDB's cast, not the server's ALTER COLUMN.
				std::string error;
				if (!VectorOperations::TryCast(context, local.old_values.data[c], target, count, &error)) {
					DUCKDB_LOG_WARNING(context, "%s: %s.%s: an older value of %s is NULL in the newer type: %s",
									   data.function, data.schema, data.table, data.columns.names[data.old_outputs[c]],
									   error);
				}
				filled[data.old_outputs[c]] = true;
			}
			for (idx_t c = 0; c < filled.size(); c++) {
				if (!filled[c]) {
					output.data[1 + c].SetVectorType(VectorType::CONSTANT_VECTOR);
					ConstantVector::SetNull(output.data[1 + c], true);
				}
			}
		}
		Vector &kind_vector = output.data[0];
		Vector &rid_vector = output.data[1 + data.columns.types.size()];
		auto *kind_out = FlatVector::GetDataMutable<string_t>(kind_vector);
		auto *rid_out = FlatVector::GetDataMutable<string_t>(rid_vector);
		for (idx_t i = 0; i < count; i++) {
			kind_out[i] = StringVector::AddString(kind_vector, ChangeName(kinds[i]));
			rid_out[i] = StringVector::AddStringOrBlob(rid_vector, rids[i]);
		}
		output.SetChildCardinality(count);
	} catch (FormatError &e) {
		throw InvalidInputException("%s: %s", data.function, e.what());
	} catch (ReservationInterrupted &) {
		throw InterruptException();
	} catch (ReservationRefused &e) {
		throw OutOfMemoryException("%s: staging a chunk of %s.%s's changes: %s", data.function, data.schema, data.table,
								   e.what());
	}
}

const char *ChangesFunction(ChangeListing listing) {
	switch (listing) {
	case ChangeListing::Insertions:
		return "mssql_backup_table_insertions";
	case ChangeListing::Deletions:
		return "mssql_backup_table_deletions";
	default:
		return CHANGES_FUNCTION;
	}
}

TableFunction MakeTableChangesFunction(const char *name, const LogicalType &path, table_function_bind_t bind) {
	TableFunction f(name, {path, LogicalType::VARCHAR, LogicalType::VARCHAR}, TableChangesExecute, bind,
					TableChangesInit, TableChangesInitLocal);
	f.get_partition_data = TableChangesPartitionData;
	f.GetSignature().AddKeywordOnly("start_version", LogicalType::ANY, Value());
	f.GetSignature().AddKeywordOnly("end_version", LogicalType::ANY, Value());
	AddDiffParameter(f);
	return f;
}

}  // namespace

void RegisterBackupTableChangesFunctions(ExtensionLoader &loader, const vector<LogicalType> &paths) {
	struct ChangesFunction {
		const char *name;
		table_function_bind_t bind;
		const char *description;
	};
	const ChangesFunction CHANGES[] = {
		{"mssql_backup_table_changes", TableChangesAllBind,
		 "The row changes of a table between two states of a SQL Server backup history, as DuckLake's "
		 "table_changes names them: change_type insert, delete, update_preimage, update_postimage, then the "
		 "table's columns in the newer state's schema and _mssql_rid (the row's key: a clustered table's key, a "
		 "heap row's place). The newer state's pages written since the older state are read with their older "
		 "versions (a differential's own pages when it lies over the older state's full), and the older state's "
		 "pages the table no longer has; rows paired by _mssql_rid and compared as stored; when LOB pages changed, "
		 "every row's value tree is walked too. A created or reloaded table: its rows inserted. The states as "
		 "mssql_backup_schema_changes takes them."},
		{"mssql_backup_table_insertions", TableInsertionsBind,
		 "The rows an importer inserts for a table between two states of a backup history: the inserts and the "
		 "updates' new rows (mssql_backup_table_changes' insert and update_postimage)."},
		{"mssql_backup_table_deletions", TableDeletionsBind,
		 "The rows an importer deletes for a table between two states of a backup history: the deletes and the "
		 "updates' old rows (mssql_backup_table_changes' delete and update_preimage), their key and values."}};
	for (const auto &c : CHANGES) {
		TableFunctionSet changes(c.name);
		for (const auto &path : paths) {
			changes.AddFunction(MakeTableChangesFunction(c.name, path, c.bind));
		}
		mssql::RegisterDocumentedFunction(
			loader, std::move(changes),
			{{},
			 c.description,
			 {std::string("FROM ") + c.name + "('bk', 'dbo', 'orders', start_version := 41, end_version := 42)",
			  std::string("FROM ") + c.name +
				  "('/backups/sales_full.bak', 'dbo', 'orders', diff := '/backups/sales_diff.bak')"},
			 {"backup"}});
	}
}

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb
