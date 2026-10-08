#include "codec/sql_server_type_text.hpp"

#include "duckdb/common/string_util.hpp"

namespace duckdb {
namespace mssql {
namespace codec {

bool ParseSqlServerTypeText(const std::string &text, SqlServerTypeText &out) {
	std::string lowered = StringUtil::Lower(text);
	StringUtil::Trim(lowered);
	out.args.clear();
	const auto open = lowered.find('(');
	out.base = lowered.substr(0, open);
	StringUtil::Trim(out.base);
	if (out.base.empty()) {
		return false;
	}
	if (open == std::string::npos) {
		return lowered.find(')') == std::string::npos;
	}
	const auto close = lowered.find(')', open);
	if (close == std::string::npos || lowered.find_first_not_of(' ', close + 1) != std::string::npos) {
		return false;
	}
	for (auto &arg : StringUtil::Split(lowered.substr(open + 1, close - open - 1), ',')) {
		StringUtil::Trim(arg);
		out.args.push_back(arg);
	}
	return true;
}

}  // namespace codec
}  // namespace mssql
}  // namespace duckdb
