//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// copy/load_transaction.hpp
//
// One server transaction around a load, on ONE connection the caller owns
// (spec 062 W1c / W2, issue #344).
//
// An INSERT BULK batch, and an INSERT ... VALUES statement, each commit on their
// own in autocommit — so a load sent as several of them was applied in part
// when one failed (#344). Inside a DuckDB transaction the pinned connection's
// transaction already spans them all; this gives the autocommit case the same
// bracket: BEGIN TRANSACTION before the first batch, COMMIT after the last,
// ROLLBACK on the way out otherwise.
//
// Deliberately NOT part of BulkLoadSession: a session is one writer's stream, a
// transaction is the connection OWNER's — the statement executors (W1c), the
// INSERT sink (W2) and spec 066's `#temp` fill each own their connection and
// bracket whatever they run on it, bulk stream and plain statements alike.
//===----------------------------------------------------------------------===//

#pragma once

#include <memory>

#include "tds/tds_connection.hpp"

namespace duckdb {
namespace mssql {

class LoadTransaction {
public:
	LoadTransaction() = default;

	//! Rolls back whatever was begun and not committed. Runs from destructors
	//! that may be on a worker thread with no ClientContext (issue #178): it
	//! needs the connection only.
	~LoadTransaction() noexcept;

	LoadTransaction(const LoadTransaction &) = delete;
	LoadTransaction &operator=(const LoadTransaction &) = delete;

	//! Send BEGIN TRANSACTION on `connection` — unless `transaction_pinned`, in
	//! which case the DuckDB transaction owns the connection and every call on
	//! this object is a no-op. Throws when the server refuses.
	void Begin(std::shared_ptr<tds::TdsConnection> connection, bool transaction_pinned);

	//! COMMIT TRANSACTION, if one was begun here. Throws when the server
	//! refuses; the transaction is then still open and the destructor rolls it
	//! back.
	void Commit();

	//! ROLLBACK TRANSACTION, if one was begun here and the connection can take
	//! a statement. A connection that is closed, or mid-stream, has rolled back
	//! or will when it is closed — nothing to send. Never throws.
	void Rollback() noexcept;

	//! A transaction begun here and not yet committed or rolled back.
	bool IsOpen() const {
		return open_;
	}

private:
	std::shared_ptr<tds::TdsConnection> connection_;
	bool open_ = false;
};

}  // namespace mssql
}  // namespace duckdb
