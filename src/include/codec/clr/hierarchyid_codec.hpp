//===----------------------------------------------------------------------===//
// codec/clr/hierarchyid_codec.hpp
//
// Spec 093 T4 (D5): a hierarchyid column (UDT 0xF0, PLP) read bare and written into a VARCHAR vector as
// its ToString() text, decoded on the client (hierarchyid.hpp) -- no server cast.
//===----------------------------------------------------------------------===//

#pragma once

#include <cstdint>
#include <vector>

#include "codec/clr/hierarchyid.hpp"
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
namespace hierarchyid {

//! The values of a column the kernel read as NULL, for the caller's WARNING (spec 093 T8: a backup's).
struct HierarchyIdColumnNotes {
	//! Bytes that are not a hierarchyid's.
	idx_t malformed = 0;
	//! The first such value's status.
	HierarchyIdStatus first_malformed = HierarchyIdStatus::Ok;
};

//! A chunk's staged values (the assembled PLP bytes) into a VARCHAR `out`: one pass decoding into one
//! buffer, one allocation for the column. A value that is not a hierarchyid's bytes is, with `notes`
//! given (a backup's pages: a damaged page or a value the server took unchecked), set NULL in `out` and
//! counted there; with none (the server's own values on the wire), an error naming the column. NULL rows
//! are left to PublishStagedValidity.
void DecodeChunkFromStaging(const staging::ColumnStaging &st, idx_t count, const tds::ColumnMetadata &meta, Vector &out,
							HierarchyIdColumnNotes *notes);
//! One value, for the per-value path (TypeConverter::ConvertValue).
void DecodeFromTds(const std::vector<uint8_t> &bytes, const tds::ColumnMetadata &meta, Vector &out, idx_t row);

}  // namespace hierarchyid
}  // namespace codec
}  // namespace mssql
}  // namespace duckdb
