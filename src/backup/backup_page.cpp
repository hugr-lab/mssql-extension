#include "backup/backup_page.hpp"

#include <algorithm>

namespace duckdb {
namespace mssql {
namespace backup {

static constexpr uint8_t PAGE_HEADER_VERSION = 1;
static constexpr uint8_t PAGE_TYPE_PAD = 101;

std::string PagePointer::ToString() const {
	return std::to_string(file) + ":" + std::to_string(page);
}

PagePointer ReadPagePointer(const ByteReader &reader, size_t offset) {
	PagePointer p;
	p.page = reader.U32(offset);
	p.file = reader.U16(offset + 4);
	return p;
}

PageHeader ReadPageHeader(const uint8_t *page) {
	ByteReader r(page, PAGE_HEADER_SIZE, "a page header", 0);
	PageHeader h;
	h.version = r.U8(0);
	h.type = r.U8(1);
	h.type_flags = r.U8(2);
	h.level = r.U8(3);
	h.flags = r.U16(4);
	h.index_id = r.U16(6);
	h.prev = ReadPagePointer(r, 8);
	h.fixed_length = r.U16(14);
	h.next = ReadPagePointer(r, 16);
	h.slot_count = r.U16(22);
	h.object_id = r.U32(24);
	h.free_count = r.U16(28);
	h.free_data = r.U16(30);
	h.page_id = ReadPagePointer(r, 32);
	h.ghost_count = r.U16(58);
	return h;
}

PageStore::PageStore(const PageIndex &index, const AllocationMap &allocation)
	: index_(index), allocation_(allocation) {}

bool PageStore::Read(uint16_t file, uint32_t page, Page &out) const {
	const PageState state = Fetch(file, page, out);
	if (state == PageState::Pending) {
		PagePointer p;
		p.file = file;
		p.page = page;
		throw FormatError("backup: page " + p.ToString() +
						  " became live while the backup ran: its content is in the backup's log tail only (a fuzzy "
						  "backup), which is not replayed yet");
	}
	return state == PageState::Present;
}

bool PageStore::IsPendingCopy(const PagePointer &want, const Page &copy) const {
	// Only a backup that ran under changes: a second pass is the server copying what changed during the
	// first (load fixture, 2022). In a quiet backup -- one pass -- a live page whose copy is a zero
	// unit is a locator's or a short backup's fault, refused below as not a page.
	bool later_pass = false;
	for (size_t i = 0; i < index_.PassCount(); i++) {
		later_pass = later_pass || index_.Pass(i).number > 0;
	}
	if (!later_pass) {
		return false;
	}
	// A unit with bytes in it that names another page (E6's F2 units carry a page id) is a misplaced
	// copy, not a pending one: the id check below refuses it.
	const bool zero = std::all_of(copy.bytes.begin(), copy.bytes.end(), [](uint8_t b) { return b == 0; });
	return zero || copy.header.page_id == want;
}

PageState PageStore::Fetch(uint16_t file, uint32_t page, Page &out) const {
	if (!allocation_.IsLive(file, page)) {
		return PageState::NotLive;
	}
	PagePointer want;
	want.file = file;
	want.page = page;
	// A live page is in a full backup's first pass (E12): an index that does not find it searched
	// wrongly or the backup is short, and either way the page is not there to read.
	if (!index_.ReadPage(file, page, out.bytes, true)) {
		throw FormatError("backup: page " + want.ToString() + " is live, and the backup carries no copy of it");
	}
	if (out.bytes.size() != PAGE_SIZE) {
		throw FormatError("backup: page " + want.ToString() + " read as " + std::to_string(out.bytes.size()) +
						  " bytes");
	}
	out.header = ReadPageHeader(out.bytes.data());
	if (out.header.version == 0 && IsPendingCopy(want, out)) {
		// A zero unit where a live page is: it became live while the backup ran (PageState::Pending).
		return PageState::Pending;
	}
	if (out.header.version != PAGE_HEADER_VERSION) {
		throw FormatError("backup: page " + want.ToString() + " is live, and its copy is not a page (header version " +
						  std::to_string(out.header.version) + ")");
	}
	if (out.header.type == PAGE_TYPE_PAD) {
		// A pad is never read further (spec 085 § Rules, page index), whatever page it names.
		throw FormatError("backup: the copy of page " + want.ToString() + " is a pad unit");
	}
	if (!(out.header.page_id == want)) {
		throw FormatError("backup: the copy of page " + want.ToString() + " says it is page " +
						  out.header.page_id.ToString());
	}
	return PageState::Present;
}

void PageStore::ReadLive(uint16_t file, uint32_t page, Page &out, const char *what) const {
	if (!Read(file, page, out)) {
		PagePointer p;
		p.file = file;
		p.page = page;
		throw FormatError(std::string("backup: ") + what + " points to page " + p.ToString() +
						  ", which the database does not use (not live)");
	}
}

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb
