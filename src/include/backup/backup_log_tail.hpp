//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// backup/backup_log_tail.hpp
//
// Spec 085 task 26 (PR 5): the log tail a data backup carries -- the MQTL streams' bytes, the log
// from the set's FirstLSN to its LastLSN (E3§2) -- read as SQL Server writes it: log blocks in sector
// order, each block's 512-byte sectors put back (sector parity), its records through the slot array,
// each record's header (E3§6, checked against fn_dump_dblog record by record). What D11's fuzzy status
// needs: every record's LSN, operation, transaction, page and allocation unit; the record's contents
// are spec 090's. DuckDB-free (D15).
//===----------------------------------------------------------------------===//

#pragma once

#include "backup/backup_lsn.hpp"
#include "backup/backup_mtf.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace duckdb {
namespace mssql {
namespace backup {

//! Log operations (E3§6, verified against fn_dblog / fn_dump_dblog): what D11 tells apart.
namespace log_op {
constexpr uint8_t FORMAT_PAGE = 0x01;
constexpr uint8_t INSERT_ROWS = 0x02;
constexpr uint8_t DELETE_ROWS = 0x03;
constexpr uint8_t MODIFY_ROW = 0x04;
constexpr uint8_t MODIFY_COLUMNS = 0x06;
constexpr uint8_t DELETE_SPLIT = 0x0b;
constexpr uint8_t EXPUNGE_ROWS = 0x0d;
constexpr uint8_t BEGIN_XACT = 0x80;
constexpr uint8_t COMMIT_XACT = 0x81;
constexpr uint8_t ABORT_XACT = 0x82;
}  // namespace log_op

//! An operation that changes a row a reader sees (D11: what flags (iii) and (iv) count): INSERT_ROWS,
//! DELETE_ROWS, MODIFY_ROW, MODIFY_COLUMNS, DELETE_SPLIT. Not FORMAT_PAGE, which lays out an empty page,
//! nor EXPUNGE_ROWS, which removes a ghost (a row already deleted): measured on the load fixture, 37 pages
//! formatted after CheckpointLSN, none in the image, held no row of the restored table (E3's redo needs
//! both, spec 090; the status does not).
bool IsRowOperation(uint8_t op);

//! A context in which a row operation changes the rows of a data or LOB page (E3§6, fn_dump_dblog's names):
//! LCX_HEAP (also a B-tree leaf's FORMAT_PAGE), LCX_CLUSTERED, LCX_INDEX_LEAF, LCX_TEXT_MIX, LCX_TEXT_TREE,
//! LCX_MARK_AS_GHOST. Not LCX_INDEX_INTERIOR (keys above the leaves), LCX_PFS (an allocation, logged as a
//! MODIFY_ROW under the unit allocated), LCX_SCHEMA_VERSION and the allocation pages' others: measured on the
//! load fixture, they named the table's unit on pages that hold none of its rows.
bool IsRowContext(uint8_t context);

//! One log record's header.
struct LogRecord {
	//! (the block's VLF sequence, its block id, the record's place in the block's slot array from 1).
	Lsn lsn;
	uint8_t op = 0;
	uint8_t context = 0;
	//! The transaction id as fn_dblog prints it high:low, here (u16 high) << 32 | (u32 low); 0 for
	//! records of no transaction (checkpoints, system work).
	uint64_t transaction = 0;
	//! The page the record changes (@24, its file @28); a record whose fixed part is shorter than 30 bytes has
	//! none. Its slot (@30) only in a fixed part of 32 bytes or more: LOP_MIGRATE_LOCKS has a page and no
	//! slot (30 bytes, measured on 2017; fn_dump_dblog gives the page).
	bool has_page = false;
	bool has_slot = false;
	uint16_t file = 0;
	uint32_t page = 0;
	uint16_t slot = 0;
	//! m_indexId << 48 | m_objId << 16 (as PageHeader::AllocationUnit), when the fixed part holds it
	//! (48 bytes or more).
	bool has_allocation_unit = false;
	uint64_t allocation_unit = 0;
	//! The previous record of its transaction (@4), zero for the first.
	Lsn previous;
	//! The record's bytes, its header first, up to the next record (or the slot array): valid only while
	//! the record is visited (ForEach) or until the cursor's next record (LogTailCursor::Next).
	const uint8_t *bytes = nullptr;
	uint16_t length = 0;
	uint16_t fixed_length = 0;
	//! The bytes of the record's log block: the next block's id is this one's plus its sectors.
	uint16_t block_size = 0;
};

//! Whether `reached`, the last record a tail read holds before its LastLSN `last` (in the block of
//! `block_size` bytes), is the one LastLSN follows: the record before it in its block, or the last of the
//! block before it -- or of the VLF before LastLSN's, whose end is not known here. A tail that stops
//! short of it was not read whole (review of #591).
bool TailReachesLast(const Lsn &reached, uint16_t block_size, const Lsn &last);

//! One element of a record's row-log contents.
struct LogElement {
	const uint8_t *data = nullptr;
	uint16_t length = 0;
};

//! The record's row-log contents (E3§6, checked against fn_dump_dblog's RowLog Contents): after its fixed
//! part, u16 n, n u16 lengths, then the n elements, each from a 4-byte boundary of the record. None when
//! the record ends with its fixed part. Contents that run past the record are a FormatError naming `where`.
std::vector<LogElement> RowLogElements(const LogRecord &record, const std::string &where);

class LogTailReader;

//! What a cursor holds while it reads: the chunk it reads the stream by (4 MiB, a block never cut across
//! two) and one block beside it, for the memory its caller reserves (D17).
constexpr uint64_t LOG_CURSOR_BYTES = (4ull << 20) + (64ull << 10);

//! The tail's records one at a time, in ForEach's order: what a table function returns a chunk of per
//! call. Reads through the reader's stream as it goes (a chunk of 4 MiB at a time).
class LogTailCursor {
public:
	explicit LogTailCursor(const LogTailReader &tail);
	//! The next record, false after the last; its bytes are valid until the next call.
	bool Next(LogRecord &out);

private:
	bool NextBlock();

