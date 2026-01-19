#include "catalog/mssql_transaction.hpp"
#include "catalog/mssql_catalog.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/client_context.hpp"

namespace duckdb {

//===----------------------------------------------------------------------===//
// MSSQLTransaction Implementation
//===----------------------------------------------------------------------===//

MSSQLTransaction::MSSQLTransaction(TransactionManager &manager, ClientContext &context, MSSQLCatalog &catalog)
	: Transaction(manager, context), catalog_(catalog) {}

MSSQLTransaction::~MSSQLTransaction() = default;

MSSQLTransaction &MSSQLTransaction::Get(ClientContext &context, Catalog &catalog) {
	return Transaction::Get(context, catalog).Cast<MSSQLTransaction>();
}

//===----------------------------------------------------------------------===//
// MSSQLTransactionManager Implementation
//===----------------------------------------------------------------------===//

MSSQLTransactionManager::MSSQLTransactionManager(AttachedDatabase &db, MSSQLCatalog &catalog)
	: TransactionManager(db), catalog_(catalog) {}

MSSQLTransactionManager::~MSSQLTransactionManager() = default;

Transaction &MSSQLTransactionManager::StartTransaction(ClientContext &context) {
	lock_guard<mutex> lock(transaction_lock_);

	auto transaction = make_uniq<MSSQLTransaction>(*this, context, catalog_);
	auto &result = *transaction;
	transactions_[context] = std::move(transaction);
	return result;
}

ErrorData MSSQLTransactionManager::CommitTransaction(ClientContext &context, Transaction &transaction) {
	lock_guard<mutex> lock(transaction_lock_);

	// Read-only catalog - commit is a no-op
	transactions_.erase(context);
	return ErrorData();
}

void MSSQLTransactionManager::RollbackTransaction(Transaction &transaction) {
	lock_guard<mutex> lock(transaction_lock_);

	// Read-only catalog - rollback is a no-op
	auto &context = *transaction.context.lock();
	transactions_.erase(context);
}

void MSSQLTransactionManager::Checkpoint(ClientContext &context, bool force) {
	// Read-only external catalog - checkpoint is a no-op
}

}  // namespace duckdb
