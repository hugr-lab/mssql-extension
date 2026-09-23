#pragma once

#include <cctype>
#include <string>

namespace duckdb {
namespace mssql {

//! Does this raw T-SQL batch possibly change schema or catalog metadata? Drives
//! the cache invalidation after mssql_exec (issue #151) and, inside a
//! transaction, the transaction's own "stop trusting the shared cache" mark
//! (#380). Over-detection costs a metadata reload -- inside a transaction the
//! whole transaction's cache, so it has to be tight (review of #382);
//! under-detection leaves the cache stale, so every form that CAN change
//! schema counts: CREATE, DROP, ALTER, TRUNCATE, sp_rename, and EXEC / EXECUTE
//! (a procedure may run DDL).
//!
//! A keyword counts only as a whole word OUTSIDE string literals (`'…'`,
//! `N'…'`), delimited identifiers (`[…]`, `"…"`) and comments (`-- …`,
//! `/* … */`, nested). It used to be a substring match over the upper-cased
//! text, so `UPDATE t SET s = 'created'`, a column named `dropped_at` and the
//! word `execution` in a comment all counted.
//!
//! Self-contained (standard library only): unit-tested without DuckDB.
inline bool SqlMayChangeSchema(const std::string &sql) {
	static const char *const KEYWORDS[] = {"CREATE", "DROP", "ALTER", "TRUNCATE", "EXEC", "EXECUTE", "SP_RENAME"};
	const size_t n = sql.size();
	size_t i = 0;
	while (i < n) {
		const char c = sql[i];
		// -- line comment
		if (c == '-' && i + 1 < n && sql[i + 1] == '-') {
			while (i < n && sql[i] != '\n') {
				i++;
			}
			continue;
		}
		// /* block comment */, nested as T-SQL allows
		if (c == '/' && i + 1 < n && sql[i + 1] == '*') {
			int depth = 1;
			i += 2;
			while (i < n && depth > 0) {
				if (sql[i] == '/' && i + 1 < n && sql[i + 1] == '*') {
					depth++;
					i += 2;
				} else if (sql[i] == '*' && i + 1 < n && sql[i + 1] == '/') {
					depth--;
					i += 2;
				} else {
					i++;
				}
			}
			continue;
		}
		// 'string' ('' is a quote inside); an N prefix is consumed as a word
		// character below and never matches a keyword on its own.
		if (c == '\'' || c == '"' || c == '[') {
			const char close = c == '[' ? ']' : c;
			i++;
			while (i < n) {
				if (sql[i] == close) {
					if (i + 1 < n && sql[i + 1] == close) {
						i += 2;
						continue;
					}
					break;
				}
				i++;
			}
			i++;
			continue;
		}
		if (std::isalpha(static_cast<unsigned char>(c)) || c == '_' || c == '@' || c == '#') {
			size_t start = i;
			while (i < n && (std::isalnum(static_cast<unsigned char>(sql[i])) || sql[i] == '_' || sql[i] == '@' ||
							 sql[i] == '#' || sql[i] == '$')) {
				i++;
			}
			std::string word = sql.substr(start, i - start);
			for (auto &ch : word) {
				ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
			}
			for (auto keyword : KEYWORDS) {
				if (word == keyword) {
					return true;
				}
			}
			continue;
		}
		i++;
	}
	return false;
}

}  // namespace mssql
}  // namespace duckdb
