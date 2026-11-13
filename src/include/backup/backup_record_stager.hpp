//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// backup/backup_record_stager.hpp
//
// Spec 085 PR 2d, task 15: a user table's records into DuckDB vectors through the codec's staging
// (codec/staging) and the live read path's family kernels, so a backup's value decodes exactly as
// TYPE mssql decodes it off the wire (D6). A page holds a value in the form TDS stages it for every
// type but four (E20): datetime and smalldatetime (the time half first, the wire's day half first),
// money (a plain i64, the wire's high half first) and bit (one bit of a byte several columns share).
// Those four are put in the wire's form as they are copied into staging, so no kernel of their own is
// needed. Each column's way in (StageArm) is resolved once; a value is located by LocateValue (spec
// 085 § Rules, records), appended by its column's arm, and a chunk is published by one kernel call per
// column (FinalizeStagedColumn, shared with RowStager). [DuckDB]
//===----------------------------------------------------------------------===//

#pragma once

#include "backup/backup_catalog_model.hpp"
#include "backup/backup_lob.hpp"
#include "backup/backup_record.hpp"
#include "backup/backup_system_tables.hpp"
#include "codec/staging/column_ops.hpp"
#include "codec/staging/column_staging.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "tds/tds_column_metadata.hpp"

#include <string>
#include <vector>

namespace duckdb {
namespace mssql {
namespace backup {

//! How a column's value goes into its staging; resolved once per column.
enum class StageArm : uint8_t {
	Direct,		 // the bytes are DuckDB's own: straight into the vector (integers, real, float)
	Fixed,		 // staged as they are for a batch kernel (decimal, date, time types, uniqueidentifier, smallmoney)
	SwapHalves,	 // datetime, smalldatetime, money: the two halves exchanged into the wire's order, then staged
	Bit,		 // a bit: its bit of the byte at its offset (a default's own byte), into the vector
	Utf16,		 // nvarchar, nchar: staged for the string kernel, which splits on a delimiter it adds
	Bytes,		 // varchar, char, varbinary, binary, timestamp: staged as they are
};

class RecordStager {
public:
	//! A record's in-row part at most: the most bytes of values one row stages.
	static constexpr uint32_t IN_ROW_BYTES = 8060;
	//! The columns `column_ids` of `table`, in that order, read as ReadingOf says: converted to D6's
	//! type, or a type still to be converted as its stored bytes (BLOB). A column that is not readable
	//! (no stored value, a sparse column), one not in the table, or one the codec would stage another
	//! way than its type says is an InvalidInputException naming it.
	RecordStager(const CatalogTable &table, const std::vector<int32_t> &column_ids);

	//! The output types, in `column_ids` order.
	const vector<LogicalType> &Types() const {
		return types_;
	}
	//! The records appended from now on are of `layout`'s rowset (a partition's own layout: an added
	//! column's default and the physical places can differ by partition). A column the layout does not
	//! hold where its type says, or a default of another width, is a FormatError naming it.
	void UseLayout(const RowsetLayout &layout);
	//! Where a value stored off row is read from (PR 3, backup_lob): pages through `fetch` (which must
	//! outlive the stager's use of it), on the units of the partition whose records are appended from
	//! now on -- set for every partition, since partitions sharing a layout have units of their own.
	void UseOffRow(const LobPageFetch *fetch, const LobUnits &units);
	//! Build each row's `_mssql_lob` map (LastRowLobMap): every value of the row stored off row,
	//! projected or not. Off: no map is built.
	void MapOffRow(bool on);
	//! The map of the row Append staged last (backup_lob_map's bytes); empty when the row has no value
	//! stored off row (the column is NULL) or MapOffRow is off.
	const std::string &LastRowLobMap() const {
		return row_lob_map_;
	}
	//! The longest value stored off row the stager assembles: a LOB's 2^31 - 1 bytes.
	static constexpr uint64_t MAX_OFF_ROW_BYTES = 2147483647ull;
	//! Start a chunk that FinalizeChunk will publish into `targets` (one flat, empty vector of Types()
	//! per column, in order), or into `out`'s first Types().size() vectors.
	void BeginChunk(const std::vector<Vector *> &targets);
	void BeginChunk(DataChunk &out);
	//! Stage one record of the layout's rowset (a primary or a forwarded record). A value stored off
	//! row (row-overflow, a LOB tree, a text pointer) is assembled into its column's staging through
	//! UseOffRow's pages (a NULL root is NULL); a malformed one is WalkLob's FormatError naming the
	//! column. A record the layout does not fit is LocateValue's FormatError. After a throw the chunk holds part of a
	//! row: the next Append or FinalizeChunk without a BeginChunk first is an InternalException, never a row shifted.
	void Append(const RecordView &record);
	idx_t Count() const {
		return count_;
	}
	//! No row may be appended to the chunk: STANDARD_VECTOR_SIZE rows, or the staged payload of its
	//! MAX columns past the codec's per-chunk budget (STAGING_CHUNK_PAYLOAD_BUDGET_BYTES), weighed as
	//! RowStager weighs it (a code-page column twice: its output heap is accounted at 3 bytes a
	//! character, #536). A bounded column's chunk is bounded by its declaration.
	//! Under LimitBytes, also when one more of the widest rows could pass the chunk's allowance.
	bool Full() const {
		return count_ == STANDARD_VECTOR_SIZE || payload_bytes_ >= codec::staging::STAGING_CHUNK_PAYLOAD_BUDGET_BYTES ||
			   (allowance_ != 0 && 2 * staged_bytes_ + count_ * extra_row_ + 2 * var_row_ + extra_row_ > allowance_);
	}
	//! The most the staging holds, for a reservation (D17): FixedStagingBytes() for any chunk (the
	//! fixed columns' strides for a vector, the variable ones' offsets and first buffers), plus twice
	//! (buffers grow by doubling) the variable values staged -- at most VarRowBytes() a row: the
	//! declared widths, a record's in-row 8,060 bytes at most, and a UTF-16 value's delimiter.
	uint64_t FixedStagingBytes() const {
		return fixed_staging_;
	}
	uint64_t VarRowBytes() const {
		return var_row_;
	}
	//! A chunk closes before its staging -- twice its variable values, plus `extra_row` a row for the
	//! caller's own (the virtual columns) -- could pass `allowance` with one more row. 0: no limit.
	void LimitBytes(uint64_t allowance, uint64_t extra_row) {
		allowance_ = allowance;
		extra_row_ = extra_row;
	}
	//! Publish the staged rows into the vectors BeginChunk was given; the caller sets its chunk's
	//! cardinality (Count()). The DataChunk form sets it.
	void FinalizeChunk();
	void FinalizeChunk(DataChunk &out);

private:
	struct Column {
		int32_t column_id = 0;
		std::string name;  // table.column, for errors
		tds::ColumnMetadata meta;
		codec::staging::ColumnOps ops;
		StageArm arm = StageArm::Bytes;
		size_t layout_index = 0;  // in the current layout
		uint8_t bit_position = 0;
		uint8_t budget_weight = 0;	  // how many times its staged bytes count toward Full(): 0, 1, 2
		uint32_t declared_bytes = 0;  // its max_length (a MAX one: a row's in-row part)
	};

	void AppendValue(Column &c, codec::staging::ColumnStaging &st, const ValueRef &v);
	void AppendOffRow(Column &c, codec::staging::ColumnStaging &st, const ValueRef &v);
	void MapRow(const RecordView &record, size_t var_count);
	const LobPageFetch &Fetch(const std::string &what) const;

	std::string table_;
	std::vector<Column> columns_;
	vector<LogicalType> types_;
	codec::staging::StagingArena arena_;
	std::vector<Vector *> targets_;
	const RowsetLayout *layout_ = nullptr;
	const LobPageFetch *fetch_ = nullptr;
	LobUnits units_;
	bool map_off_row_ = false;
	//! The layout's variable columns, for the map, ascending catalog column id.
	struct MapColumn {
		size_t layout_index;
		int32_t column_id;
		std::string name;  // table.column, for errors
	};
	std::vector<MapColumn> map_columns_;
	std::string row_lob_map_;
	idx_t count_ = 0;
	idx_t payload_bytes_ = 0;
	//! The variable values staged in this chunk, as staged (a UTF-16 one with its delimiter).
	uint64_t staged_bytes_ = 0;
	uint64_t fixed_staging_ = 0;
	uint64_t var_row_ = 0;
	uint64_t allowance_ = 0;
	uint64_t extra_row_ = 0;
	//! An Append threw after staging part of a row; cleared by BeginChunk.
	bool spoiled_ = false;
	//! BeginChunk ran and FinalizeChunk has not: Append and FinalizeChunk need it.
	bool begun_ = false;
};

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb
