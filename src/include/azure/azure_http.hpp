//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// azure_http.hpp
//
// HTTP client wrapper using DuckDB's bundled cpp-httplib with OpenSSL
//===----------------------------------------------------------------------===//

#pragma once

#include <map>
#include <string>

namespace duckdb {
namespace mssql {
namespace azure {

//! Result of an HTTP request
struct HttpResponse {
	int status;
	std::string body;
	std::string error; //! Non-empty on network/connection failure

	bool Success() const {
		return error.empty() && status >= 200 && status < 300;
	}
};

//! POST with form-encoded parameters (auto URL-encodes values)
HttpResponse HttpPost(const std::string &host, const std::string &path,
                      const std::map<std::string, std::string> &params,
                      int timeout_seconds = 30);

//! POST with raw body string
HttpResponse HttpPost(const std::string &host, const std::string &path,
                      const std::string &body, const std::string &content_type,
                      int timeout_seconds = 30);

//! URL-encode a single string value
std::string UrlEncode(const std::string &value);

} // namespace azure
} // namespace mssql
} // namespace duckdb
