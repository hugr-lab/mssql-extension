// Spec 085 D11, task 27 (PR 5): the fuzzy status from the log tail (backup_fuzzy.hpp).

#include "backup/backup_fuzzy.hpp"

#include "backup/backup_bytes.hpp"
#include "backup/backup_catalog_model.hpp"
#include "backup/backup_page.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace duckdb {
namespace mssql {
namespace backup {

namespace {

//! A SET_BITS record's fixed part: up to the value it sets (Summarizer::FreesExtents).
constexpr uint16_t SET_BITS_FIXED = 54;

bool Newer(const Lsn &a, const Lsn &b) {
	return !(a <= b);
}

//! A change's place: its allocation unit, its page and its transaction.
struct ChangeKey {
	uint64_t unit;
	uint64_t page;
	uint64_t transaction;
	bool operator==(const ChangeKey &other) const {
		return unit == other.unit && page == other.page && transaction == other.transaction;
	}
};
struct ChangeKeyHash {
	size_t operator()(const ChangeKey &k) const {
		uint64_t h = k.unit * 0x9E3779B97F4A7C15ULL;
		h ^= k.page + 0x9E3779B97F4A7C15ULL + (h << 6) + (h >> 2);
		h ^= k.transaction + 0x9E3779B97F4A7C15ULL + (h << 6) + (h >> 2);
		return static_cast<size_t>(h);
	}
};

//! The summary's builder: one pass over the records, the transactions' fates known only at its end.
class Summarizer {
public:
	Summarizer(const Lsn &first, const Lsn &last, const Lsn &checkpoint) {
		summary_.first = first;
		summary_.last = last;
		summary_.checkpoint = checkpoint;
	}

	void Add(const LogRecord &r) {
		if (!summary_.last.IsZero() && summary_.last <= r.lsn) {
			return;	 // LastLSN is the next record after the backup's (HEADERONLY): not the backup's
		}
		NoteRecord(r);
		if (r.transaction == 0) {
			// A record of no transaction (id 0) is the system's own work on the page, which no transaction of
			// the database commits or undoes: not a row change of the flags.
			return;
		}
		auto &t = summary_.transactions[r.transaction];
		t.records++;
		if (r.op == log_op::BEGIN_XACT) {
			if (t.records > 1) {
				// A BEGIN_XACT is its transaction's first record, and SQL Server does not reuse an id within
				// one log: a second life of the id would file the first's changes under the second's fate
				// (review of #591: a committed change read as open, the table as consistent).
				throw FormatError("backup: the log tail begins transaction " + TransactionIdText(r.transaction) +
								  " at " + r.lsn.ToDecimal() + " after " + std::to_string(t.records - 1) +
								  " records of it: an id begun twice, the tail is not one log");
			}
			Begin(t, r);
		} else if (r.op == log_op::COMMIT_XACT || r.op == log_op::ABORT_XACT) {
			t.state = r.op == log_op::COMMIT_XACT ? FuzzySummary::State::Committed : FuzzySummary::State::RolledBack;
			t.end = r.lsn;
		} else if (IsRowChange(r)) {
			NoteRowChange(t, r);
		}
	}

	//! The record at CheckpointLSN was read: the tail holds what it claims, at least to there.
	bool SawCheckpoint() const {
		return saw_checkpoint_;
	}

	//! What the summary holds so far, roughly, in bytes: MemoryBytes's measure of what Finish keeps, and
	//! the builder's index and set beside it.
	uint64_t Bytes() const {
		return sizeof(FuzzySummary) + summary_.transactions.size() * (sizeof(FuzzySummary::Transaction) + 48) +
			   pages_ * 64 + index_.size() * (sizeof(FuzzySummary::Change) + sizeof(ChangeKey) + 32) +
			   (summary_.formatted.size() + after_checkpoint_.size()) * (sizeof(uint64_t) + 2 * sizeof(void *));
	}

	//! The last record read before LastLSN (zero for none), and its block's bytes.
	const Lsn &Reached() const {
		return reached_;
	}
	uint16_t ReachedBlockSize() const {
		return reached_block_size_;
	}

	FuzzySummary Finish() {
		auto open = [&](uint64_t transaction) {
			auto it = summary_.transactions.find(transaction);
			return it != summary_.transactions.end() && it->second.state == FuzzySummary::State::Open;
		};
		for (const auto &t : summary_.transactions) {
			if (t.second.state == FuzzySummary::State::Open) {
				summary_.open_transactions++;
			}
		}
		auto rolled_back = [&](uint64_t transaction) {
			auto it = summary_.transactions.find(transaction);
			return it != summary_.transactions.end() && it->second.state == FuzzySummary::State::RolledBack;
		};
		for (auto transaction : after_checkpoint_) {
			if (open(transaction)) {
				continue;
			}
			(rolled_back(transaction) ? summary_.rolled_back_after_checkpoint : summary_.committed_after_checkpoint)++;
		}
		// Each page's changes freed once redistributed, and the index first: the peak stays about one copy
		// (review of #591).
		std::unordered_map<ChangeKey, size_t, ChangeKeyHash>().swap(index_);
		for (auto &unit : changes_) {
			auto &out = summary_.units[unit.first];
			for (auto &page : unit.second) {
				for (auto &c : page.second) {
					if (open(c.transaction)) {
						out.open[page.first].push_back(c);
					} else {
						c.rolled_back = rolled_back(c.transaction);
						out.finished[page.first].push_back(c);
					}
				}
				std::vector<FuzzySummary::Change>().swap(page.second);
			}
		}
		return std::move(summary_);
	}

private:
	//! What every record of the backup counts for, whatever it is: the count, the last record read, the
	//! checkpoint's own record, a formatted page.
	void NoteRecord(const LogRecord &r) {
		summary_.records++;
		if (reached_.IsZero() || Newer(r.lsn, reached_)) {
			reached_ = r.lsn;
			reached_block_size_ = r.block_size;
		}
		if (r.lsn <= summary_.checkpoint && summary_.checkpoint <= r.lsn) {
			saw_checkpoint_ = true;
		}
		if (r.op == log_op::FORMAT_PAGE && r.has_page) {
			summary_.formatted.insert(static_cast<uint64_t>(r.file) << 32 | r.page);
		}
	}

	static void Begin(FuzzySummary::Transaction &t, const LogRecord &r) {
		t.state = FuzzySummary::State::Open;
		t.begin = r.lsn;
		// Its name, the first element: UTF-16 (fn_dblog's Transaction Name). A record whose contents do not
		// parse keeps none: the name is for reading, never for a flag.
		try {
			const auto elements = RowLogElements(r, "a BEGIN_XACT");
			if (!elements.empty()) {
				t.name = Utf16LeToUtf8(elements[0].data, elements[0].length);
			}
		} catch (FormatError &) {
		}
	}

	//! A change of the rows a reader sees on the record's page, of the record's unit (fixture tail_window,
	//! each against a RESTORE, review of #591):
	//! - a row operation in a row context;
	//! - a FORMAT_PAGE in a row context that carries the page's image: an INSERT ... WITH (TABLOCK) into a
	//!   heap logs its pages so, with no INSERT_ROWS (one left open read consistent, its rows in the copy).
	//!   One that lays out an empty page has no element;
	//! - extents freed from the unit (a SET_BITS clearing bits of its IAM page): a small table's TRUNCATE
	//!   logs only its extents freed (PFS, GAM, its IAM), and read consistent with the copy's rows gone. An
	//!   index rebuild frees the old unit's extents the same way, and reads stale with its rows as the
	//!   RESTORE's: the price of not missing a TRUNCATE; so would a ghost cleanup freeing an extent emptied
	//!   before the copy. Extents allocated (bits set) are not counted: an allocation may commit on its own
	//!   (AllocFirstPage, a transaction of the system's) while the rows put there stay open, which a RESTORE
	//!   undoes -- the table equal, read stale.
	//! Still not counted, the table read consistent where a RESTORE differs: a large table's TRUNCATE (a
	//! deferred drop: the rowset replaced, its extents freed later, maybe past LastLSN), a partition switched
	//! in the tail, pages freed from a mixed extent (the IAM's single-page slots); a schema change shows on
	//! the system tables' rows only (sysrowsets, sysallocunits, syscolpars ...), not on the table's.
	static bool IsRowChange(const LogRecord &r) {
		if (!r.has_allocation_unit || !r.has_page) {
			return false;
		}
		if (IsRowOperation(r.op)) {
			return IsRowContext(r.context);
		}
		if (r.op == log_op::FORMAT_PAGE) {
			return IsRowContext(r.context) && HoldsPageImage(r);
		}
		return r.op == log_op::SET_BITS && r.context == log_context::IAM && FreesExtents(r);
	}

