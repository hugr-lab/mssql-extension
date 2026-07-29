//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// codec/staging/binary_finalize.cpp — spec 055 D6.
//===----------------------------------------------------------------------===//

#include "codec/staging/binary_finalize.hpp"

#include "mssql_compat.hpp"

#include <cstring>

namespace duckdb {
namespace mssql {
namespace codec {
namespace staging {

void FinalizeBinaryColumn(const ColumnStaging &st, idx_t count, Vector &out) {
	string_t *result = FlatVector::GetData<string_t>(out);
	const idx_t payload = st.PayloadSize();

	if (payload == 0) {
		// All-NULL or all-empty: an empty string_t is inlined, so it needs
		// neither storage nor a valid pointer, and nothing is allocated.
		static const char EMPTY[1] = {0};
		for (idx_t row = 0; row < count; row++) {
			if (st.IsValid(row)) {
				result[row] = string_t(EMPTY, 0);
			}
		}
		return;
	}

	// One allocation and one copy for the whole column, in place of one of each
	// per value. The size is exact — no upper bound to guess, because binary
	// output is its input.
	//
	// A payload of at most 12 bytes makes EmptyString inline its storage into
	// this local. That stays correct: every value is then shorter than the
	// inline threshold too, so each string_t below copies its bytes rather than
	// retaining a pointer into storage that dies with this frame.
	string_t blob_slot = StringVector::EmptyString(out, payload);
	char *const blob = blob_slot.GetDataWriteable();
	std::memcpy(blob, st.buffer.data(), payload);

	for (idx_t row = 0; row < count; row++) {
		if (!st.IsValid(row)) {
			continue;
		}
		result[row] = string_t(blob + st.offsets[row], st.lengths[row]);
	}
}

}  // namespace staging
}  // namespace codec
}  // namespace mssql
}  // namespace duckdb
