#include "codec/variant/variant_codec.hpp"

#include "codec/variant/variant_envelope.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/variant.hpp"
#include "duckdb/common/types/variant/variant_builder.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"

namespace duckdb {
namespace mssql {
namespace codec {
namespace variant {

// The DuckDB-free kernel writes DuckDB's VariantLogicalType numbers: checked here, where both are seen.
#define MSSQL_SAME_TAG(name)                                                                                \
	static_assert(static_cast<uint8_t>(VariantTag::name) == static_cast<uint8_t>(VariantLogicalType::name), \
				  "VariantTag::" #name " is not DuckDB's VariantLogicalType::" #name)
MSSQL_SAME_TAG(VARIANT_NULL);
MSSQL_SAME_TAG(BOOL_TRUE);
MSSQL_SAME_TAG(BOOL_FALSE);
MSSQL_SAME_TAG(INT16);
MSSQL_SAME_TAG(INT32);
MSSQL_SAME_TAG(INT64);
MSSQL_SAME_TAG(UINT8);
MSSQL_SAME_TAG(FLOAT);
MSSQL_SAME_TAG(DOUBLE);
MSSQL_SAME_TAG(DECIMAL);
MSSQL_SAME_TAG(VARCHAR);
MSSQL_SAME_TAG(BLOB);
MSSQL_SAME_TAG(UUID);
MSSQL_SAME_TAG(DATE);
MSSQL_SAME_TAG(TIME_MICROS);
MSSQL_SAME_TAG(TIME_NANOS);
MSSQL_SAME_TAG(TIMESTAMP_SEC);
MSSQL_SAME_TAG(TIMESTAMP_MILIS);
MSSQL_SAME_TAG(TIMESTAMP_MICROS);
MSSQL_SAME_TAG(TIMESTAMP_NANOS);
MSSQL_SAME_TAG(TIMESTAMP_MICROS_TZ);
MSSQL_SAME_TAG(TIMESTAMP_NANOS_TZ);
#undef MSSQL_SAME_TAG

namespace {

//! Emits one root primitive a row: the envelope split and encoded into the builder's row blob.
struct EnvelopeSource {
	const tds::ColumnMetadata &col;
	VariantColumnNotes *notes;

	void EmitEnvelope(const uint8_t *data, size_t size, VariantBuilder &builder) {
		WireVariant wire;
		std::string why;
		const uint32_t offset = static_cast<uint32_t>(builder.blob.size());
		VariantTag tag = VariantTag::VARIANT_NULL;
		EncodeNote note;
		if (!SplitTdsVariant(data, size, wire, why) || !EncodeVariant(wire, builder.blob, tag, note, why)) {
			throw InvalidInputException("MSSQL: column \"%s\" holds %s. The TDS stream is malformed.", col.name, why);
		}
		builder.type_ids.push_back(static_cast<uint8_t>(tag));
		builder.byte_offsets.push_back(offset);
		if (notes) {
			notes->unknown_collation += note.unknown_collation;
			notes->invalid_utf8 += note.invalid_utf8;
		}
	}
};

struct StagedSource : EnvelopeSource {
	const staging::ColumnStaging &st;
	StagedSource(const tds::ColumnMetadata &col_p, VariantColumnNotes *notes_p, const staging::ColumnStaging &st_p)
		: EnvelopeSource{col_p, notes_p}, st(st_p) {}
	bool Emit(idx_t row, VariantBuilder &builder) {
		if (!st.IsValid(row)) {
			return true;  // SQL NULL
		}
		EmitEnvelope(st.buffer.data() + st.offsets[row], st.lengths[row], builder);
		return false;
	}
};

struct OneValueSource : EnvelopeSource {
	const std::vector<uint8_t> &bytes;
	OneValueSource(const tds::ColumnMetadata &col_p, const std::vector<uint8_t> &bytes_p)
		: EnvelopeSource{col_p, nullptr}, bytes(bytes_p) {}
	bool Emit(idx_t, VariantBuilder &builder) {
		EmitEnvelope(bytes.data(), bytes.size(), builder);
		return false;
	}
};

}  // namespace

void DecodeChunkFromStaging(const staging::ColumnStaging &st, idx_t count, const tds::ColumnMetadata &col, Vector &out,
							VariantColumnNotes *notes) {
	StagedSource source(col, notes, st);
	BuildVariant(source, count, out);
}

void DecodeFromTds(const std::vector<uint8_t> &bytes, const tds::ColumnMetadata &col, Vector &out, idx_t row) {
	// BuildVariant fills a fresh vector from row 0: built alone, then copied into place.
	Vector one(LogicalType::VARIANT(), 1);
	OneValueSource source(col, bytes);
	BuildVariant(source, 1, one);
	VectorOperations::Copy(one, out, 1, 0, row);
}

}  // namespace variant
}  // namespace codec
}  // namespace mssql
}  // namespace duckdb
