#pragma once

//===----------------------------------------------------------------------===//
// Logging for TDS INFO tokens
//
// Two paths consume them and both must render a server message identically:
// MSSQLResultStream (the scans) and MSSQLSimpleQuery's caller (mssql_exec).
// They were duplicated verbatim, so adding a field to one silently changed what
// a user saw on only one of them (PR #320 review).
//===----------------------------------------------------------------------===//

#include <string>
#include "duckdb/logging/logger.hpp"
#include "duckdb/main/client_context.hpp"
#include "tds/tds_token_parser.hpp"

namespace duckdb {

//! Log one INFO token.
//!
//! Level is LOG_INFO, not LOG_WARNING, and that is deliberate. An INFO token is
//! informational BY PROTOCOL -- SQL Server sends severity 0-10 as INFO and
//! anything above as an ERROR token -- so every one of these would otherwise sit
//! at WARNING next to the non-UTF-8 collation warning, which is a real warning
//! about possibly corrupt data. Routing server chatter to the same channel
//! dilutes the one message worth acting on.
//!
//! Severity is parsed and reported, but it deliberately does NOT choose the
//! level, because it cannot: measured against SQL Server 2025, `PRINT` arrives
//! as severity 0 and 5701 "Changed database context" arrives as 0 as well --
//! `sys.messages` calls 5701 severity 10, and severity 10 is sent on the wire
//! as class 0. So the useful message and the routine one are indistinguishable
//! by severity, while a RAISERROR at 1..9 does report its own level faithfully.
//! The message NUMBER is what separates them, so both are in the text and a
//! reader can filter on either.
inline void LogTdsInfo(ClientContext &context, const tds::TdsInfo &info) {
	if (info.message.empty()) {
		return;
	}
	const std::string where = info.proc_name.empty() ? std::string() : " (in " + info.proc_name + ")";
	DUCKDB_LOG_INFO(context, "mssql: [%u sev %u] %s%s", info.number, (unsigned)info.severity, info.message.c_str(),
					where.c_str());
}

}  // namespace duckdb
