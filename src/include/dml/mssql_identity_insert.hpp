#pragma once

//===----------------------------------------------------------------------===//
// Spec 077 W2: the SET IDENTITY_INSERT bracket, as text.
//
// Self-contained (std:: only) so the two things that must not drift — the
// statement we send and how we explain the server's refusals of it — are
// unit-tested without a server. The bracket itself lives on
// MSSQLStatementConnection, which owns the one connection a DML statement runs
// on and every path that lets go of it.
//===----------------------------------------------------------------------===//

#include <cstdint>
#include <string>

#include "query/mssql_identifier.hpp"

namespace duckdb {
namespace mssql {

inline std::string BracketIdentifier(const std::string &name) {
	return QuoteIdentifier(name);
}

//! `SET IDENTITY_INSERT [schema].[table] ON|OFF`. Sent as a batch of its own,
//! never inside EXEC: a SET inside a nested batch is restored when that batch
//! ends (spec 077 § 0.3), so a wrapped one would silently do nothing.
inline std::string IdentityInsertSql(const std::string &schema, const std::string &table, bool on) {
	return "SET IDENTITY_INSERT " + BracketIdentifier(schema) + "." + BracketIdentifier(table) + (on ? " ON" : " OFF");
}

//! What to tell the user when the server refuses SET IDENTITY_INSERT ON. The
//! extension issued a statement the user did not write, so the message says
//! what was attempted and why, and puts the server's own text in as the cause.
//! Returns an empty string for a number this does not recognise — the caller
//! then reports the server's message as it is.
//!
//!   1088  the caller lacks ALTER on the table. The server says the object
//!         "does not exist or you do not have permissions", which is actively
//!         misleading for someone who can read and insert into it.
//!   8106  "does not have the identity property": the catalog's is_identity
//!         is stale — DDL through mssql_exec does not invalidate by default.
//!   8107  IDENTITY_INSERT is already ON for another table on this session.
//!         One statement targets one table, so the other came from somewhere
//!         else: a previous statement of ours that leaked its ON, or the
//!         user's own mssql_exec. Both are named; neither is accused.
inline std::string ExplainIdentityInsertError(uint32_t number, const std::string &server_message,
											  const std::string &schema, const std::string &table) {
	const std::string target = schema + "." + table;
	switch (number) {
	case 1088:
		return "MSSQL: the INSERT supplies a value for the identity column of " + target +
			   ", so the extension issued SET IDENTITY_INSERT ON for it — and the server refused: that statement "
			   "needs ALTER permission on the table, which INSERT does not. Either grant ALTER on " +
			   target +
			   " to this login, or leave the identity column out of the INSERT and let the server assign it. "
			   "(The server raises the same error for a table that no longer exists: if it was dropped or renamed "
			   "since the catalog cached it, run mssql_invalidate_cache() instead.) The server said: " +
			   server_message;
	case 8106:
		return "MSSQL: the catalog believes '" + target +
			   "' has an identity column and the server disagrees (SET IDENTITY_INSERT was refused with error "
			   "8106). The cached metadata is stale — most likely after DDL through mssql_exec, which does not "
			   "invalidate the cache by default. Run mssql_invalidate_cache() and retry. The server said: " +
			   server_message;
	case 8107:
		return "MSSQL: SET IDENTITY_INSERT ON for " + target +
			   " was refused because it is already ON for another table on this session (error 8107). One "
			   "statement targets one table, so the other setting came from somewhere else: a previous statement "
			   "of the extension's that did not turn it off, or your own SET IDENTITY_INSERT through mssql_exec on "
			   "this session (a pinned transaction connection, or a pooled one with mssql_reset_connection = "
			   "false). Turn it off for the table the server names, then retry. The server said: " +
			   server_message;
	default:
		return "";
	}
}

}  // namespace mssql
}  // namespace duckdb
