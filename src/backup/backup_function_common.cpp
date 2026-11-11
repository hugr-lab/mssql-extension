#include "backup/backup_function_common.hpp"

#include "duckdb/common/exception.hpp"
#include "tds/encoding/datetime_encoding.hpp"

#include <cstring>

namespace duckdb {
namespace mssql {
namespace backup {

vector<std::string> PathsOf(const Value &value, const char *function) {
	vector<std::string> paths;
	if (value.IsNull()) {
		throw InvalidInputException("%s: the path is NULL", function);
	}
	if (value.type().id() == LogicalTypeId::LIST) {
		for (const auto &child : ListValue::GetChildren(value)) {
			if (child.IsNull()) {
				throw InvalidInputException("%s: a NULL in the list of paths", function);
			}
			paths.push_back(child.ToString());
		}
		if (paths.empty()) {
			throw InvalidInputException("%s: an empty list of paths", function);
		}
	} else {
		paths.push_back(value.ToString());
	}
	return paths;
}

Value RecordDatetimeValue(const std::string &record_form) {
	if (record_form.size() != 8) {
		return Value(LogicalType::TIMESTAMP);
	}
	uint8_t wire[8];
	std::memcpy(wire, record_form.data() + 4, 4);
	std::memcpy(wire + 4, record_form.data(), 4);
	return Value::TIMESTAMP(tds::encoding::DateTimeEncoding::ConvertDatetime(wire));
}

Value RecordSmallDatetimeValue(const std::string &record_form) {
	if (record_form.size() != 4) {
		return Value(LogicalType::TIMESTAMP);
	}
	uint8_t wire[4];
	std::memcpy(wire, record_form.data() + 2, 2);
	std::memcpy(wire + 2, record_form.data(), 2);
	return Value::TIMESTAMP(tds::encoding::DateTimeEncoding::ConvertSmallDatetime(wire));
}

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb
