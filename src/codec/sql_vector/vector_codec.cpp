// Spec 093 T3 (D4): a SQL Server vector(n) as a DuckDB FLOAT[n]. The bytes and the text are
// vector_bytes' (DuckDB-free); this file reads them into, and writes them from, DuckDB vectors.

#include "codec/sql_vector/vector_codec.hpp"

#include <cmath>
#include <cstring>

#include "codec/sql_vector/vector_bytes.hpp"
#include "codec/staging/column_staging.hpp"
#include "copy/target_resolver.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/vector/array_vector.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "tds/tds_column_metadata.hpp"

namespace duckdb {
namespace mssql {
namespace codec {
namespace sql_vector {

namespace {

[[noreturn]] void RefuseRead(const tds::ColumnMetadata &meta, VectorStatus status, uint16_t dimensions) {
	throw InvalidInputException("MSSQL: column \"%s\": a vector value cannot be read as FLOAT[%d]: %s", meta.name,
								static_cast<int>(dimensions), DescribeStatus(status));
}

//! One value into `elements` (the column's `dimensions` floats): Ok, or why it is not one.
VectorStatus DecodeValue(const uint8_t *data, size_t size, const tds::ColumnMetadata &meta, uint16_t dimensions,
						 float *elements, std::string &scratch) {
	if (meta.IsVectorWire()) {
		const VectorStatus status = CheckValue(data, size, dimensions, meta.vector_element_type);
		if (status == VectorStatus::Ok) {
			WidenElements(data, dimensions, meta.vector_element_type, elements);
		}
		return status;
	}
	return ParseText(data, size, meta.type_id == tds::TDS_TYPE_NVARCHAR, dimensions, elements, scratch);
}

[[noreturn]] void RefuseWrite(const std::string &column, const std::string &why) {
	throw InvalidInputException("MSSQL: column \"%s\": a FLOAT[n] value cannot be written to a vector column: %s",
								column, why);
}

void RefuseElement(const std::string &column, VectorStatus status, uint16_t element) {
	RefuseWrite(column, StringUtil::Format("element %d: %s", static_cast<int>(element) + 1, DescribeStatus(status)));
}

}  // namespace

bool IsVectorType(const LogicalType &type) {
	return type.id() == LogicalTypeId::ARRAY && ArrayType::GetChildType(type).id() == LogicalTypeId::FLOAT;
}

LogicalType FloatArrayType(uint16_t dimensions) {
	return LogicalType::ARRAY(LogicalType::FLOAT, static_cast<idx_t>(dimensions));
}

void DecodeChunkFromStaging(const staging::ColumnStaging &st, idx_t count, const tds::ColumnMetadata &meta, Vector &out,
							VectorColumnNotes *notes) {
	const auto dimensions = static_cast<uint16_t>(ArrayType::GetSize(out.GetType()));
	float *elements = FlatVector::GetDataMutable<float>(ArrayVector::GetChildMutable(out));
	const uint8_t *payload = st.buffer.data();
	std::string scratch;
	for (idx_t row = 0; row < count; row++) {
		float *row_elements = elements + row * dimensions;
		if (!st.IsValid(row)) {
			std::memset(row_elements, 0, static_cast<size_t>(dimensions) * sizeof(float));
			continue;
		}
		const VectorStatus status =
			DecodeValue(payload + st.offsets[row], st.lengths[row], meta, dimensions, row_elements, scratch);
		if (status == VectorStatus::Ok) {
			continue;
		}
		if (!notes) {
			RefuseRead(meta, status, dimensions);
		}
		// Spec 093 T8d: a backup's value that is not one -- NULL, counted, its row read.
		std::memset(row_elements, 0, static_cast<size_t>(dimensions) * sizeof(float));
		FlatVector::SetNull(out, row, true);
		if (notes->malformed++ == 0) {
			notes->first_malformed = status;
		}
	}
}

void DecodeFromTds(const std::vector<uint8_t> &bytes, const tds::ColumnMetadata &meta, Vector &out, idx_t row) {
	const auto dimensions = static_cast<uint16_t>(ArrayType::GetSize(out.GetType()));
	float *elements = FlatVector::GetDataMutable<float>(ArrayVector::GetChildMutable(out));
	std::string scratch;
	const VectorStatus status =
		DecodeValue(bytes.data(), bytes.size(), meta, dimensions, elements + row * dimensions, scratch);
	if (status != VectorStatus::Ok) {
		RefuseRead(meta, status, dimensions);
	}
}

std::string FormatDdlTypeName(const LogicalType &type) {
	const auto &child = ArrayType::GetChildType(type);
	const idx_t dimensions = ArrayType::GetSize(type);
	if (child.id() == LogicalTypeId::DOUBLE) {
		throw NotImplementedException(
			"a %s column is not created as a SQL Server vector(%llu), whose elements are float32: cast it "
			"::FLOAT[%llu]",
			type.ToString(), (unsigned long long)dimensions, (unsigned long long)dimensions);
	}
	if (child.id() != LogicalTypeId::FLOAT) {
		throw NotImplementedException(
			"a %s column has no SQL Server type: SQL Server has no array type, and only a FLOAT[n] is created, as "
			"vector(n). Consider flattening or serializing to JSON",
			type.ToString());
	}
	if (dimensions < 1 || dimensions > tds::VECTOR_MAX_DIMENSIONS) {
		throw NotImplementedException("a %s column is not created as a SQL Server vector, which has 1 to %d dimensions",
									  type.ToString(), static_cast<int>(tds::VECTOR_MAX_DIMENSIONS));
	}
	return "vector(" + std::to_string(dimensions) + ")";
}

std::string FormatLiteral(const Value &value, const std::string &column) {
	if (value.IsNull()) {
		return "NULL";
	}
	const auto &children = ArrayValue::GetChildren(value);
	std::string literal = "N'[";
	for (idx_t i = 0; i < children.size(); i++) {
		if (children[i].IsNull()) {
			RefuseWrite(column, StringUtil::Format("element %d is NULL, which a vector cannot hold", i + 1));
		}
		const float element = children[i].GetValue<float>();
		if (!std::isfinite(element)) {
			RefuseElement(column, VectorStatus::NotFinite, static_cast<uint16_t>(i));
		}
		if (i > 0) {
			literal += ",";
		}
		// 9 significant digits read back as the same float32 (derived, V2); duckdb_fmt, not snprintf,
		// so a host's LC_NUMERIC cannot make the point a comma.
		literal += StringUtil::Format("%.9g", static_cast<double>(element));
	}
	return literal + "]'";
}

void EncodeToBcp(Vector &vec, const UnifiedVectorFormat &fmt, idx_t row, const BCPColumnMetadata &col,
				 vector<uint8_t> &out) {
	const uint16_t dimensions = col.vector_dims;
	const idx_t source_row = fmt.sel->get_index(row);
	const Vector &child = ArrayVector::GetChild(vec);
	// The child of an array buffer is flat; its elements for a row are dimensions apart.
	const float *elements = FlatVector::GetData<float>(child) + source_row * dimensions;
	const auto &child_validity = FlatVector::Validity(child);
	if (!child_validity.CannotHaveNull()) {
		for (uint16_t i = 0; i < dimensions; i++) {
			if (!child_validity.RowIsValid(source_row * dimensions + i)) {
				RefuseWrite(col.ReportedName(),
							StringUtil::Format("element %d is NULL, which a vector cannot hold", i + 1));
			}
		}
	}
	const size_t bytes = ValueBytes(dimensions, col.vector_element_type);
	const size_t at = out.size();
	out.resize(at + 2 + bytes);
	out[at] = static_cast<uint8_t>(bytes & 0xFF);
	out[at + 1] = static_cast<uint8_t>(bytes >> 8);
	uint16_t bad_element = 0;
	const auto status = EncodeValue(elements, dimensions, col.vector_element_type, out.data() + at + 2, bad_element);
	if (status != VectorStatus::Ok) {
		RefuseElement(col.ReportedName(), status, bad_element);
	}
}

namespace {

[[noreturn]] void RefuseRowElement(const BCPColumnMetadata &col, idx_t row, idx_t element, VectorStatus status) {
	RefuseWrite(col.ReportedName(),
				StringUtil::Format("row %llu of the chunk, element %llu: %s", (unsigned long long)row + 1,
								   (unsigned long long)element + 1, DescribeStatus(status)));
}

[[noreturn]] void RefuseNullElement(const BCPColumnMetadata &col, idx_t row, idx_t element) {
	RefuseWrite(col.ReportedName(), StringUtil::Format("row %llu of the chunk, element %llu is NULL, which a vector "
													   "cannot hold",
													   (unsigned long long)row + 1, (unsigned long long)element + 1));
}

//! The scatter's one loop: CURSOR the per-row position (NULL rows possible), else a stride (none);
//! FLOAT16 the element type, so the row's body carries no type branch.
template <bool CURSOR, bool FLOAT16>
void ScatterRows(uint8_t *dst, size_t stride, size_t *cursor, idx_t row_begin, idx_t rows, const Vector &vec,
				 const UnifiedVectorFormat &fmt, const BCPColumnMetadata &col) {
	const uint16_t dimensions = col.vector_dims;
	const size_t payload = ValueBytes(dimensions, FLOAT16 ? 1 : 0);
	const Vector &child = ArrayVector::GetChild(vec);
	const float *elements = FlatVector::GetData<float>(child);
	const auto &child_validity = FlatVector::Validity(child);
	const bool check_elements = !child_validity.CannotHaveNull();
	uint8_t header[8];
	WriteHeader(dimensions, FLOAT16 ? 1 : 0, header);
	const uint8_t length_lo = static_cast<uint8_t>(payload & 0xFF);
	const uint8_t length_hi = static_cast<uint8_t>(payload >> 8);
	for (idx_t i = 0; i < rows; i++) {
		const idx_t row = row_begin + i;
		uint8_t *out = CURSOR ? dst + cursor[row] : dst + i * stride;
		const idx_t source = fmt.sel->get_index(row);
		if (CURSOR && !fmt.validity.RowIsValid(source)) {
			out[0] = 0xFF;
			out[1] = 0xFF;
			cursor[row] += 2;
			continue;
		}
		const idx_t first = source * dimensions;
		if (check_elements) {
			for (idx_t e = 0; e < dimensions; e++) {
				if (!child_validity.RowIsValid(first + e)) {
					RefuseNullElement(col, row, e);
				}
			}
		}
		const float *row_elements = elements + first;
		out[0] = length_lo;
		out[1] = length_hi;
		std::memcpy(out + 2, header, 8);
		if (FLOAT16) {
			size_t bad = 0;
			const auto status = NarrowToHalf(row_elements, dimensions, out + 10, bad);
			if (status != VectorStatus::Ok) {
				RefuseRowElement(col, row, bad, status);
			}
		} else {
			if (!AllFinite(row_elements, dimensions)) {
				idx_t bad = 0;
				while (std::isfinite(row_elements[bad])) {
					bad++;
				}
				RefuseRowElement(col, row, bad, VectorStatus::NotFinite);
			}
			std::memcpy(out + 10, row_elements, static_cast<size_t>(dimensions) * 4);
		}
		if (CURSOR) {
			cursor[row] += 2 + payload;
		}
	}
}

}  // namespace

void ScatterChunkStrided(uint8_t *dst, size_t stride, idx_t row_begin, idx_t rows, const Vector &vec,
						 const UnifiedVectorFormat &fmt, const BCPColumnMetadata &col) {
	if (col.vector_element_type == 1) {
		ScatterRows<false, true>(dst, stride, nullptr, row_begin, rows, vec, fmt, col);
	} else {
		ScatterRows<false, false>(dst, stride, nullptr, row_begin, rows, vec, fmt, col);
	}
}

void ScatterChunkCursor(uint8_t *dst, size_t *cursor, idx_t row_begin, idx_t rows, const Vector &vec,
						const UnifiedVectorFormat &fmt, const BCPColumnMetadata &col) {
	if (col.vector_element_type == 1) {
		ScatterRows<true, true>(dst, 0, cursor, row_begin, rows, vec, fmt, col);
	} else {
		ScatterRows<true, false>(dst, 0, cursor, row_begin, rows, vec, fmt, col);
	}
}

void EncodeValueToBcp(const Value &value, const BCPColumnMetadata &col, vector<uint8_t> &out) {
	if (value.IsNull()) {
		out.push_back(0xFF);
		out.push_back(0xFF);
		return;
	}
	Vector vec(value, count_t(1));
	UnifiedVectorFormat fmt;
	vec.ToUnifiedFormat(fmt);
	EncodeToBcp(vec, fmt, 0, col, out);
}

}  // namespace sql_vector
}  // namespace codec
}  // namespace mssql
}  // namespace duckdb
