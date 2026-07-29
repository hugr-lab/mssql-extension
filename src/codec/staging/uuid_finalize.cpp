//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// codec/staging/uuid_finalize.cpp — spec 055 D6.
//===----------------------------------------------------------------------===//

#include "codec/staging/uuid_finalize.hpp"

#include "mssql_compat.hpp"
#include "tds/encoding/guid_encoding.hpp"

namespace duckdb {
namespace mssql {
namespace codec {
namespace staging {

void FinalizeUuidColumn(const ColumnStaging &st, idx_t count, Vector &out) {
	hugeint_t *result = FlatVector::GetData<hugeint_t>(out);
	const uint8_t *src = st.buffer.data();
	// Positional staging: row N is always at N * 16, so the loop needs no cursor
	// and no validity test. See the header for why running over NULL rows is
	// safe and deliberate.
	for (idx_t row = 0; row < count; row++) {
		result[row] = tds::encoding::GuidEncoding::ConvertGuid(src + row * 16);
	}
}

}  // namespace staging
}  // namespace codec
}  // namespace mssql
}  // namespace duckdb
