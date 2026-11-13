#include "backup/backup_reservation.hpp"

#include <chrono>

namespace duckdb {
namespace mssql {
namespace backup {

//! An Acquire counted among those waiting for the budget until it stops waiting, by success or by any
//! way out (a refusal, an interruption, DuckDB's side throwing). Its destructor runs with the gate's
//! lock held: every way out of Acquire that can leave it counted leaves under the lock.
struct WaitingCount {
	explicit WaitingCount(ReservationGate &gate_p) : gate(gate_p) {}
	~WaitingCount() {
		Stop();
	}
	void Start() {
		if (!counted) {
			gate.waiting_++;
			counted = true;
		}
	}
	void Stop() {
		if (counted) {
			gate.waiting_--;
			counted = false;
			gate.released_.notify_all();
		}
	}
	ReservationGate &gate;
	bool counted = false;
};

void ReservationGate::Acquire(uint64_t bytes, uint64_t duckdb_bytes, const std::function<bool()> &interrupted,
							  bool holds_nothing) {
	if (bytes > budget_) {
		// No release can make room: refused at once, whatever else is held. Keep this first: with
		// `held_ <= budget_` always and `bytes <= budget_` from here on, `held_ + bytes` below cannot
		// pass 2 * budget_, so it never overflows (review of #553).
		throw ReservationRefused(true, "");
	}
	std::unique_lock<std::mutex> guard(lock_);
	WaitingCount waiting(*this);
	// Behind those already waiting for the budget: a newcomer that holds nothing does not take what they
	// wait for.
	while (holds_nothing && waiting_ > 0) {
		WaitLocked(guard, interrupted);
	}
	for (;;) {
		if (held_ + bytes <= budget_) {
			held_ += bytes;
			holders_++;
			waiting.Stop();
			guard.unlock();
			std::string why;
			bool reserved = false;
			try {
				// DuckDB's reservation outside the lock: it may evict blocks to make room, or spill them.
				reserved = duckdb_bytes == 0 || reserve_(duckdb_bytes, why);
			} catch (...) {
				// Anything but a refusal (a spill that found the disk full): the attempt given back, and
				// the waiters woken, before it goes up (review: the counters stayed raised, a hang).
				guard.lock();
				GiveBackLocked(bytes);
				throw;
			}
			if (reserved) {
				return;
			}
			guard.lock();
			GiveBackLocked(bytes);
			if (holders_ == 0) {
				// Alone, and DuckDB cannot make room: nothing of this call will release any.
				throw ReservationRefused(false, why);
			}
			// DuckDB refused while others hold: only a release can change its answer, so wait for one
			// -- not for another waiter's failed attempt, which would retry (and evict) back to back.
			// The wait ends too when the others go without releasing anything (every one of them
			// refused, or DuckDB's side threw): no release can come then, and waiting for one parked
			// the task for ever while DuckDB waited for the task (review of #553). Falling out that
			// way, the loop tries once more -- nothing of the call is held now, so DuckDB's answer
			// may differ -- and a refusal there finds `holders_` 0 and is the refusal by name.
			const uint64_t seen = releases_;
			while (releases_ == seen && holders_ > 0) {
				WaitLocked(guard, interrupted);
			}
			continue;
		}
		// Past the budget beside the others (holders_ > 0 here: with none held, held_ is 0 and `bytes`
		// fits it): wait for any memory given back, counted as waiting.
		waiting.Start();
		const uint64_t seen = generation_;
		while (generation_ == seen) {
			WaitLocked(guard, interrupted);
		}
	}
}

bool ReservationGate::TryAcquire(uint64_t bytes, uint64_t duckdb_bytes) {
	if (bytes > budget_) {
		throw ReservationRefused(true, "");
	}
	std::unique_lock<std::mutex> guard(lock_);
	if (held_ + bytes > budget_ || waiting_ > 0) {
		return false;
	}
	held_ += bytes;
	holders_++;
	guard.unlock();
	std::string why;
	bool reserved = false;
	try {
		reserved = duckdb_bytes == 0 || reserve_(duckdb_bytes, why);
	} catch (...) {
		guard.lock();
		GiveBackLocked(bytes);
		throw;
	}
	if (!reserved) {
		guard.lock();
		GiveBackLocked(bytes);
		return false;
	}
	return true;
}

void ReservationGate::GiveBackLocked(uint64_t bytes) {
	held_ -= bytes;
	holders_--;
	generation_++;
	released_.notify_all();
}

void ReservationGate::WaitLocked(std::unique_lock<std::mutex> &guard, const std::function<bool()> &interrupted) {
	if (interrupted && interrupted()) {
		throw ReservationInterrupted();
	}
	released_.wait_for(guard, std::chrono::milliseconds(100));
}

void ReservationGate::Release(uint64_t bytes, uint64_t duckdb_bytes) {
	// DuckDB's side first, so a waiter woken below finds the room there too (else, alone by then, it
	// would be refused). Never thrown past: HeldReservation releases from its destructor, and
	// FreeReservedMemory only adjusts a counter.
	try {
		if (duckdb_bytes) {
			release_(duckdb_bytes);
		}
	} catch (...) {
	}
	std::lock_guard<std::mutex> guard(lock_);
	releases_++;
	GiveBackLocked(bytes);
}

uint64_t ReservationGate::Held() const {
	std::lock_guard<std::mutex> guard(lock_);
	return held_;
}

uint64_t ReservationGate::Holders() const {
	std::lock_guard<std::mutex> guard(lock_);
	return holders_;
}

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb
