#include "backup/backup_record.hpp"

#include <algorithm>
#include <cstring>
#include <string>

namespace duckdb {
namespace mssql {
namespace backup {

static constexpr size_t PAGE_HEADER_BYTES = 96;
static constexpr uint8_t STATUS_NULL_BITMAP = 0x10;
static constexpr uint8_t STATUS_VARIABLE = 0x20;

bool RecordView::IsNull(size_t column) const {
	if (!null_bitmap || column >= column_count) {
		return false;
	}
	return (data[null_bitmap + column / 8] >> (column % 8)) & 1;
}

uint16_t RecordView::End() const {
	if (fixed_end == 0) {
		return 0;
	}
	const uint16_t end = var_count ? VarEnd(var_count - 1) : var_start;
	return static_cast<uint16_t>(end + ((status_a & 0x40) ? 14 : 0));
}

uint16_t RecordView::VarStart(size_t i) const {
	return i == 0 ? var_start : static_cast<uint16_t>(RawVarEnd(i - 1) & 0x7FFF);
}

uint16_t RecordView::VarEnd(size_t i) const {
	return static_cast<uint16_t>(RawVarEnd(i) & 0x7FFF);
}

ByteReader RecordView::Var(size_t i, const char *what) const {
	if (i >= var_count) {
		throw FormatError(std::string("backup: ") + what + ": variable column " + std::to_string(i) +
						  " of a record with " + std::to_string(var_count));
	}
	const uint16_t start = VarStart(i);
	return ByteReader(data + start, static_cast<size_t>(VarEnd(i) - start), what, offset + start);
}

ByteReader RecordView::Fixed(const char *what) const {
	if (fixed_end < 4) {
		throw FormatError(std::string("backup: ") + what + ": a record of type " + std::to_string(Type()) +
						  " has no FixedVar fixed part");
	}
	return ByteReader(data + 4, static_cast<size_t>(fixed_end - 4), what, offset + 4u);
}

RecordView ReadRecord(const uint8_t *page, size_t page_size, size_t slot_array, uint16_t offset, const char *what) {
	if (offset < PAGE_HEADER_BYTES || slot_array > page_size || offset + 4u > slot_array) {
		throw FormatError(std::string("backup: ") + what + " at page offset " + std::to_string(offset) +
						  " lies outside the page's records");
	}
	RecordView r;
	r.data = page + offset;
	r.offset = offset;
	r.limit = static_cast<uint16_t>(slot_array - offset);
	ByteReader b(r.data, r.limit, what, offset);
	r.status_a = b.U8(0);
	r.status_b = b.U8(1);
	if (r.Type() == RECORD_FORWARDING_STUB || r.Type() == RECORD_BLOB_FRAGMENT || r.Type() == RECORD_INDEX ||
		r.Type() == RECORD_GHOST_INDEX || r.Type() == RECORD_GHOST_VERSION) {
		// Not FixedVar: a stub is a 9-byte pointer to the row's new place, a LOB fragment has its own
		// header (spec 085 § Rules, records / LOBs), and an index record is its status byte, its keys
		// packed to the page's pminlen and a child pointer, with no statusB and no fixed end @2
		// (DbReader's PROVENANCE, "Non-leaf index record layout"). A version ghost (a heap row deleted
		// under row versioning, statusA 0x4E) has a layout of its own whose bytes 2..3 are no fixed
		// end (E20: 0x0000). The view carries the status bytes only; the caller skips a stub and a
		// version ghost, reads a fragment with the LOB reader, an index record with the page's pminlen.
		return r;
	}
	r.fixed_end = b.U16(2);
	if (r.fixed_end < 4 || r.fixed_end > r.limit) {
		b.Fail(2, "a fixed part ending at " + std::to_string(r.fixed_end) + ", outside 4.." + std::to_string(r.limit));
	}
	size_t at = r.fixed_end;
	// The column count belongs to the null bitmap's section: a record without one (an index record of
	// NOT NULL columns) has its variable column count right at fixed_end.
	if (r.status_a & STATUS_NULL_BITMAP) {
		r.column_count = b.U16(at);
		at += 2;
		r.null_bitmap = static_cast<uint16_t>(at);
		at += (r.column_count + 7u) / 8u;
		b.Require(r.null_bitmap, at - r.null_bitmap);
	}
	if (r.status_a & STATUS_VARIABLE) {
		r.var_count = b.U16(at);
		at += 2;
		b.Require(at, 2u * r.var_count);
		r.var_ends = r.data + at;
		at += 2u * r.var_count;
	}
	r.var_start = static_cast<uint16_t>(at);
	uint16_t previous = r.var_start;
	for (size_t i = 0; i < r.var_count; i++) {
		const uint16_t end = r.VarEnd(i);
		if (end < previous || end > r.limit) {
			b.Fail(r.var_start, "variable column " + std::to_string(i) + " ends at " + std::to_string(end) +
									", outside " + std::to_string(previous) + ".." + std::to_string(r.limit));
		}
		previous = end;
	}
	return r;
}

RecordView ReadSlot(const uint8_t *page, size_t page_size, uint16_t slot_count, uint16_t slot, const char *what) {
	if (slot >= slot_count || 2u * slot_count + PAGE_HEADER_BYTES > page_size) {
		throw FormatError(std::string("backup: ") + what + ": slot " + std::to_string(slot) + " of a page with " +
						  std::to_string(slot_count) + " slots");
	}
	ByteReader p(page, page_size, what, 0);
	const uint16_t offset = p.U16(page_size - 2 - 2 * static_cast<size_t>(slot));
	return ReadRecord(page, page_size, page_size - 2u * slot_count, offset, what);
}

FileHeaderRecord ReadFileHeaderRecord(const uint8_t *page, size_t page_size) {
	auto r = ReadSlot(page, page_size, 1, 0, "the file header record");
	if (r.Type() != 0 || r.var_count < 5) {
		throw FormatError("backup: the file header record is of type " + std::to_string(r.Type()) + " with " +
						  std::to_string(r.var_count) + " variable columns, not a primary record with 5 or more");
	}
	auto id = r.Var(2, "the file header's file id");
	auto filegroup = r.Var(3, "the file header's filegroup id");
	auto size = r.Var(4, "the file header's Size");
	if (id.Size() != 2 || filegroup.Size() != 2 || size.Size() != 4) {
		throw FormatError("backup: the file header record's file id, filegroup and Size are " +
						  std::to_string(id.Size()) + ", " + std::to_string(filegroup.Size()) + " and " +
						  std::to_string(size.Size()) + " bytes, not 2, 2 and 4");
	}
	FileHeaderRecord h;
	h.file_id = id.U16(0);
	h.filegroup_id = filegroup.U16(0);
	h.size = size.U32(0);
	return h;
}

void RecordSpans(const uint8_t *page, size_t page_size, uint16_t slot_count, std::vector<RecordSpan> &out) {
	out.clear();
	if (static_cast<size_t>(slot_count) * 2 + PAGE_HEADER_BYTES > page_size) {
		return;	 // the page's reader names it
	}
	const size_t slot_array = page_size - 2u * slot_count;
	for (uint16_t slot = 0; slot < slot_count; slot++) {
		const size_t pos = page_size - 2 - 2 * static_cast<size_t>(slot);
		const uint16_t offset = static_cast<uint16_t>(page[pos] | (page[pos + 1] << 8));
		// Within the records' area only: an offset at or past the slot array has no bytes to compare
		// (review of #580: SameRecords compared from it).
		if (offset >= PAGE_HEADER_BYTES && offset < slot_array) {
			RecordSpan span;
			span.offset = offset;
			span.slot = slot;
			out.push_back(span);
		}
	}
	std::sort(out.begin(), out.end(), [](const RecordSpan &a, const RecordSpan &b) { return a.offset < b.offset; });
	for (size_t i = 0; i < out.size(); i++) {
		out[i].end = static_cast<uint16_t>(i + 1 < out.size() ? out[i + 1].offset : slot_array);
	}
}

void SameRecords(const uint8_t *old_page, uint16_t old_slots, const uint8_t *new_page, uint16_t new_slots,
				 size_t page_size, bool same_slot, std::vector<bool> &old_same, std::vector<bool> &new_same) {
	static thread_local std::vector<RecordSpan> old_spans, new_spans;  // kept by the thread
	RecordSpans(old_page, page_size, old_slots, old_spans);
	RecordSpans(new_page, page_size, new_slots, new_spans);
	old_same.assign(old_slots, false);
	new_same.assign(new_slots, false);
	size_t o = 0;
	for (const auto &n : new_spans) {
		while (o < old_spans.size() && old_spans[o].offset < n.offset) {
			o++;
		}
		if (o == old_spans.size()) {
			break;
		}
		const auto &old_span = old_spans[o];
		if (old_span.offset == n.offset && old_span.end == n.end && n.end > n.offset &&
			(!same_slot || old_span.slot == n.slot) &&
			std::memcmp(old_page + n.offset, new_page + n.offset, n.end - n.offset) == 0) {
			old_same[old_span.slot] = true;
			new_same[n.slot] = true;
		}
	}
}

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb
