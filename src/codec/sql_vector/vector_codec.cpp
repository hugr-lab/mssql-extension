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

//! One value into `elements` (the column's `dimensions` floats).
void DecodeValue(const uint8_t *data, size_t size, const tds::ColumnMetadata &meta, uint16_t dimensions,
				 float *elements, std::string &scratch) {
	VectorStatus status;
	if (meta.IsVectorWire()) {
		status = CheckValue(data, size, dimensions, meta.vector_element_type);
		if (status == VectorStatus::Ok) {
			WidenElements(data, dimensions, meta.vector_element_type, elements);
			return;
		}
	} else {
		status = ParseText(data, size, meta.type_id == tds::TDS_TYPE_NVARCHAR, dimensions, elements, scratch);
		if (status == VectorStatus::Ok) {
			return;
		}
	}
	RefuseRead(meta, status, dimensions);
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

void DecodeChunkFromStaging(const staging::ColumnStaging &st, idx_t count, const tds::ColumnMetadata &meta,
							Vector &out) {
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
		DecodeValue(payload + st.offsets[row], st.lengths[row], meta, dimensions, row_elements, scratch);
	}
}

void DecodeFromTds(const std::vector<uint8_t> &bytes, const tds::ColumnMetadata &meta, Vector &out, idx_t row) {
	const auto dimensions = static_cast<uint16_t>(ArrayType::GetSize(out.GetType()));
	float *elements = FlatVector::GetDataMutable<float>(ArrayVector::GetChildMutable(out));
	std::string scratch;
	DecodeValue(bytes.data(), bytes.size(), meta, dimensions, elements + row * dimensions, scratch);
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
