#include "dml/insert/mssql_value_serializer.hpp"
#include <algorithm>
#include <cctype>
#include "codec/literal_format.hpp"
#include "codec/string_codec.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/hugeint.hpp"

namespace duckdb {

//===----------------------------------------------------------------------===//
// Identifier and String Escaping
//===----------------------------------------------------------------------===//

string MSSQLValueSerializer::EscapeIdentifier(const string &name) {
	// T-SQL bracket quoting: ] becomes ]]
	string result;
	result.reserve(name.size() + 2);
	result.push_back('[');
	for (char c : name) {
		if (c == ']') {
			result.push_back(']');
		}
		result.push_back(c);
	}
	result.push_back(']');
	return result;
}

string MSSQLValueSerializer::EscapeString(const string &value) {
	// SQL string escaping: ' becomes ''
	string result;
	result.reserve(value.size());
	for (char c : value) {
		if (c == '\'') {
			result.push_back('\'');
		}
		result.push_back(c);
	}
	return result;
}

//===----------------------------------------------------------------------===//
// Decimal Serialization
//===----------------------------------------------------------------------===//

string MSSQLValueSerializer::SerializeDecimal(const hugeint_t &value, uint8_t width, uint8_t scale) {
	// Convert hugeint to string preserving scale
	string unscaled = Hugeint::ToString(value);

	// Handle negative numbers
	bool negative = false;
	size_t start = 0;
	if (!unscaled.empty() && unscaled[0] == '-') {
		negative = true;
		start = 1;
	}

	string digits = unscaled.substr(start);

	// Pad with leading zeros if needed
	while (digits.size() <= scale) {
		digits = "0" + digits;
	}

	// Insert decimal point
	string result;
	if (negative) {
		result = "-";
	}

	if (scale == 0) {
		result += digits;
	} else {
		size_t int_len = digits.size() - scale;
		result += digits.substr(0, int_len);
		result += ".";
		result += digits.substr(int_len);
	}

	return result;
}

//===----------------------------------------------------------------------===//
// Main Entry Points
//===----------------------------------------------------------------------===//

string MSSQLValueSerializer::Serialize(const Value &value, const LogicalType &target_type) {
	// All supported families route through the canonical codec dispatcher
	// (handles NULL + 9-arm family switch internally; FR-022 guarantees
	// byte-identity for LiteralContext::InsertValues vs Filter).
	auto &type = value.type();
	try {
		return mssql::codec::FormatSqlLiteral(value, type, mssql::codec::LiteralContext::InsertValues);
	} catch (const NotImplementedException &) {
		throw InvalidInputException("Cannot serialize DuckDB type '%s' for SQL Server INSERT", type.ToString());
	}
}

string MSSQLValueSerializer::WrapSpatialLiteral(const string &literal, const string &mssql_type) {
	// NULL needs no reader: STGeomFromWKB(NULL, …) does return NULL, but the
	// call would be noise in every generated statement that has one.
	if (literal == "NULL") {
		return literal;
	}
	string lower_type = mssql_type;
	std::transform(lower_type.begin(), lower_type.end(), lower_type.begin(),
				   [](unsigned char c) { return std::tolower(c); });
	if (lower_type == "geometry") {
		return "geometry::STGeomFromWKB(" + literal + ", 0)";
	}
	if (lower_type == "geography") {
		// 4326 because geography refuses SRID 0 outright — see the header.
		return "geography::STGeomFromWKB(" + literal + ", 4326)";
	}
	return literal;
}

string MSSQLValueSerializer::SerializeFromVector(Vector &vector, idx_t index, const LogicalType &target_type) {
	// For simplicity, extract the value and serialize
	// This could be optimized for bulk operations
	auto value = vector.GetValue(index);
	return Serialize(value, target_type);
}

idx_t MSSQLValueSerializer::EstimateSerializedSize(const Value &value, const LogicalType &type) {
	if (value.IsNull()) {
		return 4;  // "NULL"
	}
	// VARCHAR is value-aware: codec wrapper overhead + worst-case escape
	// factor (every char might be a doubled single quote). All other families
	// delegate to the codec layer's per-family EstimateLiteralSize.
	if (type.id() == LogicalTypeId::VARCHAR) {
		auto str_val = StringValue::Get(value);
		return mssql::codec::string::EstimateLiteralSize(type) + str_val.size() * 2;
	}
	try {
		return mssql::codec::EstimateLiteralSize(type);
	} catch (const NotImplementedException &) {
		// Unreachable in practice — the only path here is Serialize() failing on
		// an unsupported family, which throws InvalidInputException before this
		// estimator is needed. 50 bytes is a conservative upper bound that won't
		// trigger an INSERT-batch StringBuffer realloc for any plausibly-sized
		// literal an unsupported family could produce.
		return 50;
	}
}

}  // namespace duckdb
