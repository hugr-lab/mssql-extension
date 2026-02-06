#include "tds/tds_socket.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>

#ifdef _WIN32
// NOMINMAX must be defined before including winsock2.h (which includes windows.h)
// to prevent min/max macros that conflict with std::min/std::max
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#define CLOSE_SOCKET closesocket
#define SOCKET_ERROR_CODE WSAGetLastError()
#define poll WSAPoll
typedef int socklen_t;
// Windows socket functions use char* instead of void*
#define SOCK_OPT_CAST(x) reinterpret_cast<char *>(x)
#define SOCK_OPT_CONST_CAST(x) reinterpret_cast<const char *>(x)
#define SOCK_BUF_CAST(x) reinterpret_cast<char *>(x)
#define SOCK_BUF_CONST_CAST(x) reinterpret_cast<const char *>(x)
// MSG_NOSIGNAL prevents SIGPIPE on Linux; Windows doesn't have SIGPIPE
#define MSG_NOSIGNAL 0

// One-time Winsock initialization (required before any socket API call on Windows)
static std::once_flag winsock_init_flag;
static bool winsock_initialized = false;

static void InitializeWinsock() {
	WSADATA wsaData;
	int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
	if (result == 0) {
		winsock_initialized = true;
		atexit([]() { WSACleanup(); });
	}
}
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#define CLOSE_SOCKET close
#define SOCKET_ERROR_CODE errno
// POSIX socket functions use void* - no cast needed
#define SOCK_OPT_CAST(x) (x)
#define SOCK_OPT_CONST_CAST(x) (x)
#define SOCK_BUF_CAST(x) (x)
#define SOCK_BUF_CONST_CAST(x) (x)

// MSG_NOSIGNAL prevents SIGPIPE on Linux; macOS uses SO_NOSIGPIPE instead
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
#endif

// Debug logging
static int GetMssqlDebugLevel() {
	static int level = -1;
	if (level == -1) {
		const char *env = std::getenv("MSSQL_DEBUG");
		level = env ? std::atoi(env) : 0;
	}
	return level;
}

#define MSSQL_SOCKET_DEBUG_LOG(lvl, fmt, ...)                           \
	do {                                                                \
		if (GetMssqlDebugLevel() >= lvl)                                \
			fprintf(stderr, "[MSSQL SOCKET] " fmt "\n", ##__VA_ARGS__); \
	} while (0)

namespace duckdb {
namespace tds {

TdsSocket::TdsSocket() : fd_(-1), port_(0), connected_(false) {}

TdsSocket::~TdsSocket() {
	Close();
}

TdsSocket::TdsSocket(TdsSocket &&other) noexcept
	: fd_(other.fd_),
	  host_(std::move(other.host_)),
	  port_(other.port_),
	  connected_(other.connected_),
	  last_error_(std::move(other.last_error_)),
	  tls_context_(std::move(other.tls_context_)),
	  receive_buffer_(std::move(other.receive_buffer_)) {
	other.fd_ = -1;
	other.connected_ = false;
}

TdsSocket &TdsSocket::operator=(TdsSocket &&other) noexcept {
	if (this != &other) {
		Close();
		fd_ = other.fd_;
		host_ = std::move(other.host_);
		port_ = other.port_;
		connected_ = other.connected_;
		last_error_ = std::move(other.last_error_);
		tls_context_ = std::move(other.tls_context_);
		receive_buffer_ = std::move(other.receive_buffer_);
		other.fd_ = -1;
		other.connected_ = false;
	}
	return *this;
}

bool TdsSocket::Connect(const std::string &host, uint16_t port, int timeout_seconds) {
	MSSQL_SOCKET_DEBUG_LOG(1, "Connect: connecting to %s:%d (timeout=%ds)", host.c_str(), port, timeout_seconds);

	if (connected_) {
		Close();
	}

	host_ = host;
	port_ = port;
	last_error_.clear();

#ifdef _WIN32
	MSSQL_SOCKET_DEBUG_LOG(2, "Connect: initializing Winsock");
	std::call_once(winsock_init_flag, InitializeWinsock);
	if (!winsock_initialized) {
		last_error_ = "Failed to initialize Windows socket library (WSAStartup failed)";
		MSSQL_SOCKET_DEBUG_LOG(1, "Connect: WSAStartup failed");
		return false;
	}
	MSSQL_SOCKET_DEBUG_LOG(2, "Connect: Winsock initialized successfully");
#endif

	// Resolve hostname
	struct addrinfo hints, *result, *rp;
	std::memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;	  // Allow IPv4 or IPv6
	hints.ai_socktype = SOCK_STREAM;  // TCP
	hints.ai_flags = 0;
	hints.ai_protocol = IPPROTO_TCP;

	MSSQL_SOCKET_DEBUG_LOG(2, "Connect: resolving hostname '%s'", host.c_str());
	std::string port_str = std::to_string(port);
	int ret = getaddrinfo(host.c_str(), port_str.c_str(), &hints, &result);
	if (ret != 0) {
		last_error_ = "Failed to resolve hostname: " + std::string(gai_strerror(ret));
		MSSQL_SOCKET_DEBUG_LOG(1, "Connect: DNS resolution failed: %s", last_error_.c_str());
		return false;
	}
	MSSQL_SOCKET_DEBUG_LOG(2, "Connect: hostname resolved successfully");

	// Try each address until we connect
	int addr_index = 0;
	for (rp = result; rp != nullptr; rp = rp->ai_next) {
		const char *family_str = rp->ai_family == AF_INET ? "IPv4" : (rp->ai_family == AF_INET6 ? "IPv6" : "other");
		MSSQL_SOCKET_DEBUG_LOG(2, "Connect: trying address %d (%s)", addr_index, family_str);

		fd_ = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
		if (fd_ == -1) {
			MSSQL_SOCKET_DEBUG_LOG(2, "Connect: socket() failed for address %d (error=%d)", addr_index,
								   SOCKET_ERROR_CODE);
			addr_index++;
			continue;
		}

		// Set non-blocking for timeout support
		if (!SetNonBlocking(true)) {
			MSSQL_SOCKET_DEBUG_LOG(2, "Connect: SetNonBlocking failed for address %d", addr_index);
			CLOSE_SOCKET(fd_);
			fd_ = -1;
			addr_index++;
			continue;
		}

		// Attempt connection
		ret = connect(fd_, rp->ai_addr, rp->ai_addrlen);
		if (ret == 0) {
			// Connected immediately
			MSSQL_SOCKET_DEBUG_LOG(1, "Connect: connected immediately to address %d", addr_index);
			connected_ = true;
			break;
		}

		int connect_error = SOCKET_ERROR_CODE;
		MSSQL_SOCKET_DEBUG_LOG(2, "Connect: connect() returned %d, error code=%d", ret, connect_error);

#ifdef _WIN32
		if (connect_error == WSAEWOULDBLOCK) {
#else
		if (connect_error == EINPROGRESS || connect_error == EWOULDBLOCK) {
#endif
			MSSQL_SOCKET_DEBUG_LOG(2, "Connect: waiting for connection (timeout=%ds)...", timeout_seconds);
			// Wait for connection with timeout
			if (WaitForReady(true, timeout_seconds * 1000)) {
				// Check if connection succeeded
				int error = 0;
				socklen_t len = sizeof(error);
				if (getsockopt(fd_, SOL_SOCKET, SO_ERROR, SOCK_OPT_CAST(&error), &len) == 0 && error == 0) {
					MSSQL_SOCKET_DEBUG_LOG(1, "Connect: connected to address %d after wait", addr_index);
					connected_ = true;
					break;
				}
				last_error_ = "Connection failed: " + std::string(strerror(error));
				MSSQL_SOCKET_DEBUG_LOG(1, "Connect: getsockopt SO_ERROR=%d for address %d: %s", error, addr_index,
									   last_error_.c_str());
			} else {
				last_error_ = "Connection timed out";
				MSSQL_SOCKET_DEBUG_LOG(1, "Connect: timed out on address %d", addr_index);
			}
		} else {
			MSSQL_SOCKET_DEBUG_LOG(1, "Connect: connect failed on address %d with unexpected error %d", addr_index,
								   connect_error);
		}

		CLOSE_SOCKET(fd_);
		fd_ = -1;
		addr_index++;
	}

	freeaddrinfo(result);

	if (!connected_) {
		if (last_error_.empty()) {
			last_error_ = "Failed to connect to " + host + ":" + std::to_string(port);
		}
		MSSQL_SOCKET_DEBUG_LOG(1, "Connect: FAILED - %s", last_error_.c_str());
		return false;
	}

	// Set TCP_NODELAY for low latency
	int flag = 1;
	setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, SOCK_OPT_CONST_CAST(&flag), sizeof(flag));

#ifdef __APPLE__
	// On macOS, set SO_NOSIGPIPE to prevent SIGPIPE on write to closed socket
	setsockopt(fd_, SOL_SOCKET, SO_NOSIGPIPE, SOCK_OPT_CONST_CAST(&flag), sizeof(flag));
#endif

	// Set back to blocking mode for simpler I/O
	SetNonBlocking(false);

	return true;
}

void TdsSocket::Close() {
	// Close TLS first if enabled
	if (tls_context_) {
		tls_context_->Close();
		tls_context_.reset();
	}

	if (fd_ >= 0) {
		CLOSE_SOCKET(fd_);
		fd_ = -1;
	}
	connected_ = false;
	receive_buffer_.clear();
}

bool TdsSocket::IsConnected() const {
	return connected_ && fd_ >= 0;
}

bool TdsSocket::EnableTls(uint8_t &packet_id, int timeout_ms, const std::string &sni_hostname) {
	// For TDS 7.x, TLS handshake data must be wrapped in TDS PRELOGIN packets
	// See MS-TDS spec: "If encryption was negotiated in TDS 7.x, the TDS client MUST
	// initiate a TLS/SSL handshake, send to the server a TLS/SSL message obtained from
	// the TLS/SSL layer encapsulated in TDS packet(s) of type PRELOGIN (0x12)."

	MSSQL_SOCKET_DEBUG_LOG(1, "EnableTls: starting (timeout=%dms, fd=%d, packet_id=%d)", timeout_ms, fd_, packet_id);

	// Clear any leftover data in receive buffer before TLS
	if (!receive_buffer_.empty()) {
		MSSQL_SOCKET_DEBUG_LOG(1, "EnableTls: WARNING - clearing %zu leftover bytes in receive buffer",
							   receive_buffer_.size());
		receive_buffer_.clear();
	}

	if (!IsConnected()) {
		last_error_ = "Cannot enable TLS: not connected";
		MSSQL_SOCKET_DEBUG_LOG(1, "EnableTls: FAILED - not connected");
		return false;
	}

	if (tls_context_) {
		last_error_ = "TLS is already enabled";
		MSSQL_SOCKET_DEBUG_LOG(1, "EnableTls: FAILED - already enabled");
		return false;
	}

	// Create and initialize TLS context
	MSSQL_SOCKET_DEBUG_LOG(1, "EnableTls: creating TLS context...");
	tls_context_.reset(new TlsTdsContext());

	MSSQL_SOCKET_DEBUG_LOG(1, "EnableTls: initializing TLS context...");
	if (!tls_context_->Initialize()) {
		last_error_ = "TLS initialization failed: " + tls_context_->GetLastError();
		MSSQL_SOCKET_DEBUG_LOG(1, "EnableTls: FAILED - init: %s", last_error_.c_str());
		tls_context_.reset();
		return false;
	}

	// Use provided SNI hostname or fall back to connection host
	// Azure routing may require original hostname as SNI for session tracking
	const std::string &effective_hostname = sni_hostname.empty() ? host_ : sni_hostname;

	// Wrap the existing socket with hostname for SNI
	MSSQL_SOCKET_DEBUG_LOG(1, "EnableTls: wrapping socket with hostname=%s...", effective_hostname.c_str());
	if (!tls_context_->WrapSocket(fd_, effective_hostname)) {
		last_error_ = "TLS socket wrap failed: " + tls_context_->GetLastError();
		MSSQL_SOCKET_DEBUG_LOG(1, "EnableTls: FAILED - wrap: %s", last_error_.c_str());
		tls_context_.reset();
		return false;
	}

	// Set up TDS-wrapped I/O callbacks for the TLS handshake
	// The TLS data must be sent inside TDS PRELOGIN packets (type 0x12)
	MSSQL_SOCKET_DEBUG_LOG(1, "EnableTls: setting up TDS-wrapped TLS I/O...");

	// Capture context for lambdas
	int socket_fd = fd_;
	uint8_t &pkt_id = packet_id;

	// Buffer for extra TLS data from large TDS packets
	// (server may send large TLS records that mbedTLS reads in small chunks)
	auto tls_recv_buffer = std::make_shared<std::vector<uint8_t>>();

	// Send buffer - accumulate TLS data and send in batches like FreeTDS does
	auto tls_send_buffer = std::make_shared<std::vector<uint8_t>>();

	// Helper to flush the send buffer as a single TDS packet
	auto flush_send_buffer = [socket_fd, &pkt_id, tls_send_buffer]() -> bool {
		auto &buffer = *tls_send_buffer;
		if (buffer.empty()) {
			return true;
		}

		MSSQL_SOCKET_DEBUG_LOG(2, "TLS-TDS Flush: sending %zu bytes in PRELOGIN packet (id=%d)", buffer.size(), pkt_id);

		// Build TDS PRELOGIN packet with accumulated TLS data
		std::vector<uint8_t> tds_packet;
		tds_packet.reserve(8 + buffer.size());

		// TDS header (8 bytes)
		tds_packet.push_back(0x12);	 // Type = PRELOGIN
		tds_packet.push_back(0x01);	 // Status = EOM (last packet)
		uint16_t total_len = static_cast<uint16_t>(8 + buffer.size());
		tds_packet.push_back((total_len >> 8) & 0xFF);	// Length high
		tds_packet.push_back(total_len & 0xFF);			// Length low
		tds_packet.push_back(0x00);						// SPID high
		tds_packet.push_back(0x00);						// SPID low
		tds_packet.push_back(pkt_id++);					// Packet ID
		tds_packet.push_back(0x00);						// Window

		// TLS payload
		tds_packet.insert(tds_packet.end(), buffer.begin(), buffer.end());
		buffer.clear();

		// Send the wrapped packet
		size_t total_sent = 0;
		while (total_sent < tds_packet.size()) {
			ssize_t sent = send(socket_fd, SOCK_BUF_CONST_CAST(tds_packet.data() + total_sent),
								tds_packet.size() - total_sent, MSG_NOSIGNAL);
			if (sent < 0) {
				if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
					continue;
				}
				MSSQL_SOCKET_DEBUG_LOG(1, "TLS-TDS Flush: socket error %d (%s)", errno, strerror(errno));
				return false;
			}
			total_sent += sent;
		}

		MSSQL_SOCKET_DEBUG_LOG(2, "TLS-TDS Flush: sent %zu bytes total", total_sent);
		return true;
	};

	// Send callback: buffer TLS data for later sending
	// Like FreeTDS, we accumulate and send when we need to receive
	TlsSendCallback send_cb = [tls_send_buffer](const uint8_t *data, size_t len) -> int {
		MSSQL_SOCKET_DEBUG_LOG(2, "TLS-TDS Send: buffering %zu bytes (buffer now has %zu)", len,
							   tls_send_buffer->size() + len);
		tls_send_buffer->insert(tls_send_buffer->end(), data, data + len);
		return static_cast<int>(len);
	};

	// Receive callback: unwrap TLS data from TDS PRELOGIN packet
	// Uses tls_recv_buffer to buffer extra data from large TDS packets
	// Captures flush_send_buffer to send pending data before receiving (like FreeTDS does)
	TlsRecvCallback recv_cb = [socket_fd, tls_recv_buffer, tls_send_buffer, flush_send_buffer](uint8_t *buf, size_t len,
																							   int timeout_ms) -> int {
		auto &recv_buffer = *tls_recv_buffer;
		MSSQL_SOCKET_DEBUG_LOG(2, "TLS-TDS Recv: request %zu bytes (recv_buffer=%zu, send_buffer=%zu, timeout=%d)", len,
							   recv_buffer.size(), tls_send_buffer->size(), timeout_ms);

		// Like FreeTDS: flush send buffer before trying to receive
		// This ensures all pending TLS data is sent as a single TDS packet
		if (!tls_send_buffer->empty()) {
			if (!flush_send_buffer()) {
				MSSQL_SOCKET_DEBUG_LOG(1, "TLS-TDS Recv: failed to flush send buffer");
				return -1;
			}
		}

		// First, return data from buffer if we have any
		if (!recv_buffer.empty()) {
			size_t to_copy = std::min(len, recv_buffer.size());
			std::memcpy(buf, recv_buffer.data(), to_copy);
			recv_buffer.erase(recv_buffer.begin(), recv_buffer.begin() + to_copy);
			MSSQL_SOCKET_DEBUG_LOG(2, "TLS-TDS Recv: returned %zu bytes from buffer (%zu remaining)", to_copy,
								   recv_buffer.size());
			return static_cast<int>(to_copy);
		}

		// Wait for data with timeout
		struct pollfd pfd;
		pfd.fd = socket_fd;
		pfd.events = POLLIN;
		pfd.revents = 0;

		int poll_result = poll(&pfd, 1, timeout_ms);
		if (poll_result < 0) {
			MSSQL_SOCKET_DEBUG_LOG(1, "TLS-TDS Recv: poll error %d (%s)", errno, strerror(errno));
			return -1;
		}
		if (poll_result == 0) {
			MSSQL_SOCKET_DEBUG_LOG(1, "TLS-TDS Recv: timeout");
			return 0;  // Timeout
		}

		// Read TDS header first (8 bytes)
		uint8_t header[8];
		size_t header_read = 0;
		while (header_read < 8) {
			ssize_t n = recv(socket_fd, SOCK_BUF_CAST(header + header_read), 8 - header_read, 0);
			if (n < 0) {
				if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
					continue;
				}
				MSSQL_SOCKET_DEBUG_LOG(1, "TLS-TDS Recv: header read error %d (%s)", errno, strerror(errno));
				return -1;
			}
			if (n == 0) {
				MSSQL_SOCKET_DEBUG_LOG(1, "TLS-TDS Recv: connection closed by server");
				return -1;
			}
			header_read += n;
		}

		uint8_t pkt_type = header[0];
		uint16_t pkt_len = (static_cast<uint16_t>(header[2]) << 8) | header[3];

		MSSQL_SOCKET_DEBUG_LOG(2, "TLS-TDS Recv: got TDS packet type=0x%02x, total_len=%d", pkt_type, pkt_len);

		if (pkt_type != 0x04 && pkt_type != 0x12) {	 // REPLY or PRELOGIN
			MSSQL_SOCKET_DEBUG_LOG(1, "TLS-TDS Recv: unexpected packet type 0x%02x", pkt_type);
			return -1;
		}

		// Read full payload into temporary buffer
		size_t payload_len = pkt_len - 8;
		std::vector<uint8_t> payload(payload_len);

		size_t payload_read = 0;
		while (payload_read < payload_len) {
			ssize_t n = recv(socket_fd, SOCK_BUF_CAST(payload.data() + payload_read), payload_len - payload_read, 0);
			if (n < 0) {
				if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
					continue;
				}
				MSSQL_SOCKET_DEBUG_LOG(1, "TLS-TDS Recv: payload read error %d (%s)", errno, strerror(errno));
				return -1;
			}
			if (n == 0) {
				MSSQL_SOCKET_DEBUG_LOG(1, "TLS-TDS Recv: connection closed during payload");
				return -1;
			}
			payload_read += n;
		}

		MSSQL_SOCKET_DEBUG_LOG(2, "TLS-TDS Recv: read TDS payload of %zu bytes", payload_len);

		// Copy what we can to the output buffer, store rest in our buffer
		size_t to_copy = std::min(len, payload_len);
		std::memcpy(buf, payload.data(), to_copy);

		if (payload_len > to_copy) {
			// Store extra data in buffer for next call
			recv_buffer.insert(recv_buffer.end(), payload.begin() + to_copy, payload.end());
			MSSQL_SOCKET_DEBUG_LOG(2, "TLS-TDS Recv: buffered %zu extra bytes", payload_len - to_copy);
		}

		MSSQL_SOCKET_DEBUG_LOG(2, "TLS-TDS Recv: returning %zu bytes of TLS data", to_copy);
		return static_cast<int>(to_copy);
	};

	// Set the callbacks
	tls_context_->SetBioCallbacks(std::move(send_cb), std::move(recv_cb));

	// Perform TLS handshake (will use our TDS-wrapped callbacks)
	MSSQL_SOCKET_DEBUG_LOG(1, "EnableTls: performing TDS-wrapped TLS handshake...");
	if (!tls_context_->Handshake(timeout_ms)) {
		last_error_ = "TLS handshake failed: " + tls_context_->GetLastError();
		MSSQL_SOCKET_DEBUG_LOG(1, "EnableTls: FAILED - handshake: %s", last_error_.c_str());
		tls_context_->ClearBioCallbacks();
		tls_context_.reset();
		return false;
	}

	// Handshake complete - clear the TDS-wrapped callbacks
	// After handshake, normal TLS I/O goes directly over the socket
	// (the TLS layer is now established, further TDS packets go through it)
	tls_context_->ClearBioCallbacks();

	MSSQL_SOCKET_DEBUG_LOG(1, "EnableTls: SUCCESS - cipher=%s, version=%s", tls_context_->GetCipherSuite().c_str(),
						   tls_context_->GetTlsVersion().c_str());
	return true;
}

bool TdsSocket::IsTlsEnabled() const {
	return tls_context_ && tls_context_->IsInitialized();
}

std::string TdsSocket::GetTlsCipherSuite() const {
	if (!tls_context_) {
		return "";
	}
	return tls_context_->GetCipherSuite();
}

std::string TdsSocket::GetTlsVersion() const {
	if (!tls_context_) {
		return "";
	}
	return tls_context_->GetTlsVersion();
}

bool TdsSocket::Send(const uint8_t *data, size_t length) {
	if (!IsConnected()) {
		last_error_ = "Not connected";
		return false;
	}

	// Route through TLS if enabled
	if (tls_context_) {
		ssize_t sent = tls_context_->Send(data, length);
		if (sent < 0) {
			last_error_ = "TLS send failed: " + tls_context_->GetLastError();
			return false;
		}
		if (static_cast<size_t>(sent) != length) {
			last_error_ = "TLS send incomplete";
			return false;
		}
		return true;
	}

	// Plain TCP send
	size_t total_sent = 0;
	while (total_sent < length) {
		ssize_t sent = send(fd_, SOCK_BUF_CONST_CAST(data + total_sent), length - total_sent, MSG_NOSIGNAL);
		if (sent <= 0) {
			if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
				// Would block, wait and retry
				if (!WaitForReady(true, 30000)) {
					last_error_ = "Send timeout";
					return false;
				}
				continue;
			}
			last_error_ = "Send failed: " + std::string(strerror(errno));
			connected_ = false;
			return false;
		}
		total_sent += sent;
	}
	return true;
}

bool TdsSocket::Send(const std::vector<uint8_t> &data) {
	return Send(data.data(), data.size());
}

bool TdsSocket::SendPacket(const TdsPacket &packet) {
	std::vector<uint8_t> data = packet.Serialize();
	return Send(data);
}

ssize_t TdsSocket::Receive(uint8_t *buffer, size_t max_length, int timeout_ms) {
	if (!IsConnected()) {
		last_error_ = "Not connected";
		return -1;
	}

	// Route through TLS if enabled
	if (tls_context_) {
		ssize_t received = tls_context_->Receive(buffer, max_length, timeout_ms);
		if (received < 0) {
			last_error_ = "TLS receive failed: " + tls_context_->GetLastError();
			if (tls_context_->GetLastErrorCode() == TlsErrorCode::PEER_CLOSED) {
				connected_ = false;
			}
			return -1;
		}
		return received;
	}

	// Plain TCP receive
	// Wait for data with timeout
	if (!WaitForReady(false, timeout_ms)) {
		// If connected_ was set to false, it's an error not timeout
		if (!connected_) {
			return -1;
		}
		return 0;  // Timeout
	}

	ssize_t received = recv(fd_, SOCK_BUF_CAST(buffer), max_length, 0);
	if (received < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			return 0;  // No data available
		}
		last_error_ = "Receive failed: " + std::string(strerror(errno));
		connected_ = false;
		return -1;
	}
	if (received == 0) {
		last_error_ = "Connection closed by server";
		connected_ = false;
		return -1;
	}

	return received;
}

bool TdsSocket::ReceivePacket(TdsPacket &packet, int timeout_ms) {
	// Read until we have a complete packet
	uint8_t temp_buffer[TDS_DEFAULT_PACKET_SIZE];

	while (true) {
		// Try to parse from existing buffer
		if (receive_buffer_.size() >= TDS_HEADER_SIZE) {
			uint16_t expected_length = TdsPacket::GetPacketLength(receive_buffer_.data());
			if (receive_buffer_.size() >= expected_length) {
				// Have complete packet
				size_t consumed = TdsPacket::Parse(receive_buffer_.data(), receive_buffer_.size(), packet);
				if (consumed > 0) {
					receive_buffer_.erase(receive_buffer_.begin(), receive_buffer_.begin() + consumed);
					return true;
				}
			}
		}

		// Need more data
		ssize_t received = Receive(temp_buffer, sizeof(temp_buffer), timeout_ms);
		if (received <= 0) {
			return false;  // Timeout or error
		}

		receive_buffer_.insert(receive_buffer_.end(), temp_buffer, temp_buffer + received);
	}
}

bool TdsSocket::ReceiveMessage(std::vector<uint8_t> &message, int timeout_ms) {
	message.clear();

	while (true) {
		TdsPacket packet;
		if (!ReceivePacket(packet, timeout_ms)) {
			return false;
		}

		// Append payload
		const auto &payload = packet.GetPayload();
		message.insert(message.end(), payload.begin(), payload.end());

		// Check for end of message
		if (packet.IsEndOfMessage()) {
			return true;
		}
	}
}

