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

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>

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
	void Acquire(uint64_t bytes, const std::function<bool()> &interrupted);
	//! Gives back what one Acquire of `bytes` held; never throws.
	void Release(uint64_t bytes);

	uint64_t Budget() const {
		return budget_;
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
