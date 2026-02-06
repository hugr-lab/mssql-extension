#include "tds/tds_connection.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {

// Get local hostname for LOGIN7 HostName field (client workstation name)
std::string GetClientHostname() {
#if defined(_WIN32)
	char hostname[256];
	DWORD size = sizeof(hostname);
	if (GetComputerNameExA(ComputerNameDnsHostname, hostname, &size)) {
		return std::string(hostname);
	}
	// Fallback to NetBIOS name
	size = sizeof(hostname);
	if (GetComputerNameA(hostname, &size)) {
		return std::string(hostname);
	}
	return "DuckDB-Client";
#else
	char hostname[256];
	if (gethostname(hostname, sizeof(hostname)) == 0) {
		hostname[sizeof(hostname) - 1] = '\0';	// Ensure null termination
		return std::string(hostname);
	}
	return "DuckDB-Client";	 // Fallback if hostname lookup fails
#endif
}

}  // anonymous namespace

// Debug logging
static int GetMssqlDebugLevel() {
	static int level = -1;
	if (level == -1) {
		const char *env = std::getenv("MSSQL_DEBUG");
		level = env ? std::atoi(env) : 0;
	}
	return level;
}

#define MSSQL_CONN_DEBUG_LOG(lvl, fmt, ...)                           \
	do {                                                              \
		if (GetMssqlDebugLevel() >= lvl)                              \
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
	  next_packet_id_(1),
	  negotiated_packet_size_(TDS_DEFAULT_PACKET_SIZE) {}

TdsConnection::~TdsConnection() {
	Close();
}

TdsConnection::TdsConnection(TdsConnection &&other) noexcept
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
	  next_packet_id_(other.next_packet_id_),
	  negotiated_packet_size_(other.negotiated_packet_size_) {
	other.state_.store(ConnectionState::Disconnected);
	other.spid_ = 0;
	other.tls_enabled_ = false;
	other.negotiated_packet_size_ = TDS_DEFAULT_PACKET_SIZE;
}

TdsConnection &TdsConnection::operator=(TdsConnection &&other) noexcept {
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
		negotiated_packet_size_ = other.negotiated_packet_size_;

		other.state_.store(ConnectionState::Disconnected);
		other.spid_ = 0;
		other.tls_enabled_ = false;
		other.negotiated_packet_size_ = TDS_DEFAULT_PACKET_SIZE;
	}
	return *this;
}

bool TdsConnection::Connect(const std::string &host, uint16_t port, int timeout_seconds) {
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

bool TdsConnection::Authenticate(const std::string &username, const std::string &password, const std::string &database,
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
	MSSQL_CONN_DEBUG_LOG(1, "DoPrelogin: server version=%d.%d.%d, encryption=%d", prelogin_response.version_major,
						 prelogin_response.version_minor, prelogin_response.version_build,
						 static_cast<int>(prelogin_response.encryption));

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

//===----------------------------------------------------------------------===//
// Azure AD FEDAUTH Authentication (T018/T020)
//===----------------------------------------------------------------------===//

bool TdsConnection::AuthenticateWithFedAuth(const std::string &database, const std::vector<uint8_t> &fedauth_token,
											bool use_encrypt) {
	// Must be in Authenticating state
	if (state_.load() != ConnectionState::Authenticating) {
		last_error_ = "Cannot authenticate: not in Authenticating state";
		return false;
	}

	MSSQL_CONN_DEBUG_LOG(1, "AuthenticateWithFedAuth: starting Azure AD authentication for db='%s', token_size=%zu",
						 database.c_str(), fedauth_token.size());

	// Initialize TDS server name to host - may be updated if routing includes instance name
	tds_server_name_ = host_;

	// Azure SQL/Fabric may require multiple routing hops through gateway infrastructure
	// Each hop requires full PRELOGIN + LOGIN7 handshake
	constexpr int MAX_ROUTING_HOPS = 5;
	int routing_hop = 0;

	while (routing_hop <= MAX_ROUTING_HOPS) {
		MSSQL_CONN_DEBUG_LOG(1, "AuthenticateWithFedAuth: authentication attempt %d on %s:%d", routing_hop + 1,
							 host_.c_str(), port_);

		// Step 1: PRELOGIN handshake with FEDAUTHREQUIRED
		// Per go-mssqldb: after routing, use the NEW routed hostname for TLS SNI (not the original gateway)
		// The host_ is already updated to the routed server in the loop below
		if (!DoPreloginWithFedAuth(use_encrypt, "" /* use host_ for TLS SNI */)) {
			state_.store(ConnectionState::Disconnected);
			socket_->Close();
			return false;
		}

		// Step 2: LOGIN7 with FEDAUTH feature extension
		if (!DoLogin7WithFedAuth(database, fedauth_token)) {
			state_.store(ConnectionState::Disconnected);
			socket_->Close();
			return false;
		}

		// Step 3: Check if server requested routing (Azure SQL/Fabric gateway redirection)
		if (!has_routing_) {
			// No more routing - authentication complete
			break;
		}

		// Handle routing to next server
		routing_hop++;
		MSSQL_CONN_DEBUG_LOG(1, "AuthenticateWithFedAuth: routing hop %d -> %s:%d", routing_hop, routed_server_.c_str(),
							 routed_port_);

		if (routing_hop > MAX_ROUTING_HOPS) {
			last_error_ = "Too many routing hops (" + std::to_string(routing_hop) + ") - aborting";
			state_.store(ConnectionState::Disconnected);
			socket_->Close();
			return false;
		}

		// Close current connection
		socket_->Close();

		// Reset state for new connection
		// Parse routed server - may contain instance name and/or port suffix
		// Format from Azure Fabric: "hostname.pbidedicated.windows.net\INSTANCE-NAME:port"
		// We need:
		// 1. Just the hostname part for DNS resolution
		// 2. The port from after instance name (if present), otherwise use ROUTING ENVCHANGE port
		std::string next_server = routed_server_;
		uint16_t next_port = routed_port_;

		MSSQL_CONN_DEBUG_LOG(2, "AuthenticateWithFedAuth: parsing routed server '%s', envchange_port=%d",
							 next_server.c_str(), next_port);

		// First, check for port suffix in the full string (after instance name)
		// Format: hostname\instance:port - the :port is at the very end
		size_t last_colon = next_server.rfind(':');
		if (last_colon != std::string::npos) {
			// Check if this looks like a port (all digits after colon)
			std::string port_str = next_server.substr(last_colon + 1);
			bool is_port = !port_str.empty();
			for (char c : port_str) {
				if (!std::isdigit(static_cast<unsigned char>(c))) {
					is_port = false;
					break;
				}
			}
			if (is_port) {
				try {
					int parsed_port = std::stoi(port_str);
					if (parsed_port > 0 && parsed_port <= 65535) {
						next_port = static_cast<uint16_t>(parsed_port);
						next_server = next_server.substr(0, last_colon);
						MSSQL_CONN_DEBUG_LOG(2, "AuthenticateWithFedAuth: extracted port %d from server string",
											 next_port);
					}
				} catch (...) {
					// Ignore parse errors
				}
			}
		}

		// Store server name (with instance, without port) for LOGIN7 ServerName field
		// Per TDS spec and go-mssqldb: ServerName should be "hostname\instance" but NOT include port
		tds_server_name_ = next_server;

		// Now strip instance name (after backslash) - we don't use SQL Browser service
		// Keep hostname only for DNS resolution
		size_t backslash_pos = next_server.find('\\');
		if (backslash_pos != std::string::npos) {
			MSSQL_CONN_DEBUG_LOG(2, "AuthenticateWithFedAuth: stripping instance name, keeping hostname '%s'",
								 next_server.substr(0, backslash_pos).c_str());
			next_server = next_server.substr(0, backslash_pos);
		}

		MSSQL_CONN_DEBUG_LOG(1, "AuthenticateWithFedAuth: final routed target: %s:%d (tds_name=%s)",
							 next_server.c_str(), next_port, tds_server_name_.c_str());

		has_routing_ = false;
		routed_server_.clear();
		routed_port_ = 0;
		next_packet_id_ = 1;
		tls_enabled_ = false;
		fedauth_echo_ = false;

		// Update connection target
		// host_ is stripped hostname for TCP/DNS, tds_server_name_ is full name for LOGIN7
		host_ = next_server;
		port_ = next_port;

		// Connect to routed server
		if (!socket_->Connect(next_server, next_port, DEFAULT_CONNECTION_TIMEOUT)) {
			last_error_ = "Failed to connect to routed server " + next_server + ":" + std::to_string(next_port) + ": " +
						  socket_->GetLastError();
			state_.store(ConnectionState::Disconnected);
			return false;
		}

		MSSQL_CONN_DEBUG_LOG(1, "AuthenticateWithFedAuth: connected to routed server %s:%d", next_server.c_str(),
							 next_port);
		// Loop continues with PRELOGIN + LOGIN7 on new server
	}

	// Success - transition to Idle
	database_ = database;
	state_.store(ConnectionState::Idle);
	UpdateLastUsed();
	MSSQL_CONN_DEBUG_LOG(1, "AuthenticateWithFedAuth: Azure AD authentication successful after %d routing hop(s)",
						 routing_hop);
	return true;
}

bool TdsConnection::DoPreloginWithFedAuth(bool use_encrypt, const std::string &sni_hostname) {
	MSSQL_CONN_DEBUG_LOG(1, "DoPreloginWithFedAuth: sending PRELOGIN with FEDAUTHREQUIRED%s",
						 sni_hostname.empty() ? "" : (", sni_override=" + sni_hostname).c_str());

	// Build PRELOGIN packet with FEDAUTHREQUIRED option
	TdsPacket prelogin = TdsProtocol::BuildPreloginWithFedAuth(use_encrypt, true /* fedauth_required */);
	prelogin.SetPacketId(next_packet_id_++);

	// Debug: dump PRELOGIN payload bytes
	if (GetMssqlDebugLevel() >= 2) {
		const auto &payload = prelogin.GetPayload();
		std::string hex_dump;
		for (size_t i = 0; i < payload.size(); i++) {
			char buf[4];
			snprintf(buf, sizeof(buf), "%02x ", payload[i]);
			hex_dump += buf;
		}
		MSSQL_CONN_DEBUG_LOG(2, "DoPreloginWithFedAuth: PRELOGIN payload (%zu bytes): %s", payload.size(),
							 hex_dump.c_str());
	}

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

	// Store FEDAUTH echo flag - if server's FEDAUTHREQUIRED was non-zero, we must echo it back in LOGIN7
	fedauth_echo_ = prelogin_response.fedauth_echo;

	MSSQL_CONN_DEBUG_LOG(1, "DoPreloginWithFedAuth: server version=%d.%d.%d, encryption=%d, fedauth_echo=%d",
						 prelogin_response.version_major, prelogin_response.version_minor,
						 prelogin_response.version_build, static_cast<int>(prelogin_response.encryption),
						 fedauth_echo_ ? 1 : 0);

	// For Azure AD, encryption is typically required
	if (use_encrypt) {
		if (prelogin_response.encryption == EncryptionOption::ENCRYPT_NOT_SUP) {
			last_error_ = "TLS required for Azure AD but server does not support encryption";
			return false;
		}
		if (prelogin_response.encryption == EncryptionOption::ENCRYPT_OFF) {
			last_error_ = "TLS required for Azure AD but server declined encryption";
			return false;
		}

		// Enable TLS - optionally override SNI hostname for Azure routing
		if (!socket_->EnableTls(next_packet_id_, DEFAULT_CONNECTION_TIMEOUT * 1000, sni_hostname)) {
			last_error_ = "TLS handshake failed: " + socket_->GetLastError();
			return false;
		}
		tls_enabled_ = true;
		MSSQL_CONN_DEBUG_LOG(1, "DoPreloginWithFedAuth: TLS enabled%s", sni_hostname.empty() ? "" : " (SNI override)");
	} else {
		if (prelogin_response.encryption == EncryptionOption::ENCRYPT_REQ) {
			last_error_ = "Server requires encryption but TLS not requested";
			return false;
		}
		tls_enabled_ = false;
	}

	return true;
}

bool TdsConnection::DoLogin7WithFedAuth(const std::string &database, const std::vector<uint8_t> &fedauth_token) {
	// Get local hostname for LOGIN7 HostName field (per go-mssqldb: HostName = client workstation)
	std::string client_hostname = GetClientHostname();

	// ADAL workflow (per go-mssqldb):
	// 1. Send LOGIN7 with small ADAL FEDAUTH extension (no token)
	// 2. Server responds with FEDAUTHINFO token containing STS URL
	// 3. Client sends token in separate FEDAUTH_TOKEN packet
	// 4. Server responds with LOGINACK or ROUTING

	MSSQL_CONN_DEBUG_LOG(
		1, "DoLogin7WithFedAuth: sending LOGIN7 with ADAL FEDAUTH, db='%s', echo=%d, client='%s', server='%s'",
		database.c_str(), fedauth_echo_ ? 1 : 0, client_hostname.c_str(), tds_server_name_.c_str());

	// Step 1: Send LOGIN7 with ADAL FEDAUTH extension (no token embedded)
	TdsPacket login = TdsProtocol::BuildLogin7WithADAL(client_hostname, tds_server_name_, database, fedauth_echo_,
													   "DuckDB MSSQL Extension", TDS_DEFAULT_PACKET_SIZE);
	login.SetPacketId(next_packet_id_++);

	// Debug: dump LOGIN7 payload (last 20 bytes should contain FEDAUTH extension)
	if (GetMssqlDebugLevel() >= 2) {
		const auto &payload = login.GetPayload();
		MSSQL_CONN_DEBUG_LOG(2, "DoLogin7WithFedAuth: LOGIN7 payload size=%zu", payload.size());
		if (payload.size() > 20) {
			std::string hex_dump;
			// Last 20 bytes (feature extension area)
			for (size_t i = payload.size() - 20; i < payload.size(); i++) {
				char buf[4];
				snprintf(buf, sizeof(buf), "%02x ", payload[i]);
				hex_dump += buf;
			}
			MSSQL_CONN_DEBUG_LOG(2, "DoLogin7WithFedAuth: LOGIN7 last 20 bytes: %s", hex_dump.c_str());
		}
	}

	if (!socket_->SendPacket(login)) {
		last_error_ = "Failed to send LOGIN7: " + socket_->GetLastError();
		return false;
	}

	MSSQL_CONN_DEBUG_LOG(2, "DoLogin7WithFedAuth: LOGIN7 with ADAL sent, waiting for FEDAUTHINFO response...");

	// Step 2: Receive response (should contain FEDAUTHINFO token)
	std::vector<uint8_t> response;
	if (!socket_->ReceiveMessage(response, DEFAULT_CONNECTION_TIMEOUT * 1000)) {
		last_error_ = "Failed to receive LOGIN7 response: " + socket_->GetLastError();
		MSSQL_CONN_DEBUG_LOG(1, "DoLogin7WithFedAuth: ReceiveMessage failed: %s", last_error_.c_str());
		return false;
	}

	MSSQL_CONN_DEBUG_LOG(2, "DoLogin7WithFedAuth: received %zu bytes response", response.size());

	// Debug: dump first 150 bytes of LOGIN7 response
	if (GetMssqlDebugLevel() >= 2) {
		std::string hex_dump;
		for (size_t i = 0; i < std::min<size_t>(150, response.size()); i++) {
			char buf[4];
			snprintf(buf, sizeof(buf), "%02x ", response[i]);
			hex_dump += buf;
		}
		MSSQL_CONN_DEBUG_LOG(2, "DoLogin7WithFedAuth: response bytes (first 150): %s", hex_dump.c_str());
	}

	LoginResponse login_response = TdsProtocol::ParseLoginResponse(response);

	// Check if we received FEDAUTHINFO token (ADAL workflow)
	if (login_response.has_fedauth_info) {
		MSSQL_CONN_DEBUG_LOG(1, "DoLogin7WithFedAuth: received FEDAUTHINFO, STS_URL='%s', SPN='%s'",
							 login_response.sts_url.c_str(), login_response.server_spn.c_str());

		// Step 3: Send token in FEDAUTH_TOKEN packet
		MSSQL_CONN_DEBUG_LOG(1, "DoLogin7WithFedAuth: sending FEDAUTH_TOKEN packet, token_size=%zu",
							 fedauth_token.size());

		TdsPacket token_packet = TdsProtocol::BuildFedAuthToken(fedauth_token);
		// Per go-mssqldb: packet sequence resets to 1 for each new message type
		token_packet.SetPacketId(1);

		// Debug: dump complete FEDAUTH_TOKEN packet (header + first 20 bytes of payload)
		if (GetMssqlDebugLevel() >= 2) {
			auto serialized = token_packet.Serialize();
			std::string hex_header;
			for (size_t i = 0; i < std::min<size_t>(8, serialized.size()); i++) {
				char buf[4];
				snprintf(buf, sizeof(buf), "%02x ", serialized[i]);
				hex_header += buf;
			}
			MSSQL_CONN_DEBUG_LOG(
				2, "DoLogin7WithFedAuth: FEDAUTH_TOKEN TDS header: %s (type=0x%02x, status=0x%02x, len=%d, pktid=%d)",
				hex_header.c_str(), static_cast<uint8_t>(token_packet.GetType()),
				static_cast<uint8_t>(token_packet.GetStatus()), token_packet.GetLength(), token_packet.GetPacketId());

			const auto &payload = token_packet.GetPayload();
			std::string hex_dump;
			for (size_t i = 0; i < std::min<size_t>(20, payload.size()); i++) {
				char buf[4];
				snprintf(buf, sizeof(buf), "%02x ", payload[i]);
				hex_dump += buf;
			}
			MSSQL_CONN_DEBUG_LOG(2, "DoLogin7WithFedAuth: FEDAUTH_TOKEN payload (first 20): %s", hex_dump.c_str());
		}

		if (!socket_->SendPacket(token_packet)) {
			last_error_ = "Failed to send FEDAUTH_TOKEN: " + socket_->GetLastError();
			return false;
		}

		MSSQL_CONN_DEBUG_LOG(2, "DoLogin7WithFedAuth: FEDAUTH_TOKEN sent, waiting for LOGINACK...");

		// Step 4: Receive final response (LOGINACK or ROUTING)
		response.clear();
		if (!socket_->ReceiveMessage(response, DEFAULT_CONNECTION_TIMEOUT * 1000)) {
			last_error_ = "Failed to receive LOGINACK after FEDAUTH_TOKEN: " + socket_->GetLastError();
			MSSQL_CONN_DEBUG_LOG(1, "DoLogin7WithFedAuth: ReceiveMessage after token failed: %s", last_error_.c_str());
			return false;
		}

		MSSQL_CONN_DEBUG_LOG(2, "DoLogin7WithFedAuth: received %zu bytes final response", response.size());

		// Debug dump final response
		if (GetMssqlDebugLevel() >= 2) {
			std::string hex_dump;
			for (size_t i = 0; i < std::min<size_t>(150, response.size()); i++) {
				char buf[4];
				snprintf(buf, sizeof(buf), "%02x ", response[i]);
				hex_dump += buf;
			}
			MSSQL_CONN_DEBUG_LOG(2, "DoLogin7WithFedAuth: final response bytes (first 150): %s", hex_dump.c_str());
		}

		// Parse final response
		login_response = TdsProtocol::ParseLoginResponse(response);
	}

	// Check for success
	if (!login_response.success) {
		if (login_response.error_number > 0) {
			last_error_ = "Azure AD authentication failed (error " + std::to_string(login_response.error_number) +
						  "): " + login_response.error_message;
		} else {
			last_error_ = "Azure AD authentication failed: " + login_response.error_message;
		}
		return false;
	}

	spid_ = login_response.spid;
	negotiated_packet_size_ = login_response.negotiated_packet_size;

	// Check for routing (Azure SQL/Fabric gateway redirection)
	if (login_response.has_routing) {
		has_routing_ = true;
		routed_server_ = login_response.routed_server;
		routed_port_ = login_response.routed_port;
		MSSQL_CONN_DEBUG_LOG(1, "DoLogin7WithFedAuth: ROUTING requested to %s:%d", routed_server_.c_str(),
							 routed_port_);
	}

	MSSQL_CONN_DEBUG_LOG(1, "DoLogin7WithFedAuth: Azure AD login successful, spid=%d, packet_size=%d", spid_,
						 negotiated_packet_size_);

	return true;
}

bool TdsConnection::DoLogin7(const std::string &username, const std::string &password, const std::string &database) {
	MSSQL_CONN_DEBUG_LOG(1, "DoLogin7: starting authentication for user='%s', db='%s'", username.c_str(),
						 database.c_str());
	// Request default packet size - server will negotiate up if it supports larger
	// This allows the server to tell us its optimal packet size via ENVCHANGE
	TdsPacket login = TdsProtocol::BuildLogin7(host_, username, password, database, "DuckDB MSSQL Extension",
											   TDS_DEFAULT_PACKET_SIZE);
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
			last_error_ = "Authentication failed (error " + std::to_string(login_response.error_number) +
						  "): " + login_response.error_message;
		} else {
			last_error_ = "Authentication failed: " + login_response.error_message;
		}
		return false;
	}

	spid_ = login_response.spid;
	// Use server-negotiated packet size from ENVCHANGE, or keep default if not received
	negotiated_packet_size_ = login_response.negotiated_packet_size;
	MSSQL_CONN_DEBUG_LOG(1, "DoLogin7: authentication successful, spid=%d, packet_size=%d", spid_,
						 negotiated_packet_size_);

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
		return state_.load() == ConnectionState::Executing;	 // Already executing is "alive"
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
		auto elapsed =
			std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();

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

void TdsConnection::SetTransactionDescriptor(const uint8_t *descriptor) {
	if (descriptor) {
		std::memcpy(transaction_descriptor_, descriptor, 8);
		has_transaction_descriptor_ = true;
		MSSQL_CONN_DEBUG_LOG(1, "SetTransactionDescriptor: %02x %02x %02x %02x %02x %02x %02x %02x",
							 transaction_descriptor_[0], transaction_descriptor_[1], transaction_descriptor_[2],
							 transaction_descriptor_[3], transaction_descriptor_[4], transaction_descriptor_[5],
							 transaction_descriptor_[6], transaction_descriptor_[7]);
	} else {
		std::memset(transaction_descriptor_, 0, 8);
		has_transaction_descriptor_ = false;
		MSSQL_CONN_DEBUG_LOG(1, "SetTransactionDescriptor: cleared");
	}
}

const uint8_t *TdsConnection::GetTransactionDescriptor() const {
	if (!has_transaction_descriptor_) {
		return nullptr;
	}
	return transaction_descriptor_;
}

void TdsConnection::ClearTransactionDescriptor() {
	std::memset(transaction_descriptor_, 0, 8);
	has_transaction_descriptor_ = false;
	MSSQL_CONN_DEBUG_LOG(1, "ClearTransactionDescriptor: cleared");
}

bool TdsConnection::ExecuteBatch(const std::string &sql) {
	MSSQL_CONN_DEBUG_LOG(1, "ExecuteBatch: starting, state=%d, socket_connected=%d", static_cast<int>(state_.load()),
						 socket_ ? socket_->IsConnected() : -1);

	// Can only execute from Idle state
	ConnectionState expected = ConnectionState::Idle;
	if (!state_.compare_exchange_strong(expected, ConnectionState::Executing)) {
		last_error_ =
			"Cannot execute: connection not in Idle state (current: " + std::string(ConnectionStateToString(expected)) +
			")";
		MSSQL_CONN_DEBUG_LOG(1, "ExecuteBatch: FAILED - wrong state: %d", static_cast<int>(expected));
		return false;
	}

	// Build SQL_BATCH packet(s) using the server-negotiated packet size
	// This was received via ENVCHANGE during LOGIN7
	// Pass the transaction descriptor if one is set (from BEGIN TRANSACTION response)
	const uint8_t *txn_desc = has_transaction_descriptor_ ? transaction_descriptor_ : nullptr;
	std::vector<TdsPacket> packets = TdsProtocol::BuildSqlBatchMultiPacket(sql, negotiated_packet_size_, txn_desc);

	MSSQL_CONN_DEBUG_LOG(1, "ExecuteBatch: using transaction descriptor: %s",
						 has_transaction_descriptor_ ? "yes" : "no");

	MSSQL_CONN_DEBUG_LOG(1, "ExecuteBatch: sql_size=%zu, packet_count=%zu", sql.size(), packets.size());

	// If connection needs reset, set RESET_CONNECTION flag on the first packet
	if (needs_reset_ && !packets.empty()) {
		auto &first = packets[0];
		auto status = static_cast<uint8_t>(first.GetStatus()) | static_cast<uint8_t>(PacketStatus::RESET_CONNECTION);
		first.SetStatus(static_cast<PacketStatus>(status));
		needs_reset_ = false;
		MSSQL_CONN_DEBUG_LOG(1, "ExecuteBatch: RESET_CONNECTION flag set on first packet");
	}

	// For multi-packet messages:
	// - Over TLS: send each packet individually (some SQL Server versions have issues with combined)
	// - Over plain TCP: combine and send at once for efficiency
	if (packets.size() > 1) {
		bool use_combined = !socket_->IsTlsEnabled();
		uint8_t pkt_id = 1;

		if (use_combined) {
			// Plain TCP: combine all packets and send at once
			std::vector<uint8_t> combined;
			size_t total_size = 0;
			for (size_t i = 0; i < packets.size(); i++) {
				auto &packet = packets[i];
				packet.SetPacketId(pkt_id++);
				MSSQL_CONN_DEBUG_LOG(2,
									 "ExecuteBatch: preparing packet %zu/%zu, type=0x%02x, status=0x%02x, length=%u, "
									 "payload_size=%zu, eom=%d, pkt_id=%u",
									 i + 1, packets.size(), static_cast<unsigned>(packet.GetType()),
									 static_cast<unsigned>(packet.GetStatus()), packet.GetLength(),
									 packet.GetPayload().size(), packet.IsEndOfMessage(), packet.GetPacketId());
				std::vector<uint8_t> serialized = packet.Serialize();
				combined.insert(combined.end(), serialized.begin(), serialized.end());
				total_size += serialized.size();
			}
			MSSQL_CONN_DEBUG_LOG(2, "ExecuteBatch: sending %zu combined bytes", total_size);
			if (!socket_->Send(combined)) {
				last_error_ = "Failed to send multi-packet SQL_BATCH: " + socket_->GetLastError();
				state_.store(ConnectionState::Disconnected);
				return false;
			}
		} else {
			// TLS: send packets individually
			for (size_t i = 0; i < packets.size(); i++) {
				auto &packet = packets[i];
				packet.SetPacketId(pkt_id++);
				MSSQL_CONN_DEBUG_LOG(2,
									 "ExecuteBatch: sending TLS packet %zu/%zu, type=0x%02x, status=0x%02x, length=%u, "
									 "payload_size=%zu, eom=%d, pkt_id=%u",
									 i + 1, packets.size(), static_cast<unsigned>(packet.GetType()),
									 static_cast<unsigned>(packet.GetStatus()), packet.GetLength(),
									 packet.GetPayload().size(), packet.IsEndOfMessage(), packet.GetPacketId());
				if (!socket_->SendPacket(packet)) {
					last_error_ = "Failed to send TLS packet " + std::to_string(i + 1) + "/" +
								  std::to_string(packets.size()) + ": " + socket_->GetLastError();
					state_.store(ConnectionState::Disconnected);
					return false;
				}
			}
		}
	} else {
		// Single packet - send normally
		for (size_t i = 0; i < packets.size(); i++) {
			auto &packet = packets[i];
			packet.SetPacketId(next_packet_id_++);
			MSSQL_CONN_DEBUG_LOG(2,
								 "ExecuteBatch: sending packet %zu/%zu, type=0x%02x, status=0x%02x, length=%u, "
								 "payload_size=%zu, eom=%d, pkt_id=%u",
								 i + 1, packets.size(), static_cast<unsigned>(packet.GetType()),
								 static_cast<unsigned>(packet.GetStatus()), packet.GetLength(),
								 packet.GetPayload().size(), packet.IsEndOfMessage(), packet.GetPacketId());
			// Dump full packet header and first bytes of payload for debugging
			if (i == 0 && GetMssqlDebugLevel() >= 3) {
				std::vector<uint8_t> serialized = packet.Serialize();
				std::string hex_dump;
				// Dump header (8 bytes) + first 42 bytes of payload = 50 bytes total
				for (size_t j = 0; j < std::min<size_t>(50, serialized.size()); j++) {
					char buf[4];
					snprintf(buf, sizeof(buf), "%02x ", serialized[j]);
					hex_dump += buf;
				}
				MSSQL_CONN_DEBUG_LOG(3, "ExecuteBatch: packet bytes (header+payload, first 50): %s", hex_dump.c_str());
			}
			if (!socket_->SendPacket(packet)) {
				last_error_ = "Failed to send SQL_BATCH: " + socket_->GetLastError();
				state_.store(ConnectionState::Disconnected);
				return false;
			}
		}
	}

	MSSQL_CONN_DEBUG_LOG(1, "ExecuteBatch: all packets sent");

	// Connection is now in Executing state, ready to receive response
	return true;
}

ssize_t TdsConnection::ReceiveData(uint8_t *buffer, size_t buffer_size, int timeout_ms) {
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
