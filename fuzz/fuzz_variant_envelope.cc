// Fuzz harness: a sql_variant value's TDS envelope (spec 093 T1, D3): SplitTdsVariant and EncodeVariant,
// DuckDB-free. The input is one value's bytes after its u32 length. Checked beyond "no crash": a value
// split is encoded or refused with a reason, never both; a payload is well-formed for its tag (a fixed
// type's width, a DECIMAL's varint width and scale with the integer its width implies, a VARCHAR /
// BLOB's varint length equal to the bytes that follow it), and a VARCHAR is valid UTF-8. Seeds: S1's
// 50 values (fuzz/corpus/variant_envelope, written from specs/093-type-kernels/measured/S1.md).

#include <cstddef>
#include <cstdint>
#include <string>

#include "codec/variant/variant_envelope.hpp"
#include "simdutf.h"

using namespace duckdb::mssql::codec::variant;

static void Require(bool ok) {
	if (!ok) {
		__builtin_trap();
	}
}

static uint32_t Varint(const std::string &b, size_t &at) {
	uint32_t v = 0;
	for (int shift = 0;; shift += 7) {
		Require(at < b.size() && shift < 35);
		const uint8_t byte = static_cast<uint8_t>(b[at++]);
		v |= static_cast<uint32_t>(byte & 127) << shift;
		if (!(byte & 128)) {
			return v;
		}
	}
}

static size_t FixedWidth(VariantTag tag) {
	switch (tag) {
	case VariantTag::BOOL_TRUE:
	case VariantTag::BOOL_FALSE:
		return 0;
	case VariantTag::UINT8:
		return 1;
	case VariantTag::INT16:
		return 2;
	case VariantTag::INT32:
	case VariantTag::FLOAT:
	case VariantTag::DATE:
		return 4;
	case VariantTag::INT64:
	case VariantTag::DOUBLE:
	case VariantTag::TIME_MICROS:
	case VariantTag::TIME_NANOS:
	case VariantTag::TIMESTAMP_SEC:
	case VariantTag::TIMESTAMP_MILIS:
	case VariantTag::TIMESTAMP_MICROS:
	case VariantTag::TIMESTAMP_NANOS:
	case VariantTag::TIMESTAMP_MICROS_TZ:
	case VariantTag::TIMESTAMP_NANOS_TZ:
		return 8;
	case VariantTag::UUID:
		return 16;
	default:
		return SIZE_MAX;
	}
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
	WireVariant v;
	std::string why;
	if (size == 0 || !SplitTdsVariant(data, size, v, why)) {
		return 0;
	}
	Require(v.value >= data && v.value + v.value_bytes == data + size);
	std::string blob = "prefix";  // a payload is appended, never written over what the blob holds
	VariantTag tag = VariantTag::VARIANT_NULL;
	EncodeNote note;
	why.clear();
	if (!EncodeVariant(v, blob, tag, note, why)) {
		Require(!why.empty() && blob == "prefix");
		return 0;
	}
	Require(blob.compare(0, 6, "prefix") == 0);
	const std::string payload = blob.substr(6);
	if (tag == VariantTag::DECIMAL) {
		size_t at = 0;
		const uint32_t width = Varint(payload, at), scale = Varint(payload, at);
		Require(width >= 1 && width <= 38 && scale <= width);
		const size_t bytes = width <= 4 ? 2 : width <= 9 ? 4 : width <= 18 ? 8 : 16;
		Require(payload.size() - at == bytes);
	} else if (tag == VariantTag::VARCHAR || tag == VariantTag::BLOB) {
		size_t at = 0;
		const uint32_t length = Varint(payload, at);
		Require(payload.size() - at == length);
		if (tag == VariantTag::VARCHAR) {
			Require(simdutf::validate_utf8(payload.data() + at, length));
		}
	} else {
		Require(FixedWidth(tag) != SIZE_MAX && payload.size() == FixedWidth(tag));
	}
	return 0;
}
