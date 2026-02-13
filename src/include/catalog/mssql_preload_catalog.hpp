#pragma once

#include "duckdb.hpp"

namespace duckdb {

//===----------------------------------------------------------------------===//
// MSSQLPreloadCatalogBindData - Bind data for mssql_preload_catalog function
//===----------------------------------------------------------------------===//

struct MSSQLPreloadCatalogBindData : public FunctionData {
	string catalog_name;
	string schema_name;  // Optional: limit preload to specific schema

	MSSQLPreloadCatalogBindData(string catalog_name_p, string schema_name_p)
		: catalog_name(std::move(catalog_name_p)), schema_name(std::move(schema_name_p)) {}

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<MSSQLPreloadCatalogBindData>(catalog_name, schema_name);
	}

	bool Equals(const FunctionData &other) const override {
		auto &other_data = other.Cast<MSSQLPreloadCatalogBindData>();
		return catalog_name == other_data.catalog_name && schema_name == other_data.schema_name;
	}
};

//===----------------------------------------------------------------------===//
// Registration Function
//===----------------------------------------------------------------------===//

//! Register the mssql_preload_catalog scalar function
//! Signature: mssql_preload_catalog(catalog_name VARCHAR [, schema_name VARCHAR]) -> VARCHAR
void RegisterMSSQLPreloadCatalogFunction(ExtensionLoader &loader);

}  // namespace duckdb
