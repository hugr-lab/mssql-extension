//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// azure_test_function.cpp
//
// mssql_azure_auth_test() scalar function implementation
//===----------------------------------------------------------------------===//

#include "azure/azure_test_function.hpp"
#include "azure/azure_token.hpp"
#include "connection/mssql_settings.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/vector_operations/binary_executor.hpp"
#include "duckdb/common/vector_operations/generic_executor.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "mssql_function_docs.hpp"

namespace duckdb {
namespace mssql {
namespace azure {

//===----------------------------------------------------------------------===//
// TruncateToken - Format token for display
//
// Returns: first 10 chars + "... [N chars]"
// Example: "eyJ0eXAiOi... [1847 chars]"
//
// The prefix and the length, and nothing else (issue #492). Every Azure AD JWT
// begins "eyJ0eXAiOi" -- base64 of `{"typ":"` -- so the prefix only confirms
// the shape, and the LENGTH is the diagnostic that distinguishes a real token
// from a truncated or empty one.
//
// This used to append the last three characters as well, and to return a token
// of 16 characters or fewer verbatim. Three characters are not a practical
// attack on the signature, but they were the only part of the output derived
// from the secret, they bought nothing diagnostically, and this value goes into
// a QUERY RESULT: terminal scrollback, CI logs, screenshots, shared notebooks.
// The short-token branch could not be a real JWT at all.
//===----------------------------------------------------------------------===//
static std::string TruncateToken(const std::string &token) {
	if (token.empty()) {
		return "[empty]";
	}
	// The prefix is only safe to show BECAUSE it is boilerplate, so show it only
	// when it actually is: a JWT begins "eyJ" (base64 of `{"`). An opaque token
	// gets its length and nothing else -- ten characters of a 17-character
	// secret is a real fraction of it, which the JWT argument does not cover.
	const bool looks_like_a_jwt = token.rfind("eyJ", 0) == 0;
	if (!looks_like_a_jwt || token.length() <= 16) {
		return "[" + std::to_string(token.length()) + " chars]";
	}

	return token.substr(0, 10) + "... [" + std::to_string(token.length()) + " chars]";
}

//===----------------------------------------------------------------------===//
// AzureAuthTestFunction - Scalar function implementation (1 arg)
//===----------------------------------------------------------------------===//
static void AzureAuthTestFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	MSSQLRequireExternalAccess(state.GetContext(), "mssql_azure_auth_test", "requests a token from Azure AD");
	auto &secret_name_vec = args.data[0];

	UnaryExecutor::Execute<string_t, string_t>(secret_name_vec, result, args.size(), [&](string_t secret_name) {
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
	MSSQLRequireExternalAccess(state.GetContext(), "mssql_azure_auth_test", "requests a token from Azure AD");
	auto &secret_name_vec = args.data[0];
	auto &tenant_vec = args.data[1];

	BinaryExecutor::Execute<string_t, string_t, string_t>(
		secret_name_vec, tenant_vec, result, args.size(), [&](string_t secret_name, string_t tenant_id) {
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
	// Both variants hit the network (OAuth2 token acquisition) and are
	// non-deterministic: VOLATILE prevents plan-time constant folding
	// (issue #178 D1).

	// Version 1: secret_name only
	ScalarFunction func1("mssql_azure_auth_test", {LogicalType::VARCHAR},  // secret_name
						 LogicalType::VARCHAR,							   // return type
						 AzureAuthTestFunction);
	func1.SetVolatile();
	func1.SetFallible();

	// Version 2: secret_name + tenant_id (for interactive auth)
	ScalarFunction func2("mssql_azure_auth_test",
						 {LogicalType::VARCHAR, LogicalType::VARCHAR},	// secret_name, tenant_id
						 LogicalType::VARCHAR,							// return type
						 AzureAuthTestFunctionWithTenant);
	func2.SetVolatile();
	func2.SetFallible();
	RegisterDocumentedFunction(
		loader, {func1, func2},
		{{"secret", "tenant"},
		 "Acquires an Azure AD access token with the named azure secret, without connecting to SQL Server, and "
		 "returns the token abbreviated to its first and last characters and its length, or the error. tenant "
		 "overrides the secret's tenant for interactive (device code) authentication.",
		 {"mssql_azure_auth_test('my_azure_secret')"},
		 {"authentication"}});
}

}  // namespace azure
}  // namespace mssql
}  // namespace duckdb