	//! A SET_BITS's fixed part from @48: u16 the first bit (an extent of the IAM page's interval), u16 their
	//! count, u16 the value set (measured on 2025: 1 for an extent allocated, 0 for one freed). A shorter
	//! fixed part is counted, so no change is missed.
	static bool FreesExtents(const LogRecord &r) {
		if (!r.bytes || r.fixed_length < SET_BITS_FIXED) {
			return true;
		}
		return r.bytes[SET_BITS_FIXED - 2] == 0 && r.bytes[SET_BITS_FIXED - 1] == 0;
	}

	static bool HoldsPageImage(const LogRecord &r) {
		try {
			for (const auto &e : RowLogElements(r, "a FORMAT_PAGE")) {
				if (e.length != 0) {
					return true;
				}
			}
			return false;
		} catch (FormatError &) {
			return true;  // contents that do not parse: counted, so no change is missed
		}
	}

	void NoteRowChange(FuzzySummary::Transaction &t, const LogRecord &r) {
		t.row_operations++;
		t.after_checkpoint = t.after_checkpoint || Newer(r.lsn, summary_.checkpoint);
		const uint64_t page = (static_cast<uint64_t>(r.file) << 32) | r.page;
		auto &changes = changes_[r.allocation_unit][page];
		pages_ += changes.empty();
		// The page's change by this transaction, found by its index (review of #591: a linear search of the
		// page's changes was quadratic in the short transactions of a hot page).
		const ChangeKey key{r.allocation_unit, page, r.transaction};
		auto at = index_.find(key);
		if (at != index_.end()) {
			Widen(changes[at->second], r.lsn);
		} else {
			FuzzySummary::Change c;
			c.transaction = r.transaction;
			c.oldest = r.lsn;
			c.newest = r.lsn;
			index_.emplace(key, changes.size());
			changes.push_back(c);
		}
		if (Newer(r.lsn, summary_.checkpoint)) {
			after_checkpoint_.insert(r.transaction);
		}
	}

	static void Widen(FuzzySummary::Change &c, const Lsn &lsn) {
		if (Newer(lsn, c.newest)) {
			c.newest = lsn;
		}
		if (Newer(c.oldest, lsn)) {
			c.oldest = lsn;
		}
	}

	FuzzySummary summary_;
	std::unordered_map<uint64_t, std::unordered_map<uint64_t, std::vector<FuzzySummary::Change>>> changes_;
	std::unordered_set<uint64_t> after_checkpoint_;
	//! Where each (unit, page, transaction)'s change is in its page's vector.
	std::unordered_map<ChangeKey, size_t, ChangeKeyHash> index_;
	uint64_t pages_ = 0;  // (unit, page)s with a change
	bool saw_checkpoint_ = false;
	Lsn reached_;
	uint16_t reached_block_size_ = 0;
};

//! What the backup holds of a page: its copy's state, and its copy read when Present.
PageState CopyOf(const PageStore &pages, uint64_t key, Page &copy) {
	try {
		return pages.Fetch(static_cast<uint16_t>(key >> 32), static_cast<uint32_t>(key), copy);
	} catch (FormatError &) {
		return PageState::Pending;	// a live page whose copy the backup lacks or misnames: none of it holds
	}
}

}  // namespace

size_t FuzzySummary::MemoryBytes() const {
	size_t bytes = sizeof(*this) + transactions.size() * (sizeof(Transaction) + 48) +
				   formatted.size() * (sizeof(uint64_t) + 2 * sizeof(void *)) +
				   formatted.bucket_count() * sizeof(void *);
	for (const auto &unit : units) {
		for (const auto *pages : {&unit.second.finished, &unit.second.open}) {
			for (const auto &page : *pages) {
				bytes += 64 + page.second.capacity() * sizeof(Change);
			}
		}
	}
	return bytes;
}

bool FormattedInTail(const FuzzySummary &tail, uint16_t file, uint32_t page) {
	return tail.formatted.count(static_cast<uint64_t>(file) << 32 | page) != 0;
}

FuzzySummary SummarizeLogTail(const LogTailReader &tail, const Lsn &first, const Lsn &last, const Lsn &checkpoint,
							  const std::function<void(uint64_t bytes)> &poll) {
	Summarizer summarizer(first, last, checkpoint);
	uint64_t read = 0;
	tail.ForEach([&](const LogRecord &r) {
		if (poll && read++ % SUMMARY_POLL_RECORDS == 0) {
			poll(summarizer.Bytes());
		}
		summarizer.Add(r);
	});
	// A data backup's tail runs from FirstLSN (at or before the checkpoint) to LastLSN: one that does not
	// hold the checkpoint's own record was not read whole, and a summary of what it holds would read as
	// consistent (review of #591: an empty one did).
	if (!checkpoint.IsZero() && first <= checkpoint && !(last <= checkpoint) && !summarizer.SawCheckpoint()) {
		throw FormatError("backup: the log tail (" + first.ToDecimal() + " to " + last.ToDecimal() +
						  ") holds no record at its CheckpointLSN " + checkpoint.ToDecimal() +
						  ": it is not read whole");
	}
	// Nor one that stops before its end: a change committed past where it stops would read as consistent
	// (review of #591). LastLSN is the record after the backup's, so the check is the record it follows.
	if (!last.IsZero() && !(last <= first) &&
		(summarizer.Reached().IsZero() ||
		 !TailReachesLast(summarizer.Reached(), summarizer.ReachedBlockSize(), last))) {
		throw FormatError("backup: the log tail (" + first.ToDecimal() + " to " + last.ToDecimal() + ") ends at " +
						  (summarizer.Reached().IsZero() ? std::string("no record")
														 : "the record " + summarizer.Reached().ToDecimal()) +
						  ", not at the record its LastLSN follows: it is not read whole");
	}
	return summarizer.Finish();
}

FuzzySummary SummarizeLogRecords(const std::vector<LogRecord> &records, const Lsn &first, const Lsn &last,
								 const Lsn &checkpoint) {
	Summarizer summarizer(first, last, checkpoint);
	for (const auto &r : records) {
		summarizer.Add(r);
	}
	return summarizer.Finish();
}

const char *TransactionStateName(FuzzySummary::State state) {
	switch (state) {
	case FuzzySummary::State::Open:
		return "open";
	case FuzzySummary::State::Committed:
		return "committed";
	case FuzzySummary::State::RolledBack:
		return "rolled back";
	default:
		return "no begin";
	}
}

std::string TransactionIdText(uint64_t transaction) {
	char buf[24];
	snprintf(buf, sizeof(buf), "%04x:%08x", static_cast<unsigned>(transaction >> 32),
			 static_cast<unsigned>(transaction & 0xffffffffu));
	return buf;
}

const char *TableFuzziness::Status() const {
	if (stale && dirty) {
		return "stale+dirty";
	}
	if (stale) {
		return "stale";
	}
	return dirty ? "dirty" : "consistent";
}

bool ChangeShows(bool open, const FuzzySummary::Change &c, PageState state, const Lsn &copy_lsn,
				 const Lsn &checkpoint) {
	if (open) {
		// (iii): its oldest row operation on the page no newer than the copy: the copy holds what a RESTORE
		// undoes. One made after the copy (or on a page the image does not have) is not in it; a Pending or
		// unreadable page may hold it.
		return state == PageState::Present ? !Newer(c.oldest, copy_lsn) : state != PageState::NotLive;
	}
	if (c.rolled_back) {
		// Its changes undone in the tail: the copy differs only when it holds a change and lacks the
		// compensation, between the two. A page the image does not have, or does not hold, holds none of it.
		return state == PageState::Present && !Newer(c.oldest, copy_lsn) && Newer(c.newest, copy_lsn);
	}
	// (iv): a committed change the copy does not hold. A page the backup holds as not live was freed before
	// the image (its rows gone with it: a delete, a split), unless the change came after CheckpointLSN, on a
	// page taken into use after the image; a Pending or unreadable one holds none.
	return state == PageState::Present	 ? Newer(c.newest, copy_lsn)
		   : state == PageState::NotLive ? Newer(c.newest, checkpoint)
										 : true;
}

namespace {

//! The transactions of one unit whose changes show, each page's copy read once.
void UnitShowing(const FuzzySummary &summary, const FuzzySummary::Unit &unit, const PageStore &pages,
				 std::unordered_set<uint64_t> &dirty, std::unordered_set<uint64_t> &stale) {
	Page copy;
	for (int open = 0; open < 2; open++) {
		for (const auto &page : open ? unit.open : unit.finished) {
			const PageState state = CopyOf(pages, page.first, copy);
			for (const auto &c : page.second) {
				if (ChangeShows(open != 0, c, state, copy.header.lsn, summary.checkpoint)) {
					(open ? dirty : stale).insert(c.transaction);
				}
			}
		}
	}
}

TableFuzziness FuzzinessFrom(const std::unordered_set<uint64_t> &dirty, const std::unordered_set<uint64_t> &stale) {
	TableFuzziness result;
	result.dirty = !dirty.empty();
	result.stale = !stale.empty();
	result.open_transactions = static_cast<uint32_t>(dirty.size());
	result.stale_transactions = static_cast<uint32_t>(stale.size());
	return result;
}

}  // namespace

TableFuzziness FuzzinessOf(const FuzzySummary &summary, const std::vector<uint64_t> &units, const PageStore &pages) {
	std::unordered_set<uint64_t> dirty;
	std::unordered_set<uint64_t> stale;
	for (auto au : units) {
		auto it = summary.units.find(au);
		if (it != summary.units.end()) {
			UnitShowing(summary, it->second, pages, dirty, stale);
		}
	}
	return FuzzinessFrom(dirty, stale);
}

TableFuzziness FuzzinessMemo::Of(const FuzzySummary &summary, const std::vector<uint64_t> &units,
								 const PageStore &pages) {
	// A table's transactions are counted once over its units: each unit's kept as its own set, joined here.
	std::unordered_set<uint64_t> dirty;
	std::unordered_set<uint64_t> stale;
	for (auto au : units) {
		auto it = summary.units.find(au);
		if (it == summary.units.end()) {
			continue;
		}
		std::shared_ptr<const UnitShows> shows;
		{
			std::lock_guard<std::mutex> guard(lock_);
			auto kept = units_.find(au);
			if (kept != units_.end()) {
				shows = kept->second;
			}
		}
		if (!shows) {
			// Read outside the lock: the copies are fetched through the page store, which may wait on I/O.
			std::unordered_set<uint64_t> unit_dirty;
			std::unordered_set<uint64_t> unit_stale;
			UnitShowing(summary, it->second, pages, unit_dirty, unit_stale);
			auto made = std::make_shared<UnitShows>();
			made->dirty.assign(unit_dirty.begin(), unit_dirty.end());
			made->stale.assign(unit_stale.begin(), unit_stale.end());
			std::lock_guard<std::mutex> guard(lock_);
			auto put = units_.emplace(au, made);
			if (put.second) {
				transactions_ += made->dirty.size() + made->stale.size();
			}
			shows = put.first->second;
		}
		dirty.insert(shows->dirty.begin(), shows->dirty.end());
		stale.insert(shows->stale.begin(), shows->stale.end());
	}
	return FuzzinessFrom(dirty, stale);
}

size_t FuzzinessMemo::MemoryBytes() const {
	std::lock_guard<std::mutex> guard(lock_);
	return units_.size() * (sizeof(UnitShows) + 64) + transactions_ * sizeof(uint64_t);
}

std::vector<uint64_t> BaseUnitsOf(const CatalogTable &table) {
	std::vector<uint64_t> units;
	for (const auto &u : table.units) {
		if (u.index_id <= 1) {
			units.push_back(u.unit.auid);
		}
	}
	return units;
}

TableFuzziness FuzzinessOf(const FuzzySummary &summary, const CatalogTable &table, const PageStore &pages) {
	return FuzzinessOf(summary, BaseUnitsOf(table), pages);
}

bool ParseFuzzyMode(const std::string &text, FuzzyMode &mode) {
	std::string lower(text);
	std::transform(lower.begin(), lower.end(), lower.begin(),
				   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	if (lower == "warn") {
		mode = FuzzyMode::Warn;
	} else if (lower == "error") {
		mode = FuzzyMode::Error;
	} else if (lower == "ignore") {
		mode = FuzzyMode::Ignore;
	} else {
		return false;
	}
	return true;
}

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb
