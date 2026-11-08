#include "connection/mssql_login_retry.hpp"

#include "duckdb/logging/logger.hpp"
#include "duckdb/main/client_context.hpp"

namespace duckdb {
namespace mssql {

bool IsTransientLoginError(uint32_t number) {
	switch (number) {
	case 40613:
	case 40197:
	case 40501:
	case 49918:
		return true;
	default:
		return false;
	}
}

void ThrowLoginRefused(uint32_t number, const std::string &message) {
	if (IsTransientLoginError(number)) {
		throw TransientLoginError(number, message);
	}
	throw ConnectionException(message);
}

static thread_local ClientContext *login_context = nullptr;

LoginContextScope::LoginContextScope(optional_ptr<ClientContext> context) : previous_(login_context) {
	login_context = context.get();
}

LoginContextScope::~LoginContextScope() {
	login_context = previous_.get();
}

optional_ptr<ClientContext> LoginContextScope::Current() {
	return login_context;
}

static thread_local bool login_retry_off = false;

LoginRetryOffScope::LoginRetryOffScope() : previous_(login_retry_off) {
	login_retry_off = true;
}

LoginRetryOffScope::~LoginRetryOffScope() {
	login_retry_off = previous_;
}

bool LoginRetryOffScope::Active() {
	return login_retry_off;
}

LoginRetryHooks DefaultLoginRetryHooks() {
	LoginRetryHooks hooks;
	auto context = LoginContextScope::Current();
	hooks.interrupted = [context]() { return context && context->IsInterrupted(); };
	hooks.sleep_ms = [](int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); };
	hooks.now_ms = []() {
		return std::chrono::duration_cast<std::chrono::milliseconds>(
				   std::chrono::steady_clock::now().time_since_epoch())
			.count();
	};
	hooks.on_retry = [context](uint32_t number, int attempt, int wait_ms) {
		if (context) {
			DUCKDB_LOG_INFO(*context,
							"mssql: login refused with server error %u (the database is not available yet), "
							"attempt %d; trying again in %d ms",
							number, attempt, wait_ms);
		}
	};
	return hooks;
}

}  // namespace mssql
}  // namespace duckdb
