//===----------------------------------------------------------------------===//
// codec/clr/spatial_codec.hpp
//
// Spec 093 T5 (D6): a geometry / geography column (UDT 0xF0, PLP) read bare and decoded on the client
// (ssclrt_wkb.hpp) into ISO WKB -- no `.STAsBinary()` on the server, Z and M kept:
//   - into a GEOMETRY vector (geometry_as 'geometry'): a curve or FullGlobe, which a GEOMETRY cannot hold,
//     is NULL, and the stream logs one WARNING for the column (MSSQLResultStream::SurfaceWarnings);
//   - into a BLOB vector (geometry_as 'wkb'): the WKB, the curves in their ISO forms;
//   - geometry_as 'native' never comes here: the stored bytes are the binary kernel's.
// A value that is not MS-SSCLRT is NULL too, never a read past it and never an error: one value that
// cannot be read does not fail the scan of its table (owner, 2026-11-20). Both are counted in
// SpatialColumnNotes, from which the stream logs one WARNING a column for each.
//===----------------------------------------------------------------------===//

#pragma once

#include <cstdint>
#include <vector>

#include "codec/clr/ssclrt_wkb.hpp"
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

//! The values of a column the kernel read as NULL, for the stream's WARNINGs.
struct SpatialColumnNotes {
	//! A curve, a FullGlobe or collections nested past 15, into a GEOMETRY.
	idx_t not_geometry = 0;
	//! Bytes that are not an MS-SSCLRT value (truncated, a bad figure, shape or segment, ...).
	idx_t malformed = 0;
	//! The first malformed value's status, for the WARNING.
	ssclrt::SpatialStatus first_malformed = ssclrt::SpatialStatus::Ok;
};

//! A chunk's staged values (the assembled PLP bytes) into `out`, GEOMETRY or BLOB: each value sized, then
//! its WKB written straight into the vector's string heap, one decoder (and its arrays' capacity) for the
//! column-chunk, no copy. A value it cannot read -- malformed, a WKB past 4 GiB, or under GEOMETRY one a
//! GEOMETRY cannot hold -- is set NULL in `out` and counted in `notes`. NULL rows are left to
//! PublishStagedValidity.
void DecodeChunkFromStaging(const staging::ColumnStaging &st, idx_t count, const tds::ColumnMetadata &meta, Vector &out,
							SpatialColumnNotes *notes = nullptr);
//! One value, for the per-value path (TypeConverter::ConvertValue) and the constant one; NULL and counted
//! as above.
void DecodeFromTds(const std::vector<uint8_t> &bytes, const tds::ColumnMetadata &meta, Vector &out, idx_t row,
				   SpatialColumnNotes *notes = nullptr);

}  // namespace spatial
}  // namespace codec
}  // namespace mssql
}  // namespace duckdb
