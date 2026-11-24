//===----------------------------------------------------------------------===//
//                         DuckDB
//
// tds/tds_tls_handshake_transport.hpp
//
// The TLS handshake of TDS 7.x travels inside TDS packets of type PRELOGIN (MS-TDS: "the TDS client MUST
// initiate a TLS/SSL handshake, send to the server a TLS/SSL message obtained from the TLS/SSL layer
// encapsulated in TDS packet(s) of type PRELOGIN (0x12)"). This is what carries it: the bytes OpenSSL
// writes are kept and sent as one packet, the packets the server answers with are unwrapped and handed
// out in the pieces OpenSSL asks for. Once the handshake is done TLS runs over the socket directly, and
// this is not used again. No DuckDB, no OpenSSL: a socket and bytes (defined in tds_socket.cpp, with the
// platform's socket calls).
//===----------------------------------------------------------------------===//

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace duckdb {
namespace tds {

class TlsHandshakeTransport {
public:
	//! Over the connected socket `fd`; `packet_id` is the connection's, advanced by every packet sent.
	TlsHandshakeTransport(int fd, uint8_t &packet_id);

	//! TLS bytes to send: kept until the next Receive (or Flush), which sends all of them as one PRELOGIN
	//! packet, as FreeTDS does. Returns `len`.
	int Buffer(const uint8_t *data, size_t len);
	//! What Buffer kept, sent as one PRELOGIN packet (nothing kept: nothing sent). False on a socket error.
	bool Flush();
	//! Up to `len` bytes of the server's TLS data into `buf`, after sending what Buffer kept: what is left
	//! of the last packet, else the next packet's payload (the rest of it kept for the next call). The
	//! count, 0 when no whole packet arrives within `timeout_ms` (one that arrived in part is kept, and the
	//! next call reads on: a peer that stops inside a packet costs the timeout, not forever), -1 on an
	//! error: a socket error, the peer closing, a packet that is neither a reply nor a PRELOGIN, a declared
	//! length shorter than the header.
	//!
	//! Past the deadline (SetDeadline) nothing more is waited for: 0, whatever `timeout_ms` says.
	int Receive(uint8_t *buf, size_t len, int timeout_ms);

	//! The instant the whole handshake has to be done by. Each Receive is handed the handshake's timeout
	//! anew, counted from that call: without this a peer that sends a little just inside every timeout is
	//! never late -- a byte a call, and the handshake lasts as long as it has bytes to send.
	void SetDeadline(std::chrono::steady_clock::time_point deadline) {
		deadline_ = deadline;
		has_deadline_ = true;
	}

	//! The bytes kept to send, and those of the last packet not handed out yet (for the tests).
	size_t PendingSendBytes() const {
		return send_buffer_.size();
	}
	size_t PendingReceiveBytes() const {
		return in_payload_ ? 0 : recv_buffer_.size() - recv_pos_;
	}

private:
	//! What is left of the last packet, up to `len` bytes; the buffer reset once drained.
	size_t TakeBuffered(uint8_t *buf, size_t len);
	//! Reads on until `to` holds `count` bytes, `filled_` of them there already: 1 when it does, 0 when the
	//! time is up first (`timeout_ms` counted from `start`; negative: no limit), -1 on an error or the peer
	//! closing (`what` and `closed` word the debug log).
	int ReadSome(uint8_t *to, size_t count, std::chrono::steady_clock::time_point start, int timeout_ms,
				 const char *what, const char *closed);
	//! The next packet's payload into the receive buffer, read on from where the last call left it: 1 when
	//! it is whole, 0 when `timeout_ms` passes first, -1 as Receive's -1.
	int ReadPacket(int timeout_ms);

	int fd_;
	uint8_t &packet_id_;
	std::vector<uint8_t> send_buffer_;
	// Drained with a cursor rather than erase(): erase() from the front memmoves the whole remainder on
	// every call, and OpenSSL's first call of each record asks for five bytes.
	std::vector<uint8_t> recv_buffer_;
	size_t recv_pos_ = 0;
	// The packet being read, kept across calls: its header until that is whole, then its payload straight
	// into recv_buffer_ (`in_payload_`: sized for it and not whole yet, so nothing of it is handed out);
	// `filled_` is how much of the part being read has arrived.
	uint8_t header_[8] = {};
	bool in_payload_ = false;
	size_t filled_ = 0;
	std::chrono::steady_clock::time_point deadline_;
	bool has_deadline_ = false;
};

}  // namespace tds
}  // namespace duckdb
