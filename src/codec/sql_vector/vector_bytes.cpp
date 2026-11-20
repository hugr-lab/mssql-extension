// Spec 093 T3 (D4): a SQL Server vector value's bytes and text. DuckDB-free (D1); fast_float is
// DuckDB's third-party parser, header-only and locale-free.

#include "codec/sql_vector/vector_bytes.hpp"

#include <cmath>
#include <cstring>

#include "fast_float/fast_float.h"

namespace duckdb {
namespace mssql {
namespace codec {
namespace sql_vector {

namespace {

constexpr uint8_t MAGIC_0 = 0xA9;
constexpr uint8_t MAGIC_1 = 0x01;
constexpr size_t HEADER = 8;

inline uint16_t ElementBytes(uint8_t element_type) {
	return element_type == 1 ? 2 : 4;
}

inline uint16_t LoadU16(const uint8_t *p) {
	return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

inline bool IsSpace(char c) {
	return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

}  // namespace

const char *DescribeStatus(VectorStatus status) {
	switch (status) {
	case VectorStatus::Ok:
		return "valid";
	case VectorStatus::WrongLength:
		return "its length is not 8 bytes of header and the column's elements";
	case VectorStatus::BadHeader:
		return "its header is not a vector's (A9 01 ... with three zero bytes)";
	case VectorStatus::WrongDimensions:
		return "its header's dimensions are not the column's";
	case VectorStatus::WrongElementType:
		return "its header's element type is not the column's";
	case VectorStatus::TextSyntax:
		return "its text is not a JSON array of numbers";
	case VectorStatus::TextCount:
		return "its text holds another number of elements than the column's dimensions";
	case VectorStatus::NotFinite:
		return "a vector holds no NaN or infinity";
	case VectorStatus::Float16Overflow:
		return "it rounds past float16's largest value, 65504 (a magnitude of 65520 or more; below that it "
			   "rounds to 65504)";
	}
	return "invalid";
}

size_t ValueBytes(uint16_t dimensions, uint8_t element_type) {
	return HEADER + static_cast<size_t>(dimensions) * ElementBytes(element_type);
}

VectorStatus CheckValue(const uint8_t *data, size_t size, uint16_t dimensions, uint8_t element_type) {
	if (size != ValueBytes(dimensions, element_type)) {
		return VectorStatus::WrongLength;
	}
	if (data[0] != MAGIC_0 || data[1] != MAGIC_1 || data[5] != 0 || data[6] != 0 || data[7] != 0) {
		return VectorStatus::BadHeader;
	}
	if (LoadU16(data + 2) != dimensions) {
		return VectorStatus::WrongDimensions;
	}
	if (data[4] != element_type) {
		return VectorStatus::WrongElementType;
	}
	return VectorStatus::Ok;
}

void WidenElements(const uint8_t *data, uint16_t dimensions, uint8_t element_type, float *out) {
	const uint8_t *elements = data + HEADER;
	if (element_type == 0) {
		std::memcpy(out, elements, static_cast<size_t>(dimensions) * 4);  // little-endian, as every target is
		return;
	}
	for (uint16_t i = 0; i < dimensions; i++) {
		out[i] = HalfToFloat(LoadU16(elements + 2 * i));
	}
}

VectorStatus ParseText(const uint8_t *data, size_t size, bool utf16, uint16_t dimensions, float *out,
					   std::string &scratch) {
	const char *p;
	const char *end;
	if (utf16) {
		// The text is ASCII (digits, signs, 'e', '.', ',', brackets); a UTF-16 unit above 0x7F is not it.
		scratch.clear();
		for (size_t i = 0; i + 1 < size; i += 2) {
			const uint16_t unit = LoadU16(data + i);
			if (unit > 0x7F) {
				return VectorStatus::TextSyntax;
			}
			scratch.push_back(static_cast<char>(unit));
		}
		p = scratch.data();
		end = p + scratch.size();
	} else {
		p = reinterpret_cast<const char *>(data);
		end = p + size;
	}
	while (p < end && IsSpace(*p)) {
		p++;
	}
	if (p == end || *p != '[') {
		return VectorStatus::TextSyntax;
	}
	p++;
	uint16_t count = 0;
	while (true) {
		while (p < end && IsSpace(*p)) {
			p++;
		}
		if (count >= dimensions) {
			return VectorStatus::TextCount;
		}
		float value;
		const auto result = duckdb_fast_float::from_chars(p, end, value);
		if (result.ec != std::errc() || result.ptr == p) {
			return VectorStatus::TextSyntax;
		}
		out[count++] = value;
		p = result.ptr;
		while (p < end && IsSpace(*p)) {
			p++;
		}
		if (p == end) {
			return VectorStatus::TextSyntax;
		}
		if (*p == ',') {
			p++;
			continue;
		}
		if (*p != ']') {
			return VectorStatus::TextSyntax;
		}
		p++;
		break;
	}
	while (p < end && IsSpace(*p)) {
		p++;
	}
	if (p != end) {
		return VectorStatus::TextSyntax;
	}
	return count == dimensions ? VectorStatus::Ok : VectorStatus::TextCount;
}

float HalfToFloat(uint16_t half) {
	const uint32_t sign = static_cast<uint32_t>(half & 0x8000) << 16;
	const uint32_t exponent = (half >> 10) & 0x1F;
	const uint32_t mantissa = half & 0x3FF;
	uint32_t bits;
	if (exponent == 0) {
		// Zero or a subnormal: mantissa x 2^-24, exact in a float.
		const float magnitude = static_cast<float>(mantissa) * (1.0f / 16777216.0f);
		std::memcpy(&bits, &magnitude, 4);
		bits |= sign;
	} else if (exponent == 31) {
		bits = sign | 0x7F800000u | (mantissa << 13);
	} else {
		bits = sign | ((exponent + 112) << 23) | (mantissa << 13);
	}
	float out;
	std::memcpy(&out, &bits, 4);
	return out;
}

bool FloatToHalf(float value, uint16_t &half) {
	uint32_t bits;
	std::memcpy(&bits, &value, 4);
	const uint16_t sign = static_cast<uint16_t>((bits >> 16) & 0x8000);
	const uint32_t magnitude = bits & 0x7FFFFFFFu;
	if (magnitude >= 0x7F800000u) {
		return false;  // NaN, infinity
	}
	if (magnitude >= 0x477FF000u) {
		return false;  // 65520 and above round to infinity (65504 is the largest float16)
	}
	const uint32_t exponent = magnitude >> 23;
	if (magnitude < 0x38800000u) {
		// Below 2^-14: a float16 subnormal, mantissa = value / 2^-24 rounded to nearest even.
		if (exponent < 102) {
			half = sign;  // below 2^-25: rounds to zero
			return true;
		}
		const uint32_t mantissa = (magnitude & 0x7FFFFFu) | 0x800000u;
		const uint32_t shift = 126 - exponent;	// 14 .. 24
		uint32_t m = mantissa >> shift;
		const uint32_t rest = mantissa & ((1u << shift) - 1);
		const uint32_t halfway = 1u << (shift - 1);
		if (rest > halfway || (rest == halfway && (m & 1))) {
			m++;  // carries into the exponent field at 0x400: the smallest normal, as it should
		}
		half = static_cast<uint16_t>(sign | m);
		return true;
	}
	uint32_t h = ((exponent - 112) << 10) | ((magnitude & 0x7FFFFFu) >> 13);
	const uint32_t rest = magnitude & 0x1FFFu;
	if (rest > 0x1000u || (rest == 0x1000u && (h & 1))) {
		h++;  // a carry into the exponent is the next binade; past 65504 was refused above
	}
	half = static_cast<uint16_t>(sign | h);
	return true;
}

VectorStatus EncodeValue(const float *values, uint16_t dimensions, uint8_t element_type, uint8_t *out,
						 uint16_t &bad_element) {
	out[0] = MAGIC_0;
	out[1] = MAGIC_1;
	out[2] = static_cast<uint8_t>(dimensions & 0xFF);
	out[3] = static_cast<uint8_t>(dimensions >> 8);
	out[4] = element_type;
	out[5] = out[6] = out[7] = 0;
	uint8_t *elements = out + HEADER;
	for (uint16_t i = 0; i < dimensions; i++) {
		const float value = values[i];
		if (!std::isfinite(value)) {
			bad_element = i;
			return VectorStatus::NotFinite;
		}
		if (element_type == 0) {
			std::memcpy(elements + 4 * i, &value, 4);
			continue;
		}
		uint16_t half;
		if (!FloatToHalf(value, half)) {
			bad_element = i;
			return VectorStatus::Float16Overflow;
		}
		elements[2 * i] = static_cast<uint8_t>(half & 0xFF);
		elements[2 * i + 1] = static_cast<uint8_t>(half >> 8);
	}
	return VectorStatus::Ok;
}

}  // namespace sql_vector
}  // namespace codec
}  // namespace mssql
}  // namespace duckdb
