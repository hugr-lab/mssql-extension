#include "tds/tds_connection.hpp"
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

#define MSSQL_CONN_DEBUG_LOG(lvl, fmt, ...) \
	do { \
		if (GetMssqlDebugLevel() >= lvl) \
			fprintf(stderr, "[MSSQL CONN] " fmt "\n", ##__VA_ARGS__); \
	} while (0)

namespace duckdb {
namespace tds {

TdsConnection::TdsConnection()
    : socket_(new TdsSocket()),
      state_(ConnectionState::Disconnected),
      port_(0),
      spid_(0),
      created_at_(std::chrono::steady_clock::now()),
      last_used_at_(created_at_),
      tls_enabled_(false),
      next_packet_id_(1) {
}

TdsConnection::~TdsConnection() {
	Close();
}

TdsConnection::TdsConnection(TdsConnection&& other) noexcept
    : socket_(std::move(other.socket_)),
      state_(other.state_.load()),
      host_(std::move(other.host_)),
      port_(other.port_),
      database_(std::move(other.database_)),
      spid_(other.spid_),
      created_at_(other.created_at_),
      last_used_at_(other.last_used_at_),
      last_error_(std::move(other.last_error_)),
      tls_enabled_(other.tls_enabled_),
      next_packet_id_(other.next_packet_id_) {
	other.state_.store(ConnectionState::Disconnected);
	other.spid_ = 0;
	other.tls_enabled_ = false;
}

TdsConnection& TdsConnection::operator=(TdsConnection&& other) noexcept {
	if (this != &other) {
		Close();
		socket_ = std::move(other.socket_);
		state_.store(other.state_.load());
		host_ = std::move(other.host_);
		port_ = other.port_;
		database_ = std::move(other.database_);
		spid_ = other.spid_;
		created_at_ = other.created_at_;
		last_used_at_ = other.last_used_at_;
		last_error_ = std::move(other.last_error_);
		tls_enabled_ = other.tls_enabled_;
		next_packet_id_ = other.next_packet_id_;

		other.state_.store(ConnectionState::Disconnected);
		other.spid_ = 0;
		other.tls_enabled_ = false;
	}
	return *this;
}

bool TdsConnection::Connect(const std::string& host, uint16_t port, int timeout_seconds) {
	// Can only connect from Disconnected state
	ConnectionState expected = ConnectionState::Disconnected;
	if (!state_.compare_exchange_strong(expected, ConnectionState::Authenticating)) {
		last_error_ = "Invalid state for Connect: " + std::string(ConnectionStateToString(expected));
		return false;
	}

	host_ = host;
	port_ = port;
	last_error_.clear();

	if (!socket_->Connect(host, port, timeout_seconds)) {
		last_error_ = socket_->GetLastError();
		state_.store(ConnectionState::Disconnected);
		return false;
	}

	// Stay in Authenticating state - caller must call Authenticate
	return true;
}

bool TdsConnection::Authenticate(const std::string& username, const std::string& password, const std::string& database,
                                  bool use_encrypt) {
	// Must be in Authenticating state
	if (state_.load() != ConnectionState::Authenticating) {
		last_error_ = "Cannot authenticate: not in Authenticating state";
		return false;
	}

	// Step 1: PRELOGIN handshake (negotiates encryption)
	if (!DoPrelogin(use_encrypt)) {
		state_.store(ConnectionState::Disconnected);
		socket_->Close();
		return false;
	}

	// Step 2: LOGIN7 authentication
	// Note: If TLS was enabled during PRELOGIN, all subsequent traffic is encrypted
	if (!DoLogin7(username, password, database)) {
		state_.store(ConnectionState::Disconnected);
		socket_->Close();
		return false;
	}

	// Success - transition to Idle
	database_ = database;
	state_.store(ConnectionState::Idle);
	UpdateLastUsed();
	return true;
}

bool TdsConnection::DoPrelogin(bool use_encrypt) {
	TdsPacket prelogin = TdsProtocol::BuildPrelogin(use_encrypt);
	prelogin.SetPacketId(next_packet_id_++);

	if (!socket_->SendPacket(prelogin)) {
		last_error_ = "Failed to send PRELOGIN: " + socket_->GetLastError();
		return false;
	}

	std::vector<uint8_t> response;
	if (!socket_->ReceiveMessage(response, DEFAULT_CONNECTION_TIMEOUT * 1000)) {
		last_error_ = "Failed to receive PRELOGIN response: " + socket_->GetLastError();
		return false;
	}

	PreloginResponse prelogin_response = TdsProtocol::ParsePreloginResponse(response);
	if (!prelogin_response.success) {
		last_error_ = "PRELOGIN failed: " + prelogin_response.error_message;
		return false;
	}

	// Log the PRELOGIN response
	MSSQL_CONN_DEBUG_LOG(1, "DoPrelogin: server version=%d.%d.%d, encryption=%d",
		prelogin_response.version_major, prelogin_response.version_minor,
		prelogin_response.version_build, static_cast<int>(prelogin_response.encryption));

	// Handle encryption based on what we requested and what server responded
	if (use_encrypt) {
		// We requested encryption
		if (prelogin_response.encryption == EncryptionOption::ENCRYPT_NOT_SUP) {
			last_error_ = "TLS requested but server does not support encryption (ENCRYPT_NOT_SUP)";
			return false;
		}
		if (prelogin_response.encryption == EncryptionOption::ENCRYPT_OFF) {
			last_error_ = "TLS requested but server declined encryption (ENCRYPT_OFF)";
			return false;
		}

		// Server agreed to encryption (ENCRYPT_ON or ENCRYPT_REQ)
		// Enable TLS on the socket BEFORE sending LOGIN7
		// Pass next_packet_id_ so TLS handshake packets continue the sequence
		if (!socket_->EnableTls(next_packet_id_, DEFAULT_CONNECTION_TIMEOUT * 1000)) {
			last_error_ = "TLS handshake failed: " + socket_->GetLastError();
			return false;
		}
		tls_enabled_ = true;
	} else {
		// We did not request encryption
		// Accept if server doesn't require it
		if (prelogin_response.encryption == EncryptionOption::ENCRYPT_REQ) {
			last_error_ = "Server requires encryption (ENCRYPT_REQ) but use_encrypt=false";
			return false;
		}
		// ENCRYPT_OFF, ENCRYPT_NOT_SUP, or ENCRYPT_ON with client not supporting = no TLS
		tls_enabled_ = false;
	}

	return true;
}

bool TdsConnection::DoLogin7(const std::string& username, const std::string& password, const std::string& database) {
	TdsPacket login = TdsProtocol::BuildLogin7(host_, username, password, database);
	login.SetPacketId(next_packet_id_++);

	if (!socket_->SendPacket(login)) {
		last_error_ = "Failed to send LOGIN7: " + socket_->GetLastError();
		return false;
	}

	std::vector<uint8_t> response;
	if (!socket_->ReceiveMessage(response, DEFAULT_CONNECTION_TIMEOUT * 1000)) {
		last_error_ = "Failed to receive LOGIN7 response: " + socket_->GetLastError();
		return false;
	}

	LoginResponse login_response = TdsProtocol::ParseLoginResponse(response);
	if (!login_response.success) {
		if (login_response.error_number > 0) {
			last_error_ = "Authentication failed (error " + std::to_string(login_response.error_number) + "): " +
			              login_response.error_message;
		} else {
			last_error_ = "Authentication failed: " + login_response.error_message;
		}
		return false;
	}

	spid_ = login_response.spid;
	return true;
}

bool TdsConnection::IsAlive() const {
	ConnectionState current = state_.load(std::memory_order_acquire);
	return current != ConnectionState::Disconnected && socket_ && socket_->IsConnected();
}

bool TdsConnection::Ping(int timeout_ms) {
	// Can only ping from Idle state
	ConnectionState expected = ConnectionState::Idle;
	if (!state_.compare_exchange_strong(expected, ConnectionState::Executing)) {
		return state_.load() == ConnectionState::Executing;  // Already executing is "alive"
	}

	TdsPacket ping = TdsProtocol::BuildPing();
	ping.SetPacketId(next_packet_id_++);

	bool success = false;
	if (socket_->SendPacket(ping)) {
		std::vector<uint8_t> response;
		if (socket_->ReceiveMessage(response, timeout_ms)) {
			success = TdsProtocol::IsSuccessResponse(response);
		}
	}

	if (success) {
		state_.store(ConnectionState::Idle);
		UpdateLastUsed();
	} else {
		last_error_ = "Ping failed: " + socket_->GetLastError();
		state_.store(ConnectionState::Disconnected);
	}

	return success;
}

bool TdsConnection::ValidateWithPing() {
	// Quick check first
	if (!IsAlive()) {
		return false;
	}

	// Full ping validation
	return Ping();
}

void TdsConnection::Close() {
	ConnectionState current = state_.load();
	if (current == ConnectionState::Disconnected) {
		return;
	}

	// If executing, try to cancel first
	if (current == ConnectionState::Executing) {
		SendAttention();
		WaitForAttentionAck(CANCELLATION_TIMEOUT * 1000);
	}

	if (socket_) {
		socket_->Close();
	}

	state_.store(ConnectionState::Disconnected);
	spid_ = 0;
}

bool TdsConnection::SendAttention() {
	ConnectionState expected = ConnectionState::Executing;
	if (!state_.compare_exchange_strong(expected, ConnectionState::Cancelling)) {
		return false;
	}

	TdsPacket attention = TdsProtocol::BuildAttention();
	attention.SetPacketId(next_packet_id_++);

	if (!socket_->SendPacket(attention)) {
		last_error_ = "Failed to send ATTENTION: " + socket_->GetLastError();
		state_.store(ConnectionState::Disconnected);
		return false;
	}

	return true;
}

bool TdsConnection::WaitForAttentionAck(int timeout_ms) {
	if (state_.load() != ConnectionState::Cancelling) {
		return false;
	}

	std::vector<uint8_t> response;
	auto start = std::chrono::steady_clock::now();

	while (true) {
		auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
		    std::chrono::steady_clock::now() - start).count();

		if (elapsed >= timeout_ms) {
			// Timeout - force disconnect
			last_error_ = "ATTENTION acknowledgment timeout";
			state_.store(ConnectionState::Disconnected);
			return false;
		}

		int remaining = timeout_ms - static_cast<int>(elapsed);
		if (socket_->ReceiveMessage(response, remaining)) {
			if (TdsProtocol::ParseDoneForAttentionAck(response)) {
				state_.store(ConnectionState::Idle);
				UpdateLastUsed();
				return true;
			}
			response.clear();
		} else {
			// Error receiving
			state_.store(ConnectionState::Disconnected);
			return false;
		}
	}
}

bool TdsConnection::TransitionState(ConnectionState from, ConnectionState to) {
	return state_.compare_exchange_strong(from, to);
}

bool TdsConnection::IsLongIdle() const {
	auto now = std::chrono::steady_clock::now();
	auto idle_duration = std::chrono::duration_cast<std::chrono::seconds>(now - last_used_at_).count();
	return idle_duration > LONG_IDLE_THRESHOLD;
}

bool TdsConnection::ExecuteBatch(const std::string& sql) {
	// Can only execute from Idle state
	ConnectionState expected = ConnectionState::Idle;
	if (!state_.compare_exchange_strong(expected, ConnectionState::Executing)) {
		last_error_ = "Cannot execute: connection not in Idle state (current: " +
		              std::string(ConnectionStateToString(expected)) + ")";
		return false;
	}

	// Build SQL_BATCH packet(s)
	std::vector<TdsPacket> packets = TdsProtocol::BuildSqlBatchMultiPacket(sql);

	// Send all packets
	for (size_t i = 0; i < packets.size(); i++) {
		auto& packet = packets[i];
		packet.SetPacketId(next_packet_id_++);
		if (!socket_->SendPacket(packet)) {
			last_error_ = "Failed to send SQL_BATCH: " + socket_->GetLastError();
			state_.store(ConnectionState::Disconnected);
			return false;
		}
	}

	// Connection is now in Executing state, ready to receive response
	return true;
}

ssize_t TdsConnection::ReceiveData(uint8_t* buffer, size_t buffer_size, int timeout_ms) {
	ConnectionState current = state_.load();

	// Can receive data in Executing or Cancelling states
	if (current != ConnectionState::Executing && current != ConnectionState::Cancelling) {
		last_error_ = "Cannot receive: connection not in Executing or Cancelling state";
		return -1;
	}

	if (!socket_) {
		last_error_ = "Socket is null";
		return -1;
	}

	// Use the socket's receive method with timeout
	ssize_t received = socket_->Receive(buffer, buffer_size, timeout_ms);

	if (received < 0) {
		last_error_ = "Receive error: " + socket_->GetLastError();
		state_.store(ConnectionState::Disconnected);
	} else if (received == 0) {
		// Connection closed
		state_.store(ConnectionState::Disconnected);
	}

	return received;
}

}  // namespace tds
}  // namespace duckdb
