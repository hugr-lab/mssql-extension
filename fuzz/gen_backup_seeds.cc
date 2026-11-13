// Writes the seed corpora of the backup reader's fuzz harnesses (spec 085 task 10) into
// fuzz/corpus/backup_*: synthetic inputs only (the test builders' images), never a backup made on
// a SQL Server whose data is not ours to publish (spec 085 T4).
//
//   c++ -std=c++17 -I src/include -I test/cpp/backup fuzz/gen_backup_seeds.cc src/backup/backup_bytes.cpp \
//       src/backup/backup_mtf.cpp src/backup/backup_page_index.cpp src/backup/backup_pass_search.cpp \
//       src/backup/backup_allocation.cpp src/backup/backup_record.cpp src/backup/backup_page.cpp \
//       src/backup/backup_container.cpp src/backup/backup_xpress.cpp src/backup/backup_system_tables.cpp \
//       src/backup/backup_iam.cpp -o /tmp/gen_backup_seeds && /tmp/gen_backup_seeds fuzz/corpus   # from the root
//
// backup_mtf: the builder's image as a full, a log, a differential, a forked and an ANSI-string backup.
// backup_container: a full backup whose pass is real pages, in an MSSQLBAK container laid out as E13
// found them (one stored 64 KB block per extent group, the media label at 0x1E0 as a ZSTD-flagged block
// the harness's copying decoder pads, the seek index and the tail). No seed ends in .bak: the
// repository ignores *.bak, so that no real backup is committed by accident. backup_xpress:
// literal runs under a 9-bit code. backup_page: compact passes (fuzz_backup_page's format) in order, striped, and out
// of order. backup_catalog (fuzz_backup_catalog's format: options, a LOB root, pages): pages of the committed
// synthetic sample test/data/backup/full.bak (made by test/backup/gen/generate.sh on a throwaway instance) -- the
// boot page and sysallocunits, the other bootstrap tables, a mix with an IAM page -- and a synthetic LOB tree.
// backup_record (fuzz_backup_record's format: a layout, then a page): the sample's two user tables' first
// pages with their layouts, and pages of the record forms E20 measured.
// backup_lob (fuzz_backup_lob's format): every off-row form over synthetic text pages -- an in-row root
// over an INTERNAL node, a last leaf holding more than claimed, row-overflow, root pointers to a LARGE_ROOT,
// a SMALL_ROOT and a NULL root -- and a map.

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "backup/backup_bytes.hpp"
#include "backup/backup_mtf.hpp"
#include "backup/backup_page.hpp"
#include "backup/backup_page_index.hpp"
#include "backup/backup_system_tables.hpp"
#include "mtf_builder.hpp"

using namespace duckdb::mssql::backup;
using namespace mtf_builder;

static std::string out_dir;

static void Write(const std::string &target, const std::string &name, const Bytes &bytes) {
	const std::string dir = out_dir + "/" + target;
	std::string mk = "mkdir -p '" + dir + "'";
	if (std::system(mk.c_str()) != 0) {
		std::exit(1);
	}
	FILE *f = std::fopen((dir + "/" + name).c_str(), "wb");
	std::fwrite(bytes.data(), 1, bytes.size(), f);
	std::fclose(f);
	std::printf("%s/%s: %zu bytes\n", target.c_str(), name.c_str(), bytes.size());
}

static void Header(Bytes &file, size_t at, uint8_t kind, size_t output, size_t payload) {
	file[at] = kind;
	file[at + 1] = static_cast<uint8_t>(output / 512);
	Put16(file, at + 2, static_cast<uint16_t>(payload == 65536 ? 0 : payload));
}

//! One 8 KB page of file 1: the file header (its slot 0 record names file 1 and its Size), a PFS
//! page (every page allocated), a GAM page (every extent allocated), or a data page.
static Bytes RealPage(uint32_t page, uint32_t size) {
	Bytes p(8192, 0);
	p[0] = 1;
	p[1] = page == 0 ? 15 : page == 1 ? 11 : page == 2 ? 8 : 1;
	Put32(p, 32, page);
	Put16(p, 36, 1);
	if (page == 0) {
		Put16(p, 8190, 96);
		const Bytes record = FileHeaderRecordBytes(1, size);
		std::memcpy(&p[96], record.data(), record.size());
	} else if (page == 1) {
		std::memset(&p[100], 0x40, size);
	}
	return p;
}

