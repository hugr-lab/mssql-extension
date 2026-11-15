// Spec 085 task 7a, D3a: the header pass over parallel ranges (backup_open_pass) in an in-process
// DuckDB, against BuildPassMap over the same bytes in memory.
//
// A synthetic pass on disk: extents of file 1 in order (the ranked form), one out of order (runs
// from there), a zero group and a pad page. Scanned in waves of three 2-group ranges, and in one
// range, it gives BuildPassMap's map: the same groups, extents, form and every extent's group;
// the ranges counted are the groups / 2 rounded up. A unit with a bad header version in the second
// wave is refused naming the file and its offset. Groups fed out of order or past the pass are
// refused. RangedPassMapper scans a SourceAccess in ranges and anything else in memory.

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "backup/backup_bytes.hpp"
#include "backup/backup_open_pass.hpp"
#include "backup/backup_page_index.hpp"
#include "duckdb.hpp"

using namespace duckdb;
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
constexpr size_t PREFIX = 1000;	 // bytes before the data stream, as an MTF file has

//! The units of one extent group: pages of `extent` of file 1, or zeros (extent < 0), with
//! unit 3 a pad page when `pad`.
void AppendGroup(Bytes &out, int64_t extent, bool pad = false) {
	for (uint32_t k = 0; k < 8; k++) {
		Bytes unit(8192, 0);
		if (extent >= 0) {
			unit[0] = 1;
			unit[1] = 1;
			const uint32_t page = static_cast<uint32_t>(extent) * 8 + k;
			for (int i = 0; i < 4; i++) {
				unit[32 + i] = (page >> (8 * i)) & 0xFF;
			}
			unit[36] = 1;
			if (pad && k == 3) {
				unit[1] = 101;
			}
		}
		out.insert(out.end(), unit.begin(), unit.end());
	}
}

void WriteFile(const std::string &path, const Bytes &bytes) {
	std::ofstream out(path, std::ios::binary);
	out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

bool SameMap(const PassMap &a, const PassMap &b) {
	if (a.GroupCount() != b.GroupCount() || a.ExtentCount() != b.ExtentCount() || a.Ranked() != b.Ranked() ||
		a.units_offset != b.units_offset || a.number != b.number) {
		return false;
	}
	for (uint32_t e = 0; e < 64; e++) {
		ExtentId x;
		x.file = 1;
		x.extent = e;
		uint64_t ga = 0, gb = 0;
		const bool fa = a.Find(x, ga), fb = b.Find(x, gb);
		if (fa != fb || (fa && ga != gb)) {
			return false;
		}
	}
	return true;
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

}  // namespace

int main() {
	std::cout << "Header pass over parallel ranges (spec 085 task 7a, D3a)" << std::endl;
	const char *tmp = std::getenv("TMPDIR");
	std::string dir = tmp ? tmp : "/tmp";
	while (dir.size() > 1 && dir.back() == '/') {
		dir.pop_back();
	}
	dir += "/mssql_backup_open_pass_test";
	if (std::system(("mkdir -p '" + dir + "'").c_str()) != 0) {
		std::cout << "cannot create " << dir << std::endl;
		return 1;
	}

	auto build = [](const std::vector<int64_t> &extents, size_t pad_group) {
		Bytes file(PREFIX, 0xEE);
		file.push_back(0);
		file.push_back(0);	// the 2-byte lead-in
		for (size_t g = 0; g < extents.size(); g++) {
			AppendGroup(file, extents[g], g == pad_group);
		}
		file.insert(file.end(), 300, 0xEE);	 // what follows the stream
		return file;
	};
	// Ranked through extent 5, then 2 again out of order (runs), a zero group, 9 to 11.
	const std::vector<int64_t> extents = {0, 1, 3, 4, 5, -1, 2, 9, 10, 11, 7};
	const uint64_t length = 2 + extents.size() * 65536;
	auto ordered = build({0, 1, 3, 4, 5, 6, 7, 8, 9}, 4);
	auto mixed = build(extents, 2);
	WriteFile(dir + "/ordered.bak", ordered);
	WriteFile(dir + "/mixed.bak", mixed);

	DuckDB db(nullptr);
	Connection con(db);
	auto &context = *con.context;
	auto source = ByteSource::Open(context, {dir + "/mixed.bak", dir + "/ordered.bak"});

	PassScanOptions small;
	small.range_bytes = 2 * 65536;
	small.ranges_in_flight = 3;
	{
		MemoryAccess memory("mixed.bak", mixed.data(), mixed.size());
		auto expected = BuildPassMap(memory, PREFIX, length, 1);
		uint64_t ranges = 0;
		auto waves = BuildPassMapInRanges(context, *source, 0, PREFIX, length, 1, small, &ranges);
		Check(!expected.Ranked() && SameMap(expected, waves),
			  "a pass that breaks its order: waves of ranges = BuildPassMap");
		Check(ranges == 6, "eleven groups in 2-group ranges: six ranges (" + std::to_string(ranges) + ")");
		uint64_t one = 0;
		PassScanOptions whole;
		auto single = BuildPassMapInRanges(context, *source, 0, PREFIX, length, 1, whole, &one);
		Check(SameMap(expected, single) && one == 1, "the whole pass in one 8 MiB range: the same map");
	}
	{
		const uint64_t ordered_length = 2 + 9 * 65536;
		MemoryAccess memory("ordered.bak", ordered.data(), ordered.size());
		auto expected = BuildPassMap(memory, PREFIX, ordered_length, 0);
		auto waves = BuildPassMapInRanges(context, *source, 1, PREFIX, ordered_length, 0, small);
		Check(expected.Ranked() && SameMap(expected, waves), "an ordered pass stays ranked across the ranges");
		PassMapper mapper = RangedPassMapper(context, small);
		SourceAccess file(*source, 1);
		Check(SameMap(expected, mapper(file, PREFIX, ordered_length, 0)) &&
				  SameMap(expected, mapper(memory, PREFIX, ordered_length, 0)),
			  "RangedPassMapper: a family file in ranges, a stream in memory, one map");
		uint8_t at = 0;
		file.ReadAt(PREFIX + 2, &at, 1);
		Check(file.Size() == ordered.size() && at == 1, "SourceAccess reads the family file");
	}
	{
		// The thread's read-ahead (PR 2e step 7), each case on a fresh access so its fetches are counted
		// from 0: every read gives the file's bytes, and the fetches are what the read-ahead says.
		auto same = [](const SourceAccess &access, const Bytes &bytes, uint64_t offset, size_t count) {
			Bytes got(count);
			access.ReadAt(offset, got.data(), count);
			return std::equal(got.begin(), got.end(), bytes.begin() + static_cast<std::ptrdiff_t>(offset));
		};
		const uint64_t ra = SourceAccess::READ_AHEAD;
		{
			SourceAccess in_order(*source, 0);
			bool ok = true;
			uint64_t read = 0;
			for (uint64_t at = PREFIX; at + 8192 <= mixed.size(); at += 8192, read += 8192) {
				ok = ok && same(in_order, mixed, at, 8192);
			}
			// The first read as asked, then one fetch of READ_AHEAD a run of it.
			const uint64_t expect = 1 + (read - 8192 + ra - 1) / ra;
			Check(ok && in_order.Fetches() == expect,
				  "pages in order: the first fetched as asked, then one fetch a 256 KiB (" +
					  std::to_string(in_order.Fetches()) + " fetches, " + std::to_string(expect) + " expected)");
		}
		{
			SourceAccess gaps(*source, 0);
			const uint64_t p = PREFIX;
			bool ok = same(gaps, mixed, p, 8192);					   // as asked: 1
			ok = ok && same(gaps, mixed, p + 8192 + 40000, 8192);	   // 40000 past the last: read ahead, 2
			ok = ok && same(gaps, mixed, p + 2 * 8192 + 40000, 8192);  // within it: no fetch
			ok = ok && same(gaps, mixed, p + 8192 + 40000, 100);	   // a step back within it: no fetch
			const uint64_t gaps_two = gaps.Fetches();
			ok = ok && same(gaps, mixed, p + 8192 + 40000 + ra + 100000, 8192);	 // far past: as asked, 3
			const uint64_t gaps_three = gaps.Fetches();
			Check(ok && gaps_two == 2 && gaps_three == 3,
				  "a gap within 64 KiB reads ahead, a read within the read-ahead or a step back in it fetches "
				  "nothing, a gap past it is read as asked (" +
					  std::to_string(gaps_two) + ", " + std::to_string(gaps_three) + " fetches)");
		}
		{
			SourceAccess a(*source, 0);
			SourceAccess b(*source, 1);
			bool ok = same(a, mixed, PREFIX, 8192) && same(a, mixed, PREFIX + 8192, 8192);	// 1 + read ahead
			ok = ok && same(b, ordered, PREFIX, 8192);	// another file on this thread: a's read-ahead goes
			ok = ok && same(a, mixed, PREFIX + 2 * 8192, 8192) && same(a, mixed, PREFIX + 3 * 8192, 8192);
			Check(ok && a.Fetches() == 4 && b.Fetches() == 1,
				  "back from another file: read as asked, then ahead again (" + std::to_string(a.Fetches()) +
					  " fetches)");
			SourceAccess end(*source, 0);
			ok = same(end, mixed, mixed.size() - 9000, 4000) && same(end, mixed, mixed.size() - 5000, 4000) &&
				 same(end, mixed, mixed.size() - 1000, 1000) && same(end, mixed, 0, static_cast<size_t>(ra) + 10);
			Check(ok && end.Fetches() == 3,
				  "the file's end: the read-ahead clamped to it; a read as long as the read-ahead fetched as asked (" +
					  std::to_string(end.Fetches()) + " fetches)");
		}
		SourceAccess a(*source, 0);
		Bytes past(2);
		Check(ErrorOf([&]() { a.ReadAt(mixed.size() - 1, past.data(), 2); }).find("mixed.bak") != std::string::npos,
			  "a read past the end is refused naming the file");
		std::vector<std::thread> threads;
		std::atomic<int> wrong{0};
		for (int t = 0; t < 4; t++) {
			threads.emplace_back([&, t]() {
				for (uint64_t at = PREFIX + t * 4096; at + 8192 <= mixed.size(); at += 8192) {
					if (!same(a, mixed, at, 8192)) {
						wrong++;
					}
				}
			});
		}
		for (auto &thread : threads) {
			thread.join();
		}
		Check(wrong == 0, "four threads reading one file in order, each from its own read-ahead: the file's bytes");
	}
	{
		Bytes bad = mixed;
		// Group 8, unit 1: header version 7. Waves are three 2-group ranges (groups 0-5, 6-10), so
		// the bad unit is in the second wave.
		bad[PREFIX + 2 + 8 * 65536 + 8192] = 7;
		WriteFile(dir + "/bad.bak", bad);
		auto bad_source = ByteSource::Open(context, {dir + "/bad.bak"});
		const auto error = ErrorOf([&]() { BuildPassMapInRanges(context, *bad_source, 0, PREFIX, length, 1, small); });
		Check(error.find("bad.bak") != std::string::npos && error.find("header version 7") != std::string::npos,
			  "a bad unit in a later wave is refused naming the file: " + error);
	}

	{
		PassHeaderScan scan("mixed.bak", mixed.size(), PREFIX, length, 1);
		const uint8_t *g0 = mixed.data() + PREFIX + 2;
		Check(ErrorOf([&]() { scan.Feed(1, g0 + 65536, 1); }).find("out of order") != std::string::npos,
			  "a scan fed group 1 first is refused");
		Check(ErrorOf([&]() { scan.Feed(0, g0, 12); }).find("past the pass") != std::string::npos,
			  "a scan fed more groups than its pass holds is refused");
		Check(ErrorOf([&]() { scan.Finish(); }).find("finished at group 0 of 11") != std::string::npos,
			  "a scan finished before its last group is refused");
	}

	std::cout << (failures ? "FAILED: " + std::to_string(failures) + " check(s)" : "all checks passed") << std::endl;
	return failures ? 1 : 0;
}
