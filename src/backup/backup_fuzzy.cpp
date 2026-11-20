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

bool Newer(const Lsn &a, const Lsn &b) {
	return !(a <= b);
}

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
		summary_.records++;
		FuzzySummary::Transaction *t = nullptr;
		if (r.transaction != 0) {
			t = &summary_.transactions[r.transaction];
			t->records++;
		}
		if (t && r.op == log_op::BEGIN_XACT) {
			t->state = FuzzySummary::State::Open;
			t->begin = r.lsn;
			// Its name, the first element: UTF-16 (fn_dblog's Transaction Name). A record whose contents do not
			// parse keeps none: the name is for reading, never for a flag.
			try {
				const auto elements = RowLogElements(r, "a BEGIN_XACT");
				if (!elements.empty()) {
					t->name = Utf16LeToUtf8(elements[0].data, elements[0].length);
				}
			} catch (FormatError &) {
			}
			return;
		}
		if (t && (r.op == log_op::COMMIT_XACT || r.op == log_op::ABORT_XACT)) {
			t->state = r.op == log_op::COMMIT_XACT ? FuzzySummary::State::Committed : FuzzySummary::State::RolledBack;
			t->end = r.lsn;
			return;
		}
		// A record of no transaction (id 0) is the system's own work on the page, which no transaction of the
		// database commits or undoes: not a row change of the flags.
		if (!t || !IsRowOperation(r.op) || !IsRowContext(r.context) || !r.has_allocation_unit || !r.has_page) {
			return;
		}
		{
			t->row_operations++;
			t->after_checkpoint = t->after_checkpoint || Newer(r.lsn, summary_.checkpoint);
		}
		const uint64_t page = (static_cast<uint64_t>(r.file) << 32) | r.page;
		auto &changes = changes_[r.allocation_unit][page];
		bool found = false;
		for (auto &c : changes) {
			if (c.transaction == r.transaction) {
				if (Newer(r.lsn, c.newest)) {
					c.newest = r.lsn;
				}
				if (Newer(c.oldest, r.lsn)) {
					c.oldest = r.lsn;
				}
				found = true;
				break;
			}
		}
		if (!found) {
			FuzzySummary::Change c;
			c.transaction = r.transaction;
			c.oldest = r.lsn;
			c.newest = r.lsn;
			changes.push_back(c);
		}
		if (Newer(r.lsn, summary_.checkpoint)) {
			after_checkpoint_.insert(r.transaction);
		}
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
		for (auto transaction : after_checkpoint_) {
			if (!open(transaction)) {
				summary_.finished_after_checkpoint++;
			}
		}
		for (auto &unit : changes_) {
			auto &out = summary_.units[unit.first];
			for (auto &page : unit.second) {
				for (const auto &c : page.second) {
					(open(c.transaction) ? out.open : out.finished)[page.first].push_back(c);
				}
			}
		}
		return std::move(summary_);
	}

private:
	FuzzySummary summary_;
	std::unordered_map<uint64_t, std::unordered_map<uint64_t, std::vector<FuzzySummary::Change>>> changes_;
	std::unordered_set<uint64_t> after_checkpoint_;
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
	size_t bytes = sizeof(*this) + transactions.size() * (sizeof(Transaction) + 48);
	for (const auto &unit : units) {
		for (const auto *pages : {&unit.second.finished, &unit.second.open}) {
			for (const auto &page : *pages) {
				bytes += 64 + page.second.capacity() * sizeof(Change);
			}
		}
	}
	return bytes;
}

FuzzySummary SummarizeLogTail(const LogTailReader &tail, const Lsn &first, const Lsn &last, const Lsn &checkpoint) {
	Summarizer summarizer(first, last, checkpoint);
	tail.ForEach([&](const LogRecord &r) { summarizer.Add(r); });
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

TableFuzziness FuzzinessOf(const FuzzySummary &summary, const std::vector<uint64_t> &units, const PageStore &pages) {
	TableFuzziness result;
	std::unordered_set<uint64_t> dirty;
	std::unordered_set<uint64_t> stale;
	Page copy;
	for (auto au : units) {
		auto it = summary.units.find(au);
		if (it == summary.units.end()) {
			continue;
		}
		// (iv): a finished transaction's change the copy does not hold. A page the backup holds as not live was
		// freed before the image (its rows gone with it: a delete, a split), unless the change came after
		// CheckpointLSN, on a page taken into use after the image; a Pending or unreadable one holds none.
		for (const auto &page : it->second.finished) {
			const PageState state = CopyOf(pages, page.first, copy);
			for (const auto &c : page.second) {
				const bool missing = state == PageState::Present   ? Newer(c.newest, copy.header.lsn)
									 : state == PageState::NotLive ? Newer(c.newest, summary.checkpoint)
																   : true;
				if (missing) {
					stale.insert(c.transaction);
				}
			}
		}
		// (iii): an open transaction's change the copy holds -- its oldest row operation on the page no newer
		// than the copy -- which a RESTORE undoes. One made after the copy (or on a page the image does not
		// have) is not in it, as the RESTORE's undo leaves it; a Pending or unreadable page may hold it.
		for (const auto &page : it->second.open) {
			const PageState state = CopyOf(pages, page.first, copy);
			for (const auto &c : page.second) {
				const bool held = state == PageState::Present	? !Newer(c.oldest, copy.header.lsn)
								  : state == PageState::NotLive ? false
																: true;
				if (held) {
					dirty.insert(c.transaction);
				}
			}
		}
	}
	result.dirty = !dirty.empty();
	result.stale = !stale.empty();
	result.open_transactions = static_cast<uint32_t>(dirty.size());
	result.stale_transactions = static_cast<uint32_t>(stale.size());
	return result;
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