	const LogTailReader &tail_;
	size_t span_ = 0;
	uint64_t at_ = 0;  // within the span: the chunk's start
	std::vector<uint8_t> chunk_;
	size_t in_ = 0;	 // within the chunk: the next block
	bool span_ended_ = false;
	std::vector<uint8_t> block_;
	std::vector<LogRecord> records_;
	size_t next_ = 0;
};

//! The log tail of one backup file: its MQTL streams' data (each less its 2-byte lead-in), in file
//! order, as one sequence of log blocks. `stream` and `layout` are kept by reference.
class LogTailReader {
public:
	//! `block_header`: its blocks' header size, LogBlockHeaderBytes of the backup's SQL Server version.
	LogTailReader(const RandomAccess &stream, const MtfLayout &layout, size_t block_header);

	//! The tail's bytes.
	uint64_t Bytes() const {
		return bytes_;
	}
	//! Every record in log order (blocks as stored, a block's records by slot). A block that is not one
	//! -- a size that is no multiple of 512, a slot array or a record outside it -- is a FormatError
	//! naming the file and the offset. A block header of zeros ends its stream's blocks (the stream's
	//! unused end).
	void ForEach(const std::function<void(const LogRecord &)> &visit) const;

private:
	friend class LogTailCursor;
	struct Span {
		uint64_t offset;
		uint64_t length;
	};
	const RandomAccess &stream_;
	size_t block_header_;
	std::vector<Span> spans_;
	uint64_t bytes_ = 0;
};

//! The records of one log block of `size` bytes at `block` (sector parity already put back), each
//! given to `visit`; `where` names the block in a FormatError. What ForEach does per block: the
//! fuzz harness's entry point.
void ParseLogBlock(const uint8_t *block, size_t size, size_t header_bytes, const std::string &where,
				   const std::function<void(const LogRecord &)> &visit);
//! The size of a log block's header in a backup SQL Server `software_major` wrote (SSET): 72 bytes, the
//! first record after it (E3§6), from SQL Server 2019 (15) on; 48 on 2017 (14) and before.
size_t LogBlockHeaderBytes(uint8_t software_major);

//! Puts back the first byte of every 512-byte sector after the first, which the log overwrites with
//! its parity byte and keeps at `block[size - 1 - k]` (E3§6: 1.6 % of records misparsed without it).
void RestoreSectorParity(uint8_t *block, size_t size);

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb
