// Fuzz harness: a hierarchyid value's MS-SSCLRT 2.2.2 bit string (spec 093 T4, D5) as its ToString()
// text, DuckDB-free. The input is one value's bytes, as the PLP framing assembles them. Checked beyond
// "no crash": a value decoded is a path -- `/`, or levels of integers each ending in '/', labels within a
// level separated by '.' -- and encoding that text gives back exactly the input (the format has one
// encoding per value, padding included), so the decoder accepts nothing the encoder would not write. A
// value past 892 bytes is refused. Seeds: G2's server bytes and H1's shapes (fuzz/corpus/hierarchyid).

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "codec/clr/hierarchyid.hpp"

using namespace duckdb::mssql::codec::hierarchyid;

static void Require(bool ok) {
	if (!ok) {
		__builtin_trap();
	}
}

//! `/`, or `/` then one or more levels, each one or more '.'-separated integers (an optional '-' and
//! 1-15 digits, no leading zero but a lone 0) ending in '/'.
static bool IsPath(const std::string &s) {
	if (s.empty() || s[0] != '/') {
		return false;
	}
	size_t at = 1;
	while (at < s.size()) {
		while (true) {
			if (at < s.size() && s[at] == '-') {
				at++;
			}
			const size_t digits = at;
			while (at < s.size() && s[at] >= '0' && s[at] <= '9') {
				at++;
			}
			const size_t n = at - digits;
			if (n == 0 || n > 15 || (n > 1 && s[digits] == '0') || at >= s.size()) {
				return false;
			}
			if (s[at] == '/') {
				at++;
				break;
			}
			if (s[at] != '.') {
				return false;
			}
			at++;
		}
	}
	return true;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
	std::string text = "prefix";  // Decode appends: what was there stays
	const auto status = Decode(data, size, text);
	Require(text.compare(0, 6, "prefix") == 0);
	if (size > MAX_BYTES) {
		Require(status == HierarchyIdStatus::TooLong);
		return 0;
	}
	if (status != HierarchyIdStatus::Ok) {
		Require(std::strlen(DescribeStatus(status)) > 0);
		return 0;
	}
	const std::string path = text.substr(6);
	Require(IsPath(path));
	std::vector<uint8_t> again;
	Require(Encode(path, again) == HierarchyIdStatus::Ok);
	Require(again.size() == size && (size == 0 || std::memcmp(again.data(), data, size) == 0));
	return 0;
}