//! A full backup whose pass is real pages (extents `extents` of file 1, 0xFFFFFFFF a zero group), in
//! an MSSQLBAK container laid out as E13 found them: the pass's units on a block boundary, one
//! 64 KB block per extent group, seek index entries at the pass's end and at MSLS, the media label
//! at 0x1E0 and the tail.
static Bytes Container(const std::vector<uint32_t> &extents) {
	Options o;
	o.align_blocks = true;
	o.mqda_bytes = 2 + extents.size() * 65536;
	// Where MSDA starts in the data set (from SSET) does not depend on its header's size: pad the
	// header so the MQDA units (after its 22-byte stream header and 2-byte lead-in) start at 512.
	{
		auto probe = BuildImage(o);
		MemoryAccess m("probe", probe.data(), probe.size());
		auto l = WalkMtf(m);
		const size_t rel = static_cast<size_t>(l.Find("MSDA")->offset - l.Find("SSET")->offset);
		o.msda_header = 60 + (512 - (rel + 60 + 24) % 512) % 512;
	}
	uint32_t size = 0;
	for (uint32_t e : extents) {
		size = e == 0xFFFFFFFFu ? size : std::max(size, (e + 1) * 8);
	}
	o.mqda_content.assign(2, 0);
	for (uint32_t e : extents) {
		for (uint32_t k = 0; k < 8; k++) {
			Bytes page = e == 0xFFFFFFFFu ? Bytes(8192, 0) : RealPage(e * 8 + k, size);
			o.mqda_content.insert(o.mqda_content.end(), page.begin(), page.end());
		}
	}
	const Bytes image = BuildImage(o);
	MemoryAccess plain("image", image.data(), image.size());
	auto layout = WalkMtf(plain);
	const auto *sset = layout.Find("SSET");
	const auto *msda = layout.Find("MSDA");
	const auto *msls = layout.Find("MSLS");
	Bytes data(image.begin() + sset->offset, image.begin() + msls->end);
	const uint64_t units = msda->Stream("MQDA")->data_offset + 2 - sset->offset;
	const uint64_t units_end = units + extents.size() * 65536;
	const uint64_t msls_at = msls->offset - sset->offset;
	// Block boundaries: up to 64 KB pieces, cut at the pass's units, its end and MSLS.
	std::vector<uint64_t> cuts = {0, units, units_end, msls_at, data.size()};
	Bytes file(8192, 0);
	std::memcpy(&file[0], "MSSQLBAK", 8);
	Put32(file, 8, 2);
	// ZSTD (flags 0x83): fuzz_backup_container hands ZSTD blocks to a decoder that copies and pads.
	Put32(file, 0x0C, 0x83);
	// The media label: the TAPE block's bytes to its SPAD header, a "compressed" block of 4 KB output at
	// 0x1E0, as every container has it (a stored 4 KB block would not fit in the header).
	const auto &tape = layout.blocks.front();
	const size_t tape_bytes = static_cast<size_t>(tape.streams.back().header_offset + 22);
	Header(file, 0x1E0, 0x21, 4096, tape_bytes);
	std::memcpy(&file[0x1E0 + 28], image.data(), tape_bytes);
	PutTag(file, 4096, "SFMB");
	Put16(file, 4096 + 50, Xor16(file, 4096, 50));
	std::vector<std::pair<uint64_t, uint64_t>> entries;	 // logical, physical
	for (size_t c = 0; c + 1 < cuts.size(); c++) {
		for (uint64_t at = cuts[c]; at < cuts[c + 1]; at += 65536) {
			const size_t n = static_cast<size_t>(std::min<uint64_t>(65536, cuts[c + 1] - at));
			if (at == units_end || at == msls_at) {
				entries.push_back({at, file.size()});
			}
			Bytes h(28, 0);
			Header(h, 0, 0x23, n, n);
			file.insert(file.end(), h.begin(), h.end());
			file.insert(file.end(), data.begin() + at, data.begin() + at + n);
		}
	}
	const uint64_t index_at = file.size();
	Bytes index(40 + 16 * entries.size(), 0);
	Put32(index, 0, 44);
	Put32(index, 24, 0x00010001);
	Put32(index, 28, static_cast<uint32_t>(entries.size()));
	for (size_t e = 0; e < entries.size(); e++) {
		Put64(index, 40 + 16 * e, entries[e].first);
		Put64(index, 48 + 16 * e, entries[e].second);
	}
	file.insert(file.end(), index.begin(), index.end());
	while ((file.size() + 16) % 4096 != 0) {
		file.push_back(0);
	}
	Bytes record(16, 0);
	Put16(record, 0, 1);
	Put16(record, 2, 1);
	Put32(record, 4, static_cast<uint32_t>(entries.size()));
	Put32(record, 8, static_cast<uint32_t>(file.size() - index_at - 8));
	file.insert(file.end(), record.begin(), record.end());
	Bytes sfmb(4096, 0);
	PutTag(sfmb, 0, "SFMB");
	file.insert(file.end(), sfmb.begin(), sfmb.end());
	Bytes eset(4096, 0);
	Header(eset, 0, 0x23, 512, 512);
	file.insert(file.end(), eset.begin(), eset.end());
	file.insert(file.end(), sfmb.begin(), sfmb.end());
	return file;
}

static Bytes Xpress(size_t literals) {
	Bytes in;
	const size_t output = literals;
	in.push_back(static_cast<uint8_t>((output - 1) & 0xFF));
	in.push_back(static_cast<uint8_t>((output - 1) >> 8));
	in.insert(in.end(), 256, 0x99);	 // every one of the 512 symbols 9 bits: a complete canonical code
	// Literals 'a'..'z' repeated: symbol s has code s; 9-bit codes packed MSB first into u16 words.
	std::vector<bool> bits;
	for (size_t i = 0; i < literals; i++) {
		const unsigned symbol = 'a' + i % 26;
		for (int b = 8; b >= 0; b--) {
			bits.push_back((symbol >> b) & 1);
		}
	}
	while (bits.size() % 16 != 0 || bits.size() < 64) {
		bits.push_back(false);
	}
	for (size_t w = 0; w < bits.size(); w += 16) {
		uint16_t word = 0;
		for (int b = 0; b < 16; b++) {
			word = static_cast<uint16_t>((word << 1) | (bits[w + b] ? 1 : 0));
		}
		in.push_back(static_cast<uint8_t>(word & 0xFF));
		in.push_back(static_cast<uint8_t>(word >> 8));
	}
	return in;
}

//! fuzz_backup_page's format: flags, then a 64-byte chunk per unit, tiled across the unit (byte j at
//! every offset j mod 64). Page 0's chunk puts the file header's slot (u16 at 8190, offsets 62..63) at
//! 131 and its record (FileHeaderRecordBytes with an empty first column: 27 bytes) at offsets 3..29,
//! clear of the page header's bytes in the chunk (0, 1, 32..37); page 1's (PFS) fills with 0x40, so its
//! pages are allocated; page 2's (GAM) is zeros
//! beside its header, so its extents are allocated. `extents` in pass order; 0xFFFFFFFF a zero group.
static Bytes PassSeed(uint8_t flags, const std::vector<uint32_t> &extents) {
	uint32_t size = 0;
	for (uint32_t e : extents) {
		size = e == 0xFFFFFFFFu ? size : std::max(size, (e + 1) * 8);
	}
	Bytes in(1, flags);
	for (uint32_t e : extents) {
		for (uint32_t k = 0; k < 8; k++) {
			Bytes chunk(64, 0);
			if (e != 0xFFFFFFFFu) {
				const uint32_t page = e * 8 + k;
				if (page == 1) {
					std::memset(chunk.data(), 0x40, chunk.size());
				}
				chunk[0] = 1;
				chunk[1] = page == 0 ? 15 : page == 1 ? 11 : page == 2 ? 8 : 1;
				Put32(chunk, 32, page);
				Put16(chunk, 36, 1);
				if (page == 0) {
					Put16(chunk, 62, 131);
					const Bytes record = FileHeaderRecordBytes(1, size, 0);
					std::memcpy(&chunk[3], record.data(), record.size());
				}
			}
			in.insert(in.end(), chunk.begin(), chunk.end());
		}
	}
	return in;
}

