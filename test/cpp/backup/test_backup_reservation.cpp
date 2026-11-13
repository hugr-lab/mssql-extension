// Spec 085 D17: ReservationGate, the reader's rule for its working memory. A reservation past the
// budget is refused at once; one that does not fit beside others waits for a release; one DuckDB
// cannot make room for waits while others hold, and is refused when alone; a wait asks to be
// interrupted; two refused at once are both refused, neither waiting for a release that nothing can
// make. Eight threads under a budget of three reservations never hold more than three, all
// finish, and every byte DuckDB reserved is given back.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <iostream>
#include <mutex>
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
	std::atomic<int64_t> attempts{0};
	explicit FakeDuckDB(int64_t r) : room(r) {}
	ReservationGate::ReserveFn Reserve() {
		return [this](uint64_t bytes, std::string &why) {
			attempts++;
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
		// Two tasks DuckDB refuses with nothing held: each is refused by name, and neither waits for
		// a release that nothing can make (review of #553). The first to give back used to see the
		// other's in-flight attempt in `holders_` and park on `releases_` for ever, while DuckDB
		// waited for that task to end -- so the whole query hung instead of reporting the refusal.
		// Both attempts must be in flight at once for the first to see the other in `holders_`, so
		// DuckDB's side here holds each caller until the other has entered it, then refuses both.
		// `interrupted` is the test's own deadline, so a regression fails rather than hanging CI;
		// `duck.reserved` stays 0 because nothing is ever reserved, which a stray release would break.
		FakeDuckDB duck(10);
		std::mutex both_in;
		std::condition_variable entered;
		int inside_reserve = 0;
		ReservationGate gate(
			100,
			[&](uint64_t, std::string &why) {
				std::unique_lock<std::mutex> meet(both_in);
				inside_reserve++;
				entered.notify_all();
				entered.wait_for(meet, std::chrono::seconds(2), [&]() { return inside_reserve >= 2; });
				why = "out of memory (fake)";
				return false;
			},
			duck.Release());
		std::atomic<int> refused{0};
		std::atomic<int> stuck{0};
		std::atomic<int> done{0};
		std::atomic<bool> too_slow{false};
		std::vector<std::thread> tasks;
		for (int t = 0; t < 2; t++) {
			tasks.emplace_back([&]() {
				try {
					HeldReservation held(gate, 40, [&]() { return too_slow.load(); });
				} catch (ReservationRefused &e) {
					if (!e.past_budget) {
						refused++;
					}
				} catch (ReservationInterrupted &) {
					stuck++;
				}
				done++;
			});
		}
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (done < 2 && std::chrono::steady_clock::now() < deadline) {
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		too_slow = true;  // frees a task the fix would have freed, so the check below fails instead
		for (auto &t : tasks) {
			t.join();
		}
		Check(refused == 2 && stuck == 0 && gate.Held() == 0 && gate.Holders() == 0 && duck.reserved == 0,
			  "two tasks refused with nothing held: both refused by name, neither waits for a release "
			  "that cannot come");
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
	std::cout << "TryAcquire and DuckDB's share (PR 3 S4)" << std::endl;
	{
		FakeDuckDB duck(1000);
		ReservationGate gate(100, duck.Reserve(), duck.Release());
		Check(gate.TryAcquire(60, 20) && gate.Held() == 60 && duck.reserved == 20,
			  "60 held against the budget, only 20 of them reserved with DuckDB");
		Check(!gate.TryAcquire(50, 50) && gate.Held() == 60 && duck.reserved == 20,
			  "50 more do not fit the budget beside 60: refused at once, nothing taken");
		bool refused = false;
		try {
			gate.TryAcquire(101, 0);
		} catch (ReservationRefused &e) {
			refused = e.past_budget;
		}
		Check(refused, "past the budget alone: ReservationRefused, as Acquire");
		gate.Release(60, 20);
		Check(gate.Held() == 0 && duck.reserved == 0, "released with its share");
		FakeDuckDB tight(10);
		ReservationGate small(100, tight.Reserve(), tight.Release());
		Check(!small.TryAcquire(50, 50) && small.Held() == 0 && small.Holders() == 0,
			  "DuckDB cannot make room: false, the attempt given back");
		Check(small.TryAcquire(50, 5) && tight.reserved == 5, "a share DuckDB can reserve: held");
		{
			CallReservations call(small, std::function<bool()>());
			call.Acquire(30, 0);
			Check(small.Held() == 80 && call.TryAcquire(10, 0) && small.Held() == 90, "a call's reservations add up");
			call.ReleaseAll();
			Check(small.Held() == 50, "... and go back together");
			call.Acquire(20, 1);
		}
		Check(small.Held() == 50 && tight.reserved == 5, "the destructor gives back what is left");
		small.Release(50, 5);
	}

	std::cout << "A call's rows share a grant's credit (PR 3 S7)" << std::endl;
	{
		FakeDuckDB duck(1ull << 40);
		ReservationGate gate(64ull << 20, duck.Reserve(), duck.Release());	// a sixteenth: 4 MiB, CREDIT_BYTES caps
		const uint64_t credit = CallReservations::CREDIT_BYTES;
		{
			CallReservations call(gate, std::function<bool()>());
			call.Acquire(1000, 1000);  // the chunk's staging
			bool all = true;
			for (int i = 0; i < 100; i++) {
				all = all && call.TryAcquire(10000, 4000);
			}
			Check(all && gate.Holders() == 2 && gate.Held() == 1000 + credit &&
					  duck.reserved == static_cast<int64_t>(1000 + 4000 + (credit - 10000) * 4000 / 10000),
				  "100 rows of 10 KB (4 KB of it DuckDB's): one grant of 1 MiB, its credit the rows after the first, "
				  "DuckDB's share of it the rows' own");
			Check(call.TryAcquire(credit, credit) && gate.Holders() == 3 && gate.Held() == 1000 + 2 * credit,
				  "a row the credit does not cover, as large as a grant: taken alone");
			call.ReleaseAll();
			Check(gate.Held() == 0 && gate.Holders() == 0 && duck.reserved == 0, "... and every grant given back");
			Check(call.TryAcquire(10, 10) && gate.Held() == credit, "the credit went with them: a new grant");
		}
		Check(gate.Held() == 0 && duck.reserved == 0, "the destructor gives the grant back");
		FakeDuckDB tight(50000);
		ReservationGate exact(64ull << 20, tight.Reserve(), tight.Release());
		{
			CallReservations call(exact, std::function<bool()>());
			Check(call.TryAcquire(10000, 10000) && exact.Held() == 10000 && tight.reserved == 10000,
				  "DuckDB cannot reserve a grant's credit: the row is asked for alone");
		}
		{
			CallReservations call(exact, std::function<bool()>());
			const auto before = tight.attempts.load();
			Check(call.TryAcquire(10000, 10000) && call.TryAcquire(10000, 10000) && exact.Holders() == 2 &&
					  tight.attempts == before + 3,
				  "a grant DuckDB refused is not asked again in the call: one refused grant, then each row alone");
		}
		ReservationGate shared(32ull << 20, duck.Reserve(), duck.Release());
		{
			CallReservations call(shared, std::function<bool()>());
			call.Acquire(30ull << 20, 30ull << 20);
			Check(call.TryAcquire(10000, 10000) && shared.Held() == (30ull << 20) + credit, "a grant beside a chunk");
			std::atomic<bool> in{false};
			std::thread waiter([&]() {
				shared.Acquire(2ull << 20, std::function<bool()>());
				in = true;
			});
			for (int i = 0; i < 400 && !shared.HasWaiters(); i++) {
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
			}
			Check(shared.HasWaiters() && !call.TryAcquire(10000, 10000),
				  "while an Acquire waits for the budget, the credit serves no row: the chunk closes");
			call.ReleaseAll();
			waiter.join();
			Check(in && shared.Held() == (2ull << 20), "... and the waiter takes what the call gave back");
			shared.Release(2ull << 20);
		}
		ReservationGate tiny(160, duck.Reserve(), duck.Release());
		{
			CallReservations call(tiny, std::function<bool()>());
			Check(call.TryAcquire(5, 5) && tiny.Held() == 10 && call.TryAcquire(5, 5) && tiny.Held() == 10,
				  "a grant is a sixteenth of the budget at most (160: 10)");
		}
	}

	std::cout << "A waiter for the budget goes first (PR 3 S4)" << std::endl;
	{
		FakeDuckDB duck(1000);
		ReservationGate gate(100, duck.Reserve(), duck.Release());
		gate.Acquire(60, std::function<bool()>());
		std::atomic<bool> large_in{false};
		std::thread large([&]() {
			gate.Acquire(80, std::function<bool()>());
			large_in = true;
		});
		// Give it time to start waiting (60 + 80 > 100).
		for (int i = 0; i < 200 && gate.Holders() == 1 && !large_in; i++) {
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		const bool small_refused = !gate.TryAcquire(10, 10);
		Check(small_refused && !large_in, "while 80 wait, 10 more are not taken, though they would fit");
		gate.Release(60);
		large.join();
		Check(large_in && gate.Held() == 80, "the waiter took the release");
		Check(gate.TryAcquire(10, 10) && gate.Held() == 90, "... and with none waiting, 10 more fit again");
		gate.Release(10, 10);
		gate.Release(80);
	}

	std::cout << (failures ? "FAILED: " + std::to_string(failures) + " check(s)" : "all checks passed") << std::endl;
	return failures ? 1 : 0;
}
