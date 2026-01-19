//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// tds_tls_context.cpp
//
// TLS wrapper implementation using the static TLS library (mssql_tls).
// This file wraps TlsImpl from the static library which contains mbedTLS.
//===----------------------------------------------------------------------===//

#include "tds/tls/tds_tls_context.hpp"
#include "tds/tls/tds_tls_impl.hpp"

#include <cstdio>
#include <cstdlib>

// Debug logging
static int GetMssqlDebugLevel() {
	static int level = -1;
	if (level == -1) {
		const char* env = std::getenv("MSSQL_DEBUG");
		level = env ? std::atoi(env) : 0;
	}
	return level;
}

#define MSSQL_TLS_DEBUG_LOG(lvl, fmt, ...) \
	do { \
		if (GetMssqlDebugLevel() >= lvl) \
			fprintf(stderr, "[MSSQL TLS STATIC] " fmt "\n", ##__VA_ARGS__); \
	} while (0)

namespace duckdb {
namespace tds {

const char* TlsErrorCodeToString(TlsErrorCode code) {
	switch (code) {
	case TlsErrorCode::NONE: return "No error";
	case TlsErrorCode::INIT_FAILED: return "TLS initialization failed";
	case TlsErrorCode::HANDSHAKE_FAILED: return "TLS handshake failed";
	case TlsErrorCode::HANDSHAKE_TIMEOUT: return "TLS handshake timed out";
	case TlsErrorCode::SEND_FAILED: return "TLS send failed";
	case TlsErrorCode::RECV_FAILED: return "TLS receive failed";
	case TlsErrorCode::NOT_INITIALIZED: return "TLS not initialized";
	case TlsErrorCode::PEER_CLOSED: return "Server closed TLS connection";
	case TlsErrorCode::SERVER_NO_ENCRYPT: return "Server does not support encryption";
	case TlsErrorCode::TLS_NOT_AVAILABLE: return "TLS not available";
	default: return "Unknown TLS error";
	}
}

// Implementation wraps TlsImpl from the static library
struct TlsTdsContextImpl {
	std::unique_ptr<TlsImpl> tls;
	std::string last_error;
	TlsErrorCode last_error_code = TlsErrorCode::NONE;

	TlsTdsContextImpl() : tls(new TlsImpl()) {}
};

TlsTdsContext::TlsTdsContext() : impl_(new TlsTdsContextImpl()) {}

TlsTdsContext::~TlsTdsContext() {
	if (impl_ && impl_->tls) {
		impl_->tls->Close();
	}
}

TlsTdsContext::TlsTdsContext(TlsTdsContext&& other) noexcept
    : impl_(std::move(other.impl_)) {
}

TlsTdsContext& TlsTdsContext::operator=(TlsTdsContext&& other) noexcept {
	if (this != &other) {
		if (impl_ && impl_->tls) {
			impl_->tls->Close();
		}
		impl_ = std::move(other.impl_);
	}
	return *this;
}

bool TlsTdsContext::Initialize() {
	MSSQL_TLS_DEBUG_LOG(1, "Initialize: starting");
	if (!impl_->tls) {
		impl_->last_error_code = TlsErrorCode::TLS_NOT_AVAILABLE;
		impl_->last_error = "TLS implementation not available";
		MSSQL_TLS_DEBUG_LOG(1, "Initialize: FAILED - TLS impl not available");
		return false;
	}

	if (impl_->tls->Initialize()) {
		MSSQL_TLS_DEBUG_LOG(1, "Initialize: SUCCESS");
		return true;
	}

	impl_->last_error_code = static_cast<TlsErrorCode>(impl_->tls->GetLastErrorCode());
	impl_->last_error = impl_->tls->GetLastError();
	MSSQL_TLS_DEBUG_LOG(1, "Initialize: FAILED - %s", impl_->last_error.c_str());
	return false;
}

bool TlsTdsContext::WrapSocket(int socket_fd, const std::string& hostname) {
	MSSQL_TLS_DEBUG_LOG(1, "WrapSocket: fd=%d, hostname=%s", socket_fd,
	                    hostname.empty() ? "(none)" : hostname.c_str());
	if (!impl_->tls) {
		MSSQL_TLS_DEBUG_LOG(1, "WrapSocket: FAILED - TLS impl not available");
		return false;
	}

	if (impl_->tls->WrapSocket(socket_fd, hostname)) {
		MSSQL_TLS_DEBUG_LOG(1, "WrapSocket: SUCCESS");
		return true;
	}

	impl_->last_error_code = static_cast<TlsErrorCode>(impl_->tls->GetLastErrorCode());
	impl_->last_error = impl_->tls->GetLastError();
	MSSQL_TLS_DEBUG_LOG(1, "WrapSocket: FAILED - %s", impl_->last_error.c_str());
	return false;
}

void TlsTdsContext::SetBioCallbacks(TlsSendCallback send_cb, TlsRecvCallback recv_cb) {
	if (impl_->tls) {
		impl_->tls->SetBioCallbacks(std::move(send_cb), std::move(recv_cb));
	}
}

void TlsTdsContext::ClearBioCallbacks() {
	if (impl_->tls) {
		impl_->tls->ClearBioCallbacks();
	}
}

bool TlsTdsContext::Handshake(int timeout_ms) {
	MSSQL_TLS_DEBUG_LOG(1, "Handshake: starting (timeout=%dms)", timeout_ms);
	if (!impl_->tls) {
		MSSQL_TLS_DEBUG_LOG(1, "Handshake: FAILED - TLS impl not available");
		return false;
	}

	if (impl_->tls->Handshake(timeout_ms)) {
		MSSQL_TLS_DEBUG_LOG(1, "Handshake: SUCCESS");
		return true;
	}

	impl_->last_error_code = static_cast<TlsErrorCode>(impl_->tls->GetLastErrorCode());
	impl_->last_error = impl_->tls->GetLastError();
	MSSQL_TLS_DEBUG_LOG(1, "Handshake: FAILED - %s", impl_->last_error.c_str());
	return false;
}

ssize_t TlsTdsContext::Send(const uint8_t* data, size_t length) {
	if (!impl_->tls) {
		return -1;
	}

	ssize_t result = impl_->tls->Send(data, length);
	if (result < 0) {
		impl_->last_error_code = static_cast<TlsErrorCode>(impl_->tls->GetLastErrorCode());
		impl_->last_error = impl_->tls->GetLastError();
	}
	return result;
}

ssize_t TlsTdsContext::Receive(uint8_t* buffer, size_t max_length, int timeout_ms) {
	if (!impl_->tls) {
		return -1;
	}

	ssize_t result = impl_->tls->Receive(buffer, max_length, timeout_ms);
	if (result <= 0) {
		// Copy error info for both errors (< 0) and peer closed (== 0)
		impl_->last_error_code = static_cast<TlsErrorCode>(impl_->tls->GetLastErrorCode());
		impl_->last_error = impl_->tls->GetLastError();
	}
	return result;
}

void TlsTdsContext::Close() {
	if (impl_->tls) {
		impl_->tls->Close();
	}
}

bool TlsTdsContext::IsInitialized() const {
	return impl_->tls && impl_->tls->IsInitialized();
}

const std::string& TlsTdsContext::GetLastError() const {
	return impl_->last_error;
}

TlsErrorCode TlsTdsContext::GetLastErrorCode() const {
	return impl_->last_error_code;
}

std::string TlsTdsContext::GetCipherSuite() const {
	return impl_->tls ? impl_->tls->GetCipherSuite() : "";
}

std::string TlsTdsContext::GetTlsVersion() const {
	return impl_->tls ? impl_->tls->GetTlsVersion() : "";
}

}  // namespace tds
}  // namespace duckdb
