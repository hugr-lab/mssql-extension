//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// azure_test_function.cpp
//
// mssql_azure_auth_test() scalar function implementation
//===----------------------------------------------------------------------===//

#include "azure/azure_test_function.hpp"
#include "azure/azure_token.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/vector_operations/binary_executor.hpp"
#include "duckdb/common/vector_operations/generic_executor.hpp"
#include "duckdb/function/scalar_function.hpp"

namespace duckdb {
namespace mssql {
namespace azure {

//===----------------------------------------------------------------------===//
// TruncateToken - Format token for display
//
// Returns: first 10 chars + "..." + last 3 chars + " [N chars]"
// Example: "eyJ0eXAi...xyz [1847 chars]"
//===----------------------------------------------------------------------===//
static std::string TruncateToken(const std::string &token) {
	if (token.length() <= 16) {
		return token;
	}

	return token.substr(0, 10) + "..." + token.substr(token.length() - 3) +
	       " [" + std::to_string(token.length()) + " chars]";
}

//===----------------------------------------------------------------------===//
// AzureAuthTestFunction - Scalar function implementation (1 arg)
//===----------------------------------------------------------------------===//
static void AzureAuthTestFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &secret_name_vec = args.data[0];

	UnaryExecutor::Execute<string_t, string_t>(
	    secret_name_vec, result, args.size(),
	    [&](string_t secret_name) {
		    auto &context = state.GetContext();
		    std::string name = secret_name.GetString();

		    // Acquire token (no tenant override)
		    auto token_result = AcquireToken(context, name);

		    if (token_result.success) {
			    return StringVector::AddString(result, TruncateToken(token_result.access_token));
		    } else {
			    return StringVector::AddString(result, token_result.error_message);
		    }
	    });
}

//===----------------------------------------------------------------------===//
// AzureAuthTestFunctionWithTenant - Scalar function implementation (2 args)
//===----------------------------------------------------------------------===//
static void AzureAuthTestFunctionWithTenant(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &secret_name_vec = args.data[0];
	auto &tenant_vec = args.data[1];

	BinaryExecutor::Execute<string_t, string_t, string_t>(
	    secret_name_vec, tenant_vec, result, args.size(),
	    [&](string_t secret_name, string_t tenant_id) {
		    auto &context = state.GetContext();
		    std::string name = secret_name.GetString();
		    std::string tenant = tenant_id.GetString();

		    // Acquire token with tenant override for interactive auth
		    auto token_result = AcquireToken(context, name, tenant);

		    if (token_result.success) {
			    return StringVector::AddString(result, TruncateToken(token_result.access_token));
		    } else {
			    return StringVector::AddString(result, token_result.error_message);
		    }
	    });
}

//===----------------------------------------------------------------------===//
// RegisterAzureTestFunction
//===----------------------------------------------------------------------===//
void RegisterAzureTestFunction(ExtensionLoader &loader) {
	// Version 1: secret_name only
	ScalarFunction func1("mssql_azure_auth_test",
	                     {LogicalType::VARCHAR},  // secret_name
	                     LogicalType::VARCHAR,    // return type
	                     AzureAuthTestFunction);
	loader.RegisterFunction(func1);

	// Version 2: secret_name + tenant_id (for interactive auth)
	ScalarFunction func2("mssql_azure_auth_test",
	                     {LogicalType::VARCHAR, LogicalType::VARCHAR},  // secret_name, tenant_id
	                     LogicalType::VARCHAR,                          // return type
	                     AzureAuthTestFunctionWithTenant);
	loader.RegisterFunction(func2);
}

} // namespace azure
} // namespace mssql
} // namespace duckdb
