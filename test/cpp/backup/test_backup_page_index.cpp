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
		Check(index.Passes().size() == 2 && p2_page && p1_page && !index.Locate(1, 3 * 8, at),
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
