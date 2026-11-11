// Fuzz harness: a backup's page index and allocation map (spec 085 tasks 7a and 8, DQ-41).
//
// The input expands into a data pass, so a mutation lands on structure rather than on 8 KB of
// zeros: byte 0 holds the options (bit 0 a full backup, so pass 1 is ranked or searched; bit 1 the
// groups split between two families, a striped set; bit 2 pages asked for as `expected`); then each
// 64 bytes are one 8 KB unit, the chunk tiled across it (so a page header, a file header's slot and
// record -- byte j of the chunk lands at every offset j mod 64 --, a GAM bitmap or PFS bytes all
// come from the input), units padded with zero units to whole 8-unit groups, at most 256 units. Over it: the header
// scan (BuildPassMap), the rank path (BuildRankedPassMap), the index (BuildPageIndex: Locate, ReadPage, the searches
// and their fallback), the allocation map (AllocationMap: file headers, GAMs on demand, PFS) and a galloping
// PassSearch. Every malformed input must end in a FormatError; any sanitizer report, signal, hang
// or other exception is a real bug.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "backup/backup_allocation.hpp"
#include "backup/backup_bytes.hpp"
#include "backup/backup_mtf.hpp"
#include "backup/backup_page_index.hpp"
#include "backup/backup_pass_search.hpp"

using namespace duckdb::mssql::backup;

static volatile size_t g_sink = 0;
static const size_t CHUNK = 64;
static const size_t MAX_UNITS = 256;

struct Family {
	std::vector<uint8_t> stream;
	MtfLayout layout;
};

static void Pass(Family &f, const std::vector<const uint8_t *> &units, size_t lead_in = 2) {
	f.stream.assign(lead_in, 0);
	for (const auto *unit : units) {
		f.stream.insert(f.stream.end(), unit, unit + 8192);
	}
	MtfBlock block;
	block.type = "MSDA";
	MtfStream data;
	data.id = "MQDA";
	data.data_offset = 0;
	data.length = f.stream.size();
	block.streams.push_back(data);
	f.layout.blocks.push_back(block);
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
	if (size < 1 + CHUNK) {
		return 0;
	}
	const bool full = data[0] & 1;
	const bool striped = data[0] & 2;
	const bool expected = data[0] & 4;
	size_t units = std::min((size - 1) / CHUNK, MAX_UNITS);
	const size_t padded = (units + 7) / 8 * 8;
	std::vector<std::vector<uint8_t>> bytes(padded, std::vector<uint8_t>(8192, 0));
	for (size_t u = 0; u < units; u++) {
		const uint8_t *chunk = data + 1 + u * CHUNK;
		for (size_t at = 0; at < 8192; at += CHUNK) {
			std::memcpy(bytes[u].data() + at, chunk, CHUNK);
		}
	}
	// Families: the whole pass, or its groups dealt alternately to two.
	std::vector<Family> families(striped ? 2 : 1);
	std::vector<std::vector<const uint8_t *>> per(families.size());
	for (size_t u = 0; u < padded; u++) {
		per[striped ? (u / 8) % 2 : 0].push_back(bytes[u].data());
	}
	for (size_t f = 0; f < families.size(); f++) {
		Pass(families[f], per[f]);
	}
	std::vector<MemoryAccess> access;
	access.reserve(families.size());
	std::vector<FamilyStream> streams(families.size());
	for (size_t f = 0; f < families.size(); f++) {
		access.emplace_back("fuzz_page.bak", families[f].stream.data(), families[f].stream.size());
		streams[f].stream = &access[f];
		streams[f].layout = &families[f].layout;
	}
	const auto &first = access[0];
	const uint64_t length = families[0].stream.size();
	try {
		auto map = BuildPassMap(first, 0, length, 0);
		g_sink += map.ExtentCount();
	} catch (FormatError &) {
	}
	try {
		PassMap ranked;
		uint64_t reads = 0;
		g_sink += BuildRankedPassMap(first, 0, length, ranked, &reads) ? ranked.ExtentCount() : reads;
	} catch (FormatError &) {
	}
	try {
		PageIndexOptions options;
		options.full_backup = full;
		auto index = BuildPageIndex(streams, options);
		std::vector<uint8_t> page;
		for (uint32_t p = 0; p < 96; p += 3) {
			try {
				PageLocation at;
				g_sink += index.Locate(1, p, at, expected) ? at.offset : 0;
				g_sink += index.ReadPage(1, p, page, expected) ? page[0] : 0;
			} catch (FormatError &) {
			}
		}
		try {
			AllocationMap allocation(index, {1, 3});
			for (uint32_t p = 0; p < 96; p++) {
				try {
					g_sink += allocation.IsLive(1, p) ? 1 : 0;
					g_sink += allocation.IsLive(3, p) ? 1 : 0;
				} catch (FormatError &) {
				}
			}
			g_sink += allocation.MemoryBytes();
		} catch (FormatError &) {
		}
		g_sink += index.MemoryBytes();
	} catch (FormatError &) {
	}
	try {
		PassSearch search(std::unique_ptr<PassMedium>(new FilePassMedium(first, 2, (length - 2) / 65536)),
						  PassSearch::Strategy::Gallop);
		for (uint32_t e = 0; e < 40; e += 3) {
			ExtentId extent;
			extent.file = 1;
			extent.extent = e;
			uint64_t position = 0;
			g_sink += search.Find(extent, position) ? position : 0;
		}
	} catch (FormatError &) {
	}
	return 0;
}
