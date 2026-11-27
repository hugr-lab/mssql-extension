// Spec 086 PR 6: a json column's stored binary form into JSON or VARIANT (codec/json/json_codec.hpp). [DuckDB]

#include "codec/json/json_codec.hpp"

#include "codec/json/json_binary.hpp"
#include "duckdb/common/operator/cast_operators.hpp"
#include "duckdb/common/types/variant/variant_builder.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/vector_writer.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "utf8proc_wrapper.hpp"

#include <algorithm>
#include <limits>

namespace duckdb {
namespace mssql {
namespace codec {
namespace json {

namespace {

void Note(JsonColumnNotes *notes, const std::string &why) {
	if (notes && notes->malformed++ == 0) {
		notes->first_malformed = why;
	}
}

//! The value's server text into `text`, checked whole: a document E9 knows, its text valid UTF-8 (a JSON is a
//! VARCHAR, and every key and string of the VARIANT is in the text), and for the VARIANT (`check_keys`) every
//! key unescaping as DuckDB's parse would. False with `why` otherwise.
bool RenderChecked(const uint8_t *bytes, idx_t size, std::string &text, std::string &why, bool check_keys = false) {
	if (!RenderJsonText(bytes, size, text, why, check_keys)) {
		return false;
	}
	if (!Utf8Proc::IsValid(text.data(), text.size())) {
		why = "a json value whose strings or keys are not valid UTF-8";
		return false;
	}
	return true;
}

void EmitFlag(VariantBuilder &b, VariantLogicalType type) {
	b.type_ids.push_back(static_cast<uint8_t>(type));
	b.byte_offsets.push_back(static_cast<uint32_t>(b.blob.size()));
}

template <class T>
void EmitNumber(VariantBuilder &b, VariantLogicalType type, T value) {
	const auto offset = static_cast<uint32_t>(b.blob.size());
	VariantBuilderAppendFixed<T>(b.blob, value);
	b.type_ids.push_back(static_cast<uint8_t>(type));
	b.byte_offsets.push_back(offset);
}

//! A decimal as DuckDB's JSON -> VARIANT types its text (yyjson): an integer literal (scale 0) a UINT64 when
//! it is not negative and fits, an INT64 when negative and fits, else -- and any number with a fraction -- a
//! DOUBLE of the text, correctly rounded.
void EmitDecimal(VariantBuilder &b, const JsonDocument::Decimal &d) {
	if (d.scale == 0 && d.high == 0) {
		if (!d.negative) {
			EmitNumber<uint64_t>(b, VariantLogicalType::UINT64, d.low);
			return;
		}
		if (d.low <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) + 1) {
			EmitNumber<int64_t>(b, VariantLogicalType::INT64, d.low == 0 ? 0 : static_cast<int64_t>(0 - d.low));
			return;
		}
	}
	std::string text;
	AppendDecimalText(d, text);
	double value = 0;
	TryCast::Operation<string_t, double>(string_t(text.data(), static_cast<uint32_t>(text.size())), value, true);
	EmitNumber<double>(b, VariantLogicalType::DOUBLE, value);
}

struct Member {
	std::string key;  // unescaped, as DuckDB keys it
	uint32_t value;
};

//! Emits value `v` of `doc`; the document has been checked whole (RenderChecked), so every read succeeds.
void EmitValue(const JsonDocument &doc, uint32_t v, VariantBuilder &b) {
	std::string why;
	JsonDocument::Kind kind;
	doc.KindOf(v, kind, why);
	switch (kind) {
	case JsonDocument::Kind::Null:
		b.EmitNull();
		return;
	case JsonDocument::Kind::False:
		EmitFlag(b, VariantLogicalType::BOOL_FALSE);
		return;
	case JsonDocument::Kind::True:
		EmitFlag(b, VariantLogicalType::BOOL_TRUE);
		return;
	case JsonDocument::Kind::Integer: {
		int64_t value = 0;
		doc.IntegerOf(v, value, why);
		if (value >= 0) {
			EmitNumber<uint64_t>(b, VariantLogicalType::UINT64, static_cast<uint64_t>(value));
		} else {
			EmitNumber<int64_t>(b, VariantLogicalType::INT64, value);
		}
		return;
	}
	case JsonDocument::Kind::Decimal: {
		JsonDocument::Decimal d;
		doc.DecimalOf(v, d, why);
		EmitDecimal(b, d);
		return;
	}
	case JsonDocument::Kind::String: {
		const uint8_t *bytes = nullptr;
		uint32_t length = 0;
		doc.StringOf(v, bytes, length, why);
		const auto offset = static_cast<uint32_t>(b.blob.size());
		VariantBuilderAppendVarint(b.blob, length);
		VariantBuilderAppendBytes(b.blob, bytes, length);
		b.type_ids.push_back(static_cast<uint8_t>(VariantLogicalType::VARCHAR));
		b.byte_offsets.push_back(offset);
		return;
	}
	case JsonDocument::Kind::Object:
	case JsonDocument::Kind::Array: {
		JsonDocument::Container c;
		doc.ContainerOf(v, c, why);
		std::vector<Member> members;
		members.reserve(c.count);
		for (uint32_t i = 0; i < c.count; i++) {
			bool deleted = false;
			const uint8_t *key = nullptr;
			uint32_t key_length = 0;
			uint32_t member = 0;
			doc.Member(c, i, deleted, key, key_length, member, why);
			if (deleted) {
				continue;
			}
			members.push_back(Member{std::string(), member});
			if (c.object) {
				UnescapeJsonKey(key, key_length, members.back().key);  // checked by RenderChecked(…, true)
			}
		}
		if (!c.object) {
			b.EmitArray(members.size(), [&](idx_t i) { EmitValue(doc, members[i].value, b); });
			return;
		}
		// As DuckDB's JSON -> VARIANT: one child per key, the last value of a key named twice (two stored keys
		// can unescape alike, "a\/b" and "a/b"); sorted by key, as VariantBuilder's EmitObject takes them.
		std::stable_sort(members.begin(), members.end(),
						 [](const Member &l, const Member &r) { return l.key < r.key; });
		size_t kept = 0;
		for (size_t i = 0; i < members.size(); i++) {
			if (i + 1 < members.size() && members[i + 1].key == members[i].key) {
				continue;  // a later member has this key
			}
			if (kept != i) {
				members[kept] = std::move(members[i]);
			}
			kept++;
		}
		members.resize(kept);
		b.EmitObject(
			members.size(),
			[&](idx_t i) { return string_t(members[i].key.data(), static_cast<uint32_t>(members[i].key.size())); },
			[&](idx_t i) { EmitValue(doc, members[i].value, b); });
		return;
	}
	}
}

//! A value's document emitted into `builder`; false (a NULL row, noted) when it is none.
bool EmitDocument(const uint8_t *bytes, idx_t size, VariantBuilder &builder, JsonColumnNotes *notes,
				  std::string &text) {
	std::string why;
	JsonDocument doc;
	if (!RenderChecked(bytes, size, text, why, true) || !doc.Open(bytes, size, why)) {
		Note(notes, why);
		return false;
	}
	EmitValue(doc, doc.Root(), builder);
	return true;
}

struct StagedSource {
	const staging::ColumnStaging &st;
	JsonColumnNotes *notes;
	std::string text;
	//! BuildVariant's source: true for a NULL row (a NULL value, or one that is no json document).
	bool Emit(idx_t row, VariantBuilder &builder) {
		if (!st.IsValid(row)) {
			return true;
		}
		return !EmitDocument(st.buffer.data() + st.offsets[row], st.lengths[row], builder, notes, text);
	}
};

struct OneValueSource {
	const std::vector<uint8_t> &bytes;
	JsonColumnNotes *notes;
	std::string text;
	bool Emit(idx_t, VariantBuilder &builder) {
		return !EmitDocument(bytes.data(), bytes.size(), builder, notes, text);
	}
};

bool IsVariant(const Vector &out) {
	return out.GetType().id() == LogicalTypeId::VARIANT;
}

}  // namespace

void DecodeChunkFromStaging(const staging::ColumnStaging &st, idx_t count, const tds::ColumnMetadata &, Vector &out,
							JsonColumnNotes *notes) {
	if (IsVariant(out)) {
		StagedSource source{st, notes, std::string()};
		BuildVariant(source, count, out);
		return;
	}
	auto writer = FlatVector::Writer<string_t>(out, count);
	std::string text, why;
	for (idx_t row = 0; row < count; row++) {
		if (!st.IsValid(row)) {
			writer.WriteNull();
			continue;
		}
		if (!RenderChecked(st.buffer.data() + st.offsets[row], st.lengths[row], text, why)) {
			Note(notes, why);
			writer.WriteNull();
			continue;
		}
		writer.WriteValue(string_t(text.data(), static_cast<uint32_t>(text.size())));
	}
}

void DecodeFromTds(const std::vector<uint8_t> &bytes, const tds::ColumnMetadata &, Vector &out, idx_t row,
				   JsonColumnNotes *notes) {
	if (IsVariant(out)) {
		// BuildVariant fills a fresh vector from row 0: built alone, then copied into place.
		Vector one(LogicalType::VARIANT(), 1);
		OneValueSource source{bytes, notes, std::string()};
		BuildVariant(source, 1, one);
		VectorOperations::Copy(one, out, 1, 0, row);
		return;
	}
	std::string text, why;
	if (!RenderChecked(bytes.data(), bytes.size(), text, why)) {
		Note(notes, why);
		FlatVector::SetNull(out, row, true);
		return;
	}
	FlatVector::GetDataMutable<string_t>(out)[row] = StringVector::AddString(out, text);
}

}  // namespace json
}  // namespace codec
}  // namespace mssql
}  // namespace duckdb