//! fuzz_backup_catalog's input: no option, `root`, then `pages`.
static Bytes CatalogInput(const std::vector<Bytes> &pages, const Bytes &root = Bytes()) {
	Bytes out = {0, static_cast<uint8_t>(root.size())};
	out.insert(out.end(), root.begin(), root.end());
	for (const auto &p : pages) {
		out.insert(out.end(), p.begin(), p.end());
	}
	return out;
}

//! One column of fuzz_backup_record's layout: its xtype, the ti bytes above it, offset, null bit, bit
//! position and flags (fuzz/fuzz_backup_record.cc).
struct RecordColumn {
	uint8_t xtype;
	uint32_t ti_high;  // ti >> 8
	int16_t offset;
	uint16_t null_bit;
	uint8_t bit = 0;
	uint8_t flags = 0;
};

//! fuzz_backup_record's input: the column count, 10 bytes a column, then the page.
static Bytes RecordInput(const std::vector<RecordColumn> &columns, const Bytes &page) {
	Bytes out = {static_cast<uint8_t>(columns.size())};
	for (const auto &c : columns) {
		out.push_back(c.xtype);
		out.push_back(static_cast<uint8_t>(c.ti_high));
		out.push_back(static_cast<uint8_t>(c.ti_high >> 8));
		out.push_back(static_cast<uint8_t>(c.ti_high >> 16));
		out.push_back(static_cast<uint8_t>(c.offset));
		out.push_back(static_cast<uint8_t>(static_cast<uint16_t>(c.offset) >> 8));
		out.push_back(static_cast<uint8_t>(c.null_bit));
		out.push_back(static_cast<uint8_t>(c.null_bit >> 8));
		out.push_back(c.bit);
		out.push_back(c.flags);
	}
	out.insert(out.end(), page.begin(), page.end());
	return out;
}

static Bytes Hex(const std::string &hex) {
	Bytes out;
	for (size_t i = 0; i + 1 < hex.size(); i += 2) {
		out.push_back(static_cast<uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
	}
	return out;
}

//! An 8 KB data page holding `records` from offset 96, slot i at record i (its header's type and slot
//! count set, the rest zero).
static Bytes RecordPage(const std::vector<Bytes> &records) {
	Bytes page(8192, 0);
	page[0] = 1;
	page[1] = 1;
	size_t at = 96;
	for (size_t i = 0; i < records.size(); i++) {
		std::copy(records[i].begin(), records[i].end(), page.begin() + at);
		page[8190 - 2 * i] = static_cast<uint8_t>(at);
		page[8191 - 2 * i] = static_cast<uint8_t>(at >> 8);
		at += records[i].size();
	}
	page[22] = static_cast<uint8_t>(records.size());
	page[23] = static_cast<uint8_t>(records.size() >> 8);
	return page;
}

//! The pages of file 1 of the sample `path`, by the allocation unit their headers name; data pages of
//! level 0 only, and the boot page and IAM pages beside them.
struct SamplePages {
	std::map<uint64_t, std::vector<Bytes>> data;  // by allocation unit
	Bytes boot;
	Bytes iam;
};

static SamplePages ReadSample(const std::string &path) {
	std::ifstream in(path, std::ios::binary);
	const std::vector<uint8_t> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	if (file.empty()) {
		std::fprintf(stderr, "cannot read %s\n", path.c_str());
		std::exit(1);
	}
	MemoryAccess access(path, file.data(), file.size());
	const MtfLayout layout = WalkMtf(access);
	std::vector<FamilyStream> streams(1);
	streams[0].stream = &access;
	streams[0].layout = &layout;
	PageIndexOptions options;
	options.full_backup = true;
	const PageIndex index = BuildPageIndex(streams, options);
	SamplePages out;
	std::vector<uint8_t> bytes;
	for (uint32_t p = 0; p < 2048; p++) {
		if (!index.ReadPage(1, p, bytes) || bytes.size() != 8192 || bytes[0] != 1) {
			continue;
		}
		const PageHeader h = ReadPageHeader(bytes.data());
		if (p == 9 && h.type == 13) {
			out.boot = bytes;
		} else if (h.type == 10 && out.iam.empty()) {
			out.iam = bytes;
		} else if (h.type == 1 && h.level == 0) {
			out.data[h.AllocationUnit()].push_back(bytes);
		}
	}
	return out;
}

//! The pages of `pages` (bootstrap table `table`'s) holding a row that describes one of the five
//! bootstrap tables themselves: an object id among `objects` (sysschobjs, syscolpars), or a rowset
//! among `rowsets` (sysallocunits' ownerid, sysrscols' rsid); sysrowsets' rows of those objects add
//! their rowsets to `rowsets`. These are the rows the reader's self-description check needs.
static std::vector<Bytes> Describing(const std::vector<Bytes> &pages, const std::string &table,
									 const std::set<int64_t> &objects, std::set<int64_t> &rowsets) {
	const RowsetLayout &layout = BootstrapLayout(table);
	const char *key = table == "sysrowsets"		 ? "idmajor"
					  : table == "sysallocunits" ? "ownerid"
					  : table == "sysrscols"	 ? "rsid"
												 : "id";
	const size_t k = layout.IndexOf(key);
	std::vector<Bytes> out;
	for (const Bytes &bytes : pages) {
		Page page;
		page.bytes = bytes;
		page.header = ReadPageHeader(bytes.data());
		bool keep = false;
		ForEachPageRow(page, layout, [&](const SystemRow &row) {
			const int64_t v = ValueInt(layout, k, row.values[k]);
			if (table == "sysrowsets" && objects.count(v)) {
				rowsets.insert(ValueInt(layout, layout.IndexOf("rowsetid"), row.values[layout.IndexOf("rowsetid")]));
				keep = true;
			} else if ((table == "sysallocunits" || table == "sysrscols") ? rowsets.count(v) > 0
																		  : objects.count(v) > 0) {
				keep = keep || table != "sysrowsets";
			}
		});
		if (keep) {
			out.push_back(bytes);
		}
	}
	return out;
}

static std::vector<Bytes> First(const std::vector<Bytes> &pages, size_t n) {
	return std::vector<Bytes>(pages.begin(), pages.begin() + static_cast<long>(std::min(n, pages.size())));
}

//! A text page of `records` (fragment records), file 1, allocation unit 255 << 16 (no bootstrap table's).
static Bytes TextPage(uint32_t number, const std::vector<Bytes> &records) {
	Bytes p(8192, 0);
	p[0] = 1;
	p[1] = 3;
	Put16(p, 22, static_cast<uint16_t>(records.size()));
	Put32(p, 24, 255);
	Put32(p, 32, number);
	Put16(p, 36, 1);
	size_t at = 96;
	for (size_t i = 0; i < records.size(); i++) {
		std::memcpy(&p[at], records[i].data(), records[i].size());
		Put16(p, 8190 - 2 * i, static_cast<uint16_t>(at));
		at += records[i].size();
	}
	return p;
}

//! A LOB fragment record of `type` (0 SMALL_ROOT, 2 INTERNAL, 3 DATA, 5 LARGE_ROOT, 8 NULL), `body` after
//! its 14-byte header.
static Bytes Fragment(uint16_t type, const Bytes &body) {
	Bytes r(14, 0);
	r[0] = 0x08;
	Put16(r, 2, static_cast<uint16_t>(14 + body.size()));
	Put16(r, 12, type);
	r.insert(r.end(), body.begin(), body.end());
	return r;
}

static const Bytes HELLO = {'h', 'e', 'l', 'l', 'o'}, ABC = {'a', 'b', 'c'};

//! Page 100: DATA "hello" in slot 0, DATA "abc" in slot 1.
static Bytes LeafPage() {
	return TextPage(100, {Fragment(3, HELLO), Fragment(3, ABC)});
}

//! An in-row root (kind 5) of `level` whose links (cumulative, page, slot) name file 1.
static Bytes InRowRoot(uint8_t level, const std::vector<std::array<uint32_t, 3>> &links) {
	Bytes root(12 + 12 * links.size(), 0);
	root[0] = 5;
	root[2] = level;
	for (size_t i = 0; i < links.size(); i++) {
		Put32(root, 12 + 12 * i, links[i][0]);
		Put32(root, 16 + 12 * i, links[i][1]);
		Put16(root, 20 + 12 * i, 1);
		Put16(root, 22 + 12 * i, static_cast<uint16_t>(links[i][2]));
	}
	return root;
}

//! A root pointer (16 bytes) to page `page` of file 1, slot 0.
static Bytes RootPointer(uint32_t page) {
	Bytes pointer(16, 0);
	Put32(pointer, 8, page);
	Put16(pointer, 12, 1);
	return pointer;
}

//! A synthetic LOB tree: DATA fragments on page 100 and an INTERNAL node over them on page 101, and an
//! in-row root of level 1 over the node (E21).
static Bytes LobInput() {
	Bytes internal(6 + 32, 0);
	Put16(internal, 0, 2);
	Put16(internal, 2, 2);
	Put32(internal, 6, 5);	// cumulative 5 (u64), page 100, file 1, slot 0
	Put32(internal, 14, 100);
	Put16(internal, 18, 1);
	Put32(internal, 22, 8);	 // cumulative 8, page 100, file 1, slot 1
	Put32(internal, 30, 100);
	Put16(internal, 34, 1);
	Put16(internal, 36, 1);
	return CatalogInput({LeafPage(), TextPage(101, {Fragment(2, internal)})}, InRowRoot(1, {{8, 101, 0}}));
}

//! fuzz_backup_lob's seeds (its format is fuzz_backup_catalog's, options bit 7 a map): every off-row form
//! over the leaf page.
static void LobSeeds() {
	Write("backup_lob", "inrow_internal", LobInput());
	// A truncating .WRITE: the last leaf holds 3 bytes where its link claims 2.
	Write("backup_lob", "last_leaf_over", CatalogInput({LeafPage()}, InRowRoot(0, {{5, 100, 0}, {7, 100, 1}})));
	Bytes overflow(24, 0);
	overflow[0] = 2;
	Put32(overflow, 12, 5);	 // length, then page 100, file 1, slot 0
	Put32(overflow, 16, 100);
	Put16(overflow, 20, 1);
	Write("backup_lob", "row_overflow", CatalogInput({LeafPage()}, overflow));
	Bytes large(10 + 24, 0);  // LARGE_ROOT: maxlinks, count, level 0, then narrow links from record byte 24
	Put16(large, 0, 2);
	Put16(large, 2, 2);
	Put32(large, 10, 5);
	Put32(large, 14, 100);
	Put16(large, 18, 1);
	Put32(large, 22, 8);
	Put32(large, 26, 100);
	Put16(large, 30, 1);
	Put16(large, 32, 1);
	Write("backup_lob", "root_pointer_large",
		  CatalogInput({LeafPage(), TextPage(102, {Fragment(5, large)})}, RootPointer(102)));
	Bytes small(6, 0);	// SMALL_ROOT: its size, four bytes, then the value
	Put16(small, 0, 5);
	small.insert(small.end(), HELLO.begin(), HELLO.end());
	Write("backup_lob", "root_pointer_small", CatalogInput({TextPage(102, {Fragment(0, small)})}, RootPointer(102)));
	Write("backup_lob", "null_root", CatalogInput({TextPage(102, {Fragment(8, Bytes())})}, RootPointer(102)));
	// A map (backup_lob_map.hpp) behind options bit 7, as AppendLobMap writes the last_leaf_over row: one
	// in-row root (its 36 bytes), 7 bytes, two DATA nodes on page 100.
	const Bytes root = InRowRoot(0, {{5, 100, 0}, {7, 100, 1}});
	Bytes map = {0x80, 0x01, 1, 0, 1, 0, 0, 0, 2, 0, static_cast<uint8_t>(root.size()), 0};
	map.insert(map.end(), root.begin(), root.end());
	const Bytes tail = {7, 0, 0, 0, 2, 0, 0, 0,										 // length, node count
						100, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 5, 0, 0, 0, 1, 3,	 // page 100 slot 0, bytes 0-4
						100, 0, 0, 0, 1, 0, 1, 0, 5, 0, 0, 0, 2, 0, 0, 0, 1, 3}; // slot 1, bytes 5-6
	map.insert(map.end(), tail.begin(), tail.end());
	Write("backup_lob", "map", map);
}

int main(int argc, char **argv) {
	out_dir = argc > 1 ? argv[1] : "fuzz/corpus";
	Options full;
	Write("backup_mtf", "full.mtf", BuildImage(full));
	Options log;
	log.with_msda = false;
	Write("backup_mtf", "log.mtf", BuildImage(log));
	Options diff;
	diff.sset_attributes = 0x8;
	Write("backup_mtf", "differential.mtf", BuildImage(diff));
	Options forked;
	forked.forked = true;
	Write("backup_mtf", "forked.mtf", BuildImage(forked));
	Options ansi;
	ansi.string_type = 1;
	Write("backup_mtf", "ansi.mtf", BuildImage(ansi));

	Write("backup_container", "pages.container", Container({0, 1, 2, 3, 0xFFFFFFFFu, 5}));

	Write("backup_xpress", "literals_300", Xpress(300));
	Write("backup_xpress", "literals_4096", Xpress(4096));

	Write("backup_page", "ordered", PassSeed(1, {0, 1, 2, 4, 5}));
	Write("backup_page", "striped", PassSeed(3, {0, 1, 2, 3, 6, 7}));
	Write("backup_page", "out_of_order", PassSeed(5, {0, 3, 1, 0xFFFFFFFFu, 2}));
	Write("backup_page", "header_scan", PassSeed(0, {0, 0xFFFFFFFFu, 1, 2}));

	const SamplePages sample = ReadSample("test/data/backup/full.bak");
	const uint64_t SAU = 7ull << 16, ROWSETS = 5ull << 16, OBJECTS = (1ull << 48) | (34ull << 16),
				   COLUMNS = (1ull << 48) | (41ull << 16), RSCOLS = 3ull << 16;
	auto pages_of = [&](uint64_t au) {
		auto it = sample.data.find(au);
		return it == sample.data.end() ? std::vector<Bytes>() : it->second;
	};
	std::vector<Bytes> boot_sau = {sample.boot};
	for (const auto &p : First(pages_of(SAU), 3)) {
		boot_sau.push_back(p);
	}
	Write("backup_catalog", "boot_sysallocunits", CatalogInput(boot_sau));
	std::vector<Bytes> objects = First(pages_of(ROWSETS), 2);
	for (const auto &p : First(pages_of(OBJECTS), 4)) {
		objects.push_back(p);
	}
	Write("backup_catalog", "rowsets_objects", CatalogInput(objects));
	std::vector<Bytes> columns = First(pages_of(COLUMNS), 4);
	for (const auto &p : First(pages_of(RSCOLS), 4)) {
		columns.push_back(p);
	}
	Write("backup_catalog", "columns_rscols", CatalogInput(columns));
	// The pages that describe the five themselves: CheckBootstrapLayouts passes over them and their
	// described layouts decode (measured 9 pages beside the boot page; the harness takes 16).
	std::set<int64_t> five = {3, 5, 7, 34, 41}, rowsets;
	std::vector<Bytes> describing = {sample.boot};
	for (const char *table : {"sysrowsets", "sysschobjs", "syscolpars", "sysallocunits", "sysrscols"}) {
		const uint64_t au = std::string(table) == "sysrowsets"		? ROWSETS
							: std::string(table) == "sysschobjs"	? OBJECTS
							: std::string(table) == "syscolpars"	? COLUMNS
							: std::string(table) == "sysallocunits" ? SAU
																	: RSCOLS;
		for (const auto &p : Describing(pages_of(au), table, five, rowsets)) {
			describing.push_back(p);
		}
	}
	if (describing.size() < 6 || describing.size() > 16 || sample.boot.empty() || sample.iam.empty()) {
		std::fprintf(stderr, "the sample gives %zu describing pages (boot %zu, IAM %zu bytes)\n", describing.size(),
					 sample.boot.size(), sample.iam.size());
		std::exit(1);
	}
	Write("backup_catalog", "self_described", CatalogInput(describing));
	std::vector<Bytes> mix = {sample.boot, sample.iam};
	for (uint64_t au : {SAU, ROWSETS, OBJECTS, COLUMNS, RSCOLS}) {
		for (const auto &p : First(pages_of(au), 1)) {
			mix.push_back(p);
		}
	}
	Write("backup_catalog", "mix", CatalogInput(mix));
	Write("backup_catalog", "lob_tree", LobInput());
	LobSeeds();

	// backup_record: the sample's two user tables' first pages with their layouts (as the catalog
	// describes them: test/data/backup/full.bak), and pages of the record forms E20 measured on 2022
	// (test/cpp/backup/test_backup_rows.cpp's records): a version tag, a version ghost and a stub beside
	// a row, the uniquifier, bits sharing a byte, a forwarded record's back pointer.
	const uint64_t ITEMS = 72057594053984256ull, EVENTS = 72057594054049792ull;
	if (pages_of(ITEMS).empty() || pages_of(EVENTS).empty()) {
		std::fprintf(stderr, "the sample has no page of items or events\n");
		std::exit(1);
	}
	Write("backup_record", "items",
		  RecordInput({{56, 0, 4, 1},
					   {231, 80, -1, 2},
					   {167, 12, -2, 3},
					   {106, 12 | (2 << 8), 8, 4},
					   {42, 3, 17, 5},
					   {104, 0, 24, 6},
					   {165, 64, -3, 7}},
					  pages_of(ITEMS)[0]));
	Write("backup_record", "events",
		  RecordInput({{127, 0, 4, 1}, {56, 0, 12, 2}, {167, 100, -1, 3}, {61, 0, 16, 4}}, pages_of(EVENTS)[0]));
	const std::vector<RecordColumn> ver = {{56, 0, 4, 1}, {56, 0, 8, 2}, {167, 20, -1, 3}};
	Write("backup_record", "versioned",
		  RecordInput(ver, RecordPage({Hex("70000c0001000000010000000300000100150076310000000000000000a60300000000"),
									   Hex("4e0b00000009002400b9030000000067330000000000000000b60300000000"),
									   Hex("04510100000100a600"),
									   Hex("320008000700000002000002001300"
										   "1d806162000451010000"
										   "0100a600")})));
	RecordColumn uniquifier = {56, 0, -1, 1};
	uniquifier.flags = 2;
	Write(
		"backup_record", "uniquifier",
		RecordInput({uniquifier, {56, 0, 4, 2}, {167, 20, -2, 3}, {56, 0, 8, 4}},
					RecordPage({Hex("70000c00000000000a0000000400000200150018007531300000000000000000a00300000000")})));
	std::vector<RecordColumn> bits = {{104, 0, 4, 1}, {104, 0, 4, 2, 1}, {56, 0, 5, 3}};
	for (uint8_t b = 3; b <= 8; b++) {
		bits.push_back({104, 0, 4, static_cast<uint16_t>(b + 1), static_cast<uint8_t>(b - 1)});
	}
	bits.push_back({104, 0, 9, 10});
	RecordColumn added = {56, 0, 10, 11};
	added.flags = 4 | 8 | (4 << 4);	 // added after the records, with a 4-byte default
	bits.push_back(added);
	Write("backup_record", "bits_added",
		  RecordInput(bits, RecordPage({Hex("50000a005505000000010a0000000000000000000000e20300000000"),
										Hex("50000a00aa05000000000a0005000000000000000000e20300000000")})));
	return 0;
}
