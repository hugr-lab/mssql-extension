#pragma once

#include "duckdb.hpp"

namespace duckdb {

//===----------------------------------------------------------------------===//
// MSSQLRefreshCacheBindData - Bind data for mssql_refresh_cache function
//
// Holds the validated catalog name between bind and execute phases.
//===----------------------------------------------------------------------===//

struct MSSQLRefreshCacheBindData : public FunctionData {
	string catalog_name;

	explicit MSSQLRefreshCacheBindData(string catalog_name_p) : catalog_name(std::move(catalog_name_p)) {}

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<MSSQLRefreshCacheBindData>(catalog_name);
	}

	bool Equals(const FunctionData &other) const override {
		auto &other_data = other.Cast<MSSQLRefreshCacheBindData>();
		return catalog_name == other_data.catalog_name;
	}
};

//===----------------------------------------------------------------------===//
// Registration Function
//===----------------------------------------------------------------------===//

//! Register the mssql_refresh_cache scalar function
//! Signature: mssql_refresh_cache(catalog_name VARCHAR) -> BOOLEAN
void RegisterMSSQLRefreshCacheFunction(ExtensionLoader &loader);

}  // namespace duckdb
