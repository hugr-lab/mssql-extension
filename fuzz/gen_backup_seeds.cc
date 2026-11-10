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

//! A synthetic LOB tree: a text page of two DATA fragments and an INTERNAL node over them (file 1, pages
//! 100 and 101, allocation unit 255 << 16: no bootstrap table's), and an in-row root over the node.
static Bytes LobInput() {
	auto page = [](uint32_t number, const std::vector<Bytes> &records) {
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
	};
	auto fragment = [](uint16_t type, const Bytes &body) {
		Bytes r(14, 0);
		r[0] = 0x08;
		Put16(r, 2, static_cast<uint16_t>(14 + body.size()));
		Put16(r, 12, type);
		r.insert(r.end(), body.begin(), body.end());
		return r;
	};
	const Bytes hello = {'h', 'e', 'l', 'l', 'o'}, abc = {'a', 'b', 'c'};
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
	Bytes root(24, 0);
	root[0] = 5;
	Put32(root, 12, 8);	 // cumulative 8, page 101, file 1, slot 0
	Put32(root, 16, 101);
	Put16(root, 20, 1);
	return CatalogInput({page(100, {fragment(3, hello), fragment(3, abc)}), page(101, {fragment(2, internal)})}, root);
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
	return 0;
}
