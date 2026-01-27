#pragma once

#include <memory>
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/reference_map.hpp"
#include "duckdb/transaction/transaction.hpp"
#include "duckdb/transaction/transaction_manager.hpp"

namespace duckdb {

namespace tds {
class TdsConnection;
}  // namespace tds

class MSSQLCatalog;
class MSSQLTransactionManager;

//===----------------------------------------------------------------------===//
// MSSQLTransaction - Transaction for MSSQL catalog with SQL Server transaction support
//===----------------------------------------------------------------------===//

class MSSQLTransaction : public Transaction {
public:
	MSSQLTransaction(TransactionManager &manager, ClientContext &context, MSSQLCatalog &catalog);
	~MSSQLTransaction() override;

	MSSQLCatalog &GetCatalog() {
		return catalog_;
	}

	static MSSQLTransaction &Get(ClientContext &context, Catalog &catalog);

	//===--------------------------------------------------------------------===//
	// Transaction support (Spec 001-mssql-transactions)
	//===--------------------------------------------------------------------===//

	//! Get the pinned connection for this transaction (may be nullptr if not pinned yet)
	std::shared_ptr<tds::TdsConnection> GetPinnedConnection();

	//! Check if this transaction has a pinned connection
	bool HasPinnedConnection() const;

	//! Get the connection mutex for serializing operations on the pinned connection
	mutex &GetConnectionMutex();

	//! Check if SQL Server transaction has been started on the pinned connection
	bool IsSqlServerTransactionActive() const;

	//! Set the pinned connection for this transaction
	//! Called by ConnectionProvider on first DML/scan operation
	void SetPinnedConnection(std::shared_ptr<tds::TdsConnection> conn);

	//! Mark SQL Server transaction as started
	void SetSqlServerTransactionActive(bool active);

	//! Get the transaction descriptor (8 bytes) returned by SQL Server
	//! Returns pointer to 8 bytes, or nullptr if not set
	const uint8_t *GetTransactionDescriptor() const;

	//! Set the transaction descriptor from ENVCHANGE response
	void SetTransactionDescriptor(const uint8_t *descriptor);

	//! Generate next savepoint name (for future savepoint support)
	string GetNextSavepointName();

private:
	MSSQLCatalog &catalog_;

	//===--------------------------------------------------------------------===//
	// Transaction state fields (Spec 001-mssql-transactions)
	//===--------------------------------------------------------------------===//

	//! Pinned SQL Server connection for this transaction
	//! nullptr when not in transaction or autocommit mode
	std::shared_ptr<tds::TdsConnection> pinned_connection_;

	//! Mutex for serializing concurrent operations on pinned connection
	mutable mutex connection_mutex_;

	//! True if BEGIN TRANSACTION has been sent to SQL Server
	bool sql_server_transaction_active_ = false;

	//! Transaction descriptor returned by SQL Server (8 bytes)
	//! Required for ALL_HEADERS in subsequent SQL_BATCH requests
	uint8_t transaction_descriptor_[8] = {0};

	//! Whether transaction descriptor has been set
	bool has_transaction_descriptor_ = false;

	//! Counter for generating unique savepoint names
	uint32_t savepoint_counter_ = 0;
};

//===----------------------------------------------------------------------===//
// MSSQLTransactionManager - Transaction manager for read-only MSSQL catalog
//===----------------------------------------------------------------------===//

class MSSQLTransactionManager : public TransactionManager {
public:
	MSSQLTransactionManager(AttachedDatabase &db, MSSQLCatalog &catalog);
	~MSSQLTransactionManager() override;

	Transaction &StartTransaction(ClientContext &context) override;
	ErrorData CommitTransaction(ClientContext &context, Transaction &transaction) override;
	void RollbackTransaction(Transaction &transaction) override;
	void Checkpoint(ClientContext &context, bool force = false) override;

private:
	MSSQLCatalog &catalog_;
	mutex transaction_lock_;
	reference_map_t<ClientContext, unique_ptr<MSSQLTransaction>> transactions_;
};

}  // namespace duckdb
