#pragma once

#include <cstdio>
#include <cstdlib>

// House debug pattern: a static level read from MSSQL_DEBUG. Shared by the
// catalog's translation units (mssql_catalog*.cpp).
inline int GetCatalogWarmDebugLevel() {
	static const int level = []() {
		const char *env = std::getenv("MSSQL_DEBUG");
		return env ? std::atoi(env) : 0;
	}();
	return level;
}

#define MSSQL_CATALOG_DEBUG_LOG(lvl, fmt, ...)                           \
	do {                                                                 \
		if (GetCatalogWarmDebugLevel() >= (lvl)) {                       \
			fprintf(stderr, "[MSSQL CATALOG] " fmt "\n", ##__VA_ARGS__); \
		}                                                                \
	} while (0)
