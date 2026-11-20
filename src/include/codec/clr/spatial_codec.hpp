//===----------------------------------------------------------------------===//
// codec/clr/spatial_codec.hpp
//
// Spec 093 T5 (D6): a geometry / geography column (UDT 0xF0, PLP) read bare and decoded on the client
// (ssclrt_wkb.hpp) into ISO WKB -- no `.STAsBinary()` on the server, Z and M kept:
//   - into a GEOMETRY vector (geometry_as 'geometry'): a curve or FullGlobe, which a GEOMETRY cannot hold,
//     is NULL, and the stream logs one WARNING for the column (MSSQLResultStream::SurfaceWarnings);
//   - into a BLOB vector (geometry_as 'wkb'): the WKB, the curves in their ISO forms;
//   - geometry_as 'native' never comes here: the stored bytes are the binary kernel's.
// A value that is not MS-SSCLRT is an error naming the column, never a read past it.
//===----------------------------------------------------------------------===//

#pragma once

#include <cstdint>
#include <vector>

#include "duckdb/common/types.hpp"
#include "duckdb/common/types/vector.hpp"

namespace duckdb {
namespace tds {
struct ColumnMetadata;
}
namespace mssql {
namespace codec {
namespace staging {
struct ColumnStaging;
}
namespace spatial {

//! A chunk's staged values (the assembled PLP bytes) into `out`, GEOMETRY or BLOB: each value sized, then
//! its WKB written straight into the vector's string heap, one decoder (and its arrays' capacity) for the
//! column-chunk, no copy. A WKB past 4 GiB is refused by name. Under GEOMETRY a value it cannot hold is set NULL in
//! `out` (the kernel's NULLs are in the mask before the staged validity is ANDed in, which is how the
//! stager counts them). NULL rows are left to PublishStagedValidity.
void DecodeChunkFromStaging(const staging::ColumnStaging &st, idx_t count, const tds::ColumnMetadata &meta,
							Vector &out);
//! One value, for the per-value path (TypeConverter::ConvertValue) and the constant one. Under GEOMETRY a
//! value it cannot hold is set NULL.
void DecodeFromTds(const std::vector<uint8_t> &bytes, const tds::ColumnMetadata &meta, Vector &out, idx_t row);

}  // namespace spatial
}  // namespace codec
}  // namespace mssql
}  // namespace duckdb
