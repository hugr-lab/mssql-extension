//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// backup/backup_fuzzy.hpp
//
// Spec 085 D11, task 27 (PR 5): the fuzzy status of a data backup from its log tail. A backup holds each
// page as it was when it copied it, dirty reads included (E3§2); the tail (FirstLSN to LastLSN) says where
// that differs from a RESTORE, per allocation unit, by E3's flag (v), the one that held at table level on
// every run (precision 1.00, recall 1.00 over 54 table-runs):
//   (iii) a transaction open at LastLSN has a row operation on the unit: its changes may show (dirty);
//   (iv)  a committed transaction's row operation in the tail is newer than its page's copy, or the page
//         has no copy: the change is missing (stale).
// Not "page LSN > CheckpointLSN": a page copied before it changed keeps its old LSN (recall 0.42).
// A rolled-back transaction is stale on a page only when the copy is between its oldest change there
// and its newest (the rollback's compensation after the copy): the copy holds what it undid. A copy
// before all of them, or after all of them, is what a RESTORE has (review of #591: counted as committed
// before, a table equal to the RESTORE read stale). It takes the undo to land on the page of the change
// it undoes; a logical undo moved to another page by a split between them (a clustered index) can be
// missed -- the copy of either page judged alone. Not measured: the fuzzy fixture's rolled_only is updated
// in place. A transaction whose BEGIN is not in the tail counts
// as committed: in a data backup's tail (FirstLSN covers the oldest open one's BEGIN) that is the
// system's work. A record of no transaction (id 0) is no row change of the flags. LastLSN is exclusive:
// the next record after the backup's (HEADERONLY). The rules are ChangeShows's, the one place both the
// tables' status and mssql_backup_transactions' effects ask. DuckDB-free (D15).
//===----------------------------------------------------------------------===//

#pragma once

#include "backup/backup_log_tail.hpp"
#include "backup/backup_lsn.hpp"
#include "backup/backup_page.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace duckdb {
namespace mssql {
namespace backup {

struct CatalogTable;

//! What the tail of one data backup says (stage 5): read once, before any page is.
struct FuzzySummary {
	//! The set's FirstLSN, LastLSN and CheckpointLSN; records past LastLSN are not the backup's.
	Lsn first, last, checkpoint;
	uint64_t records = 0;
	//! Transactions begun in the tail and not ended by LastLSN.
	uint32_t open_transactions = 0;
	//! Committed transactions (and the system's, no BEGIN) with a row operation after CheckpointLSN:
	//! changes the pages may not hold.
	uint32_t committed_after_checkpoint = 0;
	//! Rolled-back ones with a row operation after CheckpointLSN: a page copied between a change and its
	//! compensation holds what was undone (the tables' status says which).
	uint32_t rolled_back_after_checkpoint = 0;

	//! A page's row operations by one transaction: the LSNs of its oldest and its newest.
	struct Change {
		uint64_t transaction = 0;
		Lsn oldest;
		Lsn newest;
		//! Its transaction was rolled back (in `finished` only).
		bool rolled_back = false;
	};
	struct Unit {
		//! By page, (file << 32) | page: the finished transactions' changes (flag (iv)) and the open ones'
		//! (flag (iii)).
		std::unordered_map<uint64_t, std::vector<Change>> finished;
		std::unordered_map<uint64_t, std::vector<Change>> open;
	};
	//! By allocation unit (PageHeader::AllocationUnit's form).
	std::unordered_map<uint64_t, Unit> units;
	//! The pages a FORMAT_PAGE of the tail lays out, (file << 32) | page: taken into use while the backup ran
	//! (a zero copy of one in a one-pass backup is Pending: ZeroCopyError).
	std::unordered_set<uint64_t> formatted;

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

	//! D11's database level: no open transaction, no finished row operation after CheckpointLSN (a
	//! rolled-back one too: whether a copy holds what it undid is per page, the tables' status).
	bool Consistent() const {
		return open_transactions == 0 && committed_after_checkpoint == 0 && rolled_back_after_checkpoint == 0;
	}
	//! What it holds, for the opened-file cache's estimate.
	size_t MemoryBytes() const;
};

//! Whether the tail lays `page` out (a FORMAT_PAGE before LastLSN): taken into use while the backup ran.
bool FormattedInTail(const FuzzySummary &tail, uint16_t file, uint32_t page);

//! The summary of the records `tail` gives, for a backup set with those LSNs. `poll`, when given, is
//! called every SUMMARY_POLL_RECORDS records with what the summary holds so far, in bytes (an estimate,
//! as MemoryBytes): a log backup's whole log is read here, and its caller reserves that memory as it
//! grows and ends the read by throwing from it (an interrupt, a reservation refused).
constexpr uint64_t SUMMARY_POLL_RECORDS = 4096;
FuzzySummary SummarizeLogTail(const LogTailReader &tail, const Lsn &first, const Lsn &last, const Lsn &checkpoint,
							  const std::function<void(uint64_t bytes)> &poll = std::function<void(uint64_t)>());
//! The same over records already read (the tests, the fuzz harness).
FuzzySummary SummarizeLogRecords(const std::vector<LogRecord> &records, const Lsn &first, const Lsn &last,
								 const Lsn &checkpoint);

//! "open", "committed", "rolled back" or "no begin".
const char *TransactionStateName(FuzzySummary::State state);
//! fn_dblog's spelling of a transaction id: "hhhh:hhhhhhhh".
std::string TransactionIdText(uint64_t transaction);

//! D11's rule for one transaction's changes on one page (`c`, from `open` or `finished`), its copy's
//! `state` and, when Present, `copy_lsn`: whether the copy differs from a RESTORE by them -- an open
//! one's change it holds (dirty), a committed one's it lacks, a rolled-back one's it holds undone after
//! it (stale).
bool ChangeShows(bool open, const FuzzySummary::Change &c, PageState state, const Lsn &copy_lsn, const Lsn &checkpoint);

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

//! FuzzinessOf's answers kept per allocation unit, for one summary over one PageStore -- an OpenedBackup's
//! tail and its own pages, for its life -- so a table's page copies are read once, not at every statement
//! and for every listing (review of #591: on a backup taken under load, tens of thousands of reads, a range
//! request each over s3://). Thread-safe; two statements asking at once may both compute a unit, the
//! same answer.
class FuzzinessMemo {
public:
	TableFuzziness Of(const FuzzySummary &summary, const std::vector<uint64_t> &units, const PageStore &pages);
	//! What it holds, for the opened-file cache's estimate.
	size_t MemoryBytes() const;

private:
	//! A unit's transactions whose changes show: open ones (dirty) and finished ones (stale).
	struct UnitShows {
		std::vector<uint64_t> dirty;
		std::vector<uint64_t> stale;
	};
	mutable std::mutex lock_;
	std::unordered_map<uint64_t, std::shared_ptr<const UnitShows>> units_;
	size_t transactions_ = 0;  // over every unit kept
};

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
