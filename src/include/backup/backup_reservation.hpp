//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// backup/backup_reservation.hpp
//
// Spec 085 D17: the one rule for the reader's working memory (a chunk's decode staging, PR 2e; a LOB
// value, PR 3). A reservation is held against the call's budget (half of `max_memory`) and DuckDB's
// (BufferManager::ReserveMemory, under the EXTENSION tag), before the memory is allocated. One that
// does not fit, in either, waits for another holder to release; one that still cannot fit when
// nothing else of the call is held is refused. DuckDB-free: DuckDB's side comes as two callbacks, so
// the waits and refusals are tested on threads (test_backup_reservation.cpp).
//===----------------------------------------------------------------------===//

#pragma once

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace duckdb {
namespace mssql {
namespace backup {

//! A reservation D17 refuses: past the call's budget, or one DuckDB could not make room for with
//! nothing else of the call held. The caller words it, naming the object and the limits.
class ReservationRefused : public std::runtime_error {
public:
	ReservationRefused(bool past_budget, const std::string &duckdb_reason)
		: std::runtime_error(past_budget ? "past the call's budget" : duckdb_reason),
		  past_budget(past_budget),
		  duckdb_reason(duckdb_reason) {}
	//! True: the reservation alone is more than the budget. False: DuckDB refused it (duckdb_reason).
	bool past_budget;
	std::string duckdb_reason;
};

//! A wait the caller interrupted (its `interrupted` answered true).
class ReservationInterrupted : public std::runtime_error {
public:
	ReservationInterrupted() : std::runtime_error("interrupted while waiting for memory") {}
};

class ReservationGate {
public:
	//! DuckDB's side: reserve `bytes` (true), or false with why it cannot; and give them back.
	using ReserveFn = std::function<bool(uint64_t bytes, std::string &why)>;
	using ReleaseFn = std::function<void(uint64_t bytes)>;

	ReservationGate(uint64_t budget, ReserveFn reserve, ReleaseFn release)
		: budget_(budget), reserve_(std::move(reserve)), release_(std::move(release)) {}

	//! Holds `bytes` once they fit the budget and DuckDB has reserved them; until then waits for a
	//! release, asking `interrupted` (may be empty) every 100 ms. ReservationRefused when they cannot
	//! fit with nothing else held; ReservationInterrupted when asked to stop.
	void Acquire(uint64_t bytes, const std::function<bool()> &interrupted) {
		Acquire(bytes, bytes, interrupted);
	}
	//! The same, of which only `duckdb_bytes` (at most `bytes`) are reserved with DuckDB: the rest is
	//! memory DuckDB accounts itself when it is allocated (a value written straight into an output
	//! vector's heap, PR 3), held against the budget only, so the call's values in flight stay within
	//! it without being counted twice in DuckDB's memory.
	//! `holds_nothing`: the caller holds no reservation of this gate, so it may wait behind those
	//! already waiting for the budget (it waits for nobody it blocks). One that holds some must not:
	//! a waiter could be waiting for exactly what it holds.
	void Acquire(uint64_t bytes, uint64_t duckdb_bytes, const std::function<bool()> &interrupted,
				 bool holds_nothing = false);
	//! The same without waiting: false when they do not fit the budget beside what is held, when an
	//! Acquire is waiting for the budget, or when DuckDB cannot make room now. ReservationRefused only
	//! when `bytes` alone is past the budget.
	bool TryAcquire(uint64_t bytes, uint64_t duckdb_bytes);
	//! Gives back what one Acquire of `bytes` (`duckdb_bytes` of them DuckDB's) held; never throws.
	void Release(uint64_t bytes) {
		Release(bytes, bytes);
	}
	void Release(uint64_t bytes, uint64_t duckdb_bytes);

	uint64_t Budget() const {
		return budget_;
	}
	//! An Acquire waits for the budget now (read with no lock: a caller holding credit yields to it).
	bool HasWaiters() const {
		return waiting_.load(std::memory_order_relaxed) > 0;
	}
	//! Bytes held now, and by how many Acquires.
	uint64_t Held() const;
	uint64_t Holders() const;

private:
	//! An Acquire's bytes given back; waiters woken. The lock is held.
	void GiveBackLocked(uint64_t bytes);
	//! One wait of up to 100 ms, asking `interrupted` first. The lock is held.
	void WaitLocked(std::unique_lock<std::mutex> &guard, const std::function<bool()> &interrupted);

	const uint64_t budget_;
	ReserveFn reserve_;
	ReleaseFn release_;
	mutable std::mutex lock_;
	std::condition_variable released_;
	uint64_t held_ = 0;
	uint64_t holders_ = 0;
	//! Counts memory given back: releases and failed attempts (a waiter on the budget wakes on either).
	uint64_t generation_ = 0;
	//! Counts releases only (a waiter DuckDB refused wakes on these).
	uint64_t releases_ = 0;
	//! Acquires waiting for the budget: while there are any, a new Acquire of a caller that holds
	//! nothing waits before it tries, and a TryAcquire answers false, so a large one is not starved by
	//! smaller ones taking every release (review of PR 3 S4). The one waiting behind holds nothing, so
	//! the tasks it waits for are running and will release.
	std::atomic<uint64_t> waiting_{0};
	friend struct WaitingCount;
};

//! Every reservation of one call (a chunk's, then a row's values stored off row, PR 3), given back
//! together by ReleaseAll or the destructor (an error mid-chunk gives the memory back too).
//!
//! A row's TryAcquire is served from the call's credit when it covers it: a grant to a row takes up to
//! CREDIT_BYTES (a sixteenth of the budget at most) beyond it against the budget, and of that extra
//! the row's own share with DuckDB (the rows of a column are alike: a part a row writes straight into
//! an output vector is not reserved with DuckDB for the rows after it either), so the rows of a chunk
//! do not each take the gate's lock and DuckDB's. Measured (PR 3 S7, 200,000 rows of 10 KB off row,
//! 8 threads): one grant a row spent 0.95 s of the scan's CPU, nearly all of it waiting for the gate's
//! mutex, where the in-row columns alone took 0.04 s. The credit is not used while an Acquire waits
//! for the budget (the row asks the gate, which answers no, and the chunk closes, as without it). A
//! grant that does not fit is asked again for the row alone, and not asked again in the call. What a
//! call holds unused is at most a grant: up to CREDIT_BYTES a running call, held from the others.
class CallReservations {
public:
	CallReservations(ReservationGate &gate, std::function<bool()> interrupted)
		: gate_(gate), interrupted_(std::move(interrupted)) {}
	~CallReservations() {
		ReleaseAll();
	}
	CallReservations(const CallReservations &) = delete;
	CallReservations &operator=(const CallReservations &) = delete;
	// The list grows before the gate grants anything: a reservation granted is always recorded, so it
	// is always given back (review of PR 3 S4: a bad_alloc after the grant left it held for ever).
	void Acquire(uint64_t bytes, uint64_t duckdb_bytes) {
		held_.reserve(held_.size() + 1);
		gate_.Acquire(bytes, duckdb_bytes, interrupted_, held_.empty());
		held_.emplace_back(bytes, duckdb_bytes);
	}
	bool TryAcquire(uint64_t bytes, uint64_t duckdb_bytes) {
		if (bytes <= credit_ && duckdb_bytes <= credit_duckdb_ && !gate_.HasWaiters()) {
			credit_ -= bytes;
			credit_duckdb_ -= duckdb_bytes;
			return true;
		}
		held_.reserve(held_.size() + 1);
		const uint64_t grant = std::min<uint64_t>(CREDIT_BYTES, gate_.Budget() / 16);
		if (bytes < grant && !grant_refused_) {
			const uint64_t extra = grant - bytes;
			const uint64_t extra_duckdb = bytes ? extra * duckdb_bytes / bytes : 0;	 // < 2^40: both below 1 MiB
			if (gate_.TryAcquire(grant, duckdb_bytes + extra_duckdb)) {
				held_.emplace_back(grant, duckdb_bytes + extra_duckdb);
				credit_ += extra;
				credit_duckdb_ += extra_duckdb;
				return true;
			}
			// Not again in this call: each refused grant is one more reservation DuckDB tries (and may
			// evict for) and one more wakeup of the waiters, before the row is asked for alone.
			grant_refused_ = true;
		}
		if (!gate_.TryAcquire(bytes, duckdb_bytes)) {
			return false;
		}
		held_.emplace_back(bytes, duckdb_bytes);
		return true;
	}
	void ReleaseAll() {
		for (const auto &h : held_) {
			gate_.Release(h.first, h.second);
		}
		held_.clear();
		credit_ = credit_duckdb_ = 0;
		grant_refused_ = false;
	}
	//! The most a row's grant takes beyond the row, for the rows after it.
	static constexpr uint64_t CREDIT_BYTES = 1 << 20;

private:
	ReservationGate &gate_;
	std::function<bool()> interrupted_;
	std::vector<std::pair<uint64_t, uint64_t>> held_;
	//! Held and not yet given to a row: against the budget, and of it with DuckDB.
	uint64_t credit_ = 0;
	uint64_t credit_duckdb_ = 0;
	bool grant_refused_ = false;
};

//! One Acquire, released by its destructor (an error mid-chunk gives the memory back too).
class HeldReservation {
public:
	HeldReservation(ReservationGate &gate, uint64_t bytes, const std::function<bool()> &interrupted)
		: gate_(gate), bytes_(bytes) {
		gate_.Acquire(bytes_, interrupted);
	}
	~HeldReservation() {
		gate_.Release(bytes_);
	}
	HeldReservation(const HeldReservation &) = delete;
	HeldReservation &operator=(const HeldReservation &) = delete;

private:
	ReservationGate &gate_;
	uint64_t bytes_;
};

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb
