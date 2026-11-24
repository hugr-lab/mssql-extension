// Spec 085 task 26 (PR 5): the log tail's blocks and record headers (backup_log_tail.hpp).

#include "backup/backup_log_tail.hpp"

#include "backup/backup_bytes.hpp"

#include <algorithm>
#include <cstring>

namespace duckdb {
namespace mssql {
namespace backup {

namespace {

//! A log block's header: u16 record count @2, u16 end of the slot array @4, u16 size @6, u32 VLF
//! sequence @12, u32 block id @16; its first record at 72 (E3§6), at 48 on SQL Server 2017
//! (LogBlockHeaderBytes).
constexpr size_t LONGEST_BLOCK_HEADER = 72;
constexpr size_t SHORTEST_BLOCK_HEADER = 48;
constexpr size_t SECTOR = 512;
//! A record's header up to its page and file (@24 .. @29), its slot (@30 .. @31), and its allocation unit's
//! high part (@46).
constexpr size_t RECORD_WITH_PAGE = 30;
constexpr size_t RECORD_WITH_SLOT = 32;
constexpr size_t RECORD_WITH_UNIT = 48;
//! The tail is read in chunks of this many bytes, a block never cut across two.
constexpr size_t CHUNK = 4u << 20;
static_assert(CHUNK + (1u << 16) <= LOG_CURSOR_BYTES, "a cursor's chunk and one block (a u16 size) within it");

uint16_t U16(const uint8_t *p) {
	return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

uint32_t U32(const uint8_t *p) {
	return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
		   (static_cast<uint32_t>(p[3]) << 24);
}

bool AllZero(const uint8_t *p, size_t n) {
	for (size_t i = 0; i < n; i++) {
		if (p[i]) {
			return false;
		}
	}
	return true;
}

}  // namespace

bool IsRowOperation(uint8_t op) {
	switch (op) {
	case log_op::INSERT_ROWS:
	case log_op::DELETE_ROWS:
	case log_op::MODIFY_ROW:
	case log_op::MODIFY_COLUMNS:
	case log_op::DELETE_SPLIT:
		return true;
	default:
		return false;
	}
}

bool IsRowContext(uint8_t context) {
	switch (context) {
	case 0x01:	// LCX_HEAP
	case 0x02:	// LCX_CLUSTERED
	case 0x03:	// LCX_INDEX_LEAF
	case 0x05:	// LCX_TEXT_MIX
	case 0x06:	// LCX_TEXT_TREE
	case 0x13:	// LCX_MARK_AS_GHOST
		return true;
	default:
		return false;
	}
}

bool TailReachesLast(const Lsn &reached, uint16_t block_size, const Lsn &last) {
	if (last.slot > 1) {
		return reached.vlf == last.vlf && reached.block == last.block && reached.slot + 1u == last.slot;
	}
	if (reached.vlf != last.vlf) {
		// LastLSN opens the next VLF (a VLF's sequence number is its predecessor's plus one): where this
		// one ends is not in the log. Not met in the fixtures (every tail measured ends in one VLF).
		return reached.vlf + 1u == last.vlf;
	}
	return reached.block + block_size / SECTOR == last.block;
}

void RestoreSectorParity(uint8_t *block, size_t size) {
	const size_t sectors = size / SECTOR;
	for (size_t k = 1; k < sectors; k++) {
		block[SECTOR * k] = block[size - 1 - k];
	}
}

size_t LogBlockHeaderBytes(uint8_t software_major) {
	// SQL Server 2017 (major 14) and before: records from 48 on (measured in the oracle lane: record 1 of
	// every block of a 2017 backup at 48, where 2019's and later are at 72).
	return software_major != 0 && software_major < 15 ? SHORTEST_BLOCK_HEADER : LONGEST_BLOCK_HEADER;
}

void ParseLogBlock(const uint8_t *block, size_t size, size_t header_bytes, const std::string &where,
				   const std::function<void(const LogRecord &)> &visit) {
	if (size < header_bytes || size % SECTOR) {
		throw FormatError(where + ": a log block of " + std::to_string(size) + " bytes (a multiple of 512 expected)");
	}
	const uint16_t count = U16(block + 2);
	const uint16_t slot_end = U16(block + 4);
	if (slot_end > size || slot_end < header_bytes + 2u * count) {
		throw FormatError(where + ": a log block whose slot array ends at " + std::to_string(slot_end) + " for " +
						  std::to_string(count) + " records in " + std::to_string(size) + " bytes");
	}
	const size_t records_end = slot_end - 2u * count;
	// Each record runs to the next one in the block, the last to the records' end.
	std::vector<uint16_t> starts(count);
	for (uint16_t i = 0; i < count; i++) {
		starts[i] = U16(block + slot_end - 2u * (i + 1u));
	}
	std::vector<uint16_t> sorted(starts);
	std::sort(sorted.begin(), sorted.end());
	// Two slots at one offset would give one record twice (review of #591).
	const auto twice = std::adjacent_find(sorted.begin(), sorted.end());
	if (twice != sorted.end()) {
		throw FormatError(where + ": two records at " + std::to_string(*twice));
	}
	LogRecord record;
	record.lsn.vlf = U32(block + 12);
	record.lsn.block = U32(block + 16);
	record.block_size = static_cast<uint16_t>(size);
	for (uint16_t i = 0; i < count; i++) {
		// The slot array runs backwards from its end: the first record's offset is its last entry.
		const size_t at = U16(block + slot_end - 2u * (i + 1u));
		const std::string record_where = where + ", record " + std::to_string(i + 1u);
		if (at < header_bytes || at + 4 > records_end) {
			throw FormatError(record_where + " at " + std::to_string(at) + ": outside the block's records (" +
							  std::to_string(header_bytes) + " .. " + std::to_string(records_end) + ")");
		}
		const uint16_t fixed = U16(block + at + 2);
		const size_t header = std::max<size_t>(fixed, 24);
		// The record runs to the next one (the last to the records' end): its fixed part within that, or
		// its page and unit fields would be the next record's bytes (review of #591).
		auto next = std::upper_bound(sorted.begin(), sorted.end(), static_cast<uint16_t>(at));
		const size_t end = next == sorted.end() ? records_end : std::min<size_t>(*next, records_end);
		if (at + header > end) {
			throw FormatError(record_where + ": a fixed part of " + std::to_string(fixed) + " bytes at " +
							  std::to_string(at) + " runs past the record's end (" + std::to_string(end) + ")");
		}
		const uint8_t *r = block + at;
		record.bytes = r;
		record.length = static_cast<uint16_t>(end - at);
		record.fixed_length = fixed;
		record.previous.vlf = U32(r + 4);
		record.previous.block = U32(r + 8);
		record.previous.slot = U16(r + 12);
		record.lsn.slot = static_cast<uint16_t>(i + 1u);
		record.transaction = (static_cast<uint64_t>(U16(r + 20)) << 32) | U32(r + 16);
		record.op = r[22];
		record.context = r[23];
		record.has_page = fixed >= RECORD_WITH_PAGE;
		record.page = record.has_page ? U32(r + 24) : 0;
		record.file = record.has_page ? U16(r + 28) : 0;
		record.has_slot = fixed >= RECORD_WITH_SLOT;
		record.slot = record.has_slot ? U16(r + 30) : 0;
		record.has_allocation_unit = fixed >= RECORD_WITH_UNIT;
		record.allocation_unit = record.has_allocation_unit ? (static_cast<uint64_t>(U16(r + 46)) << 48) |
																  (static_cast<uint64_t>(U32(r + 32)) << 16)
															: 0;
		visit(record);
	}
}

LogTailReader::LogTailReader(const RandomAccess &stream, const MtfLayout &layout, size_t block_header)
	: stream_(stream), block_header_(block_header) {
	for (const auto &block : layout.blocks) {
		for (const auto &s : block.streams) {
			if (s.id != "MQTL" || s.length <= 2) {
				continue;
			}
			if (s.encryption != 0) {
				throw FormatError("backup file \"" + stream.Name() + "\": an encrypted log stream (spec 087)");
			}
			if (s.data_offset + s.length > stream.Size() || s.data_offset + s.length < s.data_offset) {
				throw FormatError("backup file \"" + stream.Name() + "\": a log stream at " +
								  std::to_string(s.data_offset) + " runs past the file's end");
			}
			// A stream's 2-byte lead-in is not log (§ Rules, MTF).
			spans_.push_back(Span{s.data_offset + 2, s.length - 2});
			bytes_ += s.length - 2;
		}
	}
}

std::vector<LogElement> RowLogElements(const LogRecord &record, const std::string &where) {
	std::vector<LogElement> elements;
	const size_t fixed = record.fixed_length;
	if (!record.bytes || fixed + 2 > record.length) {
		return elements;
	}
	const uint16_t n = U16(record.bytes + fixed);
	if (fixed + 2 + 2u * n > record.length) {
		throw FormatError(where + ": a record of " + std::to_string(record.length) + " bytes lists " +
						  std::to_string(n) + " elements past its end");
	}
	size_t at = (fixed + 2 + 2u * n + 3) & ~static_cast<size_t>(3);
	for (uint16_t i = 0; i < n; i++) {
		const uint16_t length = U16(record.bytes + fixed + 2 + 2u * i);
		if (at + length > record.length) {
			throw FormatError(where + ": element " + std::to_string(i) + " of " + std::to_string(length) +
							  " bytes runs past its record (" + std::to_string(record.length) + " bytes)");
		}
		LogElement e;
		e.data = record.bytes + at;
		e.length = length;
		elements.push_back(e);
		at = (at + length + 3) & ~static_cast<size_t>(3);
	}
	return elements;
}

LogTailCursor::LogTailCursor(const LogTailReader &tail) : tail_(tail) {}

bool LogTailCursor::NextBlock() {
	const auto &stream = tail_.stream_;
	while (span_ < tail_.spans_.size()) {
		const auto &span = tail_.spans_[span_];
		if (span_ended_ || at_ + in_ + tail_.block_header_ > span.length) {
			span_++;
			at_ = 0;
			in_ = 0;
			chunk_.clear();
			span_ended_ = false;
			continue;
		}
		if (in_ + tail_.block_header_ > chunk_.size()) {
			// A chunk from the next block on: whole blocks are parsed from it.
			at_ += in_;
			in_ = 0;
			const size_t take = static_cast<size_t>(std::min<uint64_t>(CHUNK, span.length - at_));
			chunk_.resize(take);
			stream.ReadAt(span.offset + at_, chunk_.data(), take);
			if (take < tail_.block_header_) {
				span_ended_ = true;
				continue;
			}
		}
		const uint8_t *head = chunk_.data() + in_;
		const uint16_t size = U16(head + 6);
		const std::string where =
			"backup file \"" + stream.Name() + "\", the log block at " + std::to_string(span.offset + at_ + in_);
		if (size == 0) {
			if (!AllZero(head, tail_.block_header_)) {
				throw FormatError(where + ": a block of size 0");
			}
			span_ended_ = true;	 // the stream's unused end
			continue;
		}
		if (at_ + in_ + size > span.length) {
			throw FormatError(where + ": a block of " + std::to_string(size) + " bytes runs past its stream");
		}
		if (in_ + size > chunk_.size()) {
			if (in_ == 0) {
				// No block whole in a full chunk: only a block larger than CHUNK, which a u16 size cannot be.
				throw FormatError(where + ": a block larger than the tail reads at once");
			}
			at_ += in_;	 // read again from this block
			in_ = 0;
			chunk_.clear();
			continue;
		}
		// Each block follows the one before it, across the streams too: the next in its VLF (its id plus the
		// sectors before it), or one of the next VLF (review of #591: a tail with its middle missing was
		// summarised). Caught: a block missing within a VLF, a VLF skipped, a block twice, an older one. Not
		// caught: a VLF's blocks cut short where the next stream starts the next VLF -- where a VLF ends is
		// not in the backup; LastLSN's check catches it at the tail's end only. A stream ends in zeros after
		// its last block, in a VLF reused too (measured on 2025 with a log wrapped twelve VLF sequences: no
		// earlier generation's blocks there); tail_window's log is wrapped so the lane reads such a tail on
		// every version.
		const uint32_t vlf = U32(head + 12);
		const uint32_t id = U32(head + 16);
		if (has_previous_ &&
			!(vlf == previous_vlf_ ? id == previous_id_ + previous_size_ / SECTOR : vlf == previous_vlf_ + 1u)) {
			auto name = [](uint32_t v, uint32_t b) {
				return "VLF " + std::to_string(v) + " block " + std::to_string(b);
			};
			throw FormatError(where + ": " + name(vlf, id) + " does not follow " + name(previous_vlf_, previous_id_) +
							  " of " + std::to_string(previous_size_) + " bytes before it: the tail is not read whole");
		}
		has_previous_ = true;
		previous_vlf_ = vlf;
		previous_id_ = id;
		previous_size_ = size;
		block_.assign(head, head + size);
		in_ += size;
		if (size % SECTOR == 0) {
			RestoreSectorParity(block_.data(), size);
		}
		records_.clear();
		next_ = 0;
		ParseLogBlock(block_.data(), size, tail_.block_header_, where,
					  [&](const LogRecord &r) { records_.push_back(r); });
		if (!records_.empty()) {
			return true;
		}
	}
	return false;
}

bool LogTailCursor::Next(LogRecord &out) {
	while (next_ >= records_.size()) {
		if (!NextBlock()) {
			return false;
		}
	}
	out = records_[next_++];
	return true;
}

void LogTailReader::ForEach(const std::function<void(const LogRecord &)> &visit) const {
	LogTailCursor cursor(*this);
	LogRecord record;
	while (cursor.Next(record)) {
		visit(record);
	}
}

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb
