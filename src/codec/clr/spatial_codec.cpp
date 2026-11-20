// Spec 093 T5 (D6): a geometry / geography column decoded on the client into ISO WKB.

#include "codec/clr/spatial_codec.hpp"

#include "codec/clr/ssclrt_wkb.hpp"
#include "codec/staging/column_staging.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/string_vector.hpp"
#include "tds/tds_column_metadata.hpp"

namespace duckdb {
namespace mssql {
namespace codec {
namespace spatial {

namespace {

//! One value into `out`'s slot `row`: its WKB written straight into the vector's string heap (Read sizes
//! it first), or NULL for a value a GEOMETRY cannot hold. Anything else is an error naming the column.
void DecodeInto(ssclrt::WkbDecoder &decoder, const uint8_t *data, size_t size, const tds::ColumnMetadata &meta,
				bool geography, bool into_geometry, Vector &out, string_t *result, idx_t row) {
	const auto status = decoder.Read(data, size, geography, !into_geometry);
	if (status == ssclrt::SpatialStatus::NotGeometry) {
		FlatVector::SetNull(out, row, true);
		return;
	}
	if (status != ssclrt::SpatialStatus::Ok) {
		throw InvalidInputException("MSSQL: column '%s': a %s value cannot be read: %s", meta.name, meta.udt_type_name,
									ssclrt::DescribeStatus(status));
	}
	string_t wkb = StringVector::EmptyString(out, decoder.Size());
	decoder.Write(wkb.GetDataWriteable());
	wkb.Finalize();
	result[row] = wkb;
}

}  // namespace

void DecodeChunkFromStaging(const staging::ColumnStaging &st, idx_t count, const tds::ColumnMetadata &meta,
							Vector &out) {
	const bool into_geometry = out.GetType().id() == LogicalTypeId::GEOMETRY;
	const bool geography = meta.IsGeographyUdt();
	// One decoder for the column-chunk: its arrays keep their capacity from one value to the next.
	ssclrt::WkbDecoder decoder;
	string_t *result = FlatVector::GetDataMutable<string_t>(out);
	const uint8_t *payload = st.buffer.data();
	for (idx_t row = 0; row < count; row++) {
		if (st.IsValid(row)) {
			DecodeInto(decoder, payload + st.offsets[row], st.lengths[row], meta, geography, into_geometry, out, result,
					   row);
		}
	}
}

void DecodeFromTds(const std::vector<uint8_t> &bytes, const tds::ColumnMetadata &meta, Vector &out, idx_t row) {
	ssclrt::WkbDecoder decoder;
	DecodeInto(decoder, bytes.data(), bytes.size(), meta, meta.IsGeographyUdt(),
			   out.GetType().id() == LogicalTypeId::GEOMETRY, out, FlatVector::GetDataMutable<string_t>(out), row);
}

}  // namespace spatial
}  // namespace codec
}  // namespace mssql
}  // namespace duckdb
