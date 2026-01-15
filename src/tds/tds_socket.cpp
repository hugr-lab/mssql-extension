#include "tds/tds_socket.hpp"

#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <cerrno>
#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#define CLOSE_SOCKET closesocket
#define SOCKET_ERROR_CODE WSAGetLastError()
#else
#define CLOSE_SOCKET close
#define SOCKET_ERROR_CODE errno
#endif

namespace duckdb {
namespace tds {

TdsSocket::TdsSocket()
    : fd_(-1),
      port_(0),
      connected_(false) {
}

TdsSocket::~TdsSocket() {
	Close();
}

TdsSocket::TdsSocket(TdsSocket&& other) noexcept
    : fd_(other.fd_),
      host_(std::move(other.host_)),
      port_(other.port_),
      connected_(other.connected_),
      last_error_(std::move(other.last_error_)),
      receive_buffer_(std::move(other.receive_buffer_)) {
	other.fd_ = -1;
	other.connected_ = false;
}

TdsSocket& TdsSocket::operator=(TdsSocket&& other) noexcept {
	if (this != &other) {
		Close();
		fd_ = other.fd_;
		host_ = std::move(other.host_);
		port_ = other.port_;
		connected_ = other.connected_;
		last_error_ = std::move(other.last_error_);
		receive_buffer_ = std::move(other.receive_buffer_);
		other.fd_ = -1;
		other.connected_ = false;
	}
	return *this;
}

bool TdsSocket::Connect(const std::string& host, uint16_t port, int timeout_seconds) {
	if (connected_) {
		Close();
	}

	host_ = host;
	port_ = port;
	last_error_.clear();

	// Resolve hostname
	struct addrinfo hints, *result, *rp;
	std::memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;     // Allow IPv4 or IPv6
	hints.ai_socktype = SOCK_STREAM; // TCP
	hints.ai_flags = 0;
	hints.ai_protocol = IPPROTO_TCP;

	std::string port_str = std::to_string(port);
	int ret = getaddrinfo(host.c_str(), port_str.c_str(), &hints, &result);
	if (ret != 0) {
		last_error_ = "Failed to resolve hostname: " + std::string(gai_strerror(ret));
		return false;
	}

	// Try each address until we connect
	for (rp = result; rp != nullptr; rp = rp->ai_next) {
		fd_ = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
		if (fd_ == -1) {
			continue;
		}

		// Set non-blocking for timeout support
		if (!SetNonBlocking(true)) {
			CLOSE_SOCKET(fd_);
			fd_ = -1;
			continue;
		}

		// Attempt connection
		ret = connect(fd_, rp->ai_addr, rp->ai_addrlen);
		if (ret == 0) {
			// Connected immediately
			connected_ = true;
			break;
		}

		if (errno == EINPROGRESS || errno == EWOULDBLOCK) {
			// Wait for connection with timeout
			if (WaitForReady(true, timeout_seconds * 1000)) {
				// Check if connection succeeded
				int error = 0;
				socklen_t len = sizeof(error);
				if (getsockopt(fd_, SOL_SOCKET, SO_ERROR, &error, &len) == 0 && error == 0) {
					connected_ = true;
					break;
				}
				last_error_ = "Connection failed: " + std::string(strerror(error));
			} else {
				last_error_ = "Connection timed out";
			}
		}

		CLOSE_SOCKET(fd_);
		fd_ = -1;
	}

	freeaddrinfo(result);

	if (!connected_) {
		if (last_error_.empty()) {
			last_error_ = "Failed to connect to " + host + ":" + std::to_string(port);
		}
		return false;
	}

	// Set TCP_NODELAY for low latency
	int flag = 1;
	setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

	// Set back to blocking mode for simpler I/O
	SetNonBlocking(false);

	return true;
}

void TdsSocket::Close() {
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

bool TdsSocket::Send(const uint8_t* data, size_t length) {
	if (!IsConnected()) {
		last_error_ = "Not connected";
		return false;
	}

	size_t total_sent = 0;
	while (total_sent < length) {
		ssize_t sent = send(fd_, data + total_sent, length - total_sent, 0);
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

bool TdsSocket::Send(const std::vector<uint8_t>& data) {
	return Send(data.data(), data.size());
}

bool TdsSocket::SendPacket(const TdsPacket& packet) {
	std::vector<uint8_t> data = packet.Serialize();
	return Send(data);
}

ssize_t TdsSocket::Receive(uint8_t* buffer, size_t max_length, int timeout_ms) {
	if (!IsConnected()) {
		last_error_ = "Not connected";
		return -1;
	}

	// Wait for data with timeout
	if (!WaitForReady(false, timeout_ms)) {
		return 0;  // Timeout
	}

	ssize_t received = recv(fd_, buffer, max_length, 0);
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

bool TdsSocket::ReceivePacket(TdsPacket& packet, int timeout_ms) {
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

bool TdsSocket::ReceiveMessage(std::vector<uint8_t>& message, int timeout_ms) {
	message.clear();

	while (true) {
		TdsPacket packet;
		if (!ReceivePacket(packet, timeout_ms)) {
			return false;
		}

		// Append payload
		const auto& payload = packet.GetPayload();
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
		return false;  // Timeout
	}

	// Check for errors
	if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
		last_error_ = "Socket error during poll";
		connected_ = false;
		return false;
	}

	return true;
}

}  // namespace tds
}  // namespace duckdb
