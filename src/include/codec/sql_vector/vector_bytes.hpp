//===----------------------------------------------------------------------===//
// codec/sql_vector/vector_bytes.hpp
//
// Spec 093 T3 (D4): a SQL Server `vector(n)` value, DuckDB-free (D1) so the fuzz harness links it alone.
//
// The bytes (measured V1, V4: the 0xF5 wire value and the stored value are the same): an 8-byte header
// -- A9 01, the dimensions as a u16, the element type (0 float32, 1 float16), three zero bytes -- then
// the little-endian elements. The JSON text the server sends without VECTORSUPPORT, and for float16
// under version 1 (V1, V2): `[e1,e2,...]`, each element printed %.7e.
//===----------------------------------------------------------------------===//

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace duckdb {
namespace mssql {
namespace codec {
namespace sql_vector {

enum class VectorStatus : uint8_t {
	Ok,
	//! The value's length is not 8 + dimensions x element size.
	WrongLength,
	//! The header does not start A9 01, or its reserved bytes are not zero.
	BadHeader,
	//! The header's dimensions are not the column's.
	WrongDimensions,
	//! The header's element type is not the column's.
	WrongElementType,
	//! The text is not `[number,...]`.
	TextSyntax,
	//! The text holds another number of elements than the column's dimensions.
	TextCount,
	//! A NaN or an infinity, which a vector cannot hold (written values only).
	NotFinite,
	//! A value that rounds past float16's +-65504 -- a magnitude of 65520 or more -- for a float16 column
	//! (written values only).
	Float16Overflow
};

//! A few words for an error message: "the header's dimensions are not the column's".
const char *DescribeStatus(VectorStatus status);

//! 8 + dimensions x element size.
size_t ValueBytes(uint16_t dimensions, uint8_t element_type);

//! Whether `size` bytes at `data` are a vector of the column's dimensions and element type.
VectorStatus CheckValue(const uint8_t *data, size_t size, uint16_t dimensions, uint8_t element_type);

//! The elements of a value CheckValue accepted, as float (float32 copied, float16 widened exactly).
void WidenElements(const uint8_t *data, uint16_t dimensions, uint8_t element_type, float *out);

//! The text form, UTF-8 (`utf16` false) or UTF-16LE, into `dimensions` floats: each element parsed to
//! the nearest float (locale-free). A float32 element is the server's 8 significant digits, so about
//! 1.5 % of values read back one ulp from the stored one (V2); a float16 element is exact. `scratch`
//! holds the UTF-16 text narrowed, kept by the caller across values.
VectorStatus ParseText(const uint8_t *data, size_t size, bool utf16, uint16_t dimensions, float *out,
					   std::string &scratch);

//! IEEE binary16 -> float, exact (subnormals included).
float HalfToFloat(uint16_t half);
//! float -> IEEE binary16, rounded to nearest even; false for a NaN, an infinity or a value that rounds
//! past +-65504.
bool FloatToHalf(float value, uint16_t &half);

//! A value's bytes from `dimensions` floats into `out` (ValueBytes of them). On a value it cannot hold
//! (NotFinite, Float16Overflow) `bad_element` is the element's index and nothing is guaranteed in `out`.
VectorStatus EncodeValue(const float *values, uint16_t dimensions, uint8_t element_type, uint8_t *out,
						 uint16_t &bad_element);

}  // namespace sql_vector
}  // namespace codec
}  // namespace mssql
}  // namespace duckdb
