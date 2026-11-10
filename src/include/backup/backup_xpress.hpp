//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// backup/backup_xpress.hpp
//
// Spec 085 task 5: the LZ77 + Huffman ("LZXPRESS Huffman", [MS-XCA] 2.2) decoder for an
// MS_XPRESS backup's blocks. Each 64 KB of output is preceded by a 256-byte table of 512 4-bit
// code lengths; the stream is 16-bit little-endian words read MSB first; decoding stops at the
// block's declared output size (symbol 256 is a match, not an end mark). Every block starts with
// an empty history (D10: 181 of 181 blocks of the sample, every block of E4's encrypted one): a
// match before the block's start is a named error, never a silent fallback. The table is checked
// (not over-subscribed), every read is bounded, the output never passes its size. DuckDB-free.
//===----------------------------------------------------------------------===//

#pragma once

#include <cstddef>
#include <cstdint>

namespace duckdb {
namespace mssql {
namespace backup {

//! Decodes `input` into exactly `output_size` bytes at `output`, or throws FormatError.
//! `output_size` is at least 1: a chain block's output is checked to be 1..65536 bytes.
void XpressHuffmanDecode(const uint8_t *input, size_t input_size, uint8_t *output, size_t output_size);

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb
