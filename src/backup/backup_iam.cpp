#include "backup/backup_iam.hpp"

#include "backup/backup_record.hpp"

#include <set>
#include <string>
#include <utility>

namespace duckdb {
namespace mssql {
namespace backup {

static constexpr uint8_t PAGE_TYPE_IAM = 10;
static constexpr size_t IAM_SEQUENCE_AT = 4;
static constexpr size_t IAM_START_AT = 40;
static constexpr size_t IAM_SINGLE_AT = 46;
static constexpr size_t IAM_HEADER_NEEDED = IAM_SINGLE_AT + 6 * IAM_SINGLE_PAGES;
static constexpr size_t IAM_BITMAP_BYTES = 7988;
static constexpr uint32_t GAM_INTERVAL_PAGES = 511232;
static constexpr uint32_t GAM_INTERVAL_EXTENTS = GAM_INTERVAL_PAGES / 8;

uint64_t AuPages::PageCount() const {
	uint64_t n = single_pages.size();
	for (const auto &r : extents) {
		n += 8ull * r.count;
	}
	return n;
}

IamPage ReadIamPage(const Page &page) {
	const std::string name = "the IAM page " + page.header.page_id.ToString();
	if (page.header.type != PAGE_TYPE_IAM) {
		throw FormatError("backup: page " + page.header.page_id.ToString() + " is of type " +
						  std::to_string(page.header.type) + ", not an IAM page (10)");
	}
	if (page.header.slot_count < 2) {
		throw FormatError("backup: " + name + " has " + std::to_string(page.header.slot_count) +
						  " records, not a header and a bitmap");
	}
	auto header = ReadSlot(page.bytes.data(), page.bytes.size(), page.header.slot_count, 0, "an IAM header");
	auto bitmap = ReadSlot(page.bytes.data(), page.bytes.size(), page.header.slot_count, 1, "an IAM bitmap");
	if (header.Type() != 0 || header.fixed_end < IAM_HEADER_NEEDED) {
		throw FormatError("backup: " + name + "'s header record is not a primary record of " +
						  std::to_string(IAM_HEADER_NEEDED) + " bytes or more");
	}
	if (bitmap.Type() != 0 || bitmap.fixed_end != 4 + IAM_BITMAP_BYTES) {
		throw FormatError("backup: " + name + "'s bitmap record is not a primary record of " +
						  std::to_string(4 + IAM_BITMAP_BYTES) + " bytes");
	}
	ByteReader h(header.data, header.fixed_end, "an IAM header", header.offset);
	IamPage iam;
	iam.sequence = h.U32(IAM_SEQUENCE_AT);
	iam.start = ReadPagePointer(h, IAM_START_AT);
	for (size_t i = 0; i < IAM_SINGLE_PAGES; i++) {
		iam.single[i] = ReadPagePointer(h, IAM_SINGLE_AT + 6 * i);
	}
	auto bits = bitmap.Fixed("an IAM bitmap");
	const uint8_t *b = bits.Bytes(0, IAM_BITMAP_BYTES);
	iam.extents.assign(b, b + IAM_BITMAP_BYTES);
	return iam;
}

AuPages ReadIamChain(const PageStore &store, PagePointer first_iam, uint64_t au) {
	AuPages pages;
	std::set<std::pair<uint16_t, uint32_t>> seen;
	std::set<std::pair<uint16_t, uint32_t>> intervals;
	std::set<std::pair<uint16_t, uint32_t>> singles;
	PagePointer at = first_iam;
	PagePointer previous;
	Page page;
	uint32_t last_sequence = 0;
	for (size_t position = 0; !at.IsNull(); position++) {
		if (!seen.insert(std::make_pair(at.file, at.page)).second) {
			throw FormatError("backup: the IAM chain of allocation unit " + std::to_string(au) +
							  " comes back to page " + at.ToString());
		}
		store.ReadLive(at.file, at.page, page, "an IAM chain");
		const IamPage iam = ReadIamPage(page);
		const std::string name = "the IAM page " + at.ToString();
		if (page.header.AllocationUnit() != au) {
			throw FormatError("backup: " + name + " belongs to allocation unit " +
							  std::to_string(page.header.AllocationUnit()) + ", not " + std::to_string(au));
		}
		// Ascending, not from 0 nor without gaps: once a file is emptied and removed, the pages that
		// mapped it leave the chain unrenumbered, and a chain can start at 1 (E19).
		if (position > 0 && iam.sequence <= last_sequence) {
			throw FormatError("backup: " + name + " is number " + std::to_string(iam.sequence) +
							  " of its chain, after number " + std::to_string(last_sequence));
		}
		last_sequence = iam.sequence;
		if (!(page.header.prev == previous)) {
			throw FormatError("backup: " + name + "'s previous page is " + page.header.prev.ToString() + ", not " +
							  (previous.IsNull() ? std::string("none") : previous.ToString()));
		}
		if (iam.start.page % GAM_INTERVAL_PAGES != 0 || iam.start.file == 0) {
			throw FormatError("backup: " + name + " maps from page " + iam.start.ToString() +
							  ", which opens no GAM interval");
		}
		if (!intervals.insert(std::make_pair(iam.start.file, iam.start.page)).second) {
			throw FormatError("backup: " + name + " maps the interval from " + iam.start.ToString() +
							  ", which an earlier page of its chain maps");
		}
		pages.iam_pages.push_back(at);
		for (const auto &single : iam.single) {
			if (single.IsNull()) {
				continue;
			}
			// The single-page slots are the first IAM page's (Randal: "only used in the first IAM page").
			if (position != 0) {
				throw FormatError("backup: " + name + ", page " + std::to_string(position + 1) +
								  " of its chain, lists a single page (" + single.ToString() + ")");
			}
			if (!singles.insert(std::make_pair(single.file, single.page)).second) {
				throw FormatError("backup: " + name + " lists the single page " + single.ToString() + " twice");
			}
			pages.single_pages.push_back(single);
		}
		// The bitmap's set bits, as runs of the start page's file.
		const uint32_t base = iam.start.page / 8;
		for (uint32_t i = 0; i < GAM_INTERVAL_EXTENTS; i++) {
			if (!((iam.extents[i / 8] >> (i % 8)) & 1)) {
				continue;
			}
			if (static_cast<uint64_t>(base) + i >= (static_cast<uint64_t>(1) << 29)) {
				throw FormatError("backup: " + name + " lists extent " + std::to_string(uint64_t(base) + i) +
								  ", past the last page a 32-bit page id names");
			}
			auto &runs = pages.extents;
			if (!runs.empty() && runs.back().file == iam.start.file &&
				runs.back().first + runs.back().count == base + i) {
				runs.back().count++;
			} else {
				IamExtentRun run;
				run.file = iam.start.file;
				run.first = base + i;
				run.count = 1;
				runs.push_back(run);
			}
		}
		previous = at;
		at = page.header.next;
	}
	// A single page inside one of the AU's own extents would be listed twice.
	for (const auto &single : pages.single_pages) {
		for (const auto &run : pages.extents) {
			if (run.file == single.file && single.page / 8 >= run.first && single.page / 8 < run.first + run.count) {
				throw FormatError("backup: the IAM chain of allocation unit " + std::to_string(au) + " lists page " +
								  single.ToString() + " both as a single page and in one of its extents");
			}
		}
	}
	return pages;
}

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb
