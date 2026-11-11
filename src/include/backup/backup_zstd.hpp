//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// backup/backup_zstd.hpp
//
// Spec 085 task 6: the ZSTD blocks of a compressed backup. Each block's payload is one
// self-contained zstd frame (X§5, W§3) of the block's declared output size: decoded into a buffer
// of exactly that size, refused if the frame declares more, holds trailing bytes, or decodes to
// anything else. zstd from vcpkg, statically linked (vcpkg.json's note). DuckDB-free (D15).
//===----------------------------------------------------------------------===//

#pragma once

#include <cstddef>
#include <cstdint>

namespace duckdb {
namespace mssql {
namespace backup {

//! Decodes the zstd frame `input` into exactly `output_size` bytes at `output`, or throws.
void ZstdDecode(const uint8_t *input, size_t input_size, uint8_t *output, size_t output_size);

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb
