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
	//! Start a chunk that FinalizeChunk will publish into `targets` (one flat, empty vector of Types()
	//! per column, in order), or into `out`'s first Types().size() vectors.
	void BeginChunk(const std::vector<Vector *> &targets);
	void BeginChunk(DataChunk &out);
	//! Stage one record of the layout's rowset (a primary or a forwarded record). A value stored off
	//! row (row-overflow, a LOB) is a FormatError naming the column until PR 3; a record the layout
	//! does not fit is LocateValue's FormatError. After a throw the chunk holds part of a row: the next
	//! Append or FinalizeChunk without a BeginChunk first is an InternalException, never a row shifted.
	void Append(const RecordView &record);
	idx_t Count() const {
		return count_;
	}
	//! No row may be appended to the chunk: STANDARD_VECTOR_SIZE rows, or the staged payload of its
	//! MAX columns past the codec's per-chunk budget (STAGING_CHUNK_PAYLOAD_BUDGET_BYTES), weighed as
	//! RowStager weighs it (a code-page column twice: its output heap is accounted at 3 bytes a
	//! character, #536). A bounded column's chunk is bounded by its declaration.
	bool Full() const {
		return count_ == STANDARD_VECTOR_SIZE || payload_bytes_ >= codec::staging::STAGING_CHUNK_PAYLOAD_BUDGET_BYTES;
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
		uint8_t budget_weight = 0;	// how many times its staged bytes count toward Full(): 0, 1, 2
		bool text_pointer = false;	// text / ntext / image: its in-row bytes are a pointer (PR 3)
	};

	void AppendValue(Column &c, codec::staging::ColumnStaging &st, const ValueRef &v);

	std::string table_;
	std::vector<Column> columns_;
	vector<LogicalType> types_;
	codec::staging::StagingArena arena_;
	std::vector<Vector *> targets_;
	const RowsetLayout *layout_ = nullptr;
	idx_t count_ = 0;
	idx_t payload_bytes_ = 0;
	//! An Append threw after staging part of a row; cleared by BeginChunk.
	bool spoiled_ = false;
};

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb
