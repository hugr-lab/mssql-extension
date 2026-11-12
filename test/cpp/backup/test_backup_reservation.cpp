// Spec 085 D17: ReservationGate, the reader's rule for its working memory. A reservation past the
// budget is refused at once; one that does not fit beside others waits for a release; one DuckDB
// cannot make room for waits while others hold, and is refused when alone; a wait asks to be
// interrupted. Eight threads under a budget of three reservations never hold more than three, all
// finish, and every byte DuckDB reserved is given back.

#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "backup/backup_reservation.hpp"

using namespace duckdb::mssql::backup;

namespace {

int failures = 0;

void Check(bool ok, const std::string &what) {
	std::cout << (ok ? "  ok   " : "  FAIL ") << what << std::endl;
	if (!ok) {
		failures++;
	}
}

//! DuckDB's side, faked: a pool of `room` bytes it can reserve from.
struct FakeDuckDB {
	std::atomic<int64_t> room;
	std::atomic<int64_t> reserved{0};
	explicit FakeDuckDB(int64_t r) : room(r) {}
	ReservationGate::ReserveFn Reserve() {
		return [this](uint64_t bytes, std::string &why) {
			if (room.fetch_sub(static_cast<int64_t>(bytes)) < static_cast<int64_t>(bytes)) {
				room += static_cast<int64_t>(bytes);
				why = "out of memory (fake)";
				return false;
			}
			reserved += static_cast<int64_t>(bytes);
			return true;
		};
	}
	ReservationGate::ReleaseFn Release() {
		return [this](uint64_t bytes) {
			room += static_cast<int64_t>(bytes);
			reserved -= static_cast<int64_t>(bytes);
		};
	}
};

}  // namespace

int main() {
	std::cout << "ReservationGate (spec 085 D17)" << std::endl;
	{
		FakeDuckDB duck(1000);
		ReservationGate gate(100, duck.Reserve(), duck.Release());
		bool refused = false;
		try {
			gate.Acquire(101, nullptr);
		} catch (ReservationRefused &e) {
			refused = e.past_budget;
		}
		Check(refused && gate.Held() == 0 && duck.reserved == 0, "past the budget: refused at once, nothing held");
	}
	{
		// Eight threads, each reserving 30 of a budget of 100: at most three at once, all finish.
		FakeDuckDB duck(1000);
		ReservationGate gate(100, duck.Reserve(), duck.Release());
		std::atomic<int> inside{0};
		std::atomic<int> most{0};
		std::vector<std::thread> tasks;
		for (int t = 0; t < 8; t++) {
			tasks.emplace_back([&]() {
				for (int i = 0; i < 50; i++) {
					HeldReservation held(gate, 30, nullptr);
					const int now = ++inside;
					int seen = most.load();
					while (now > seen && !most.compare_exchange_weak(seen, now)) {
					}
					std::this_thread::sleep_for(std::chrono::microseconds(200));
					--inside;
				}
			});
		}
		for (auto &t : tasks) {
			t.join();
		}
		Check(most <= 3 && gate.Held() == 0 && gate.Holders() == 0 && duck.reserved == 0,
			  "eight tasks under a budget of three: at most " + std::to_string(most.load()) +
				  " held at once, all released");
	}
	{
		// DuckDB short of room while another holds: the second waits for the first's release, then fits.
		FakeDuckDB duck(50);
		ReservationGate gate(100, duck.Reserve(), duck.Release());
		gate.Acquire(40, nullptr);
		std::atomic<bool> second{false};
		std::thread waiter([&]() {
			gate.Acquire(40, nullptr);
			second = true;
			gate.Release(40);
		});
		std::this_thread::sleep_for(std::chrono::milliseconds(150));
		const bool waited = !second;
		gate.Release(40);
		waiter.join();
		Check(waited && second && duck.reserved == 0,
			  "DuckDB without room beside another holder: waits for its release, then holds");
	}
	{
		// DuckDB short of room and nothing else held: refused, with DuckDB's reason.
		FakeDuckDB duck(10);
		ReservationGate gate(100, duck.Reserve(), duck.Release());
		std::string why;
		bool alone = false;
		try {
			gate.Acquire(40, nullptr);
		} catch (ReservationRefused &e) {
			alone = !e.past_budget;
			why = e.duckdb_reason;
		}
		Check(alone && why == "out of memory (fake)" && gate.Held() == 0 && gate.Holders() == 0,
			  "DuckDB without room and nothing else held: refused, naming DuckDB's reason");
	}
	{
		// A wait asked to stop: interrupted, nothing held by the waiter.
		FakeDuckDB duck(1000);
		ReservationGate gate(100, duck.Reserve(), duck.Release());
		gate.Acquire(80, nullptr);
		std::atomic<bool> stop{false};
		bool interrupted = false;
		std::thread waiter([&]() {
			try {
				gate.Acquire(80, [&]() { return stop.load(); });
			} catch (ReservationInterrupted &) {
				interrupted = true;
			}
		});
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		stop = true;
		waiter.join();
		Check(interrupted && gate.Held() == 80 && gate.Holders() == 1, "a wait interrupted: the waiter holds nothing");
		gate.Release(80);
	}
	{
		// DuckDB's side throwing (a spill that found the disk full): the attempt given back, the error
		// goes up, and a waiter on the budget is woken and holds.
		FakeDuckDB duck(1000);
		bool fail_next = false;
		ReservationGate gate(
			100,
			[&](uint64_t bytes, std::string &why) {
				if (fail_next) {
					fail_next = false;
					throw std::runtime_error("disk full");
				}
				return duck.Reserve()(bytes, why);
			},
			duck.Release());
		gate.Acquire(60, nullptr);
		std::atomic<bool> second{false};
		std::thread waiter([&]() {
			HeldReservation held(gate, 60, nullptr);
			second = true;
		});
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		bool thrown = false;
		fail_next = true;
		try {
			gate.Acquire(30, nullptr);
		} catch (std::runtime_error &e) {
			thrown = std::string(e.what()) == "disk full";
		}
		const bool counters = gate.Holders() == 1 && gate.Held() == 60;
		gate.Release(60);
		waiter.join();
		Check(thrown && counters && second && gate.Holders() == 0 && duck.reserved == 0,
			  "DuckDB's side throwing: the attempt given back, the error raised, the waiter served");
	}
	{
		// Two waiters DuckDB refuses beside a holder do not wake each other: each tries again only
		// after a release (a retry evicts DuckDB's blocks).
		FakeDuckDB duck(45);
		std::atomic<int> attempts{0};
		ReservationGate gate(
			100,
			[&](uint64_t bytes, std::string &why) {
				attempts++;
				return duck.Reserve()(bytes, why);
			},
			duck.Release());
		gate.Acquire(40, nullptr);
		std::vector<std::thread> waiters;
		for (int i = 0; i < 2; i++) {
			waiters.emplace_back([&]() { HeldReservation held(gate, 20, nullptr); });
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(300));
		const int before = attempts.load();
		gate.Release(40);
		for (auto &w : waiters) {
			w.join();
		}
		Check(before <= 3 && gate.Holders() == 0 && duck.reserved == 0,
			  "waiters DuckDB refused retry on releases only (" + std::to_string(before) +
				  " attempts while one held, 3 at most), then hold");
	}
	std::cout << (failures ? "FAILED: " + std::to_string(failures) + " check(s)" : "all checks passed") << std::endl;
	return failures ? 1 : 0;
}
