//===----------------------------------------------------------------------===//
// codec/sql_vector/vector_codec.hpp
//
// Spec 093 T3 (D4): a SQL Server `vector(n)` as a DuckDB FLOAT[n] -- the read into an ARRAY(FLOAT, n)
// vector from either wire form (VECTORTYPE 0xF5, or the JSON text under UserType 255), the statement
// path's literal and the bulk wire's value. The bytes and the text are vector_bytes' (DuckDB-free).
//===----------------------------------------------------------------------===//

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "duckdb/common/types.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/vector/unified_vector_format.hpp"

namespace duckdb {
namespace tds {
struct ColumnMetadata;
}
namespace mssql {
struct BCPColumnMetadata;
namespace codec {
namespace staging {
struct ColumnStaging;
}
namespace sql_vector {

//! FLOAT[n]: the DuckDB type a vector(n) is read as and written from.
bool IsVectorType(const LogicalType &type);
//! ARRAY(FLOAT, dimensions).
LogicalType FloatArrayType(uint16_t dimensions);

//! A chunk's staged values (either wire form) into `out`, an ARRAY(FLOAT, n): a value of another shape
//! than the column's is an error naming the column. NULL rows' elements are zero; their validity is
//! PublishStagedValidity's.
void DecodeChunkFromStaging(const staging::ColumnStaging &st, idx_t count, const tds::ColumnMetadata &meta,
							Vector &out);
//! One value, for the per-value path (TypeConverter::ConvertValue).
void DecodeFromTds(const std::vector<uint8_t> &bytes, const tds::ColumnMetadata &meta, Vector &out, idx_t row);

//! The column a CTAS / COPY / CREATE TABLE creates for an ARRAY type: `vector(n)` for FLOAT[n] (n up to
//! 1998), on a server that has the type (2025, Azure SQL; any other refuses it by name, error 2715).
//! A DOUBLE[n] is refused with the cast named (vector elements are float32: a narrowing the user
//! writes), and any other array, as before, for SQL Server having no array type.
std::string FormatDdlTypeName(const LogicalType &type);

//! The statement path's literal of one FLOAT[n] value: N'[e1,...]' with 9 significant digits, which reads
//! back exactly as the float32 it was; the server converts the text to the column's vector type,
//! float16 included (rounding as it rounds). A NULL is NULL; a NULL element, a NaN or an infinity is
//! refused naming the column.
std::string FormatLiteral(const Value &value, const std::string &column);

//! The bulk wire's value of row `row` of a FLOAT[n] vector into a vector column (declared varbinary on
//! the wire, measured V6): a u16 length and the bytes, header and elements, float16 rounded to nearest
//! even for a float16 column. A NULL element, a NaN / infinity or a float16 overflow is refused naming
//! the column. The family encoder's signature (BCPRowEncoder); `fmt` is `vec`'s.
void EncodeToBcp(Vector &vec, const UnifiedVectorFormat &fmt, idx_t row, const BCPColumnMetadata &col,
				 vector<uint8_t> &out);
//! The columnar bulk path (BCPRowEncoder's scatter, one call per column per block of rows): rows
//! [row_begin, row_begin + rows) of a FLOAT[n] `vec` (its child flat, as PrepareColumnStates leaves it),
//! each its u16 length and bytes -- the header and, for float32, the row's elements copied whole, for
//! float16 narrowed -- or 0xFFFF for a NULL row. Strided: row i at `dst + i * stride`, every row valid.
//! Cursor: row r at `dst + cursor[r]`, which it advances. A NULL element, a NaN / infinity or a float16
//! overflow is refused by name, the row named.
void ScatterChunkStrided(uint8_t *dst, size_t stride, idx_t row_begin, idx_t rows, const Vector &vec,
						 const UnifiedVectorFormat &fmt, const BCPColumnMetadata &col);
void ScatterChunkCursor(uint8_t *dst, size_t *cursor, idx_t row_begin, idx_t rows, const Vector &vec,
						const UnifiedVectorFormat &fmt, const BCPColumnMetadata &col);

//! The same for one Value (BCPRowEncoder::EncodeValue).
void EncodeValueToBcp(const Value &value, const BCPColumnMetadata &col, vector<uint8_t> &out);

}  // namespace sql_vector
}  // namespace codec
}  // namespace mssql
}  // namespace duckdb
