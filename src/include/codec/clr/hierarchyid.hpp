//===----------------------------------------------------------------------===//
// codec/clr/hierarchyid.hpp
//
// Spec 093 T4 (D5): a SQL Server `hierarchyid` value -- the MS-SSCLRT 2.2.2 bit string the server stores
// and sends (UDT 0xF0, PLP) -- as the `ToString()` text, `/1/2.5/-3/`. DuckDB-free (D1), so the fuzz
// harness links it alone.
//
// The value is a bit string, most significant bit first, padded with zero bits to a byte; the root is
// zero bytes. Each level is one or more labels -- the integers of `a.b.c` -- and each label is a prefix
// naming its range, the bits of (value - the range's minimum) interleaved with fixed bits, and a last bit
// T: 1 ends the level (`/`), 0 continues it (`.`). A label that continues its level is stored as its
// value + 1. Checked against the server's own bytes (G2: 0x5B40 is /1/2/, 0x6467D0 is /1.5/-2/).
//===----------------------------------------------------------------------===//

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace duckdb {
namespace mssql {
namespace codec {
namespace hierarchyid {

//! A value as stored is at most 892 bytes (the UDT's MAX_BYTE_SIZE, measured G2).
constexpr size_t MAX_BYTES = 892;

enum class HierarchyIdStatus : uint8_t {
	Ok,
	//! More than 892 bytes.
	TooLong,
	//! No label range starts with these bits.
	UnknownPrefix,
	//! A fixed bit of the label's pattern is not what the pattern says.
	BadFixedBit,
	//! The bits end inside a label, or inside a level (its last label continues it).
	Truncated,
	//! The padding after the last level is not zero bits, or not shorter than a byte.
	BadPadding,
	//! The text is not `/`, or `/label[.label].../` levels of integers.
	TextSyntax,
	//! A label past the range the format holds.
	OutOfRange
};

//! A few words for an error message.
const char *DescribeStatus(HierarchyIdStatus status);

//! The bytes as the `ToString()` text, appended to `out`.
HierarchyIdStatus Decode(const uint8_t *data, size_t size, std::string &out);

//! The text as the bytes (the inverse of Decode: one encoding per value). For the tests and the fuzz
//! harness's round trip; writes stay a string literal the server parses (D5).
HierarchyIdStatus Encode(const std::string &text, std::vector<uint8_t> &out);

}  // namespace hierarchyid
}  // namespace codec
}  // namespace mssql
}  // namespace duckdb
