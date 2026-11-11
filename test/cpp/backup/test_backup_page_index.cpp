// Spec 085 task 7a: PassMap / PageIndex over synthetic data passes.
//
// A pass is built unit by unit (page headers: version 1, m_type, m_pageId @32, m_fileId @36; zero
// units; pads of m_type 101), as E12 found them. Checked: an ascending pass with pad groups only at
// its end is ranked and finds every extent's group; an empty group inside, or a descending order,
// gives runs that find the same; the later pass wins (D4); a page no pass carries is not found;
// refused: two extents in one group, a page off its position, an extent carried twice, a pass that
// is not whole groups. Locally the index is checked on all 110 experiment backups: every page of
// every pass is located at its latest copy.

#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "backup/backup_bytes.hpp"
#include "backup/backup_mtf.hpp"
#include "backup/backup_page_index.hpp"
#include "mtf_builder.hpp"

using namespace duckdb::mssql::backup;

namespace {

int failures = 0;

void Check(bool ok, const std::string &what) {
	std::cout << (ok ? "  ok   " : "  FAIL ") << what << std::endl;
	if (!ok) {
		failures++;
	}
}

template <class F>
std::string ErrorOf(F f) {
	try {
		f();
	} catch (FormatError &e) {
		return e.what();
	}
	return "";
}

using Bytes = std::vector<uint8_t>;

//! A unit: kind 'p' a page (file, page), 'z' a zero unit, 'P' a pad.
struct Unit {
	char kind;
	uint16_t file;
	uint32_t page;
	const Bytes *body = nullptr;  // the page's bytes after its header fields (a file header, a GAM)
};

Bytes Pass(const std::vector<Unit> &units) {
	Bytes b(2, 0);	// the lead-in
	for (const auto &u : units) {
		Bytes page(8192, 0);
		if (u.kind == 'p' || u.kind == 'P' || u.kind == 'v' || u.kind == 'x') {
			// 'v': a version-0 unit carrying a page id (a page caught while formatted); 'x': version 2.
			page[0] = u.kind == 'v' ? 0 : (u.kind == 'x' ? 2 : 1);
			page[1] = u.kind == 'P' ? 101 : (u.kind == 'p' ? 1 : 0);
			for (int i = 0; i < 4; i++) {
				page[32 + i] = (u.page >> (8 * i)) & 0xFF;
			}
			page[36] = u.file & 0xFF;
			page[37] = u.file >> 8;
			if (u.body) {
				for (size_t i = 0; i < u.body->size(); i++) {
					if ((*u.body)[i]) {
						page[i] = (*u.body)[i];
					}
				}
			}
		}
		b.insert(b.end(), page.begin(), page.end());
	}
	return b;
}

//! The 8 units of extent `extent` of `file`, with `missing` positions as zero units.
void AddExtent(std::vector<Unit> &units, uint16_t file, uint32_t extent, unsigned missing = 0) {
	for (uint32_t k = 0; k < 8; k++) {
		units.push_back((missing >> k) & 1 ? Unit{'z', 0, 0} : Unit{'p', file, extent * 8 + k});
	}
}
void AddEmpty(std::vector<Unit> &units, char kind) {
	for (int k = 0; k < 8; k++) {
		units.push_back(Unit{kind, 0, 0});
	}
}

//! A file header page body: slot 0 at 96, its record's variable columns 2 / 4 the file id and Size.
Bytes FileHeader(uint16_t file, uint32_t size) {
	Bytes b(8192, 0);
	b[1] = 15;	// m_type: a file header page
	b[8190] = 96;
	const Bytes record = mtf_builder::FileHeaderRecordBytes(file, size);
	std::memcpy(&b[96], record.data(), record.size());
	return b;
}

//! A GAM page body: bit e set (free) for every extent not in `allocated`.
Bytes Gam(const std::vector<uint32_t> &allocated) {
	Bytes b(8192, 0);
	b[1] = 8;  // m_type: a GAM page
	for (size_t i = 194; i < 194 + 7988; i++) {
		b[i] = 0xFF;
	}
	for (uint32_t e : allocated) {
		b[194 + e / 8] &= static_cast<uint8_t>(~(1u << (e % 8)));
	}
	return b;
}

//! Extent 0 of `file`, its page 0 a file header and its page 2 a GAM.
void AddFirstExtent(std::vector<Unit> &units, uint16_t file, const Bytes *header, const Bytes *gam) {
	for (uint32_t k = 0; k < 8; k++) {
		Unit u{'p', file, k};
		u.body = k == 0 ? header : (k == 2 ? gam : nullptr);
		units.push_back(u);
	}
}

//! A data pass computed on the fly: its groups' extents in order, then pad groups; pages carry
//! their ids, and the bodies given for (file, page). A pass of any size costs nothing to hold.
class ComputedPass : public RandomAccess {
public:
	std::vector<ExtentId> groups;
	size_t pad_groups = 0;
	std::vector<std::pair<std::pair<uint16_t, uint32_t>, const Bytes *>> bodies;
	uint64_t throw_past = UINT64_MAX;  // a read past this offset fails
	std::string name = "computed.bak";

	uint64_t Size() const override {
		return 2 + (groups.size() + pad_groups) * 8 * 8192ull;
	}
	const std::string &Name() const override {
		return name;
	}
	void ReadAt(uint64_t offset, uint8_t *dest, size_t count) const override {
		if (offset + count > throw_past) {
			throw FormatError("computed pass: a read past the test's limit");
		}
		for (size_t i = 0; i < count;) {
			const uint64_t at = offset + i;
			if (at < 2) {
				dest[i++] = 0;
				continue;
			}
			const uint64_t unit = (at - 2) / 8192;
			const size_t within = static_cast<size_t>((at - 2) % 8192);
			const size_t n = std::min(count - i, static_cast<size_t>(8192 - within));
			Bytes page = Unit(unit);
			std::memcpy(dest + i, page.data() + within, n);
			i += n;
		}
	}

private:
	Bytes Unit(uint64_t unit) const {
		Bytes page(8192, 0);
		const uint64_t g = unit / 8;
		page[0] = 1;
		if (g >= groups.size()) {
			page[1] = 101;	// a pad
			return page;
		}
		const ExtentId e = groups[g];
		const uint32_t id = e.extent * 8 + static_cast<uint32_t>(unit % 8);
		page[1] = 1;
		for (int i = 0; i < 4; i++) {
			page[32 + i] = (id >> (8 * i)) & 0xFF;
		}
		page[36] = e.file & 0xFF;
		page[37] = e.file >> 8;
		for (const auto &b : bodies) {
			if (b.first.first == e.file && b.first.second == id) {
				for (size_t i = 0; i < b.second->size(); i++) {
					if ((*b.second)[i]) {
						page[i] = (*b.second)[i];
					}
				}
			}
		}
		return page;
	}
};

}  // namespace

int main() {
	std::cout << "PassMap / PageIndex (spec 085 task 7a)" << std::endl;

	std::vector<Unit> ascending;
	const uint32_t extents[] = {0, 1, 2, 5, 6, 900, 1011};
	for (uint32_t e : extents) {
		AddExtent(ascending, 1, e, e == 5 ? 0x0F : 0);	// extent 5 half allocated: zero units in place
	}
	AddExtent(ascending, 3, 0);
	AddExtent(ascending, 3, 7);
	AddEmpty(ascending, 'P');
	AddEmpty(ascending, 'P');
	auto bytes = Pass(ascending);
	MemoryAccess file("pass.bak", bytes.data(), bytes.size());
	auto map = BuildPassMap(file, 0, bytes.size(), 0);
	Check(map.Ranked() && map.ExtentCount() == 9 && map.GroupCount() == 11,
		  "an ascending pass with trailing pad groups is ranked");
	bool all = true;
	uint64_t g = 0;
	for (size_t i = 0; i < 7; i++) {
		ExtentId e;
		e.file = 1;
		e.extent = extents[i];
		all = all && map.Find(e, g) && g == i;
	}
	ExtentId f3;
	f3.file = 3;
	f3.extent = 7;
	all = all && map.Find(f3, g) && g == 8;
	ExtentId f3_first;
	f3_first.file = 3;
	f3_first.extent = 0;
	all = all && map.Find(f3_first, g) && g == 7;
	ExtentId absent;
	absent.file = 1;
	absent.extent = 3;
	ExtentId other_file;
	other_file.file = 2;
	other_file.extent = 0;
	Check(all && !map.Find(absent, g) && !map.Find(other_file, g),
		  "each extent's group is its rank; an extent the pass lacks is not found");

	{
		std::vector<Unit> units;
		AddExtent(units, 1, 0);
		AddEmpty(units, 'z');
		AddExtent(units, 1, 4);
		auto b = Pass(units);
		MemoryAccess f("gap.bak", b.data(), b.size());
		auto m = BuildPassMap(f, 0, b.size(), 1);
		ExtentId e;
		e.file = 1;
		e.extent = 4;
		Check(!m.Ranked() && m.Find(e, g) && g == 2, "an empty group inside the pass: runs, and the group is right");
	}
	{
		std::vector<Unit> units;
		AddExtent(units, 1, 9);
		AddExtent(units, 1, 10);
		AddExtent(units, 1, 2);
		auto b = Pass(units);
		MemoryAccess f("desc.bak", b.data(), b.size());
		auto m = BuildPassMap(f, 0, b.size(), 1);
		ExtentId e2;
		e2.file = 1;
		e2.extent = 2;
		ExtentId e10;
		e10.file = 1;
		e10.extent = 10;
		uint64_t g2 = 0;
		uint64_t g10 = 0;
		Check(!m.Ranked() && m.Find(e2, g2) && g2 == 2 && m.Find(e10, g10) && g10 == 1,
			  "a pass out of order (a busy backup's second pass): runs");
	}

	// The later pass wins: pass 2 holds extent 1:1 again.
	{
		std::vector<Unit> pass2_units;
		AddExtent(pass2_units, 1, 1);
		auto p1 = Pass(ascending);
		auto p2 = Pass(pass2_units);
		Bytes stream = p1;
		stream.insert(stream.end(), p2.begin(), p2.end());
		MemoryAccess f("two_passes.bak", stream.data(), stream.size());
		MtfLayout layout;
		for (int pass = 0; pass < 2; pass++) {
			MtfBlock block;
			block.type = "MSDA";
			MtfStream data;
			data.id = "MQDA";
			data.data_offset = pass == 0 ? 0 : p1.size();
			data.length = pass == 0 ? p1.size() : p2.size();
			block.streams.push_back(data);
			MtfStream end;
			end.id = "MQDA";
			block.streams.push_back(end);
			layout.blocks.push_back(block);
		}
		auto index = BuildPageIndex(f, layout);
		PageLocation at;
		const bool p2_page = index.Locate(1, 13, at) && at.pass == 1 && at.offset == p1.size() + 2 + 5 * 8192;
		const bool p1_page = index.Locate(1, 900 * 8 + 3, at) && at.pass == 0 && at.offset == 2 + (5 * 8 + 3) * 8192;
		Check(index.PassCount() == 2 && p2_page && p1_page && !index.Locate(1, 3 * 8, at),
			  "the later pass wins; an earlier pass serves the rest; a page in no pass is not found");
		Check(index.MemoryBytes() < 4096, "the index of a small backup is a few KB");
	}

	{
		std::vector<Unit> units;
		AddExtent(units, 1, 0);
		units[3] = Unit{'p', 1, 8 + 3};
		auto b = Pass(units);
		MemoryAccess f("mixed.bak", b.data(), b.size());
		Check(ErrorOf([&]() { BuildPassMap(f, 0, b.size(), 0); }).find("two extents") != std::string::npos,
			  "a group holding pages of two extents is refused");
		units[3] = Unit{'p', 1, 4};
		auto c = Pass(units);
		MemoryAccess h("position.bak", c.data(), c.size());
		Check(ErrorOf([&]() { BuildPassMap(h, 0, c.size(), 0); }).find("at position 3") != std::string::npos,
			  "a page off its position in the group is refused");
	}
	{
		std::vector<Unit> units;
		AddExtent(units, 1, 4);
		AddExtent(units, 1, 2);
		AddExtent(units, 1, 4);
		auto b = Pass(units);
		MemoryAccess f("twice.bak", b.data(), b.size());
		Check(ErrorOf([&]() { BuildPassMap(f, 0, b.size(), 0); }).find("twice") != std::string::npos,
			  "a pass carrying an extent twice is refused");
	}
	{
		auto b = Pass(std::vector<Unit>(5, Unit{'z', 0, 0}));
		MemoryAccess f("partial.bak", b.data(), b.size());
		Check(ErrorOf([&]() { BuildPassMap(f, 0, b.size(), 0); }).find("whole 8-unit") != std::string::npos,
			  "a pass that is not whole 8-unit groups is refused");
		Bytes odd(2 + 8192 + 100, 0);
		MemoryAccess o("odd.bak", odd.data(), odd.size());
		Check(ErrorOf([&]() { BuildPassMap(o, 0, odd.size(), 0); }).find("whole 8 KB units") != std::string::npos,
			  "a data stream that is not whole units is refused");
	}
	{
		// Runs: an extent just past a run, between runs, in a file the pass lacks.
		std::vector<Unit> units;
		AddExtent(units, 1, 20);
		AddExtent(units, 1, 21);
		AddExtent(units, 1, 5);
		AddExtent(units, 2, 1);
		auto b = Pass(units);
		MemoryAccess f("edges.bak", b.data(), b.size());
		auto m = BuildPassMap(f, 0, b.size(), 1);
		ExtentId past;
		past.file = 1;
		past.extent = 22;
		ExtentId between;
		between.file = 1;
		between.extent = 10;
		ExtentId lacking;
		lacking.file = 4;
		lacking.extent = 1;
		ExtentId second;
		second.file = 2;
		second.extent = 1;
		Check(!m.Ranked() && !m.Find(past, g) && !m.Find(between, g) && !m.Find(lacking, g) && m.Find(second, g) &&
				  g == 3,
			  "runs: past a run's end, between runs and in a missing file nothing is found");
	}
	{
		// A sparse pass: one extent near the end of a large file. A bitmap would be 32 MB for one bit.
		std::vector<Unit> units;
		AddExtent(units, 1, 1u << 28);
		auto b = Pass(units);
		MemoryAccess f("sparse.bak", b.data(), b.size());
		auto m = BuildPassMap(f, 0, b.size(), 1);
		ExtentId e;
		e.file = 1;
		e.extent = 1u << 28;
		Check(!m.Ranked() && m.MemoryBytes() < 1024 && m.Find(e, g) && g == 0,
			  "a sparse pass is kept as runs, not a bitmap sized by its highest extent");
	}
	{
		// Crafted: a new file with a huge extent in every group, ascending.
		std::vector<Unit> units;
		for (uint16_t file = 1; file <= 200; file++) {
			AddExtent(units, file, 1u << 24);
		}
		auto b = Pass(units);
		MemoryAccess f("crafted.bak", b.data(), b.size());
		auto m = BuildPassMap(f, 0, b.size(), 1);
		ExtentId e;
		e.file = 150;
		e.extent = 1u << 24;
		Check(!m.Ranked() && m.MemoryBytes() < 64 * 1024 && m.Find(e, g) && g == 149,
			  "a pass that would grow the bitmaps past their budget turns into runs (" +
				  std::to_string(m.MemoryBytes()) + " bytes)");
	}
	{
		std::vector<Unit> units;
		AddExtent(units, 1, 3);
		units[2] = Unit{'v', 1, 3 * 8 + 2};
		auto b = Pass(units);
		MemoryAccess f("formatted.bak", b.data(), b.size());
		auto m = BuildPassMap(f, 0, b.size(), 0);
		Check(m.ExtentCount() == 1, "a version-0 unit carrying a page id is that page's copy, in its extent's group");
		units[2] = Unit{'x', 1, 3 * 8 + 2};
		auto c = Pass(units);
		MemoryAccess h("v2.bak", c.data(), c.size());
		Check(ErrorOf([&]() { BuildPassMap(h, 0, c.size(), 0); }).find("header version 2") != std::string::npos,
			  "a unit of an unknown header version is refused");
	}
	{
		auto p = Pass(ascending);
		MemoryAccess f("layout.bak", p.data(), p.size());
		auto make_block = [&](bool encrypted, int data_streams) {
			MtfBlock block;
			block.type = "MSDA";
			for (int i = 0; i < data_streams; i++) {
				MtfStream data;
				data.id = "MQDA";
				data.data_offset = 0;
				data.length = p.size();
				data.encryption = encrypted ? 1 : 0;
				block.streams.push_back(data);
			}
			return block;
		};
		MtfLayout two_streams;
		two_streams.blocks.push_back(make_block(false, 2));
		Check(ErrorOf([&]() { BuildPageIndex(f, two_streams); }).find("two data streams") != std::string::npos,
			  "an MSDA block with two data streams is refused");
		MtfLayout encrypted;
		encrypted.blocks.push_back(make_block(true, 1));
		Check(ErrorOf([&]() { BuildPageIndex(f, encrypted); }).find("encrypted") != std::string::npos,
			  "an encrypted data stream is refused (spec 087)");
		MtfLayout appended;
		MtfBlock sset;
		sset.type = "SSET";
		appended.blocks.push_back(sset);
		appended.blocks.push_back(make_block(false, 1));
		appended.blocks.push_back(sset);
		Check(ErrorOf([&]() { BuildPageIndex(f, appended); }).find("more than one backup set") != std::string::npos,
			  "a file of appended backup sets is refused by name");
	}
	{
		// The rank path (E12): file 1 of 40 pages, GAM-allocated extents 0, 2, 4 (1 holds the boot
		// page); file 3 of 16 pages, extents 0 and 1. The pass carries them in order, then pads.
		Bytes h1 = FileHeader(1, 40);
		Bytes g1 = Gam({0, 2, 4});
		Bytes h3 = FileHeader(3, 16);
		Bytes g3 = Gam({0, 1});
		std::vector<Unit> units;
		AddFirstExtent(units, 1, &h1, &g1);
		AddExtent(units, 1, 1);
		AddExtent(units, 1, 2);
		AddExtent(units, 1, 4);
		AddFirstExtent(units, 3, &h3, &g3);
		AddExtent(units, 3, 1);
		AddEmpty(units, 'P');
		auto b = Pass(units);
		MemoryAccess f("ranked_pass.bak", b.data(), b.size());
		PassMap ranked;
		uint64_t reads = 0;
		const bool ok = BuildRankedPassMap(f, 0, b.size(), ranked, &reads);
		auto scanned = BuildPassMap(f, 0, b.size(), 0);
		bool same = ok && ranked.Ranked() && ranked.ExtentCount() == scanned.ExtentCount();
		for (uint16_t file : {1, 2, 3}) {
			for (uint32_t e = 0; e < 8; e++) {
				ExtentId x;
				x.file = file;
				x.extent = e;
				uint64_t a = 0;
				uint64_t c = 0;
				const bool fa = ranked.Find(x, a);
				const bool fc = scanned.Find(x, c);
				same = same && fa == fc && (!fa || a == c);
			}
		}
		Check(same, "the rank path maps the pass from its allocation pages exactly as the header scan does");
		Check(reads < scanned.GroupCount() + 1,
			  "it reads " + std::to_string(reads) + " of " + std::to_string(scanned.GroupCount()) + " groups");
		// The rule broken: the GAM says extent 3 is allocated, which the pass lacks.
		Bytes g1_wrong = Gam({0, 2, 3, 4});
		units[2].body = &g1_wrong;
		auto w = Pass(units);
		MemoryAccess fw("wrong_gam.bak", w.data(), w.size());
		PassMap refused;
		Check(!BuildRankedPassMap(fw, 0, w.size(), refused),
			  "a pass that disagrees with its GAM is refused by the rank path");
		MtfLayout layout;
		MtfBlock block;
		block.type = "MSDA";
		MtfStream data;
		data.id = "MQDA";
		data.data_offset = 0;
		data.length = w.size();
		block.streams.push_back(data);
		layout.blocks.push_back(block);
		auto index = BuildPageIndex(fw, layout, true);
		PageLocation at;
		Check(index.PassCount() == 1 && index.Locate(1, 4 * 8 + 1, at) && at.offset == 2 + (3 * 8 + 1) * 8192,
			  "and the index searches the pass instead (DQ-41), which locates the pages right");
	}
	{
		// The recorded Size is a lower bound (a header image older than an autogrow, as on a live
		// database; DbReader's PROVENANCE): file 1 records 16 pages, its GAM allocates extents 0, 2 and
		// 4 (pages to 39). The rank path takes every GAM-allocated extent, as the header scan finds.
		Bytes h1 = FileHeader(1, 16);
		Bytes g1 = Gam({0, 2, 4});
		std::vector<Unit> units;
		AddFirstExtent(units, 1, &h1, &g1);
		AddExtent(units, 1, 1);
		AddExtent(units, 1, 2);
		AddExtent(units, 1, 4);
		AddEmpty(units, 'P');
		auto b = Pass(units);
		MemoryAccess f("past_size.bak", b.data(), b.size());
		PassMap ranked;
		const bool ok = BuildRankedPassMap(f, 0, b.size(), ranked);
		auto scanned = BuildPassMap(f, 0, b.size(), 0);
		bool same = ok && ranked.Ranked() && ranked.ExtentCount() == 4 && scanned.ExtentCount() == 4;
		for (uint32_t e : {0u, 1u, 2u, 4u}) {
			ExtentId x;
			x.file = 1;
			x.extent = e;
			uint64_t a = 0;
			uint64_t c = 0;
			same = same && ranked.Find(x, a) && scanned.Find(x, c) && a == c;
		}
		Check(same, "a file whose data runs past its recorded Size is ranked whole (its GAM, not its Size)");
	}
	{
		// Two GAM intervals: file 1 of 511232 + 64 pages. Interval 0's GAM allocates extents 0..20
		// and 30000; interval 1's (page 511232, the interval's first: E17; extent 63904) its first four. The PFS pages
		// 8088 k are copied though not GAM-allocated (E12). File 3 is small. Expected extents computed here.
		const uint32_t size1 = 511232 + 64;
		std::vector<uint32_t> gam0_bits;
		for (uint32_t e = 0; e <= 20; e++) {
			gam0_bits.push_back(e);
		}
		gam0_bits.push_back(30000);
		Bytes gam0 = Gam(gam0_bits);
		Bytes gam1 = Gam({0, 1, 2, 3});
		Bytes header1 = FileHeader(1, size1);
		Bytes header3 = FileHeader(3, 16);
		Bytes gam3 = Gam({0, 1});
		std::vector<uint32_t> expected = gam0_bits;
		for (uint32_t k = 1; 8088ull * k < size1; k++) {
			expected.push_back(8088 * k / 8);
		}
		for (uint32_t e = 63904; e < 63908; e++) {
			expected.push_back(e);
		}
		std::sort(expected.begin(), expected.end());
		expected.erase(std::unique(expected.begin(), expected.end()), expected.end());
		ComputedPass pass;
		for (uint32_t e : expected) {
			ExtentId x;
			x.file = 1;
			x.extent = e;
			pass.groups.push_back(x);
		}
		for (uint32_t e = 0; e < 2; e++) {
			ExtentId x;
			x.file = 3;
			x.extent = e;
			pass.groups.push_back(x);
		}
		pass.pad_groups = 2;
		pass.bodies = {{{1, 0}, &header1}, {{1, 2}, &gam0}, {{1, 511232}, &gam1}, {{3, 0}, &header3}, {{3, 2}, &gam3}};
		PassMap ranked;
		uint64_t reads = 0;
		Check(BuildRankedPassMap(pass, 0, pass.Size(), ranked, &reads), "a file of two GAM intervals is ranked");
		auto scanned = BuildPassMap(pass, 0, pass.Size(), 0);
		bool same = ranked.ExtentCount() == expected.size() + 2 && scanned.ExtentCount() == ranked.ExtentCount();
		for (size_t i = 0; i < expected.size(); i++) {
			ExtentId x;
			x.file = 1;
			x.extent = expected[i];
			uint64_t a = 0;
			uint64_t c = 0;
			same = same && ranked.Find(x, a) && scanned.Find(x, c) && a == i && c == i;
		}
		ExtentId uncopied;
		uncopied.file = 1;
		uncopied.extent = 21;
		Check(same && !ranked.Find(uncopied, g),
			  "every extent's group as the header scan finds it; the PFS extents "
			  "and the second GAM's in place (" +
				  std::to_string(expected.size()) + " extents)");
		Check(reads == 21, "it reads 21 groups (file 1: group 0, the second GAM, 16 checks; file 3: 2; the pad): " +
							   std::to_string(reads));
		// A crafted Size: refused at once, nothing allocated for it.
		Bytes huge = FileHeader(1, 0xFFFFFFFFu);
		pass.bodies[0].second = &huge;
		PassMap refused;
		uint64_t huge_reads = 0;
		Check(!BuildRankedPassMap(pass, 0, pass.Size(), refused, &huge_reads) && huge_reads == 1,
			  "a file header whose Size the pass cannot hold is refused after one read");
		pass.bodies[0].second = &header1;
		// A read that throws: the rank path says no, it does not throw.
		pass.throw_past = 2 + 30 * 8 * 8192ull;
		Check(!BuildRankedPassMap(pass, 0, pass.Size(), refused),
			  "a read that fails turns the rank path down, not the index");
	}
	{
		ExtentBitmap bm;
		for (uint32_t e = 0; e < 5000; e += 3) {
			bm.Add(e);
		}
		bm.Seal();
		bool ranks = true;
		for (uint32_t e = 0; e < 5000; e += 3) {
			ranks = ranks && bm.Rank(e) == e / 3 && bm.Contains(e) && !bm.Contains(e + 1);
		}
		Check(ranks && bm.Count() == 1667 && bm.Rank(1000000) == 1667,
			  "the bitmap's rank directory counts across its 512-extent blocks");
	}

	std::cout << (failures ? "FAILED: " + std::to_string(failures) + " check(s)" : "all checks passed") << std::endl;
	return failures ? 1 : 0;
}
