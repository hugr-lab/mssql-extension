// Spec 085 PR 4c-3: the row changes' digests (backup_hash). XXH64 against the reference test vectors,
// a digest independent of how its input is cut, the cases a multiply-only mix missed (two top bits that
// cancel), and SHA-256 against its own vectors.

#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

#include "backup/backup_hash.hpp"

using namespace duckdb::mssql::backup;

namespace {

int failures = 0;

void Check(bool ok, const std::string &what) {
	if (!ok) {
		std::cerr << "FAIL: " << what << "\n";
		failures++;
	}
}

uint64_t Xxh(const std::string &s, uint64_t seed = 0) {
	Xxh64 h(seed);
	h.Update(s.data(), s.size());
	return h.Digest();
}

std::string Hex(const Digest &d, size_t n) {
	std::string out;
	char buf[3];
	for (size_t i = 0; i < n; i++) {
		std::snprintf(buf, sizeof(buf), "%02x", d.bytes[i]);
		out += buf;
	}
	return out;
}

}  // namespace

int main() {
	// The reference implementation's results (xxhash r39, XXH64, seed 0 unless given).
	Check(Xxh("") == 0xEF46DB3751D8E999ULL, "XXH64('')");
	Check(Xxh("a") == 0xD24EC4F1A98C6E5BULL, "XXH64('a')");
	Check(Xxh("abc") == 0x44BC2CF5AD770999ULL, "XXH64('abc')");
	Check(Xxh("Nobody inspects the spammish repetition") == 0xFBCEA83C8A378BF1ULL, "XXH64 of 39 bytes");
	Check(Xxh("", 1) != Xxh(""), "a seed changes it");

	// One digest however the bytes are cut, through every path (buffered, whole stripes, the tail).
	std::string data;
	for (int i = 0; i < 1000; i++) {
		data.push_back(static_cast<char>((i * 131 + 7) & 0xFF));
	}
	const uint64_t whole = Xxh(data);
	for (size_t piece : {1u, 3u, 7u, 8u, 31u, 32u, 33u, 64u, 999u}) {
		Xxh64 h;
		for (size_t at = 0; at < data.size(); at += piece) {
			h.Update(data.data() + at, std::min(piece, data.size() - at));
		}
		Check(h.Digest() == whole, "XXH64 in pieces of " + std::to_string(piece));
	}
	BytesHash bytes;
	const Digest once = bytes.Of(data);
	for (size_t at = 0; at < data.size(); at += 17) {
		bytes.Update(data.data() + at, std::min<size_t>(17, data.size() - at));
	}
	Check(bytes.Finish() == once, "BytesHash in pieces, and reset after Finish");

	// What `(h ^ block) * C` alone missed: bit 7 of bytes 7 and 15 flipped together (review of 4c-3b).
	std::string a = "Bonjour Le Andre, ca va?";
	std::string b = a;
	b[7] = static_cast<char>(b[7] ^ 0x80);
	b[15] = static_cast<char>(b[15] ^ 0x80);
	Check(bytes.Of(a) != bytes.Of(b), "two top bits do not cancel");
	Check(bytes.Of("") != bytes.Of(std::string(1, '\0')), "the length counts");

	// SHA-256's first 16 bytes (FIPS 180-2 vectors).
	BytesHash sha;
	sha.UseSha256();
	Check(Hex(sha.Of("abc"), 16) == "ba7816bf8f01cfea414140de5dae2223", "SHA-256('abc')");
	Check(Hex(sha.Of(""), 16) == "e3b0c44298fc1c149afbf4c8996fb924", "SHA-256('')");
	for (size_t at = 0; at < data.size(); at += 13) {
		sha.Update(data.data() + at, std::min<size_t>(13, data.size() - at));
	}
	// Each digest taken on its own: the operands of == are evaluated in no set order, and GCC took the
	// right one first, appending `data` to the pieces (CI, linux_amd64).
	const Digest pieces = sha.Finish();
	Check(pieces == sha.Of(data), "SHA-256 in pieces");

	if (failures) {
		std::cerr << failures << " check(s) failed\n";
		return 1;
	}
	std::cout << "test_backup_hash: all checks passed\n";
	return 0;
}
