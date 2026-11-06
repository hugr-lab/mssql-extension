// Spec 091: where the single-byte kernel spends its time, tier by tier. Manual, not in CI:
//   make bench-code-page-tiers
//
// Every input is 1251 text built so that each 32-byte step of the kernel lands in one
// tier, then decoded as one buffer (DecodeToUtf8) over and over:
//   ascii        32 ASCII bytes                 -> the store-only step
//   run dense    32 bytes of C0-FF              -> run arithmetic, every lane 2-byte
//   run words    Cyrillic words and spaces      -> run arithmetic, mixed 1/2-byte lanes
//   table        2-byte bytes outside the run   -> the vqtbx4q planes
//   3-byte       a dash in every 32 bytes       -> Block3
// and the same 50/50 mix of ascii and run-words steps in a fixed ABAB order and in a
// random one: the gap between those two is what branch mispredictions cost.
// ns per 32-byte step, median of interleaved rounds.

#include "codec/code_page/code_page.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

using namespace duckdb::mssql::codec::code_page;

namespace {

constexpr size_t STEP = 32;
constexpr size_t STEPS = 2048;	// 64 KB: L2-resident on every core we build for
constexpr int ROUNDS = 9;

using Step = void (*)(std::mt19937 &, std::vector<uint8_t> &);

void Ascii(std::mt19937 &rng, std::vector<uint8_t> &v) {
	for (size_t i = 0; i < STEP; i++) {
		v.push_back(static_cast<uint8_t>('a' + rng() % 26));
	}
}
void RunDense(std::mt19937 &rng, std::vector<uint8_t> &v) {
	for (size_t i = 0; i < STEP; i++) {
		v.push_back(static_cast<uint8_t>(0xC0 + rng() % 64));
	}
}
void RunWords(std::mt19937 &rng, std::vector<uint8_t> &v) {
	for (size_t i = 0; i < STEP; i++) {
		v.push_back(i % 7 == 6 ? ' ' : static_cast<uint8_t>(0xC0 + rng() % 64));
	}
}
// 1251 bytes outside the C0-FF run that still read as 2-byte UTF-8 (Ђ Ѓ ѓ Љ Њ Ќ Ћ Џ ...).
void Table(std::mt19937 &rng, std::vector<uint8_t> &v) {
	static const uint8_t two[] = {0x80, 0x81, 0x83, 0x8A, 0x8C, 0x8D, 0x8E, 0x8F, 0x90, 0x9A, 0x9C, 0x9D,
								  0x9E, 0x9F, 0xA1, 0xA2, 0xA3, 0xA5, 0xA8, 0xAA, 0xAF, 0xB2, 0xB3, 0xB4};
	for (size_t i = 0; i < STEP; i++) {
		v.push_back(i % 7 == 6 ? ' ' : two[rng() % sizeof(two)]);
	}
}
void ThreeByte(std::mt19937 &rng, std::vector<uint8_t> &v) {
	for (size_t i = 0; i < STEP; i++) {
		v.push_back(i == 13 ? 0x96 : (i % 7 == 6 ? ' ' : static_cast<uint8_t>(0xC0 + rng() % 64)));
	}
}

std::vector<uint8_t> Build(const std::vector<Step> &order) {
	std::mt19937 rng(91);
	std::vector<uint8_t> v;
	for (Step s : order) {
		s(rng, v);
	}
	return v;
}

std::vector<Step> Repeat(Step s) {
	return std::vector<Step>(STEPS, s);
}

volatile size_t sink;

double NsPerStep(const CodePageTable &page, const std::vector<uint8_t> &in, std::vector<char> &out, int reps) {
	const auto t0 = std::chrono::steady_clock::now();
	for (int r = 0; r < reps; r++) {
		sink = DecodeToUtf8(page, in.data(), in.size(), out.data());
	}
	const auto t1 = std::chrono::steady_clock::now();
	return std::chrono::duration<double, std::nano>(t1 - t0).count() / reps / static_cast<double>(in.size() / STEP);
}

}  // namespace

int main() {
	const CodePageTable *page = FindCodePage(1251);
	std::vector<std::pair<std::string, std::vector<uint8_t>>> inputs;
	inputs.emplace_back("ascii", Build(Repeat(Ascii)));
	inputs.emplace_back("run dense", Build(Repeat(RunDense)));
	inputs.emplace_back("run words", Build(Repeat(RunWords)));
	inputs.emplace_back("table", Build(Repeat(Table)));
	inputs.emplace_back("3-byte", Build(Repeat(ThreeByte)));
	std::vector<Step> abab, shuffled;
	for (size_t i = 0; i < STEPS; i++) {
		abab.push_back(i % 2 ? RunWords : Ascii);
	}
	shuffled = abab;
	std::shuffle(shuffled.begin(), shuffled.end(), std::mt19937(7));
	inputs.emplace_back("mix ascii/words, ABAB", Build(abab));
	inputs.emplace_back("mix ascii/words, random", Build(shuffled));
	// The same mixes 16x longer (1 MB, still in L2): too long a random sequence for the
	// branch predictor to learn over the repetitions, which the 64 KB one may let it do.
	std::vector<Step> abab_long, random_long, typo_long, words_long;
	for (size_t i = 0; i < 16 * STEPS; i++) {
		abab_long.push_back(i % 2 ? RunWords : Ascii);
		words_long.push_back(RunWords);
		typo_long.push_back(i % 5 == 0 ? ThreeByte : RunWords);	 // a dash in 1 of 5 steps
	}
	random_long = abab_long;
	std::shuffle(random_long.begin(), random_long.end(), std::mt19937(7));
	std::vector<Step> typo_random = typo_long;
	std::shuffle(typo_random.begin(), typo_random.end(), std::mt19937(9));
	inputs.emplace_back("1 MB mix ABAB", Build(abab_long));
	inputs.emplace_back("1 MB mix random", Build(random_long));
	inputs.emplace_back("1 MB words", Build(words_long));
	inputs.emplace_back("1 MB 1-in-5 dash, regular", Build(typo_long));
	inputs.emplace_back("1 MB 1-in-5 dash, random", Build(typo_random));

	std::vector<char> out(Utf8Bound(16 * STEPS * STEP));
	std::vector<std::vector<double>> times(inputs.size());
	for (int round = 0; round < ROUNDS; round++) {
		for (size_t i = 0; i < inputs.size(); i++) {
			times[i].push_back(
				NsPerStep(*page, inputs[i].second, out, inputs[i].second.size() > STEPS * STEP ? 25 : 400));
		}
	}
	std::printf("1251, ns per 32-byte step (median of %d interleaved rounds)\n", ROUNDS);
	for (size_t i = 0; i < inputs.size(); i++) {
		std::sort(times[i].begin(), times[i].end());
		std::printf("  %-26s %6.2f\n", inputs[i].first.c_str(), times[i][times[i].size() / 2]);
	}
	return 0;
}
