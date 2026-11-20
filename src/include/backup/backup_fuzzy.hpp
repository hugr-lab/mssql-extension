//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// backup/backup_fuzzy.hpp
//
// Spec 085 D11, task 27 (PR 5): the fuzzy status of a data backup from its log tail. A backup's pages
// hold the database as of its CheckpointLSN, dirty reads included (E3§2); the tail (FirstLSN to LastLSN)
// says where that differs from a RESTORE, per allocation unit, by E3's flag (v), the one that held at
// table level on every run (precision 1.00, recall 1.00 over 54 table-runs):
//   (iii) a transaction open at LastLSN has a row operation on the unit: its changes may show (dirty);
//   (iv)  a finished transaction's row operation in the tail is newer than its page's copy, or the page
//         has no copy: the change is missing (stale).
// Not "page LSN > CheckpointLSN": a page copied before it changed keeps its old LSN (recall 0.42).
// A finished transaction is a committed one, and also one rolled back: a rollback written after a
// page's copy leaves the copy holding what it undid (E3 counted committed ones; a rolled-back one
// before the checkpoint never left dirt there, so this widens nothing E3 measured). A transaction whose
// BEGIN is not in the tail counts as finished: in a data backup's tail (FirstLSN covers the oldest open
// one's BEGIN) that is the system's work. A record of no transaction (id 0) is no row change of the
// flags. LastLSN is exclusive: the next record after the backup's (HEADERONLY). DuckDB-free (D15).
//===----------------------------------------------------------------------===//

#pragma once

#include "backup/backup_log_tail.hpp"
#include "backup/backup_lsn.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace duckdb {
namespace mssql {
namespace backup {

class PageStore;
struct CatalogTable;

//! What the tail of one data backup says (stage 5): read once, before any page is.
struct FuzzySummary {
	//! The set's FirstLSN, LastLSN and CheckpointLSN; records past LastLSN are not the backup's.
	Lsn first, last, checkpoint;
	uint64_t records = 0;
	//! Transactions begun in the tail and not ended by LastLSN.
	uint32_t open_transactions = 0;
	//! Finished transactions with a row operation after CheckpointLSN: changes the pages do not hold.
	uint32_t finished_after_checkpoint = 0;

	//! A page's row operations by one transaction: the LSNs of its oldest and its newest.
	struct Change {
		uint64_t transaction = 0;
		Lsn oldest;
		Lsn newest;
	};
	struct Unit {
		//! By page, (file << 32) | page: the finished transactions' changes (flag (iv)) and the open ones'
		//! (flag (iii)).
		std::unordered_map<uint64_t, std::vector<Change>> finished;
		std::unordered_map<uint64_t, std::vector<Change>> open;
	};
	//! By allocation unit (PageHeader::AllocationUnit's form).
	std::unordered_map<uint64_t, Unit> units;

	//! A transaction the tail holds a record of.
	enum class State : uint8_t {
		Open,		 // begun in the tail, not ended by LastLSN
		Committed,	 // COMMIT_XACT
		RolledBack,	 // ABORT_XACT
		NoBegin,	 // records, but no BEGIN in the log read: the system's work in a data backup's tail (FirstLSN
					 // covers every open transaction's BEGIN); in a log backup, also one begun in an earlier log
	};
	struct Transaction {
		State state = State::NoBegin;
		Lsn begin;		   // its BEGIN_XACT, zero when none
		Lsn end;		   // its COMMIT / ABORT, zero when none
		std::string name;  // the BEGIN_XACT's (fn_dblog's Transaction Name)
		uint32_t records = 0;
		uint32_t row_operations = 0;	// IsRowOperation in a row context
		bool after_checkpoint = false;	// a row operation after CheckpointLSN
	};
	//! By transaction id; the id 0 (records of no transaction) is not one.
	std::unordered_map<uint64_t, Transaction> transactions;

	//! D11's database level: no open transaction, no finished row operation after CheckpointLSN.
	bool Consistent() const {
		return open_transactions == 0 && finished_after_checkpoint == 0;
	}
	//! What it holds, for the opened-file cache's estimate.
	size_t MemoryBytes() const;
};

//! The summary of the records `tail` gives, for a backup set with those LSNs.
FuzzySummary SummarizeLogTail(const LogTailReader &tail, const Lsn &first, const Lsn &last, const Lsn &checkpoint);
//! The same over records already read (the tests, the fuzz harness).
FuzzySummary SummarizeLogRecords(const std::vector<LogRecord> &records, const Lsn &first, const Lsn &last,
								 const Lsn &checkpoint);

//! "open", "committed", "rolled back" or "no begin".
const char *TransactionStateName(FuzzySummary::State state);
//! fn_dblog's spelling of a transaction id: "hhhh:hhhhhhhh".
std::string TransactionIdText(uint64_t transaction);

//! A table's status: over its base rowsets' allocation units (in-row, row-overflow, LOB).
struct TableFuzziness {
	bool stale = false;
	bool dirty = false;
	//! The open transactions whose changes the table's page copies hold, and the finished ones with a change
	//! newer than the copy of its page.
	uint32_t open_transactions = 0;
	uint32_t stale_transactions = 0;

	//! "consistent", "stale", "dirty" or "stale+dirty" (D11).
	const char *Status() const;
	bool Consistent() const {
		return !stale && !dirty;
	}
};

//! The status of the table whose base rowsets are `units`, each page a transaction changed looked up in
//! `pages`: stale when a finished transaction's newest row operation on a page is newer than its copy (a
//! page the backup holds as not live: only when the operation is after CheckpointLSN, the page taken into
//! use after the image; a Pending or unreadable page: always); dirty when an open transaction's oldest row
//! operation on a page is no newer than its copy (the copy holds what a RESTORE undoes; a page the image
//! does not have holds none of it). Measured on the load fixture: by the transaction alone, a table whose
//! open transaction wrote only after the copies read dirty while equal to the RESTORE.
TableFuzziness FuzzinessOf(const FuzzySummary &summary, const std::vector<uint64_t> &units, const PageStore &pages);

//! A table's base rowsets' allocation units (index 0, a heap, or 1, its clustered index: in-row,
//! row-overflow and LOB): what its scan reads, what its status is over.
std::vector<uint64_t> BaseUnitsOf(const CatalogTable &table);
//! FuzzinessOf over `table`'s BaseUnitsOf.
TableFuzziness FuzzinessOf(const FuzzySummary &summary, const CatalogTable &table, const PageStore &pages);

//! D11's `fuzzy` parameter.
enum class FuzzyMode : uint8_t { Warn, Error, Ignore };

//! 'warn', 'error' or 'ignore' (any case); false for anything else.
bool ParseFuzzyMode(const std::string &text, FuzzyMode &mode);

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb
