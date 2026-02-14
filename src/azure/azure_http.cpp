//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// azure_http.cpp
//
// HTTP client implementation using DuckDB's bundled cpp-httplib with OpenSSL.
// Single compilation unit for httplib to avoid header bloat and ODR issues.
//
// Compiled with CPPHTTPLIB_OPENSSL_SUPPORT → namespace duckdb_httplib_openssl
// (separate from DuckDB core's duckdb_httplib → no symbol conflicts)
//===----------------------------------------------------------------------===//

// CPPHTTPLIB_OPENSSL_SUPPORT is set via CMake compile_definitions
#include "httplib.hpp"

#include "azure/azure_http.hpp"

namespace duckdb {
namespace mssql {
namespace azure {

namespace httplib = duckdb_httplib_openssl;

HttpResponse HttpPost(const std::string &host, const std::string &path,
					  const std::map<std::string, std::string> &params, int timeout_seconds) {
	httplib::SSLClient client(host);
	client.set_connection_timeout(timeout_seconds);
	client.set_read_timeout(timeout_seconds);
	client.set_write_timeout(timeout_seconds);

	// Convert std::map to httplib::Params (multimap)
	httplib::Params http_params;
	for (const auto &kv : params) {
		http_params.emplace(kv.first, kv.second);
	}

	auto res = client.Post(path, http_params);

	HttpResponse response;
	if (!res) {
		response.status = 0;
		response.error = httplib::to_string(res.error());
		return response;
	}

	response.status = res->status;
	response.body = res->body;
	return response;
}

HttpResponse HttpPost(const std::string &host, const std::string &path, const std::string &body,
					  const std::string &content_type, int timeout_seconds) {
	httplib::SSLClient client(host);
	client.set_connection_timeout(timeout_seconds);
	client.set_read_timeout(timeout_seconds);
	client.set_write_timeout(timeout_seconds);

	auto res = client.Post(path, body, content_type);

	HttpResponse response;
	if (!res) {
		response.status = 0;
		response.error = httplib::to_string(res.error());
		return response;
	}

	response.status = res->status;
	response.body = res->body;
	return response;
}

std::string UrlEncode(const std::string &value) {
	return httplib::encode_query_component(value);
}

}  // namespace azure
}  // namespace mssql
}  // namespace duckdb
