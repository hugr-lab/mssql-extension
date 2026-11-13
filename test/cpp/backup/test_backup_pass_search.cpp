// Spec 085 task 7a part 4, DQ-41 / E13: PassSearch over a family file's pass and over a
// container's pass.
//
// A file pass of 300 groups: ascending extents of files 1 and 3 with gaps, and zero groups between
// them. Every carried extent is found at its group; absent ones (in a gap, before the first, after
// the last, of a file the pass lacks) are not; a cold lookup reads at most log2(groups) + 2 times,
// and with the samples kept a second round reads less. A container pass of 64 KB stored blocks
// (one group each) after a lead-in block and before trailer blocks: the same answers, probes
// starting anywhere (a resync takes the fourth of four chained headers). A pass whose order breaks
// throws OrderBroken; so does a block of a pass whose output is not 64 KB.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "backup/backup_bytes.hpp"
#include "backup/backup_pass_search.hpp"

using namespace duckdb::mssql::backup;

namespace {

int failures = 0;

void Check(bool ok, const std::string &what) {
	std::cout << (ok ? "  ok   " : "  FAIL ") << what << std::endl;
	if (!ok) {
		failures++;
	}
}

using Bytes = std::vector<uint8_t>;
const ExtentId NO_EXTENT = {0, 0};

ExtentId E(uint16_t file, uint32_t extent) {
	ExtentId e;
	e.file = file;
	e.extent = extent;
	return e;
}

//! One 8-unit group: pages of `e`, or zeros when `e` is NO_EXTENT (file 0).
Bytes Group(const ExtentId &e) {
	Bytes g(65536, 0);
	if (e.file == 0) {
		return g;
	}
	for (uint32_t k = 0; k < 8; k++) {
		uint8_t *u = g.data() + k * 8192;
		u[0] = 1;
		u[1] = 1;
		const uint32_t page = e.extent * 8 + k;
		std::memcpy(u + 32, &page, 4);
		std::memcpy(u + 36, &e.file, 2);
	}
	return g;
}

//! The pass: file 1's extents 5, 6, 7, then every third from 20, a zero group every 9th, then
//! file 3's extents 0..39.
std::vector<ExtentId> PassLayout() {
	std::vector<ExtentId> groups;
	for (uint32_t e : {5u, 6u, 7u}) {
		groups.push_back(E(1, e));
	}
	for (uint32_t i = 0; groups.size() < 260; i++) {
		groups.push_back(i % 9 == 4 ? NO_EXTENT : E(1, 20 + 3 * i));
	}
	for (uint32_t e = 0; e < 40; e++) {
		groups.push_back(E(3, e));
	}
	return groups;
}

void Put16(Bytes &b, size_t at, uint16_t v) {
	b[at] = v & 0xFF;
	b[at + 1] = v >> 8;
}

void AppendBlock(Bytes &out, const Bytes &data, std::vector<uint64_t> *starts = nullptr) {
	if (starts) {
		starts->push_back(out.size());
	}
	Bytes h(28, 0);
	h[0] = 0x23;
	h[1] = static_cast<uint8_t>(data.size() / 512);
	Put16(h, 2, data.size() == 65536 ? 0 : static_cast<uint16_t>(data.size()));
	out.insert(out.end(), h.begin(), h.end());
	out.insert(out.end(), data.begin(), data.end());
}

template <class F>
std::string ErrorOf(F f) {
	try {
		f();
	} catch (OrderBroken &e) {
		return std::string("OrderBroken: ") + e.what();
	} catch (FormatError &e) {
		return e.what();
	}
	return "";
}

//! Every carried extent found where it is, and the absent ones not: the count of misses.
size_t Misses(PassSearch &search, const std::vector<ExtentId> &groups, const std::vector<uint64_t> &positions) {
	size_t misses = 0;
	for (size_t g = 0; g < groups.size(); g++) {
		if (groups[g].file == 0) {
			continue;
		}
		uint64_t at = 0;
		if (!search.Find(groups[g], at) || at != positions[g]) {
			misses++;
		}
	}
	for (const auto &absent : {E(1, 0), E(1, 8), E(1, 21), E(1, 100000), E(2, 5), E(3, 40), E(9, 0)}) {
		uint64_t at = 0;
		misses += search.Find(absent, at) ? 1 : 0;
	}
	return misses;
}

}  // namespace

int main() {
	std::cout << "PassSearch (spec 085 task 7a part 4, DQ-41)" << std::endl;
	const auto groups = PassLayout();
	{
		Bytes file(1000, 0xEE);
		for (const auto &g : groups) {
			auto bytes = Group(g);
			file.insert(file.end(), bytes.begin(), bytes.end());
		}
		MemoryAccess access("pass.bak", file.data(), file.size());
		std::vector<uint64_t> positions;
		for (size_t g = 0; g < groups.size(); g++) {
			positions.push_back(g);
		}
		PassSearch cold(std::unique_ptr<PassMedium>(new FilePassMedium(access, 1000, groups.size())));
		uint64_t at = 0;
		const uint64_t bound = static_cast<uint64_t>(std::ceil(std::log2(groups.size()))) + 2;
		PassSearch one(std::unique_ptr<PassMedium>(new FilePassMedium(access, 1000, groups.size())));
		Check(one.Find(E(1, 20 + 3 * 100), at) && at == 3 + 100 && one.Reads() <= bound,
			  "a file pass: one cold lookup, " + std::to_string(one.Reads()) + " reads (bound " +
				  std::to_string(bound) + ")");
		Check(Misses(cold, groups, positions) == 0, "a file pass: every extent at its group, absent ones absent");
		const uint64_t first_round = cold.Reads();
		Check(Misses(cold, groups, positions) == 0 && cold.Reads() == first_round,
			  "with the samples kept, a second round reads nothing (" + std::to_string(first_round) + " reads, " +
				  std::to_string(cold.SampleCount()) + " samples)");

		{
			// Galloping (a container walked from its known blocks, DQ-41): the same answers, and an
			// extent near the start read no further than about twice its group and a window.
			struct Reach : public RandomAccess {
				const RandomAccess &inner;
				mutable uint64_t furthest = 0;
				explicit Reach(const RandomAccess &i) : inner(i) {}
				uint64_t Size() const override {
					return inner.Size();
				}
				void ReadAt(uint64_t o, uint8_t *d, size_t n) const override {
					furthest = std::max<uint64_t>(furthest, o + n);
					inner.ReadAt(o, d, n);
				}
				const std::string &Name() const override {
					return inner.Name();
				}
			};
			Reach reach(access);
			PassSearch gallop(std::unique_ptr<PassMedium>(new FilePassMedium(reach, 1000, groups.size())),
							  PassSearch::Strategy::Gallop);
			uint64_t a = 0;
			const bool near = gallop.Find(groups[20], a) && a == 20;
			const uint64_t groups_reached = (reach.furthest - 1000 + 65535) / 65536;
			Check(near && groups_reached <= 2 * 20 + 16 + 1, "galloping to group 20 of " +
																 std::to_string(groups.size()) + " read up to group " +
																 std::to_string(groups_reached));
			PassSearch all(std::unique_ptr<PassMedium>(new FilePassMedium(access, 1000, groups.size())),
						   PassSearch::Strategy::Gallop);
			Check(Misses(all, groups, positions) == 0, "galloping: every extent at its group, absent ones absent");
		}

		Bytes reversed(1000, 0xEE);
		for (size_t g = groups.size(); g-- > 0;) {
			auto bytes = Group(groups[g]);
			reversed.insert(reversed.end(), bytes.begin(), bytes.end());
		}
		MemoryAccess rev("reversed.bak", reversed.data(), reversed.size());
		PassSearch broken(std::unique_ptr<PassMedium>(new FilePassMedium(rev, 1000, groups.size())));
		Check(ErrorOf([&]() {
				  uint64_t a;
				  for (const auto &g : groups) {
					  broken.Find(g, a);
				  }
			  }).find("OrderBroken") == 0,
			  "a pass in descending order: OrderBroken, for the caller's header scan");
	}
	{
		// A run of 2000 empty groups between 100 extents and 100 more: forty lookups of extents the
		// gap would hold read it about once, not once each (the empty spans are kept), and a second
		// round reads nothing.
		std::vector<ExtentId> layout;
		for (uint32_t e = 0; e < 100; e++) {
			layout.push_back(E(1, e));
		}
		layout.insert(layout.end(), 2000, NO_EXTENT);
		for (uint32_t e = 0; e < 100; e++) {
			layout.push_back(E(1, 5000 + e));
		}
		Bytes file;
		for (const auto &g : layout) {
			auto bytes = Group(g);
			file.insert(file.end(), bytes.begin(), bytes.end());
		}
		MemoryAccess access("gap.bak", file.data(), file.size());
		PassSearch search(std::unique_ptr<PassMedium>(new FilePassMedium(access, 0, layout.size())));
		uint64_t at = 0;
		bool none = true;
		for (uint32_t i = 0; i < 40; i++) {
			none = none && !search.Find(E(1, 200 + 100 * i), at);
		}
		const uint64_t first = search.BytesRead();
		const uint64_t gap = 2000 * 65536ull;
		for (uint32_t i = 0; i < 40; i++) {
			none = none && !search.Find(E(1, 200 + 100 * i), at);
		}
		Check(none && first <= 2 * gap && search.BytesRead() == first,
			  "a long empty run read about once (" + std::to_string(first >> 20) + " MiB for a " +
				  std::to_string(gap >> 20) + " MiB gap), then not at all");
	}
	{
		// The guess (PR 2e): a table's extents following one another in the pass, an extent far past
		// the only sample is found by reading its own group alone, not the groups of the extents
		// between, which a gallop from the sample would read. With zero
		// groups before it the guess lands short and the search goes on from there: still found.
		std::vector<ExtentId> layout;
		for (uint32_t e = 0; e < 1000; e++) {
			layout.push_back(E(1, e));
		}
		layout.insert(layout.begin() + 600, 5, NO_EXTENT);
		Bytes file;
		for (const auto &g : layout) {
			auto bytes = Group(g);
			file.insert(file.end(), bytes.begin(), bytes.end());
		}
		MemoryAccess access("contiguous.bak", file.data(), file.size());
		PassSearch search(std::unique_ptr<PassMedium>(new FilePassMedium(access, 0, layout.size())));
		uint64_t at = 0;
		const bool first = search.Find(E(1, 0), at) && at == 0;
		const uint64_t before = search.BytesRead();
		const bool far = search.Find(E(1, 400), at) && at == 400;
		const uint64_t read = search.BytesRead() - before;
		const bool past_gap = search.Find(E(1, 900), at) && at == 905;
		Check(first && far && read == 65536 && past_gap,
			  "a guessed group: an extent 400 groups past the sample read as its one group (" +
				  std::to_string(read >> 10) + " KB), and one past zero groups found where it is");
	}
	{
		// A container: a 8 KB lead-in block, the pass's 64 KB blocks, five trailer blocks.
		Bytes file(8192, 0);
		AppendBlock(file, Bytes(8192, 0));
		std::vector<uint64_t> starts;
		for (const auto &g : groups) {
			AppendBlock(file, Group(g), &starts);
		}
		const uint64_t pass_end = file.size();
		for (int i = 0; i < 5; i++) {
			AppendBlock(file, Bytes(4096, 0));
		}
		const uint64_t chain_end = file.size();
		file.insert(file.end(), 4096, 0);
		MemoryAccess access("comp.bak", file.data(), file.size());
		PassSearch search(std::unique_ptr<PassMedium>(
			new ContainerPassMedium(access, starts.front(), pass_end, chain_end, BlockDecoder())));
		Check(Misses(search, groups, starts) == 0, "a container pass: every extent at its block, absent ones absent");
		PassSearch one(std::unique_ptr<PassMedium>(
			new ContainerPassMedium(access, starts.front(), pass_end, chain_end, BlockDecoder())));
		uint64_t at = 0;
		Check(one.Find(E(3, 17), at) && at == starts[groups.size() - 40 + 17],
			  "a container pass: one cold lookup, " + std::to_string(one.Reads()) + " reads");

		Bytes odd = file;
		// Block 100 of the pass claims 32 KB of output: not a pass's block.
		odd[starts[100]] = 0x21;
		odd[starts[100] + 1] = 64;
		MemoryAccess oa("odd.bak", odd.data(), odd.size());
		PassSearch odd_search(std::unique_ptr<PassMedium>(
			new ContainerPassMedium(oa, starts.front(), pass_end, chain_end, BlockDecoder())));
		Check(ErrorOf([&]() {
				  uint64_t a;
				  for (const auto &g : groups) {
					  odd_search.Find(g, a);
				  }
			  }).find("OrderBroken") == 0,
			  "a block of a pass that is not 64 KB: OrderBroken");
	}

	std::cout << (failures ? "FAILED: " + std::to_string(failures) + " check(s)" : "all checks passed") << std::endl;
	return failures ? 1 : 0;
}
