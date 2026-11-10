// Writes the seed corpora of the backup reader's fuzz harnesses (spec 085 task 10) into
// fuzz/corpus/backup_*: synthetic inputs only (the test builders' images), never a backup made on
// a SQL Server whose data is not ours to publish (spec 085 T4).
//
//   c++ -std=c++17 -I src/include -I test/cpp/backup fuzz/gen_backup_seeds.cc src/backup/backup_bytes.cpp \
//       src/backup/backup_mtf.cpp -o /tmp/gen_backup_seeds && /tmp/gen_backup_seeds fuzz/corpus
//
// backup_mtf: the builder's image as a full, a log, a differential, a forked and an ANSI-string backup.
// backup_container: a full backup whose pass is real pages, in an MSSQLBAK container laid out as E13
// found them (one stored 64 KB block per extent group, the media label at 0x1E0 as a ZSTD-flagged block
// the harness's copying decoder pads, the seek index and the tail). No seed ends in .bak: the
// repository ignores *.bak, so that no real backup is committed by accident. backup_xpress:
// literal runs under a 9-bit code. backup_page: compact passes (fuzz_backup_page's format) in order, striped, and out
// of order.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "backup/backup_bytes.hpp"
#include "backup/backup_mtf.hpp"
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
static Bytes Page(uint8_t flags, const std::vector<uint32_t> &extents) {
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

	Write("backup_page", "ordered", Page(1, {0, 1, 2, 4, 5}));
	Write("backup_page", "striped", Page(3, {0, 1, 2, 3, 6, 7}));
	Write("backup_page", "out_of_order", Page(5, {0, 3, 1, 0xFFFFFFFFu, 2}));
	Write("backup_page", "header_scan", Page(0, {0, 0xFFFFFFFFu, 1, 2}));
	return 0;
}
