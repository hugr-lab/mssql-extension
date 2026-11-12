#include "backup/backup_reservation.hpp"

#include <chrono>

namespace duckdb {
namespace mssql {
namespace backup {

void ReservationGate::Acquire(uint64_t bytes, const std::function<bool()> &interrupted) {
	if (bytes > budget_) {
		// No release can make room: refused at once, whatever else is held.
		throw ReservationRefused(true, "");
	}
	std::unique_lock<std::mutex> guard(lock_);
	for (;;) {
		if (held_ + bytes <= budget_) {
			held_ += bytes;
			holders_++;
			guard.unlock();
			std::string why;
			bool reserved = false;
			try {
				// DuckDB's reservation outside the lock: it may evict blocks to make room, or spill them.
				reserved = reserve_(bytes, why);
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
			const uint64_t seen = releases_;
			while (releases_ == seen) {
				WaitLocked(guard, interrupted);
			}
			continue;
		}
		// Past the budget beside the others (holders_ > 0 here: with none held, held_ is 0 and `bytes`
		// fits it): wait for any memory given back.
		const uint64_t seen = generation_;
		while (generation_ == seen) {
			WaitLocked(guard, interrupted);
		}
	}
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

void ReservationGate::Release(uint64_t bytes) {
	// DuckDB's side first, so a waiter woken below finds the room there too (else, alone by then, it
	// would be refused). Never thrown past: HeldReservation releases from its destructor, and
	// FreeReservedMemory only adjusts a counter.
	try {
		release_(bytes);
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
