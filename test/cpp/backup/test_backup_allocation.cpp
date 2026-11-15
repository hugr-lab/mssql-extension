// Spec 085 task 8: AllocationMap and the liveness rule (E2 R3) over a synthetic data pass.
//
// File 1 of 40 pages: its header (page 0) says Size 40; its PFS (page 1) marks pages 0..20 but 13
// allocated; its GAM (page 2) allocates extents 0, 1 and 2. The pass also carries extent 4, which
// the GAM marks free (a dead extent, as full backups carry, E2 §1.3). Checked: the allocation pages
// are live; a page is live iff below Size, its extent allocated and its PFS byte 0x40; a page the
// PFS frees, a page of a free extent and a page past Size are not; a header naming another file is
// refused when the map is built, a missing GAM page when its interval is first asked about (the
// GAMs are read on demand, DQ-41); page 9 is the boot page in file 1 only. Over a striped set (the
// same pages split between two families) the index names each page's family and the map answers as
// over one stream; a full's pass 1 is searched there (DQ-41) and locates every page where the
// header-scanned index does, and a family out of order falls back to its header scan; passes are
// numbered by MSDA block, so a family's pass with no data does not renumber the rest; an extent two
// families carry in one pass is refused. (Checked by hand, not here: on the local samples every
// page of the probe full's allocation truth is live; the default, trust_layout and header-scanned
// indexes read the same bytes for every page of all 107 backups, striped and compressed included,
// design/001-mssql-backup-reader/experiments/E13/e13_index.cpp.)

#include <algorithm>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "backup/backup_allocation.hpp"
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

//! A page: header version 1, m_type, m_pageId, m_fileId, then `body` bytes where non-zero.
Bytes Page(uint16_t file, uint32_t page, uint8_t type, const Bytes *body = nullptr) {
	Bytes b(8192, 0);
	if (body) {
		b = *body;
	}
	b[0] = 1;
	b[1] = type;
	for (int i = 0; i < 4; i++) {
		b[32 + i] = (page >> (8 * i)) & 0xFF;
	}
	b[36] = file & 0xFF;
	b[37] = file >> 8;
	return b;
}

struct Fixture {
	Bytes stream;
	MtfLayout layout;
};

//! Data passes of file `file`, one MSDA block each: pass k carries extents `passes[k]` (an empty
//! list is an MSDA block with no data stream), with the header, PFS and GAM given. Ordinary pages
//! carry `marker` at byte 96, so a test can tell which family's copy it was handed.
Fixture BuildPasses(uint16_t file, const Bytes &header, const Bytes &pfs, const Bytes *gam,
					const std::vector<std::vector<uint32_t>> &passes, uint8_t marker = 0) {
	Fixture f;
	for (const auto &extents : passes) {
		MtfBlock block;
		block.type = "MSDA";
		if (!extents.empty()) {
			MtfStream data;
			data.id = "MQDA";
			data.data_offset = f.stream.size();
			f.stream.insert(f.stream.end(), 2, 0);
			for (uint32_t extent : extents) {
				for (uint32_t k = 0; k < 8; k++) {
					const uint32_t page = extent * 8 + k;
					Bytes p;
					if (page == 0) {
						p = Page(file, 0, 15, &header);
					} else if (page == 1) {
						p = Page(file, 1, 11, &pfs);
					} else if (page == 2 && gam) {
						p = Page(file, 2, 8, gam);
					} else if (page == 2) {
						p = Bytes(8192, 0);	 // no GAM page: a zero unit
					} else {
						p = Page(file, page, 1);
						p[96] = marker;
					}
					f.stream.insert(f.stream.end(), p.begin(), p.end());
				}
			}
			data.length = f.stream.size() - data.data_offset;
			block.streams.push_back(data);
		}
		f.layout.blocks.push_back(block);
	}
	return f;
}

//! One pass: extents `extents` (by default 0, 1, 2 and 4).
Fixture Build(uint16_t file, const Bytes &header, const Bytes &pfs, const Bytes *gam,
			  const std::vector<uint32_t> &extents = {0, 1, 2, 4}) {
	return BuildPasses(file, header, pfs, gam, {extents});
}

Bytes Header(uint16_t file, uint32_t size) {
	Bytes b(8192, 0);
	b[8190] = 96;
	const Bytes record = mtf_builder::FileHeaderRecordBytes(file, size);
	std::memcpy(&b[96], record.data(), record.size());
	return b;
}

Bytes Pfs() {
	Bytes b(8192, 0);
	for (uint32_t p = 0; p <= 20; p++) {
		b[100 + p] = p == 13 ? 0x00 : 0x40;
	}
	for (uint32_t p = 32; p < 40; p++) {
		b[100 + p] = 0x40;	// extent 4's pages: allocated by PFS, but the extent is GAM-free
	}
	return b;
}

Bytes Gam(const std::vector<uint32_t> &allocated) {
	Bytes b(8192, 0);
	for (size_t i = 194; i < 194 + 7988; i++) {
		b[i] = 0xFF;
	}
	for (uint32_t e : allocated) {
		b[194 + e / 8] &= static_cast<uint8_t>(~(1u << (e % 8)));
	}
	return b;
}

}  // namespace

int main() {
	std::cout << "AllocationMap (spec 085 task 8)" << std::endl;
	const Bytes header = Header(1, 40);
	const Bytes pfs = Pfs();
	const Bytes gam = Gam({0, 1, 2});
	auto fx = Build(1, header, pfs, &gam);
	MemoryAccess stream("alloc.bak", fx.stream.data(), fx.stream.size());
	auto index = BuildPageIndex(stream, fx.layout);
	AllocationMap map(index, {1});

	Check(map.FileSize(1) == 40 && map.FileSize(2) == 0, "the file's Size from its header; an unknown file has none");
	Check(map.IsLive(1, 0) && map.IsLive(1, 1) && map.IsLive(1, 2) && map.IsLive(1, 9),
		  "the allocation pages (header, PFS, GAM, boot) are live");
	Check(map.IsLive(1, 12) && map.IsLive(1, 20), "a page below Size, its extent allocated, PFS 0x40: live");
	Check(!map.IsLive(1, 13), "a page the PFS marks free is dead, though its extent is allocated");
	Check(!map.IsLive(1, 33) && !map.ExtentAllocated(1, 33), "a page of a GAM-free extent the pass carries is dead");
	Check(!map.IsLive(1, 45) && !map.IsLive(2, 12),
		  "a page past Size in a GAM-free extent, or of a file the map lacks, is not live");
	Check(AllocationMap::IsAllocationPage(3, 8088) && AllocationMap::IsAllocationPage(3, 2) &&
			  AllocationMap::IsAllocationPage(3, 3) && AllocationMap::IsAllocationPage(3, 6) &&
			  AllocationMap::IsAllocationPage(3, 7) && !AllocationMap::IsAllocationPage(1, 511232 + 9) &&
			  !AllocationMap::IsAllocationPage(1, 12) && AllocationMap::IsAllocationPage(1, 9) &&
			  !AllocationMap::IsAllocationPage(3, 9),
		  "allocation pages: PFS every 8088, GAM/SGAM/DCM/BCM 2/3/6/7, the boot page 1:9 only");
	// A later GAM interval opens with its GAM and SGAM (E17, SQL Server 2022: 511232 GAM, 511233 SGAM,
	// 511238 DCM, 511239 BCM); 511234 / 511235 are ordinary pages there.
	Check(AllocationMap::IsAllocationPage(3, 511232) && AllocationMap::IsAllocationPage(3, 511233) &&
			  AllocationMap::IsAllocationPage(3, 511238) && AllocationMap::IsAllocationPage(3, 511239) &&
			  !AllocationMap::IsAllocationPage(3, 511234) && !AllocationMap::IsAllocationPage(3, 511235) &&
			  AllocationMap::IsAllocationPage(3, 2 * 511232) && !AllocationMap::IsAllocationPage(3, 2 * 511232 + 2),
		  "a later interval: GAM/SGAM/DCM/BCM at +0/+1/+6/+7 (E17), not +2/+3");
	{
		// In file 3 page 9 is an ordinary page: freed by the PFS, it is dead (file 1's is live).
		Bytes freed = pfs;
		freed[100 + 9] = 0x00;
		auto three = Build(3, Header(3, 40), freed, &gam);
		MemoryAccess s("file3.bak", three.stream.data(), three.stream.size());
		auto i = BuildPageIndex(s, three.layout);
		AllocationMap m(i, {3});
		Check(m.IsLive(3, 1) && m.IsLive(3, 12) && !m.IsLive(3, 9), "in file 3 page 9 follows the PFS");
	}
	{
		auto no_gam = Build(1, header, pfs, nullptr);
		MemoryAccess s("no_gam.bak", no_gam.stream.data(), no_gam.stream.size());
		auto i = BuildPageIndex(s, no_gam.layout);
		// Its extent is carried, but the GAM's unit is a zero unit: not that page. The GAM is read when
		// its interval is first asked about (DQ-41), so the map builds and the question fails.
		AllocationMap m(i, {1});
		Check(ErrorOf([&]() { m.IsLive(1, 12); }).find("a GAM page (page 1:2)") != std::string::npos,
			  "a file whose GAM page the backup lacks: the first question about its interval is refused by name");
	}
	{
		// The recorded Size is a lower bound (a header older than an autogrow, as on a live database):
		// 40 pages recorded, extent 5 (pages 40..47) GAM-allocated, carried and marked by the PFS.
		Bytes pfs_past = Pfs();
		for (uint32_t p = 40; p < 48; p++) {
			pfs_past[100 + p] = 0x40;
		}
		Bytes gam_past = Gam({0, 1, 2, 5});
		auto past = Build(1, Header(1, 40), pfs_past, &gam_past, {0, 1, 2, 5});
		MemoryAccess s("past_size.bak", past.stream.data(), past.stream.size());
		auto i = BuildPageIndex(s, past.layout);
		AllocationMap m(i, {1});
		Check(m.FileSize(1) == 40 && m.ExtentAllocated(1, 42) && m.IsLive(1, 42) && !m.IsLive(1, 50) &&
				  !m.IsLive(1, 511232),
			  "a page past the recorded Size is live when its extent is allocated and the PFS marks it; the "
			  "next interval's GAM page is not, when the backup carries none");
	}
	{
		const Bytes other = Header(7, 40);
		auto wrong = Build(1, other, pfs, &gam);
		MemoryAccess s("wrong_header.bak", wrong.stream.data(), wrong.stream.size());
		auto i = BuildPageIndex(s, wrong.layout);
		Check(ErrorOf([&]() { AllocationMap m(i, {1}); }).find("header record names file 7") != std::string::npos,
			  "a file header whose record names another file is refused");
	}

	{
		// A striped set: family 0 carries extents 1 and 4, family 1 the allocation pages' extent 0
		// and extent 2. The map reads each page from the family the index names.
		auto a = Build(1, header, pfs, &gam, {1, 4});
		auto b = Build(1, header, pfs, &gam, {0, 2});
		MemoryAccess sa("stripe_a.bak", a.stream.data(), a.stream.size());
		MemoryAccess sb("stripe_b.bak", b.stream.data(), b.stream.size());
		std::vector<FamilyStream> families(2);
		families[0].stream = &sa;
		families[0].layout = &a.layout;
		families[1].stream = &sb;
		families[1].layout = &b.layout;
		PageIndexOptions full;
		full.full_backup = true;
		auto i = BuildPageIndex(families, full);
		PageLocation at0, at12, at33;
		Check(i.Locate(1, 0, at0) && at0.family == 1 && i.Locate(1, 12, at12) && at12.family == 0 &&
				  i.Locate(1, 33, at33) && at33.family == 0,
			  "a striped index names the family holding each page");
		Check(i.Pass(0).Searched() && i.Pass(1).Searched(),
			  "a striped full's pass 1 is searched in each family (DQ-41)");
		AllocationMap m(i, {1});
		bool same = true;
		for (uint32_t p = 0; p < 48; p++) {
			same = same && m.IsLive(1, p) == map.IsLive(1, p);
		}
		Check(m.FileSize(1) == 40 && same, "over two families the map answers as over one");
		auto scanned = BuildPageIndex(families, PageIndexOptions());
		bool located_alike = true;
		for (uint32_t p = 0; p < 48; p++) {
			PageLocation x, y;
			const bool fx = i.Locate(1, p, x), fy = scanned.Locate(1, p, y);
			located_alike = located_alike && fx == fy && (!fx || (x.family == y.family && x.offset == y.offset));
		}
		Check(located_alike, "the searched index locates every page where the header-scanned one does");
		// Family 0 out of order (extent 4 before extent 1): its search gives up, the pass is mapped by
		// its headers on the spot, and every page is still located where it is.
		auto disordered = Build(1, header, pfs, &gam, {4, 1});
		MemoryAccess sd("disordered.bak", disordered.stream.data(), disordered.stream.size());
		std::vector<FamilyStream> mixed(2);
		mixed[0].stream = &sd;
		mixed[0].layout = &disordered.layout;
		mixed[1].stream = &sb;
		mixed[1].layout = &b.layout;
		auto searched = BuildPageIndex(mixed, full);
		auto by_headers = BuildPageIndex(mixed, PageIndexOptions());
		bool fallback_alike = true;
		for (uint32_t p = 0; p < 48; p++) {
			PageLocation x, y;
			const bool fx = searched.Locate(1, p, x), fy = by_headers.Locate(1, p, y);
			fallback_alike = fallback_alike && fx == fy && (!fx || (x.family == y.family && x.offset == y.offset));
		}
		Check(fallback_alike && !searched.Pass(0).Searched(),
			  "a family out of order: its search gives up to a header scan, every page still where it is");
		{
			// The fallback maps the pass through the index's mapper (task 16c: the DuckDB layer's reads a
			// family file as parallel ranges), not BuildPassMap behind its back.
			int mapped = 0;
			PageIndexOptions counted = full;
			counted.mapper = [&mapped](const RandomAccess &stream, uint64_t offset, uint64_t length, uint32_t number) {
				mapped++;
				return BuildPassMap(stream, offset, length, number);
			};
			auto with_mapper = BuildPageIndex(mixed, counted);
			const int at_build = mapped;
			bool alike = true;
			for (uint32_t p = 0; p < 48; p++) {
				PageLocation x, y;
				const bool fx = with_mapper.Locate(1, p, x), fy = by_headers.Locate(1, p, y);
				alike = alike && fx == fy && (!fx || (x.family == y.family && x.offset == y.offset));
			}
			Check(alike && !with_mapper.Pass(0).Searched() && mapped == at_build + 1,
				  "the fallback maps the given-up pass through the index's mapper, once (" +
					  std::to_string(mapped - at_build) + " call(s))");
		}
		{
			// The review's case: one plain family, extents 0..299 with 150 and 200 swapped; the rank
			// fails, the pass is searched, and asked in order without `expected` a search that has not
			// met the break calls some of 150..199 absent. Asked as `expected` (the page must be
			// there), absence maps the pass by its headers and finds every one where it is.
			std::vector<uint32_t> order;
			for (uint32_t e = 0; e < 300; e++) {
				order.push_back(e);
			}
			std::swap(order[150], order[200]);
			auto swapped = Build(1, Header(1, 4000), pfs, &gam, order);
			MemoryAccess sw("swapped.bak", swapped.stream.data(), swapped.stream.size());
			auto group_of = [&](uint32_t e) {
				return static_cast<uint64_t>(std::find(order.begin(), order.end(), e) - order.begin());
			};
			auto unsure = BuildPageIndex(sw, swapped.layout, true);
			PageLocation at;
			uint32_t hidden = 0;
			for (uint32_t e = 0; e < 300; e++) {
				hidden += unsure.Locate(1, e * 8 + 3, at) ? 0 : 1;
			}
			Check(hidden > 0, "  (without `expected`, the unseen break hides " + std::to_string(hidden) +
								  " extents: the hazard is real)");
			auto idx = BuildPageIndex(sw, swapped.layout, true);
			bool all = true;
			for (uint32_t e = 0; e < 300; e++) {
				all = all && idx.Locate(1, e * 8 + 3, at, true) && at.offset == 2 + (group_of(e) * 8 + 3) * 8192;
			}
			Check(all, "a break the search did not see: every expected page is still found, where it is");
		}
	}

	{
		// Two families, three MSDA blocks each; the pass with no data differs between them. Family
		// A: extents {0, 1}, none, {3}; family B: {2, 3}, {3}, none. Extent 3's latest copy is A's
		// pass 3, though A has only two data streams: passes are numbered by their MSDA block.
		auto a = BuildPasses(1, header, pfs, &gam, {{0, 1}, {}, {3}}, 0xA);
		auto b = BuildPasses(1, header, pfs, &gam, {{2, 3}, {3}, {}}, 0xB);
		MemoryAccess sa("three_a.bak", a.stream.data(), a.stream.size());
		MemoryAccess sb("three_b.bak", b.stream.data(), b.stream.size());
		std::vector<FamilyStream> families(2);
		families[0].stream = &sa;
		families[0].layout = &a.layout;
		families[1].stream = &sb;
		families[1].layout = &b.layout;
		auto i = BuildPageIndex(families);
		PageLocation at;
		uint8_t marker = 0;
		const bool found = i.Locate(1, 24, at);
		if (found && at.family == 0) {
			sa.ReadAt(at.offset + 96, &marker, 1);
		}
		Check(found && at.family == 0 && at.pass == 2 && marker == 0xA,
			  "a family's pass with no data does not renumber its later passes");
	}
	{
		// The same extent in one pass of two families: a crafted set, refused when the page is
		// located.
		auto a = Build(1, header, pfs, &gam, {0, 1});
		auto b = Build(1, header, pfs, &gam, {1, 2});
		MemoryAccess sa("dup_a.bak", a.stream.data(), a.stream.size());
		MemoryAccess sb("dup_b.bak", b.stream.data(), b.stream.size());
		std::vector<FamilyStream> families(2);
		families[0].stream = &sa;
		families[0].layout = &a.layout;
		families[1].stream = &sb;
		families[1].layout = &b.layout;
		auto i = BuildPageIndex(families);
		PageLocation at;
		Check(i.Locate(1, 20, at) && at.family == 1, "an extent one family carries is located there");
		Check(ErrorOf([&]() { i.Locate(1, 12, at); }).find("in two media families") != std::string::npos,
			  "an extent two families carry in one pass is refused by name");
	}

	std::cout << (failures ? "FAILED: " + std::to_string(failures) + " check(s)" : "all checks passed") << std::endl;
	return failures ? 1 : 0;
}
