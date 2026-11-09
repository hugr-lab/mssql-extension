// Spec 085 task 1: ByteReader, the bounded reads every backup parser makes.
//
// Little-endian reads at every width, sub-views that keep the file offset, and a named error --
// never a crash -- for every read past a span, including the overflow cases an offset near
// SIZE_MAX would hit.

#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

#include "backup/backup_bytes.hpp"

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

}  // namespace

int main() {
	std::cout << "ByteReader (spec 085 task 1)" << std::endl;

	const std::vector<uint8_t> bytes = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x88, 0xFF, 0xFE};
	ByteReader r(bytes.data(), bytes.size(), "test span", 1000);

	Check(r.U8(0) == 0x01 && r.U16(0) == 0x0201 && r.U32(0) == 0x04030201, "little-endian u8 / u16 / u32");
	Check(r.U64(0) == 0x8807060504030201ULL, "little-endian u64");
	Check(r.I16(8) == -257 && r.I32(6) == static_cast<int32_t>(0xFEFF8807u), "signed reads");
	Check(r.I64(0) == static_cast<int64_t>(0x8807060504030201ULL), "signed u64");

	Check(r.Has(0, 10) && r.Has(10, 0) && !r.Has(0, 11) && !r.Has(11, 0), "Has at the edges");
	Check(!r.Has(std::numeric_limits<size_t>::max(), 2) && !r.Has(2, std::numeric_limits<size_t>::max()),
		  "Has does not overflow");

	auto sub = r.Sub(4, 4, "inner");
	Check(sub.Size() == 4 && sub.U32(0) == 0x88070605 && sub.Base() == 1004, "a sub-view keeps the file offset");

	auto past = ErrorOf([&]() { (void)r.U32(8); });
	Check(past.find("test span") != std::string::npos && past.find("file offset 1008") != std::string::npos &&
			  past.find("4 byte(s)") != std::string::npos,
		  "a read past the end names the span, the file offset and the size");
	auto inner = ErrorOf([&]() { (void)sub.U16(3); });
	Check(inner.find("inner") != std::string::npos && inner.find("file offset 1007") != std::string::npos,
		  "a sub-view's error names its own span at the file offset");
	Check(!ErrorOf([&]() { (void)r.Bytes(std::numeric_limits<size_t>::max(), 1); }).empty(),
		  "an offset near SIZE_MAX is refused, not wrapped");
	Check(!ErrorOf([&]() { (void)r.Sub(9, 2, "x"); }).empty(), "a sub-view past the end is refused");
	auto far = ErrorOf([&]() { (void)r.Bytes(std::numeric_limits<size_t>::max(), 1); });
	Check(far.find("file offset") == std::string::npos && far.find("of its 10-byte span") != std::string::npos,
		  "an offset past the view is named in the view, not as a wrapped file offset");
	Check(!ErrorOf([&]() { (void)r.U8(10); }).empty() && !ErrorOf([&]() { (void)r.U16(9); }).empty() &&
			  !ErrorOf([&]() { (void)r.I64(3); }).empty(),
		  "u8 / u16 / i64 past the end are refused");
	auto straddle = ErrorOf([&]() { (void)r.U64(4); });
	Check(straddle.find("8 byte(s)") != std::string::npos && straddle.find("file offset 1004") != std::string::npos,
		  "a u64 straddling the end is refused as an 8-byte read at its own offset");
	ByteReader empty(bytes.data(), 0, "empty span");
	Check(empty.Has(0, 0) && !empty.Has(0, 1) && !ErrorOf([&]() { (void)empty.U8(0); }).empty(),
		  "a zero-size view holds nothing");
	Check(r.Bytes(10, 0) == bytes.data() + 10 && ErrorOf([&]() { (void)r.Bytes(10, 0); }).empty(),
		  "a zero-length read at the end is allowed");
	auto fail = ErrorOf([&]() { r.Fail(2, "checksum 0x1 is not 0x2"); });
	Check(fail.find("file offset 1002: checksum 0x1 is not 0x2") != std::string::npos, "Fail names the offset");

	std::cout << (failures ? "FAILED: " + std::to_string(failures) + " check(s)" : "all checks passed") << std::endl;
	return failures ? 1 : 0;
}
