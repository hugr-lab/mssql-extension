//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// azure_test_function.hpp
//
// mssql_azure_auth_test() function registration
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {
namespace mssql {
namespace azure {

//===----------------------------------------------------------------------===//
// RegisterAzureTestFunction - Register mssql_azure_auth_test() scalar function
//
// Function signature: mssql_azure_auth_test(secret_name VARCHAR) -> VARCHAR
//
// On success: Returns truncated token (first 10 + "..." + last 3 + " [N chars]")
// On failure: Returns error message with Azure AD error code if available
//===----------------------------------------------------------------------===//
void RegisterAzureTestFunction(ExtensionLoader &loader);

}  // namespace azure
}  // namespace mssql
}  // namespace duckdb
