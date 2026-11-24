#include "tds/tds_socket.hpp"

#include "tds/tds_tls_handshake_transport.hpp"

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
// After winsock2.h, which defines the types it uses -- its own block, so the
// formatter's include sorting cannot move it first (it did: MinGW then knew no
// u_long, MSVC no SOCKET_DEFAULT2_QM_POLICY).
#include <mstcpip.h>  // SIO_KEEPALIVE_VALS
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
	static const int level = []() {
		const char *env = std::getenv("MSSQL_DEBUG");
		return env ? std::atoi(env) : 0;
	}();
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

TdsSocket::~TdsSocket() noexcept {
	try {
		Close();
	} catch (const std::exception &e) {
		// PR #118 review M1: debug-gated stderr surfaces the swallow.
		MSSQL_SOCKET_DEBUG_LOG(1, "~TdsSocket: swallowed exception during Close: %s", e.what());
	} catch (...) {
		MSSQL_SOCKET_DEBUG_LOG(1, "~TdsSocket: swallowed unknown exception during Close");
	}
}

TdsSocket::TdsSocket(TdsSocket &&other) noexcept
	: fd_(other.fd_),
	  host_(std::move(other.host_)),
	  port_(other.port_),
	  connected_(other.connected_),
	  last_error_(std::move(other.last_error_)),
	  tls_context_(std::move(other.tls_context_)),
	  receive_buffer_(std::move(other.receive_buffer_)),
	  receive_len_(other.receive_len_),
	  receive_pos_(other.receive_pos_),
	  recv_read_size_(other.recv_read_size_) {
	other.fd_ = -1;
	other.connected_ = false;
	other.receive_len_ = 0;
	other.receive_pos_ = 0;
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
		receive_len_ = other.receive_len_;
		receive_pos_ = other.receive_pos_;
		recv_read_size_ = other.recv_read_size_;
		other.fd_ = -1;
		other.connected_ = false;
		other.receive_len_ = 0;
		other.receive_pos_ = 0;
	}
	return *this;
}

//! connect() on a non-blocking socket answered that the connection is under way.
static bool ConnectInProgress(int error) {
#ifdef _WIN32
	return error == WSAEWOULDBLOCK;
#else
	return error == EINPROGRESS || error == EWOULDBLOCK;
#endif
}

//! A socket error's text. On Windows strerror() knows no Winsock code ("Unknown error" for every one), so
//! the codes a dial ends on are named as POSIX names them -- the words MSSQLTranslateConnectionError
//! classifies (review of #610).
static std::string SocketErrorText(int error) {
#ifdef _WIN32
	static const struct {
		int code;
		const char *text;
	} NAMES[] = {{WSAECONNREFUSED, "Connection refused"},
				 {WSAETIMEDOUT, "Connection timed out"},
				 {WSAEHOSTUNREACH, "No route to host"},
				 {WSAENETUNREACH, "Network is unreachable"},
				 {WSAEHOSTDOWN, "Host is down"},
				 {WSAEADDRNOTAVAIL, "Cannot assign requested address"},
				 {WSAEAFNOSUPPORT, "Address family not supported by protocol"},
				 {WSAECONNRESET, "Connection reset by peer"}};
	for (const auto &name : NAMES) {
		if (name.code == error) {
			return name.text;
		}
	}
	return "socket error " + std::to_string(error);
#else
	return strerror(error);
#endif
}

//! A dial that failed before reaching the network: this host has no route or address of that family
//! (an AAAA address on a host without IPv6, say). On a name of several addresses it says nothing about
//! the server, so it must not replace what an address actually tried reported (review of #610).
static bool IsLocalDialFailure(int error) {
#ifdef _WIN32
	return error == WSAENETUNREACH || error == WSAEADDRNOTAVAIL || error == WSAEAFNOSUPPORT;
#else
	return error == ENETUNREACH || error == EADDRNOTAVAIL || error == EAFNOSUPPORT;
#endif
}

void TdsSocket::NoteDialError(int error) {
	if (IsLocalDialFailure(error) && !last_error_.empty()) {
		return;
	}
	last_error_ = "Connection failed: " + SocketErrorText(error);
}

static const char *AddressFamilyName(int family) {
	if (family == AF_INET) {
		return "IPv4";
	}
	return family == AF_INET6 ? "IPv6" : "other";
}

bool TdsSocket::AwaitConnect(int addr_index, int timeout_seconds) {
	MSSQL_SOCKET_DEBUG_LOG(2, "Connect: waiting for connection (timeout=%ds)...", timeout_seconds);
	// Wait for connection with timeout. The wait ends on a socket error too -- a refused port is one -- so
	// whether it timed out is asked of the socket, not of the wait: SO_ERROR is 0 when nothing came back in
	// time. (Every failed wait used to be worded "Connection timed out", and a refusal reached the user as
	// advice about firewalls.)
	const bool ready = WaitForReady(true, timeout_seconds * 1000);
	int error = 0;
	socklen_t len = sizeof(error);
	const bool known = getsockopt(fd_, SOL_SOCKET, SO_ERROR, SOCK_OPT_CAST(&error), &len) == 0;
	if (ready && known && error == 0) {
		MSSQL_SOCKET_DEBUG_LOG(1, "Connect: connected to address %d after wait", addr_index);
		return true;
	}
	if (!ready && (!known || error == 0)) {
		last_error_ = "Connection timed out";
		MSSQL_SOCKET_DEBUG_LOG(1, "Connect: timed out on address %d", addr_index);
		return false;
	}
	NoteDialError(error);
	MSSQL_SOCKET_DEBUG_LOG(1, "Connect: getsockopt SO_ERROR=%d for address %d: %s", error, addr_index,
						   last_error_.c_str());
	return false;
}

bool TdsSocket::ConnectAddress(const struct addrinfo &address, int addr_index, int timeout_seconds) {
	MSSQL_SOCKET_DEBUG_LOG(2, "Connect: trying address %d (%s)", addr_index, AddressFamilyName(address.ai_family));

	fd_ = socket(address.ai_family, address.ai_socktype, address.ai_protocol);
	if (fd_ == -1) {
		MSSQL_SOCKET_DEBUG_LOG(2, "Connect: socket() failed for address %d (error=%d)", addr_index, SOCKET_ERROR_CODE);
		return false;
	}

	// Set non-blocking for timeout support
	bool connected = false;
	if (!SetNonBlocking(true)) {
		MSSQL_SOCKET_DEBUG_LOG(2, "Connect: SetNonBlocking failed for address %d", addr_index);
	} else if (connect(fd_, address.ai_addr, address.ai_addrlen) == 0) {
		// Connected immediately
		MSSQL_SOCKET_DEBUG_LOG(1, "Connect: connected immediately to address %d", addr_index);
		connected = true;
	} else {
		const int connect_error = SOCKET_ERROR_CODE;
		MSSQL_SOCKET_DEBUG_LOG(2, "Connect: connect() failed, error code=%d", connect_error);
		if (ConnectInProgress(connect_error)) {
			connected = AwaitConnect(addr_index, timeout_seconds);
		} else {
			// Refused at once (no wait): the same wording as a refusal that arrives after it.
			NoteDialError(connect_error);
			MSSQL_SOCKET_DEBUG_LOG(1, "Connect: connect failed on address %d with error %d", addr_index, connect_error);
		}
	}
	if (!connected) {
		CLOSE_SOCKET(fd_);
		fd_ = -1;
	}
	return connected;
}

void TdsSocket::ConfigureConnectedSocket() {
	// Set TCP_NODELAY for low latency
	int flag = 1;
	setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, SOCK_OPT_CONST_CAST(&flag), sizeof(flag));

	// TCP keepalive (spec 080 PR 3d): a statement may wait as long as it takes
	// for its answer (a pushed INSERT ... SELECT has no deadline), so a peer
	// that is gone -- a dropped network, a restarted server -- must end the
	// wait with an error rather than leave it hanging. Probes after 30 s idle,
	// every 10 s, 6 unanswered: about 90 s to notice. Also keeps an idle pooled
	// connection alive through a NAT / load balancer idle timeout. Best effort:
	// a platform refusing an option keeps its defaults.
	setsockopt(fd_, SOL_SOCKET, SO_KEEPALIVE, SOCK_OPT_CONST_CAST(&flag), sizeof(flag));
	{
#ifdef _WIN32
		struct tcp_keepalive keepalive_values;
		keepalive_values.onoff = 1;
		keepalive_values.keepalivetime = 30000;		 // ms
		keepalive_values.keepaliveinterval = 10000;	 // ms; Windows sends 10 probes
		DWORD returned = 0;
		WSAIoctl(fd_, SIO_KEEPALIVE_VALS, &keepalive_values, sizeof(keepalive_values), nullptr, 0, &returned, nullptr,
				 nullptr);
#else
		int idle = 30;
		int interval = 10;
		int count = 6;
#ifdef __APPLE__
		setsockopt(fd_, IPPROTO_TCP, TCP_KEEPALIVE, &idle, sizeof(idle));
#else
		setsockopt(fd_, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
#endif
		setsockopt(fd_, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval));
		setsockopt(fd_, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof(count));
#endif
	}

#ifdef __APPLE__
	// On macOS, set SO_NOSIGPIPE to prevent SIGPIPE on write to closed socket
	setsockopt(fd_, SOL_SOCKET, SO_NOSIGPIPE, SOCK_OPT_CONST_CAST(&flag), sizeof(flag));
#endif

	// Set back to blocking mode for simpler I/O
	SetNonBlocking(false);
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
	for (rp = result; rp != nullptr && !connected_; rp = rp->ai_next) {
		connected_ = ConnectAddress(*rp, addr_index, timeout_seconds);
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

	ConfigureConnectedSocket();
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
	receive_len_ = 0;
	receive_pos_ = 0;
}

bool TdsSocket::IsConnected() const {
	return connected_ && fd_ >= 0;
}

//===----------------------------------------------------------------------===//
// TlsHandshakeTransport: the handshake's bytes inside PRELOGIN packets
//===----------------------------------------------------------------------===//

TlsHandshakeTransport::TlsHandshakeTransport(int fd, uint8_t &packet_id) : fd_(fd), packet_id_(packet_id) {}

int TlsHandshakeTransport::Buffer(const uint8_t *data, size_t len) {
	MSSQL_SOCKET_DEBUG_LOG(2, "TLS-TDS Send: buffering %zu bytes (buffer now has %zu)", len, send_buffer_.size() + len);
	send_buffer_.insert(send_buffer_.end(), data, data + len);
	return static_cast<int>(len);
}

bool TlsHandshakeTransport::Flush() {
	if (send_buffer_.empty()) {
		return true;
	}

	MSSQL_SOCKET_DEBUG_LOG(2, "TLS-TDS Flush: sending %zu bytes in PRELOGIN packet (id=%d)", send_buffer_.size(),
						   packet_id_);

	// Build TDS PRELOGIN packet with accumulated TLS data
	std::vector<uint8_t> tds_packet;
	tds_packet.reserve(8 + send_buffer_.size());

	// TDS header (8 bytes)
	tds_packet.push_back(0x12);	 // Type = PRELOGIN
	tds_packet.push_back(0x01);	 // Status = EOM (last packet)
	uint16_t total_len = static_cast<uint16_t>(8 + send_buffer_.size());
	tds_packet.push_back((total_len >> 8) & 0xFF);	// Length high
	tds_packet.push_back(total_len & 0xFF);			// Length low
	tds_packet.push_back(0x00);						// SPID high
	tds_packet.push_back(0x00);						// SPID low
	tds_packet.push_back(packet_id_++);				// Packet ID
	tds_packet.push_back(0x00);						// Window

	// TLS payload
	tds_packet.insert(tds_packet.end(), send_buffer_.begin(), send_buffer_.end());
	send_buffer_.clear();

	// Send the wrapped packet
	size_t total_sent = 0;
	while (total_sent < tds_packet.size()) {
		ssize_t sent = send(fd_, SOCK_BUF_CONST_CAST(tds_packet.data() + total_sent), tds_packet.size() - total_sent,
							MSG_NOSIGNAL);
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
}

size_t TlsHandshakeTransport::TakeBuffered(uint8_t *buf, size_t len) {
	const size_t to_copy = std::min(len, recv_buffer_.size() - recv_pos_);
	std::memcpy(buf, recv_buffer_.data() + recv_pos_, to_copy);
	recv_pos_ += to_copy;
	if (recv_pos_ == recv_buffer_.size()) {
		// Fully drained: reset rather than hold the allocation at an
		// ever-growing offset. Capacity is retained for the next packet.
		recv_buffer_.clear();
		recv_pos_ = 0;
	}
	return to_copy;
}

bool TlsHandshakeTransport::ReadFully(uint8_t *to, size_t count, const char *what, const char *closed) {
	size_t read = 0;
	while (read < count) {
		ssize_t n = recv(fd_, SOCK_BUF_CAST(to + read), count - read, 0);
		if (n < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
				continue;
			}
			MSSQL_SOCKET_DEBUG_LOG(1, "TLS-TDS Recv: %s read error %d (%s)", what, errno, strerror(errno));
			return false;
		}
		if (n == 0) {
			MSSQL_SOCKET_DEBUG_LOG(1, "TLS-TDS Recv: %s", closed);
			return false;
		}
		read += n;
	}
	return true;
}

bool TlsHandshakeTransport::ReadPacket() {
	// Read TDS header first (8 bytes)
	uint8_t header[8];
	if (!ReadFully(header, 8, "header", "connection closed by server")) {
		return false;
	}

	uint8_t pkt_type = header[0];
	uint16_t pkt_len = (static_cast<uint16_t>(header[2]) << 8) | header[3];

	MSSQL_SOCKET_DEBUG_LOG(2, "TLS-TDS Recv: got TDS packet type=0x%02x, total_len=%d", pkt_type, pkt_len);

	if (pkt_type != 0x04 && pkt_type != 0x12) {	 // REPLY or PRELOGIN
		MSSQL_SOCKET_DEBUG_LOG(1, "TLS-TDS Recv: unexpected packet type 0x%02x", pkt_type);
		return false;
	}

	// Read the payload straight into the buffer that will hold the remainder.
	// This used to allocate a fresh vector per TDS packet — one malloc, one
	// value-initialisation and one free for every 16 KB frame of the result
	// set, all of it discarded a few lines later.
	if (pkt_len < TDS_HEADER_SIZE) {
		// Before the subtraction, because it is unsigned: a declared length of
		// 3 makes payload_len SIZE_MAX-4, and the resize() below then throws
		// std::length_error from inside a std::function that OpenSSL invokes
		// through a C function pointer. This runs during the handshake, before
		// the peer's certificate is validated.
		MSSQL_SOCKET_DEBUG_LOG(1, "TLS-TDS Recv: packet length %u is shorter than the header", pkt_len);
		return false;
	}
	size_t payload_len = pkt_len - 8;
	recv_buffer_.clear();
	recv_pos_ = 0;
	recv_buffer_.resize(payload_len);
	if (!ReadFully(recv_buffer_.data(), payload_len, "payload", "connection closed during payload")) {
		return false;
	}

	MSSQL_SOCKET_DEBUG_LOG(2, "TLS-TDS Recv: read TDS payload of %zu bytes", payload_len);
	return true;
}

int TlsHandshakeTransport::Receive(uint8_t *buf, size_t len, int timeout_ms) {
	MSSQL_SOCKET_DEBUG_LOG(2, "TLS-TDS Recv: request %zu bytes (recv_buffer=%zu, send_buffer=%zu, timeout=%d)", len,
						   recv_buffer_.size() - recv_pos_, send_buffer_.size(), timeout_ms);

	// Like FreeTDS: flush send buffer before trying to receive
	// This ensures all pending TLS data is sent as a single TDS packet
	if (!Flush()) {
		MSSQL_SOCKET_DEBUG_LOG(1, "TLS-TDS Recv: failed to flush send buffer");
		return -1;
	}

	// First, return data from buffer if we have any
	if (recv_buffer_.size() > recv_pos_) {
		const size_t taken = TakeBuffered(buf, len);
		MSSQL_SOCKET_DEBUG_LOG(2, "TLS-TDS Recv: returned %zu bytes from buffer (%zu remaining)", taken,
							   recv_buffer_.size() - recv_pos_);
		return static_cast<int>(taken);
	}

	// Wait for data with timeout
	struct pollfd pfd;
	pfd.fd = fd_;
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

	if (!ReadPacket()) {
		return -1;
	}

	// Hand out what was asked for; the rest stays where it already is, behind
	// the cursor. The old code copied the remainder into a second buffer,
	// which was a full extra pass over almost every byte of every frame.
	const size_t taken = TakeBuffered(buf, len);
	MSSQL_SOCKET_DEBUG_LOG(2, "TLS-TDS Recv: returning %zu bytes of TLS data", taken);
	return static_cast<int>(taken);
}

//! Why the handshake failed, as the connection reports it.
static std::string HandshakeError(const TlsTdsContext &tls) {
	if (tls.GetLastErrorCode() == TlsErrorCode::CERT_VERIFY_FAILED) {
		// Spec 074 D2: OpenSSL's reason verbatim, and the two ways out by name.
		return tls.GetLastError() +
			   ". Set TrustServerCertificate=yes to accept this server's certificate without "
			   "verification, or HostNameInCertificate=<name> if the certificate is valid but issued "
			   "for a different name";
	}
	return "TLS handshake failed: " + tls.GetLastError();
}

bool TdsSocket::EnableTls(uint8_t &packet_id, int timeout_ms, const std::string &sni_hostname,
						  const TlsOptions &tls_options) {
	// For TDS 7.x, TLS handshake data must be wrapped in TDS PRELOGIN packets
	// See MS-TDS spec: "If encryption was negotiated in TDS 7.x, the TDS client MUST
	// initiate a TLS/SSL handshake, send to the server a TLS/SSL message obtained from
	// the TLS/SSL layer encapsulated in TDS packet(s) of type PRELOGIN (0x12)."

	MSSQL_SOCKET_DEBUG_LOG(1, "EnableTls: starting (timeout=%dms, fd=%d, packet_id=%d)", timeout_ms, fd_, packet_id);

	// Clear any leftover data in receive buffer before TLS
	if (receive_len_ > receive_pos_) {
		MSSQL_SOCKET_DEBUG_LOG(1, "EnableTls: WARNING - clearing %zu leftover bytes in receive buffer",
							   receive_len_ - receive_pos_);
	}
	receive_len_ = 0;
	receive_pos_ = 0;

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
	if (!tls_context_->Initialize(tls_options)) {
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
	auto transport = std::make_shared<TlsHandshakeTransport>(fd_, packet_id);
	TlsSendCallback send_cb = [transport](const uint8_t *data, size_t len) -> int {
		return transport->Buffer(data, len);
	};
	TlsRecvCallback recv_cb = [transport](uint8_t *buf, size_t len, int timeout_ms) -> int {
		return transport->Receive(buf, len, timeout_ms);
	};

	// Set the callbacks
	tls_context_->SetBioCallbacks(std::move(send_cb), std::move(recv_cb));

	// Perform TLS handshake (will use our TDS-wrapped callbacks)
	MSSQL_SOCKET_DEBUG_LOG(1, "EnableTls: performing TDS-wrapped TLS handshake...");
	if (!tls_context_->Handshake(timeout_ms)) {
		last_error_ = HandshakeError(*tls_context_);
		MSSQL_SOCKET_DEBUG_LOG(1, "EnableTls: FAILED - handshake: %s", last_error_.c_str());
		tls_context_->ClearBioCallbacks();
		tls_context_.reset();
		return false;
	}

	// What the handshake wrote last and nothing was read after: in TLS 1.3 the client's Finished. Kept bytes
	// go out when the next read asks, and after the handshake no read comes through here -- it stayed in
	// the buffer, TLS "enabled" and the server still waiting. (TLS 1.2 ends on a read: nothing is kept.)
	if (!transport->Flush()) {
		last_error_ = "TLS handshake failed: its last message could not be sent";
		MSSQL_SOCKET_DEBUG_LOG(1, "EnableTls: FAILED - %s", last_error_.c_str());
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

bool TdsSocket::SendTls(const uint8_t *data, size_t length) {
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

bool TdsSocket::Send(const uint8_t *data, size_t length) {
	if (!IsConnected()) {
		last_error_ = "Not connected";
		return false;
	}

	// Route through TLS if enabled
	if (tls_context_) {
		return SendTls(data, length);
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

ssize_t TdsSocket::ReceiveTls(uint8_t *buffer, size_t max_length, int timeout_ms) {
	ssize_t received = tls_context_->Receive(buffer, max_length, timeout_ms);
	if (received < 0) {
		// Every TLS read error is fatal: after SSL_ERROR_SYSCALL / SSL_ERROR_SSL
		// OpenSSL allows no further I/O on the connection. Left "connected", a
		// caller that waits in slices (mssql::ExecuteDmlBatch: a pushed DML
		// has no deadline) read again at once, forever (issue #456).
		last_error_ = "TLS receive failed: " + tls_context_->GetLastError();
		connected_ = false;
		return -1;
	}
	if (received == 0 && tls_context_->GetLastErrorCode() == TlsErrorCode::PEER_CLOSED) {
		// TlsImpl::Receive answers 0 for a closed peer as it does for a
		// timeout; only the code tells them apart. Closed is an error, as
		// recv() == 0 is on the plain TCP path below.
		last_error_ = "Connection closed by server: " + tls_context_->GetLastError();
		connected_ = false;
		return -1;
	}
	return received;
}

ssize_t TdsSocket::Receive(uint8_t *buffer, size_t max_length, int timeout_ms) {
	if (!IsConnected()) {
		last_error_ = "Not connected";
		return -1;
	}

	// Route through TLS if enabled
	if (tls_context_) {
		return ReceiveTls(buffer, max_length, timeout_ms);
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

void TdsSocket::SetReceiveFraming(uint32_t packet_size, uint32_t frames) {
	if (packet_size < TDS_MIN_PACKET_SIZE) {
		packet_size = static_cast<uint32_t>(TDS_DEFAULT_PACKET_SIZE);
	}
	if (frames == 0) {
		frames = 1;
	}
	const size_t scratch = static_cast<size_t>(packet_size) * frames;
	// Nothing is staged in a scratch buffer any more — recv() reads into the tail
	// of the assembly buffer — so this is only the read granularity.
	recv_read_size_ = scratch;
	receive_len_ = 0;
	receive_pos_ = 0;
	// Sized ONCE, here: one full read plus a whole frame, so a frame split across
	// two reads never forces a growth. resize() rather than reserve() because
	// this vector's size is its capacity — the content length is receive_len_.
	receive_buffer_.resize(scratch + packet_size);
	MSSQL_SOCKET_DEBUG_LOG(1, "SetReceiveFraming: packet_size=%u frames=%u read=%zuB", packet_size, frames, scratch);
}

bool TdsSocket::BufferedPacket(const uint8_t *&head, size_t &packet_length, bool &invalid) {
	invalid = false;
	const size_t buffered = receive_len_ - receive_pos_;
	if (buffered < TDS_HEADER_SIZE) {
		return false;
	}
	head = receive_buffer_.data() + receive_pos_;
	const uint16_t expected_length = TdsPacket::GetPacketLength(head);
	if (expected_length < TDS_HEADER_SIZE) {
		// A frame cannot be shorter than its own header. Rejected HERE
		// rather than by the caller: `buffered >= expected_length` is
		// trivially true for such a length, so returning it would advance
		// the cursor by 0-7 bytes and park the socket on the same bytes
		// forever, reporting a timeout instead of a malformed stream.
		last_error_ =
			"Invalid TDS packet length: " + std::to_string(expected_length) + " is shorter than the 8-byte header";
		// The stream cannot be resynchronised: the connection is done. Left
		// "connected", a caller reading in slices came straight back to
		// these same bytes, forever (issue #456).
		connected_ = false;
		invalid = true;
		return false;
	}
	if (buffered < expected_length) {
		return false;
	}
	packet_length = expected_length;
	receive_pos_ += expected_length;
	return true;
}

void TdsSocket::CompactReceiveBuffer() {
	// Compact before reading more: move the straddling tail to the front so
	// the read always has a full read_size of room. One memmove per read, not
	// one per packet, and it moves at most one frame.
	if (receive_pos_ == 0) {
		return;
	}
	const size_t tail = receive_len_ - receive_pos_;
	if (tail > 0) {
		std::memmove(receive_buffer_.data(), receive_buffer_.data() + receive_pos_, tail);
	}
	receive_len_ = tail;
	receive_pos_ = 0;
}

const uint8_t *TdsSocket::NextPacket(size_t &packet_length, int timeout_ms) {
	// The one frame-assembly loop. Both receive entry points differ only in what
	// they do with the bytes, so the framing lives here once.
	//
	// The returned view points into the assembly buffer and stays valid until the
	// next receive call on this socket: receive_pos_ advances now, but the buffer
	// is only cleared or compacted at the top of the NEXT call.
	while (true) {
		if (receive_pos_ > 0 && receive_pos_ == receive_len_) {
			// Fully drained — rewind instead of walking an ever-growing offset.
			receive_len_ = 0;
			receive_pos_ = 0;
		}

		const uint8_t *head = nullptr;
		bool invalid = false;
		if (BufferedPacket(head, packet_length, invalid)) {
			return head;
		}
		if (invalid) {
			return nullptr;
		}

		CompactReceiveBuffer();
		if (!FillReceiveBuffer(timeout_ms)) {
			return nullptr;
		}
	}
}

bool TdsSocket::ReceivePacket(TdsPacket &packet, int timeout_ms) {
	size_t packet_length = 0;
	const uint8_t *head = NextPacket(packet_length, timeout_ms);
	if (head == nullptr) {
		return false;  // Timeout or error
	}
	// Parse validates the length and throws on a malformed header, which is the
	// behaviour the pre-login callers rely on.
	return TdsPacket::Parse(head, packet_length, packet) > 0;
}

bool TdsSocket::ReceivePayloadView(const uint8_t *&payload, size_t &payload_length, int timeout_ms) {
	// The streaming read path needs the payload bytes in the token parser and
	// nothing else from the packet. ReceivePacket copied them into
	// TdsPacket::payload_ so the caller could copy them again — two passes over
	// every byte of the result set to move it eight bytes to the left. Handing
	// back a view leaves exactly one copy, the one the parser genuinely needs to
	// assemble tokens across packet boundaries.
	size_t packet_length = 0;
	const uint8_t *head = NextPacket(packet_length, timeout_ms);
	if (head == nullptr) {
		return false;
	}
	if (packet_length < TDS_HEADER_SIZE || packet_length > TDS_MAX_PACKET_SIZE) {
		last_error_ = "Invalid TDS packet length";
		return false;
	}
	payload = head + TDS_HEADER_SIZE;
	payload_length = packet_length - TDS_HEADER_SIZE;
	return true;
}

bool TdsSocket::FillReceiveBuffer(int timeout_ms) {
	// recv() straight into the tail of the assembly buffer, with no resize.
	//
	// Two passes over the read buffer used to happen here for no reason: a
	// scratch buffer that was immediately insert()ed into this one, and then —
	// once that was removed — a resize() that value-initialised the very bytes
	// recv() was about to write. The vector is sized once in SetReceiveFraming
	// and `receive_len_` carries the content length instead.
	const size_t room = receive_buffer_.size() - receive_len_;
	if (room < recv_read_size_) {
		// Only reachable before SetReceiveFraming has run (PRELOGIN/LOGIN7 on the
		// default frame size) or if a peer sent a frame larger than negotiated.
		receive_buffer_.resize(receive_len_ + recv_read_size_);
	}
	const ssize_t received = Receive(receive_buffer_.data() + receive_len_, recv_read_size_, timeout_ms);
	if (received <= 0) {
		return false;
	}
	receive_len_ += static_cast<size_t>(received);
	return true;
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
