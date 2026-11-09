// Issue #481: RetryTransientLogin, driven by a fake clock with no server.
//
// A login Azure SQL refuses with 40613 / 40197 / 40501 / 49918 is tried again, the pause 1 s
// doubling to 10 s, until the caller's login timeout would run out before the next try; any
// other refusal (18456) fails at once; Ctrl-C ends the wait.

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "connection/mssql_login_retry.hpp"

using namespace duckdb;
using namespace duckdb::mssql;

namespace {

int failures = 0;

void Check(bool ok, const std::string &what) {
	std::cout << (ok ? "  ok   " : "  FAIL ") << what << std::endl;
	if (!ok) {
		failures++;
	}
}

struct FakeClock {
	int64_t now = 0;
	std::vector<int> waits;	 // one entry per on_retry
	int slept = 0;
	bool interrupt_after_first_slice = false;
	int slices = 0;
	LoginRetryHooks Hooks() {
		LoginRetryHooks hooks;
		hooks.now_ms = [this]() { return now; };
		hooks.sleep_ms = [this](int ms) {
			now += ms;
			slept += ms;
			slices++;
		};
		hooks.interrupted = [this]() { return interrupt_after_first_slice && slices > 0; };
		hooks.on_retry = [this](uint32_t, int, int wait_ms) { waits.push_back(wait_ms); };
		return hooks;
	}
};

}  // namespace

int main() {
	std::cout << "RetryTransientLogin (issue #481)" << std::endl;

	Check(IsTransientLoginError(40613) && IsTransientLoginError(40197) && IsTransientLoginError(40501) &&
			  IsTransientLoginError(49918),
		  "the transient numbers");
	Check(!IsTransientLoginError(18456) && !IsTransientLoginError(0) && !IsTransientLoginError(4060),
		  "not a login failure, a socket error (0) or a missing database");

	{
		FakeClock clock;
		int attempts = 0;
		std::function<int()> attempt = [&]() -> int {
			attempts++;
			if (attempts < 3) {
				ThrowLoginRefused(40613, "Database 'db' is not currently available");
			}
			return 42;
		};
		const int got = RetryTransientLogin<int>(attempt, 30000, clock.Hooks());
		Check(got == 42 && attempts == 3, "two refusals while resuming, then the login");
		Check(clock.waits.size() == 2 && clock.waits[0] == 1000 && clock.waits[1] == 2000, "pauses of 1 s, then 2 s");
		Check(clock.slept == 3000, "slept 3 s in all");
	}

	{
		FakeClock clock;
		int attempts = 0;
		std::function<int()> attempt = [&]() -> int {
			attempts++;
			ThrowLoginRefused(18456, "Login failed for user 'sa'.");
		};
		bool connection_error = false;
		try {
			RetryTransientLogin<int>(attempt, 30000, clock.Hooks());
		} catch (TransientLoginError &) {
		} catch (ConnectionException &) {
			connection_error = true;
		}
		Check(connection_error && attempts == 1 && clock.slept == 0, "18456 fails at once, not retried");
	}

	{
		FakeClock clock;
		int attempts = 0;
		std::function<int()> attempt = [&]() -> int {
			attempts++;
			ThrowLoginRefused(40613, "Database 'db' is not currently available");
		};
		std::string message;
		try {
			RetryTransientLogin<int>(attempt, 5000, clock.Hooks());
		} catch (ConnectionException &e) {
			message = ErrorData(e).RawMessage();
		}
		// t = 0 fails, waits 1 s; t = 1 s fails, waits 2 s; t = 3 s fails, and 3 + 4 s is past 5 s.
		Check(attempts == 3, "within a 5 s budget: three attempts");
		Check(message.find("not currently available") != std::string::npos &&
				  message.find("tried 3 time(s) over 3.0 s") != std::string::npos,
			  "out of budget: the last refusal, the attempts and the time");
	}

	{
		FakeClock clock;
		clock.interrupt_after_first_slice = true;
		int attempts = 0;
		std::function<int()> attempt = [&]() -> int {
			attempts++;
			ThrowLoginRefused(40613, "Database 'db' is not currently available");
		};
		bool interrupted = false;
		try {
			RetryTransientLogin<int>(attempt, 30000, clock.Hooks());
		} catch (InterruptException &) {
			interrupted = true;
		}
		Check(interrupted && attempts == 1 && clock.slept == 50, "Ctrl-C ends the pause within one 50 ms slice");
	}

	std::cout << (failures ? "FAILED: " + std::to_string(failures) + " check(s)" : "all checks passed") << std::endl;
	return failures ? 1 : 0;
}
