//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// tds_tls_stub.cpp
//
// Stub TLS implementation for static builds. TLS is not available in the
// static extension due to mbedTLS symbol conflicts with DuckDB's bundled
// crypto library. Use the loadable extension for TLS support.
//===----------------------------------------------------------------------===//

#include "tds/tls/tds_tls_context.hpp"
#include "tds/tls/tds_tls_impl.hpp"

namespace duckdb {
namespace tds {

//===----------------------------------------------------------------------===//
// TlsErrorCodeToString
//===----------------------------------------------------------------------===//

const char *TlsErrorCodeToString(TlsErrorCode code) {
	switch (code) {
	case TlsErrorCode::NONE:
		return "No error";
	case TlsErrorCode::INIT_FAILED:
		return "TLS initialization failed";
	case TlsErrorCode::HANDSHAKE_FAILED:
		return "TLS handshake failed";
	case TlsErrorCode::HANDSHAKE_TIMEOUT:
		return "TLS handshake timeout";
	case TlsErrorCode::SEND_FAILED:
		return "TLS send failed";
	case TlsErrorCode::RECV_FAILED:
		return "TLS receive failed";
	case TlsErrorCode::NOT_INITIALIZED:
		return "TLS not initialized";
	case TlsErrorCode::PEER_CLOSED:
		return "Peer closed connection";
	case TlsErrorCode::SERVER_NO_ENCRYPT:
		return "Server does not support encryption";
	case TlsErrorCode::TLS_NOT_AVAILABLE:
		return "TLS not available in static build - use loadable extension";
	default:
		return "Unknown TLS error";
	}
}

//===----------------------------------------------------------------------===//
// TlsImpl Stub Implementation
//===----------------------------------------------------------------------===//

// Internal context structure - empty for stub
struct TlsImplContext {
	std::string last_error;
	int last_error_code;

	TlsImplContext() : last_error("TLS not available in static build"), last_error_code(-1) {}
};

TlsImpl::TlsImpl() : ctx_(std::make_unique<TlsImplContext>()) {}

TlsImpl::~TlsImpl() = default;

bool TlsImpl::Initialize() {
	ctx_->last_error =
		"TLS not available in static build - use the loadable extension (.duckdb_extension) for encrypted connections";
	return false;
}

bool TlsImpl::WrapSocket(int /*socket_fd*/, const std::string & /*hostname*/) {
	ctx_->last_error = "TLS not available in static build";
	return false;
}

void TlsImpl::SetBioCallbacks(TlsSendCallback /*send_cb*/, TlsRecvCallback /*recv_cb*/) {
	// No-op
}

void TlsImpl::ClearBioCallbacks() {
	// No-op
}

bool TlsImpl::Handshake(int /*timeout_ms*/) {
	ctx_->last_error = "TLS not available in static build";
	return false;
}

ssize_t TlsImpl::Send(const uint8_t * /*data*/, size_t /*length*/) {
	ctx_->last_error = "TLS not available in static build";
	return -1;
}

ssize_t TlsImpl::Receive(uint8_t * /*buffer*/, size_t /*max_length*/, int /*timeout_ms*/) {
	ctx_->last_error = "TLS not available in static build";
	return -1;
}

void TlsImpl::Close() {
	// No-op
}

bool TlsImpl::IsInitialized() const {
	return false;
}

const std::string &TlsImpl::GetLastError() const {
	return ctx_->last_error;
}

int TlsImpl::GetLastErrorCode() const {
	return ctx_->last_error_code;
}

std::string TlsImpl::GetCipherSuite() const {
	return "none";
}

std::string TlsImpl::GetTlsVersion() const {
	return "none";
}

//===----------------------------------------------------------------------===//
// TlsTdsContext Stub Implementation
//===----------------------------------------------------------------------===//

// Internal context structure - empty for stub
struct TlsTdsContextImpl {
	std::string last_error;
	TlsErrorCode last_error_code;

	TlsTdsContextImpl()
		: last_error("TLS not available in static build - use loadable extension"),
		  last_error_code(TlsErrorCode::TLS_NOT_AVAILABLE) {}
};

TlsTdsContext::TlsTdsContext() : impl_(std::make_unique<TlsTdsContextImpl>()) {}

TlsTdsContext::~TlsTdsContext() = default;

TlsTdsContext::TlsTdsContext(TlsTdsContext &&other) noexcept = default;

TlsTdsContext &TlsTdsContext::operator=(TlsTdsContext &&other) noexcept = default;

bool TlsTdsContext::Initialize() {
	impl_->last_error =
		"TLS not available in static build - use the loadable extension (.duckdb_extension) for encrypted connections";
	impl_->last_error_code = TlsErrorCode::TLS_NOT_AVAILABLE;
	return false;
}

bool TlsTdsContext::WrapSocket(int /*socket_fd*/, const std::string & /*hostname*/) {
	impl_->last_error = "TLS not available in static build";
	impl_->last_error_code = TlsErrorCode::TLS_NOT_AVAILABLE;
	return false;
}

void TlsTdsContext::SetBioCallbacks(TlsSendCallback /*send_cb*/, TlsRecvCallback /*recv_cb*/) {
	// No-op
}

void TlsTdsContext::ClearBioCallbacks() {
	// No-op
}

bool TlsTdsContext::Handshake(int /*timeout_ms*/) {
	impl_->last_error = "TLS not available in static build";
	impl_->last_error_code = TlsErrorCode::TLS_NOT_AVAILABLE;
	return false;
}

ssize_t TlsTdsContext::Send(const uint8_t * /*data*/, size_t /*length*/) {
	impl_->last_error = "TLS not available in static build";
	impl_->last_error_code = TlsErrorCode::TLS_NOT_AVAILABLE;
	return -1;
}

ssize_t TlsTdsContext::Receive(uint8_t * /*buffer*/, size_t /*max_length*/, int /*timeout_ms*/) {
	impl_->last_error = "TLS not available in static build";
	impl_->last_error_code = TlsErrorCode::TLS_NOT_AVAILABLE;
	return -1;
}

void TlsTdsContext::Close() {
	// No-op
}

bool TlsTdsContext::IsInitialized() const {
	return false;
}

const std::string &TlsTdsContext::GetLastError() const {
	return impl_->last_error;
}

TlsErrorCode TlsTdsContext::GetLastErrorCode() const {
	return impl_->last_error_code;
}

std::string TlsTdsContext::GetCipherSuite() const {
	return "none";
}

std::string TlsTdsContext::GetTlsVersion() const {
	return "none";
}

}  // namespace tds
}  // namespace duckdb
