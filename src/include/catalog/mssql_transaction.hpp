#pragma once

#include "duckdb/common/mutex.hpp"
#include "duckdb/common/reference_map.hpp"
#include "duckdb/transaction/transaction.hpp"
#include "duckdb/transaction/transaction_manager.hpp"

namespace duckdb {

class MSSQLCatalog;
class MSSQLTransactionManager;

//===----------------------------------------------------------------------===//
// MSSQLTransaction - Transaction for read-only MSSQL catalog
//===----------------------------------------------------------------------===//

class MSSQLTransaction : public Transaction {
public:
	MSSQLTransaction(TransactionManager &manager, ClientContext &context, MSSQLCatalog &catalog);
	~MSSQLTransaction() override;

	MSSQLCatalog &GetCatalog() {
		return catalog_;
	}

	static MSSQLTransaction &Get(ClientContext &context, Catalog &catalog);

private:
	MSSQLCatalog &catalog_;
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
