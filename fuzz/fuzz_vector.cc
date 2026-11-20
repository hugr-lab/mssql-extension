// Fuzz harness: a SQL Server vector(n) value (spec 093 T3, D4) -- the bytes a VECTORTYPE value or a backup
// holds, and the JSON text a session without VECTORSUPPORT receives. DuckDB-free (vector_bytes).
//
// Input: byte 0 picks the form (even: bytes, odd: text, its bit 1 UTF-16), bytes 1-2 the column's
// dimensions (1..1998), byte 3 its element type (bit 0), the rest the value. Checked beyond "no crash":
// a value CheckValue accepts widens and encodes back to exactly its bytes when its elements are finite
// (float16 included: every finite float16 is a float exactly); a text ParseText accepts gives the
// column's number of elements, and its float16 elements round-trip through FloatToHalf. Seeds: V1 / V4's
// bytes and V2's texts (fuzz/corpus/vector).

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "codec/sql_vector/vector_bytes.hpp"

using namespace duckdb::mssql::codec::sql_vector;

static void Require(bool ok) {
	if (!ok) {
		__builtin_trap();
	}
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
	if (size < 4) {
		return 0;
	}
	const uint8_t form = data[0];
	uint16_t dimensions = static_cast<uint16_t>(data[1] | (data[2] << 8)) % 1998 + 1;
	const uint8_t element_type = data[3] & 1;
	const uint8_t *value = data + 4;
	const size_t value_size = size - 4;
	std::vector<float> elements(dimensions);

	if ((form & 1) == 0) {
		// The bytes: the header names its own dimensions, so take them when they are plausible.
		if (value_size >= 4) {
			const uint16_t stated = static_cast<uint16_t>(value[2] | (value[3] << 8));
			if (stated >= 1 && stated <= 1998 && (form & 2)) {
				dimensions = stated;
				elements.resize(dimensions);
			}
		}
		if (CheckValue(value, value_size, dimensions, element_type) != VectorStatus::Ok) {
			return 0;
		}
		Require(value_size == ValueBytes(dimensions, element_type));
		WidenElements(value, dimensions, element_type, elements.data());
		bool finite = true;
		for (float e : elements) {
			finite = finite && std::isfinite(e);
		}
		std::vector<uint8_t> back(ValueBytes(dimensions, element_type));
		uint16_t bad = 0;
		const auto status = EncodeValue(elements.data(), dimensions, element_type, back.data(), bad);
		if (finite) {
			Require(status == VectorStatus::Ok);
			Require(std::memcmp(back.data(), value, value_size) == 0);
		} else {
			Require(status == VectorStatus::NotFinite && bad < dimensions);
		}
		return 0;
	}

	// The text.
	std::string scratch;
	const bool utf16 = (form & 2) != 0;
	const auto status = ParseText(value, value_size, utf16, dimensions, elements.data(), scratch);
	Require(status == VectorStatus::Ok || status == VectorStatus::TextSyntax || status == VectorStatus::TextCount);
	if (status != VectorStatus::Ok) {
		return 0;
	}
	for (float e : elements) {
		uint16_t half = 0;
		if (std::isfinite(e) && FloatToHalf(e, half)) {
			const float widened = HalfToFloat(half);
			uint16_t again = 0;
			Require(FloatToHalf(widened, again) && again == half);
		}
	}
	return 0;
}
