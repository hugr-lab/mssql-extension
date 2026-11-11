#include "backup/backup_collation.hpp"

#include <vector>

namespace duckdb {
namespace mssql {
namespace backup {

uint32_t FlagsOfName(const std::string &name) {
	const std::string padded = "_" + name + "_";
	auto has = [&](const char *token) { return padded.find(std::string("_") + token + "_") != std::string::npos; };
	const uint32_t utf8 = has("UTF8") ? 0x40 : 0;
	if (has("BIN2")) {
		return 0x20 | utf8;
	}
	if (has("BIN")) {
		return 0x10;
	}
	return (has("CI") ? 0x01 : 0) | (has("AI") ? 0x02 : 0) | (has("KS") ? 0 : 0x04) | (has("WS") ? 0 : 0x08) | utf8;
}

static bool IsSqlCollation(const char *name) {
	return std::string(name).compare(0, 4, "SQL_") == 0;
}

bool CollationMatchesTds(const CollationRow &row, uint32_t info, uint8_t sort_id) {
	if (sort_id != 0) {
		return IsSqlCollation(row.name) && (static_cast<uint32_t>(row.collation_id) >> 24) == sort_id;
	}
	const uint32_t lcid = info & 0xFFFFF;
	const uint32_t flags = (info >> 20) & 0xFF;
	const uint32_t version = info >> 28;
	return !IsSqlCollation(row.name) && row.lcid == lcid && row.version == version && FlagsOfName(row.name) == flags;
}

std::string CollationNameOfTds(uint32_t info, uint8_t sort_id) {
	if (info == 0 && sort_id == 0) {
		return std::string();
	}
	using codec::code_page::tables::COLLATION_COUNT;
	using codec::code_page::tables::COLLATIONS;
	std::vector<std::string> names;
	for (size_t i = 0; i < COLLATION_COUNT; i++) {
		if (CollationMatchesTds(COLLATIONS[i], info, sort_id)) {
			names.push_back(COLLATIONS[i].name);
		}
	}
	if (names.size() > 1) {
		std::vector<std::string> without_sc;
		for (const auto &n : names) {
			if (("_" + n + "_").find("_SC_") == std::string::npos) {
				without_sc.push_back(n);
			}
		}
		names = without_sc;
	}
	return names.size() == 1 ? names.front() : std::string();
}

const CollationRow *CollationById(int32_t id) {
	// A binary search over the table (sorted by collation_id), so the backup parsers need the
	// generated table alone, not the code-page kernels' code_page.cpp.
	using codec::code_page::tables::COLLATION_COUNT;
	using codec::code_page::tables::COLLATIONS;
	size_t lo = 0;
	size_t hi = COLLATION_COUNT;
	while (lo < hi) {
		const size_t mid = lo + (hi - lo) / 2;
		if (COLLATIONS[mid].collation_id < id) {
			lo = mid + 1;
		} else {
			hi = mid;
		}
	}
	return lo < COLLATION_COUNT && COLLATIONS[lo].collation_id == id ? &COLLATIONS[lo] : nullptr;
}

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb
