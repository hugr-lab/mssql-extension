// Spec 093 T4 (D5): a hierarchyid column decoded on the client into its ToString() text.

#include "codec/clr/hierarchyid_codec.hpp"

#include <cstring>
#include <string>

#include "codec/clr/hierarchyid.hpp"
#include "codec/staging/column_staging.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/string_vector.hpp"
#include "tds/tds_column_metadata.hpp"

namespace duckdb {
namespace mssql {
namespace codec {
namespace hierarchyid {

namespace {

void DecodeOrRefuse(const uint8_t *data, size_t size, const tds::ColumnMetadata &meta, std::string &out) {
	const auto status = Decode(data, size, out);
	if (status != HierarchyIdStatus::Ok) {
		throw InvalidInputException("MSSQL: column '%s': a hierarchyid value cannot be read: %s", meta.name,
									DescribeStatus(status));
	}
}

}  // namespace

void DecodeChunkFromStaging(const staging::ColumnStaging &st, idx_t count, const tds::ColumnMetadata &meta, Vector &out,
							HierarchyIdColumnNotes *notes) {
	// The texts back to back in one buffer, then one allocation for the column, as the binary kernel does.
	std::string text;
	text.reserve(st.payload_used * 3 + count * 2);
	std::vector<uint32_t> ends(count, 0);
	const uint8_t *payload = st.buffer.data();
	for (idx_t row = 0; row < count; row++) {
		if (st.IsValid(row)) {
			if (!notes) {
				DecodeOrRefuse(payload + st.offsets[row], st.lengths[row], meta, text);
			} else {
				const size_t begin = text.size();
				const auto status = Decode(payload + st.offsets[row], st.lengths[row], text);
				if (status != HierarchyIdStatus::Ok) {
					text.resize(begin);	 // what Decode appended before it stopped
					FlatVector::SetNull(out, row, true);
					if (notes->malformed++ == 0) {
						notes->first_malformed = status;
					}
				}
			}
		}
		ends[row] = static_cast<uint32_t>(text.size());
	}
	string_t *result = FlatVector::GetDataMutable<string_t>(out);
	// The buffer may be empty (every value NULL, or read as NULL); a text of at most 12 bytes is inlined by
	// string_t, so the blob below may be inline too.
	string_t blob_slot = StringVector::EmptyString(out, text.size());
	char *const blob = blob_slot.GetDataWriteable();
	std::memcpy(blob, text.data(), text.size());
	uint32_t begin = 0;
	for (idx_t row = 0; row < count; row++) {
		if (st.IsValid(row)) {	// a value read as NULL is an empty text under its NULL
			result[row] = string_t(blob + begin, ends[row] - begin);
		}
		begin = ends[row];
	}
}

void DecodeFromTds(const std::vector<uint8_t> &bytes, const tds::ColumnMetadata &meta, Vector &out, idx_t row) {
	std::string text;
	DecodeOrRefuse(bytes.data(), bytes.size(), meta, text);
	FlatVector::GetDataMutable<string_t>(out)[row] = StringVector::AddString(out, text);
}

}  // namespace hierarchyid
}  // namespace codec
}  // namespace mssql
}  // namespace duckdb