bool TdsSocket::SetNonBlocking(bool enable) {
#ifdef _WIN32
	u_long mode = enable ? 1 : 0;
	return ioctlsocket(fd_, FIONBIO, &mode) == 0;
#else
	int flags = fcntl(fd_, F_GETFL, 0);
	if (flags < 0) {
		return false;
	}
	if (enable) {
		flags |= O_NONBLOCK;
	} else {
		flags &= ~O_NONBLOCK;
	}
	return fcntl(fd_, F_SETFL, flags) == 0;
#endif
}

bool TdsSocket::WaitForReady(bool for_write, int timeout_ms) {
	struct pollfd pfd;
	pfd.fd = fd_;
	pfd.events = for_write ? POLLOUT : POLLIN;
	pfd.revents = 0;

	int ret = poll(&pfd, 1, timeout_ms);
	if (ret < 0) {
		last_error_ = "Poll failed: " + std::string(strerror(errno));
		return false;
	}
	if (ret == 0) {
		last_error_ = "Socket timeout waiting for data";
		return false;  // Timeout
	}

	// Check for hard errors
	if (pfd.revents & (POLLERR | POLLNVAL)) {
		last_error_ = "Socket error during poll";
		connected_ = false;
		return false;
	}

	// If we're waiting for read and data is available, allow read even if POLLHUP is set
	// This handles the case where server sends an error response and then closes
	if (!for_write && (pfd.revents & POLLIN)) {
		// Data available to read - allow the read even if POLLHUP is also set
		// After reading, the next poll will detect the closed connection
		return true;
	}

	// No data available and connection closed
	if (pfd.revents & POLLHUP) {
		last_error_ = "Connection closed by server (POLLHUP)";
		connected_ = false;
		return false;
	}

	return true;
}

}  // namespace tds
}  // namespace duckdb
