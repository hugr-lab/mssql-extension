//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// connection/mssql_login_retry.hpp
//
// Issue #481: a login Azure SQL refuses for a while (a serverless database resuming, a
// reconfiguration) is tried again, within the time the caller already allows for a login.
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/error_data.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/string_util.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>

namespace duckdb {
class ClientContext;

namespace mssql {

//! The server error numbers a login is retried on, and only these: 40613 a database not
//! available yet (serverless resuming), 40197 a service error while processing, 40143 a
//! service error asking for a retry, 40501 the service busy, 49918 not enough resources,
//! 42109 a Synapse / Fabric SQL pool warming up. All are in SqlClient's baseline transient set
//! (SqlConfigurableRetryFactory). Not its 42108, a PAUSED pool: nothing resumes it while a
//! login waits, so a retry would only spend the timeout. Never 18456 (credentials or state),
//! never a socket, TLS or DNS failure.
bool IsTransientLoginError(uint32_t number);

//! A refused login whose number IsTransientLoginError: what RetryTransientLogin tries again.
//! A ConnectionException, so every other catch sees the error it always saw.
class TransientLoginError : public ConnectionException {
public:
	TransientLoginError(uint32_t number_p, const std::string &message)
		: ConnectionException(message), number(number_p) {}
	uint32_t number;
};

//! A factory's refused login: TransientLoginError for the transient numbers, else
//! ConnectionException, with `message`.
[[noreturn]] void ThrowLoginRefused(uint32_t number, const std::string &message);

//! How the retry waits, so a test drives it with no clock and no server.
struct LoginRetryHooks {
	std::function<bool()> interrupted;
	std::function<void(int)> sleep_ms;
	std::function<int64_t()> now_ms;
	std::function<void(uint32_t number, int attempt, int wait_ms)> on_retry;
};

//! The hooks of a real login: the clock, a sleep, and the context of the statement waiting on
//! it (LoginContextScope) for Ctrl-C and the INFO line. With no context in scope -- a pool
//! prewarm thread -- nothing interrupts it, and nothing is logged.
LoginRetryHooks DefaultLoginRetryHooks();

//! The ClientContext of the statement whose connection is being made on this thread, for the
//! time of one acquisition: ConnectionProvider::GetConnection, the ATTACH (MSSQLAttach around
//! Initialize), every direct pool acquisition that has a context (metadata loads, catalog DDL,
//! CTAS, a deferred bulk-load adoption). An acquisition with none in scope -- the prewarm,
//! the schema lookup DuckDB makes with no context -- still retries, but Ctrl-C does not end
//! its wait and nothing is logged.
class LoginContextScope {
public:
	explicit LoginContextScope(optional_ptr<ClientContext> context);
	~LoginContextScope();
	static optional_ptr<ClientContext> Current();

private:
	optional_ptr<ClientContext> previous_;
};

//! A login made inside it is tried once: a best-effort acquisition (the planner's row count,
//! a failing CTAS's cleanup DROP) falls back at once, as it did before issue #481.
class LoginRetryOffScope {
public:
	LoginRetryOffScope();
	~LoginRetryOffScope();
	static bool Active();

private:
	bool previous_;
};

//! Run `attempt` until it returns, throws anything but a TransientLoginError, or `budget_ms` --
//! the caller's own login timeout -- would run out before the next try. The pause starts at 1 s
//! and doubles to 10 s, in 50 ms slices that ask `interrupted`. Out of budget, the last refusal
//! with the attempts and the time spent. The budget decides whether to START another attempt:
//! one started inside it takes as long as a login takes, so the whole can exceed it by one
//! login's time (each read of which is bounded by the same timeout).
template <class T>
T RetryTransientLogin(const std::function<T()> &attempt, int64_t budget_ms, const LoginRetryHooks &hooks) {
	constexpr int FIRST_WAIT_MS = 1000;
	constexpr int MAX_WAIT_MS = 10000;
	constexpr int SLICE_MS = 50;
	const int64_t start = hooks.now_ms();
	int wait_ms = FIRST_WAIT_MS;
	for (int attempts = 1;; attempts++) {
		try {
			return attempt();
		} catch (TransientLoginError &e) {
			const int64_t elapsed = hooks.now_ms() - start;
			if (elapsed + wait_ms > budget_ms) {
				throw ConnectionException(
					"%s (tried %d time(s) over %.1f s; the database stayed unavailable for the login "
					"timeout)",
					ErrorData(e).RawMessage(), attempts, static_cast<double>(elapsed) / 1000.0);
			}
			if (hooks.on_retry) {
				hooks.on_retry(e.number, attempts, wait_ms);
			}
			for (int waited = 0; waited < wait_ms; waited += SLICE_MS) {
				if (hooks.interrupted && hooks.interrupted()) {
					throw InterruptException();
				}
				hooks.sleep_ms(std::min(SLICE_MS, wait_ms - waited));
			}
			wait_ms = std::min(wait_ms * 2, MAX_WAIT_MS);
		}
	}
}

}  // namespace mssql
}  // namespace duckdb
