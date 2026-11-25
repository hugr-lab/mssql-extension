// test/cpp/test_tds_socket.cpp
//
// TdsSocket against loopback peers, with no SQL Server: the dial (Connect), plain send and receive, the
// frame assembly behind ReceivePacket / ReceiveMessage, the TLS handshake carried in PRELOGIN packets
// (EnableTls, against an in-process OpenSSL server that speaks the same framing) and the bytes after it,
// and TlsHandshakeTransport on its own over a socket pair.
//
// Every observation is also printed as a `note:` line, ports replaced. Built with -DTDS_SOCKET_TEST_NO_TRANSPORT
// against the code as it was before TdsSocket was split, the notes were the same but for the two regressions
// marked below, which that code fails (how the split was checked).
//
// Part of STANDALONE_TEST_SOURCES (`make test-cpp`), which CI runs.

#ifdef _WIN32
#include <cstdio>
int main() {
	std::printf("test_tds_socket: not built on Windows (CI runs the standalone list on POSIX)\n");
	return 0;
}
#else

#include <openssl/bn.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <csignal>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <iostream>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "tds/tds_packet.hpp"
#include "tds/tds_socket.hpp"
#ifndef TDS_SOCKET_TEST_NO_TRANSPORT
#include "tds/tds_tls_handshake_transport.hpp"
#endif

using duckdb::tds::TdsPacket;
using duckdb::tds::TdsSocket;
using duckdb::tds::TlsOptions;

namespace {

int failures = 0;
uint16_t noted_port = 0;  // replaced by <port> in the notes

void Check(bool ok, const std::string &what) {
	std::cout << (ok ? "  ok   " : "  FAIL ") << what << std::endl;
	if (!ok) {
		failures++;
	}
}

void Note(const std::string &label, std::string value) {
	const std::string port = std::to_string(noted_port);
	for (size_t at = value.find(port); noted_port && at != std::string::npos; at = value.find(port)) {
		value.replace(at, port.size(), "<port>");
	}
	std::cout << "note: " << label << " = " << value << std::endl;
}

using Bytes = std::vector<uint8_t>;

bool WriteAll(int fd, const uint8_t *data, size_t length) {
	size_t done = 0;
	while (done < length) {
		const ssize_t n = ::send(fd, data + done, length - done, 0);
		if (n <= 0) {
			return false;
		}
		done += static_cast<size_t>(n);
	}
	return true;
}
bool WriteAll(int fd, const Bytes &b) {
	return WriteAll(fd, b.data(), b.size());
}

//! `length` bytes, or fewer when the peer closes first.
Bytes ReadUpTo(int fd, size_t length) {
	Bytes out(length);
	size_t done = 0;
	while (done < length) {
		const ssize_t n = ::recv(fd, out.data() + done, length - done, 0);
		if (n <= 0) {
			break;
		}
		done += static_cast<size_t>(n);
	}
	out.resize(done);
	return out;
}

//! A TDS packet: type, status, the payload's length in its header.
Bytes Packet(uint8_t type, uint8_t status, const Bytes &payload, uint8_t id = 1) {
	const size_t total = 8 + payload.size();
	Bytes p = {type, status, static_cast<uint8_t>(total >> 8), static_cast<uint8_t>(total), 0, 0, id, 0};
	p.insert(p.end(), payload.begin(), payload.end());
	return p;
}

Bytes Pattern(size_t length, uint8_t seed) {
	Bytes b(length);
	for (size_t i = 0; i < length; i++) {
		b[i] = static_cast<uint8_t>(seed + i * 7);
	}
	return b;
}

//! A listening loopback socket on a port the system picks.
struct Listener {
	int fd = -1;
	uint16_t port = 0;

	Listener() {
		fd = ::socket(AF_INET, SOCK_STREAM, 0);
		sockaddr_in addr;
		std::memset(&addr, 0, sizeof(addr));
		addr.sin_family = AF_INET;
		addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		addr.sin_port = 0;
		socklen_t len = sizeof(addr);
		if (fd < 0 || ::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0 || ::listen(fd, 8) != 0 ||
			::getsockname(fd, reinterpret_cast<sockaddr *>(&addr), &len) != 0) {
			std::cerr << "listener: could not listen on loopback" << std::endl;
			std::exit(2);
		}
		port = ntohs(addr.sin_port);
	}
	~Listener() {
		Close();
	}
	void Close() {
		if (fd >= 0) {
			::close(fd);
			fd = -1;
		}
	}
	int Accept() const {
		return ::accept(fd, nullptr, nullptr);
	}
};

//! A connected pair: the TdsSocket under test and the peer's descriptor.
struct Pair {
	Listener listener;
	TdsSocket socket;
	int peer = -1;

	Pair() {
		noted_port = listener.port;
		const bool connected = socket.Connect("127.0.0.1", listener.port, 5);
		peer = connected ? listener.Accept() : -1;
		if (peer < 0) {
			std::cerr << "pair: could not connect on loopback: " << socket.GetLastError() << std::endl;
			std::exit(2);
		}
	}
	~Pair() {
		ClosePeer();
	}
	void ClosePeer() {
		if (peer >= 0) {
			::close(peer);
			peer = -1;
		}
	}
};

//------------------------------------------------------------------------------
// Connect
//------------------------------------------------------------------------------
void TestConnect() {
	std::cout << "Connect" << std::endl;
	{
		Listener listener;
		noted_port = listener.port;
		TdsSocket socket;
		Check(!socket.IsConnected() && socket.GetSocketFd() == -1, "a new socket is not connected");
		const bool connected = socket.Connect("127.0.0.1", listener.port, 5);
		const int peer = listener.Accept();
		Check(connected && socket.IsConnected() && peer >= 0, "a listening loopback port is connected to");
		Check(socket.GetHost() == "127.0.0.1" && socket.GetPort() == listener.port && socket.GetSocketFd() >= 0,
			  "the socket keeps the host and the port it dialled");
		Note("connect ok error", socket.GetLastError());

		// Connecting again closes the first connection: its peer reads the end.
		Listener second;
		const bool again = socket.Connect("localhost", second.port, 5);
		const int second_peer = second.Accept();
		Check(again && socket.IsConnected() && second_peer >= 0 && socket.GetHost() == "localhost" &&
				  socket.GetPort() == second.port,
			  "a connected socket connects elsewhere, by name");
		Check(ReadUpTo(peer, 1).empty(), "and the first connection's peer sees it closed");
		socket.Close();
		Check(!socket.IsConnected() && socket.GetSocketFd() == -1, "Close leaves no descriptor");
		Check(ReadUpTo(second_peer, 1).empty(), "Close closes the connection");
		socket.Close();
		Check(!socket.IsConnected(), "closing twice is harmless");
		::close(peer);
		::close(second_peer);
	}
	{
		// A port nothing listens on: taken from a listener that is then closed.
		uint16_t port;
		{
			Listener listener;
			port = listener.port;
		}
		noted_port = port;
		TdsSocket socket;
		const auto start = std::chrono::steady_clock::now();
		const bool connected = socket.Connect("127.0.0.1", port, 5);
		const auto took =
			std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
		const std::string error = socket.GetLastError();
		Check(!connected && !socket.IsConnected() && socket.GetSocketFd() == -1, "a refused port is not connected to");
		Check(took < 3000, "and is refused at once, not after the timeout");
		{
			// The same port by name: localhost may resolve to ::1 and 127.0.0.1, every address tried; what is
			// reported is a refusal, not what a later address failed with locally (review of #610).
			TdsSocket by_name;
			const bool named = by_name.Connect("localhost", port, 5);
			Check(!named && by_name.GetLastError() == std::string("Connection failed: ") + std::strerror(ECONNREFUSED),
				  "by name too, the refusal is the reason (" + by_name.GetLastError() + ")");
		}
		// Regression: the wait for the connection sees the refusal as a socket error, and Connect used to
		// word every failed wait as "Connection timed out" -- which the ATTACH error then turned into advice
		// about firewalls. The socket's own error is the reason.
		Check(error == std::string("Connection failed: ") + std::strerror(ECONNREFUSED),
			  "and the error is the refusal, not a timeout (" + error + ")");
		Note("refused", error);
	}
	{
		TdsSocket socket;
		const bool connected = socket.Connect("no-such-host.invalid", 1433, 5);
		Check(!connected && !socket.IsConnected() && socket.GetLastError().find("Failed to resolve hostname: ") == 0,
			  "a name that does not resolve is reported as such (" + socket.GetLastError() + ")");
		Check(socket.GetHost() == "no-such-host.invalid" && socket.GetPort() == 1433,
			  "the host and port asked for are kept after a failure");
	}
	{
		// A failure after a success: the error of the attempt, not the connection kept.
		Listener listener;
		noted_port = listener.port;
		TdsSocket socket;
		const bool first = socket.Connect("127.0.0.1", listener.port, 5);
		const bool second = socket.Connect("no-such-host.invalid", 1433, 5);
		Check(first && !second && !socket.IsConnected(), "a failed reconnect leaves the socket closed");
	}
}

//------------------------------------------------------------------------------
// Plain send and receive
//------------------------------------------------------------------------------
void TestPlainIo() {
	std::cout << "Send / Receive" << std::endl;
	{
		Pair pair;
		const Bytes out = Pattern(100000, 3);  // more than a socket buffer takes at once
		Bytes got;
		std::thread reader([&]() { got = ReadUpTo(pair.peer, out.size()); });
		const bool sent = pair.socket.Send(out);
		if (!sent) {
			pair.socket.Close();  // the reader waits for every byte: end it with the connection (review of #610)
		}
		reader.join();
		Check(sent && got == out, "Send delivers every byte, in order");
		Check(pair.socket.Send(nullptr, 0), "an empty Send succeeds");

		uint8_t buffer[64];
		Check(pair.socket.Receive(buffer, sizeof(buffer), 50) == 0 && pair.socket.IsConnected(),
			  "Receive with nothing to read times out with 0, the connection kept");
		Note("receive timeout error", pair.socket.GetLastError());

		const Bytes in = Pattern(40, 9);
		WriteAll(pair.peer, in);
		const ssize_t n = pair.socket.Receive(buffer, sizeof(buffer), 2000);
		Check(n == 40 && Bytes(buffer, buffer + 40) == in, "Receive returns what the peer sent");
		WriteAll(pair.peer, Pattern(10, 1));
		Check(pair.socket.Receive(buffer, 4, 2000) == 4 && pair.socket.Receive(buffer, 64, 2000) == 6,
			  "Receive hands out no more than asked, the rest on the next call");

		pair.ClosePeer();
		const ssize_t closed = pair.socket.Receive(buffer, sizeof(buffer), 2000);
		Check(closed == -1 && !pair.socket.IsConnected(), "the peer closing is -1, and the socket no longer connected");
		Note("peer closed", pair.socket.GetLastError());
		Check(pair.socket.GetLastError().find("Connection closed by server") == 0, "and says the server closed it");
		Check(!pair.socket.Send(out) && pair.socket.GetLastError() == "Not connected",
			  "Send on it afterwards is refused as not connected");
		Check(pair.socket.Receive(buffer, sizeof(buffer), 10) == -1 && pair.socket.GetLastError() == "Not connected",
			  "and so is Receive");
	}
	{
		TdsSocket socket;
		uint8_t byte = 0;
		Check(!socket.Send(&byte, 1) && socket.GetLastError() == "Not connected" && socket.Receive(&byte, 1, 10) == -1,
			  "a socket never connected sends and receives nothing");
		TdsPacket packet;
		Check(!socket.ReceivePacket(packet, 10), "nor a packet");
	}
}

//------------------------------------------------------------------------------
// Frame assembly: ReceivePacket, ReceivePayloadView, ReceiveMessage
//------------------------------------------------------------------------------
void TestFraming() {
	std::cout << "Packets" << std::endl;
	{
		Pair pair;
		TdsPacket packet;
		Check(!pair.socket.ReceivePacket(packet, 50) && pair.socket.IsConnected(),
			  "no packet within the timeout: false, the connection kept");

		// Two packets in one write, then one split across two writes.
		const Bytes a = Pattern(20, 1), b = Pattern(300, 2), c = Pattern(5000, 3);
		Bytes both = Packet(0x04, 0x00, a);
		const Bytes second = Packet(0x04, 0x01, b);
		both.insert(both.end(), second.begin(), second.end());
		WriteAll(pair.peer, both);
		const Bytes third = Packet(0x04, 0x01, c);
		std::thread writer([&]() {
			WriteAll(pair.peer, third.data(), 3);  // not even a header
			std::this_thread::sleep_for(std::chrono::milliseconds(30));
			WriteAll(pair.peer, third.data() + 3, 2000);
			std::this_thread::sleep_for(std::chrono::milliseconds(30));
			WriteAll(pair.peer, third.data() + 2003, third.size() - 2003);
		});
		TdsPacket p1, p2, p3;
		const bool r1 = pair.socket.ReceivePacket(p1, 2000);
		const bool r2 = pair.socket.ReceivePacket(p2, 2000);
		const bool r3 = pair.socket.ReceivePacket(p3, 2000);
		writer.join();
		Check(r1 && p1.GetPayload() == a && !p1.IsEndOfMessage(), "the first of two packets read at once");
		Check(r2 && p2.GetPayload() == b && p2.IsEndOfMessage(), "the second, from what the first read left");
		Check(r3 && p3.GetPayload() == c, "a packet arriving in three pieces is assembled");

		// A packet one byte short is not a packet yet, nor is a header of seven bytes.
		const Bytes d = Pattern(64, 8);
		const Bytes fourth = Packet(0x04, 0x01, d);
		TdsPacket p4;
		WriteAll(pair.peer, fourth.data(), 7);
		const bool header_short = pair.socket.ReceivePacket(p4, 100);
		WriteAll(pair.peer, fourth.data() + 7, fourth.size() - 8);
		const bool byte_short = pair.socket.ReceivePacket(p4, 100);
		Check(!header_short && !byte_short && pair.socket.IsConnected(),
			  "seven bytes of a header, then a packet less its last byte: no packet yet, the connection kept");
		WriteAll(pair.peer, fourth.data() + fourth.size() - 1, 1);
		Check(pair.socket.ReceivePacket(p4, 2000) && p4.GetPayload() == d, "its last byte completes it");

		// A message of three packets, and the view of a payload.
		const Bytes m1 = Pattern(100, 4), m2 = Pattern(200, 5), m3 = Pattern(7, 6);
		WriteAll(pair.peer, Packet(0x04, 0x00, m1));
		WriteAll(pair.peer, Packet(0x04, 0x00, m2));
		WriteAll(pair.peer, Packet(0x04, 0x01, m3));
		Bytes message;
		Bytes expected = m1;
		expected.insert(expected.end(), m2.begin(), m2.end());
		expected.insert(expected.end(), m3.begin(), m3.end());
		Check(pair.socket.ReceiveMessage(message, 2000) && message == expected,
			  "ReceiveMessage joins the payloads up to the end-of-message packet");
		WriteAll(pair.peer, Packet(0x04, 0x01, m2));
		const uint8_t *view = nullptr;
		size_t view_length = 0;
		Check(pair.socket.ReceivePayloadView(view, view_length, 2000) && view_length == m2.size() &&
				  Bytes(view, view + view_length) == m2,
			  "ReceivePayloadView is the payload, without its header");
		WriteAll(pair.peer, Packet(0x04, 0x01, Bytes()));
		TdsPacket empty;
		Check(pair.socket.ReceivePacket(empty, 2000) && empty.GetPayload().empty(), "a packet of a header alone");

		// A declared length shorter than the header ends the connection.
		const Bytes bad = {0x04, 0x01, 0x00, 0x03, 0, 0, 1, 0};
		WriteAll(pair.peer, bad);
		TdsPacket refused;
		Check(!pair.socket.ReceivePacket(refused, 2000) && !pair.socket.IsConnected(),
			  "a packet length shorter than the header is refused, and the connection done");
		Note("short packet", pair.socket.GetLastError());
		Check(pair.socket.GetLastError() == "Invalid TDS packet length: 3 is shorter than the 8-byte header",
			  "naming the length");
	}
	{
		// The peer closing in the middle of a packet.
		Pair pair;
		const Bytes whole = Packet(0x04, 0x01, Pattern(100, 1));
		WriteAll(pair.peer, whole.data(), 50);
		pair.ClosePeer();
		TdsPacket packet;
		Check(!pair.socket.ReceivePacket(packet, 2000) && !pair.socket.IsConnected(),
			  "the peer closing inside a packet: no packet, the socket no longer connected");
		Note("closed inside a packet", pair.socket.GetLastError());
	}
	// Many packets of every size, written in pieces that ignore packet borders, under several framings.
	const uint32_t framings[][2] = {{0, 0}, {512, 1}, {4096, 4}, {16384, 8}, {32767, 2}};
	for (const auto &framing : framings) {
		Pair pair;
		if (framing[0] || framing[1]) {
			pair.socket.SetReceiveFraming(framing[0], framing[1]);
		}
		std::mt19937 rng(framing[0] * 31 + framing[1]);
		std::vector<Bytes> payloads;
		Bytes stream;
		for (int i = 0; i < 400; i++) {
			const size_t size = i % 50 == 0 ? 32759 : (i % 7 == 0 ? 0 : rng() % 3000);
			payloads.push_back(Pattern(size, static_cast<uint8_t>(i)));
			const Bytes p = Packet(0x04, i % 3 == 0 ? 0x01 : 0x00, payloads.back(), static_cast<uint8_t>(i));
			stream.insert(stream.end(), p.begin(), p.end());
		}
		std::thread writer([&]() {
			std::mt19937 cuts(7);
			size_t at = 0;
			while (at < stream.size()) {
				const size_t piece = std::min<size_t>(stream.size() - at, 1 + cuts() % 9000);
				if (!WriteAll(pair.peer, stream.data() + at, piece)) {
					return;
				}
				at += piece;
			}
		});
		size_t matched = 0;
		for (size_t i = 0; i < payloads.size(); i++) {
			TdsPacket packet;
			if (!pair.socket.ReceivePacket(packet, 5000) || packet.GetPayload() != payloads[i] ||
				packet.IsEndOfMessage() != (i % 3 == 0)) {
				break;
			}
			matched++;
		}
		if (matched != payloads.size()) {
			// The writer blocks on a full socket nobody reads: closing it fails the writer's sends, so the
			// failure is a FAIL line and not a hung run (review of #610).
			pair.socket.Close();
		}
		writer.join();
		Check(matched == payloads.size(), "400 packets in arbitrary pieces, framing " + std::to_string(framing[0]) +
											  " x " + std::to_string(framing[1]) + ": " + std::to_string(matched) +
											  " read as sent");
	}
}

//------------------------------------------------------------------------------
// EnableTls: the handshake inside PRELOGIN packets, against an OpenSSL server
//------------------------------------------------------------------------------
struct ServerIdentity {
	EVP_PKEY *key = nullptr;
	X509 *cert = nullptr;

	ServerIdentity() {
		key = EVP_EC_gen("P-256");
		cert = X509_new();
		X509_set_version(cert, 2);
		ASN1_INTEGER_set(X509_get_serialNumber(cert), 1);
		X509_gmtime_adj(X509_getm_notBefore(cert), -60);
		X509_gmtime_adj(X509_getm_notAfter(cert), 3600);
		X509_NAME *name = X509_get_subject_name(cert);
		X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
								   reinterpret_cast<const unsigned char *>("sql.test.invalid"), -1, -1, 0);
		X509_set_issuer_name(cert, name);
		X509_set_pubkey(cert, key);
		if (!key || !X509_sign(cert, key, EVP_sha256())) {
			std::cerr << "tls: could not make a certificate" << std::endl;
			std::exit(2);
		}
	}
	~ServerIdentity() {
		X509_free(cert);
		EVP_PKEY_free(key);
	}
};

//! How the server misbehaves, for the refusals.
enum class ServerFault {
	None,
	WrongPacketType,
	ShortLength,
	CloseAfterHello,
	CloseInsidePayload,
	Silent,
	StallInsidePacket,
	// Its answer a byte a packet, one every 200 ms: never silent for a whole timeout of 300.
	Drip,
	// A good handshake, then every answer 300 ms late.
	SlowEcho,
	// After a good handshake: TLS closed in order (close_notify), or the socket dropped without it.
	ShutdownAfterHandshake,
	DropAfterHandshake
};

//! What the server saw of the client's handshake packets.
struct ServerSeen {
	std::vector<uint8_t> types, statuses, ids;
	bool handshake_done = false;
	Bytes application;	// what the client sent after the handshake, decrypted
};

//! The server's side of one connection: TLS through memory BIOs, each flight wrapped in a TDS packet as SQL
//! Server wraps it; after the handshake TLS runs on the socket itself, and the server answers what it reads
//! with every byte incremented.
void ServeTls(int fd, const ServerIdentity &identity, ServerFault fault, size_t reply_pieces, int max_version,
			  ServerSeen &seen) {
	SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
	SSL_CTX_use_certificate(ctx, identity.cert);
	SSL_CTX_use_PrivateKey(ctx, identity.key);
	// SQL Server wraps a TLS 1.2 handshake (it takes TLS 1.3 under TDS 8, whose handshake is not wrapped). A
	// TLS 1.3 handshake differs in who speaks last: the client, whose Finished nothing is read after.
	SSL_CTX_set_max_proto_version(ctx, max_version);
	SSL_CTX_set_num_tickets(ctx, 0);  // no post-handshake messages: after the handshake TLS is on the socket
	SSL *ssl = SSL_new(ctx);
	BIO *in = BIO_new(BIO_s_mem());
	BIO *out = BIO_new(BIO_s_mem());
	SSL_set_bio(ssl, in, out);
	SSL_set_accept_state(ssl);

	bool alive = true;
	while (alive && !seen.handshake_done) {
		const Bytes header = ReadUpTo(fd, 8);
		if (header.size() != 8) {
			break;
		}
		const size_t length = (static_cast<size_t>(header[2]) << 8) | header[3];
		const Bytes payload = ReadUpTo(fd, length - 8);
		seen.types.push_back(header[0]);
		seen.statuses.push_back(header[1]);
		seen.ids.push_back(header[6]);
		BIO_write(in, payload.data(), static_cast<int>(payload.size()));
		const int accepted = SSL_accept(ssl);
		seen.handshake_done = accepted == 1;
		Bytes flight(static_cast<size_t>(BIO_ctrl_pending(out)));
		if (!flight.empty()) {
			BIO_read(out, flight.data(), static_cast<int>(flight.size()));
		}
		switch (fault) {
		case ServerFault::WrongPacketType:
			WriteAll(fd, Packet(0x01, 0x01, flight));
			alive = false;
			break;
		case ServerFault::ShortLength: {
			const Bytes bad = {0x04, 0x01, 0x00, 0x05, 0, 0, 1, 0};
			WriteAll(fd, bad);
			alive = false;
			break;
		}
		case ServerFault::CloseAfterHello:
			alive = false;
			break;
		case ServerFault::CloseInsidePayload: {
			const Bytes whole = Packet(0x04, 0x01, flight);
			WriteAll(fd, whole.data(), whole.size() / 2);
			alive = false;
			break;
		}
		case ServerFault::Silent:
			ReadUpTo(fd, 1);  // until the client gives up and closes
			alive = false;
			break;
		case ServerFault::StallInsidePacket: {
			// Half of its answer, and then nothing: the connection stays open.
			const Bytes whole = Packet(0x04, 0x01, flight);
			WriteAll(fd, whole.data(), whole.size() / 2);
			ReadUpTo(fd, 1);  // until the client gives up and closes
			alive = false;
			break;
		}
		case ServerFault::Drip: {
			for (size_t i = 0; i < flight.size() && i < 20; i++) {
				std::this_thread::sleep_for(std::chrono::milliseconds(200));
				if (!WriteAll(fd, Packet(0x04, 0x01, Bytes(1, flight[i])))) {
					break;	// the client gave up and closed
				}
			}
			ReadUpTo(fd, 1);  // until the client gives up and closes
			alive = false;
			break;
		}
		case ServerFault::SlowEcho:
		case ServerFault::ShutdownAfterHandshake:
		case ServerFault::DropAfterHandshake:
		case ServerFault::None: {
			// The flight in `reply_pieces` packets: OpenSSL then asks for records that straddle them.
			const size_t pieces = flight.empty() ? 0 : std::min(reply_pieces, flight.size());
			for (size_t i = 0; i < pieces; i++) {
				const size_t from = flight.size() * i / pieces, to = flight.size() * (i + 1) / pieces;
				WriteAll(fd, Packet(i % 2 ? 0x12 : 0x04, i + 1 == pieces ? 0x01 : 0x00,
									Bytes(flight.begin() + from, flight.begin() + to)));
			}
			break;
		}
		}
	}
	if (alive && seen.handshake_done) {
		// From here TLS is on the socket, with no TDS wrapping.
		BIO *direct = BIO_new_socket(fd, BIO_NOCLOSE);
		SSL_set_bio(ssl, direct, direct);
		uint8_t buffer[4096];
		if (fault == ServerFault::ShutdownAfterHandshake) {
			SSL_shutdown(ssl);
		}
		for (; fault == ServerFault::None || fault == ServerFault::SlowEcho;) {
			const int n = SSL_read(ssl, buffer, sizeof(buffer));
			if (n <= 0) {
				break;
			}
			if (fault == ServerFault::SlowEcho) {
				std::this_thread::sleep_for(std::chrono::milliseconds(300));
			}
			seen.application.insert(seen.application.end(), buffer, buffer + n);
			for (int i = 0; i < n; i++) {
				buffer[i]++;
			}
			if (SSL_write(ssl, buffer, n) != n) {
				break;
			}
		}
	}
	SSL_free(ssl);
	SSL_CTX_free(ctx);
	::close(fd);
}

void TestEnableTls(const ServerIdentity &identity) {
	std::cout << "EnableTls" << std::endl;
	TlsOptions trust;
	trust.verify_certificate = false;
	struct Shape {
		int version;
		const char *name;
		size_t pieces;
	};
	// TLS 1.3 is the regression: its last handshake message is the client's, and it used to stay in the
	// transport's buffer when the handshake ended -- "enabled", and nothing after it understood.
	const Shape shapes[] = {{TLS1_2_VERSION, "TLSv1.2", 1},
							{TLS1_2_VERSION, "TLSv1.2", 2},
							{TLS1_2_VERSION, "TLSv1.2", 7},
							{TLS1_3_VERSION, "TLSv1.3", 1},
							{TLS1_3_VERSION, "TLSv1.3", 3}};
	for (const auto &shape : shapes) {
		const size_t pieces = shape.pieces;
		Pair pair;
		ServerSeen seen;
		std::thread server(ServeTls, pair.peer, std::cref(identity), ServerFault::None, pieces, shape.version,
						   std::ref(seen));
		pair.peer = -1;	 // the server's now
		uint8_t packet_id = 5;
		const bool enabled = pair.socket.EnableTls(packet_id, 5000, "", trust);
		const std::string label =
			std::string(shape.name) + ", server flights in " + std::to_string(pieces) + " packet(s)";
		Check(enabled && pair.socket.IsTlsEnabled(),
			  "the handshake completes, " + label + " (" + pair.socket.GetLastError() + ")");
		Note("tls version, " + label, pair.socket.GetTlsVersion());
		Check(pair.socket.GetTlsVersion() == shape.name && !pair.socket.GetTlsCipherSuite().empty(),
			  std::string("as ") + shape.name + " with a cipher suite");
		uint8_t unused = 0;
		Check(!pair.socket.EnableTls(unused, 100, "", trust) && pair.socket.GetLastError() == "TLS is already enabled",
			  "TLS cannot be enabled twice");

		// Application bytes now go through TLS, unwrapped: the server answers each byte plus one.
		const Bytes out = Pattern(20000, 11);
		const bool sent = pair.socket.Send(out);
		Bytes back;
		uint8_t buffer[8192];
		while (sent && back.size() < out.size()) {
			const ssize_t n = pair.socket.Receive(buffer, sizeof(buffer), 5000);
			if (n <= 0) {
				break;
			}
			back.insert(back.end(), buffer, buffer + n);
		}
		Bytes expected = out;
		for (auto &b : expected) {
			b++;
		}
		Check(sent && back == expected, "bytes sent after the handshake come back through TLS");
		Check(pair.socket.Receive(buffer, sizeof(buffer), 50) == 0 && pair.socket.IsConnected(),
			  "Receive through TLS with nothing to read times out with 0");
		pair.socket.Close();
		server.join();
		Check(seen.handshake_done && seen.application == out, "the server read them decrypted");
		bool prelogin = !seen.types.empty();
		for (size_t i = 0; i < seen.types.size(); i++) {
			prelogin = prelogin && seen.types[i] == 0x12 && seen.statuses[i] == 0x01 &&
					   seen.ids[i] == static_cast<uint8_t>(5 + i);
		}
		Check(prelogin && packet_id == static_cast<uint8_t>(5 + seen.types.size()),
			  "every handshake packet is a PRELOGIN with end-of-message, the ids following the connection's (" +
				  std::to_string(seen.types.size()) + " sent)");
		Note("client handshake packets, " + label, std::to_string(seen.types.size()));
	}
	struct Refusal {
		ServerFault fault;
		const char *what;
	};
	const Refusal refusals[] = {{ServerFault::WrongPacketType, "an answer that is neither a reply nor a PRELOGIN"},
								{ServerFault::ShortLength, "an answer whose length is shorter than its header"},
								{ServerFault::CloseAfterHello, "the server closing after the client's hello"},
								{ServerFault::CloseInsidePayload, "the server closing inside its answer"}};
	for (const auto &refusal : refusals) {
		Pair pair;
		ServerSeen seen;
		std::thread server(ServeTls, pair.peer, std::cref(identity), refusal.fault, size_t(1), TLS1_2_VERSION,
						   std::ref(seen));
		pair.peer = -1;
		uint8_t packet_id = 1;
		const bool enabled = pair.socket.EnableTls(packet_id, 3000, "", trust);
		const std::string error = pair.socket.GetLastError();
		pair.socket.Close();
		server.join();
		Check(!enabled && !pair.socket.IsTlsEnabled() && error.find("TLS handshake failed: ") == 0,
			  std::string(refusal.what) + " fails the handshake (" + error + ")");
		Note(refusal.what, error);
	}
	{
		Pair pair;
		ServerSeen seen;
		std::thread server(ServeTls, pair.peer, std::cref(identity), ServerFault::Silent, size_t(1), TLS1_2_VERSION,
						   std::ref(seen));
		pair.peer = -1;
		uint8_t packet_id = 1;
		const auto start = std::chrono::steady_clock::now();
		const bool enabled = pair.socket.EnableTls(packet_id, 300, "", trust);
		const auto took =
			std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
		const std::string error = pair.socket.GetLastError();
		pair.socket.Close();
		server.join();
		Check(!enabled && took >= 250 && took < 3000 && error.find("TLS handshake failed: ") == 0,
			  "a server that never answers fails the handshake at the timeout (" + error + ")");
	}
	{
		// Regression: only the wait for a packet's first byte had the timeout, the rest of it was read with
		// blocking reads. A server that stopped inside a packet held the login for as long as it kept the
		// connection open (this case then never returns).
		Pair pair;
		ServerSeen seen;
		std::thread server(ServeTls, pair.peer, std::cref(identity), ServerFault::StallInsidePacket, size_t(1),
						   TLS1_2_VERSION, std::ref(seen));
		pair.peer = -1;
		uint8_t packet_id = 1;
		const auto start = std::chrono::steady_clock::now();
		const bool enabled = pair.socket.EnableTls(packet_id, 300, "", trust);
		const auto took =
			std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
		const std::string error = pair.socket.GetLastError();
		pair.socket.Close();
		server.join();
		Check(!enabled && took >= 250 && took < 3000 && error.find("TLS handshake failed: Timeout") == 0,
			  "a server that stops inside a packet fails the handshake at the timeout (" + error + ")");
	}
	{
		// Regression (review of #617): every read was given the whole timeout anew, so a server that sent a
		// little just inside each one was never late -- here a byte every 200 ms under a timeout of 300, which
		// took as long as the server had bytes (4.4 s for these 20). The handshake has one deadline.
		Pair pair;
		ServerSeen seen;
		std::thread server(ServeTls, pair.peer, std::cref(identity), ServerFault::Drip, size_t(1), TLS1_2_VERSION,
						   std::ref(seen));
		pair.peer = -1;
		uint8_t packet_id = 1;
		const auto start = std::chrono::steady_clock::now();
		const bool enabled = pair.socket.EnableTls(packet_id, 300, "", trust);
		const auto took =
			std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
		const std::string error = pair.socket.GetLastError();
		pair.socket.Close();
		server.join();
		Check(!enabled && took >= 250 && took < 900 && error.find("TLS handshake failed: Timeout") == 0,
			  "a server that drips its answer fails the handshake at the timeout, not when it runs dry (" +
				  std::to_string(took) + " ms, " + error + ")");
	}
	{
		// The default verifies: a self-signed certificate is refused, with the two ways out named.
		Pair pair;
		ServerSeen seen;
		std::thread server(ServeTls, pair.peer, std::cref(identity), ServerFault::None, size_t(1), TLS1_2_VERSION,
						   std::ref(seen));
		pair.peer = -1;
		uint8_t packet_id = 1;
		const bool enabled = pair.socket.EnableTls(packet_id, 3000);
		const std::string error = pair.socket.GetLastError();
		pair.socket.Close();
		server.join();
		Check(!enabled && error.find("Set TrustServerCertificate=yes") != std::string::npos &&
				  error.find("HostNameInCertificate=<name>") != std::string::npos,
			  "a certificate that does not verify is refused, both ways out named");
		Note("unverified certificate", error);
	}
	{
		TdsSocket socket;
		uint8_t packet_id = 1;
		Check(
			!socket.EnableTls(packet_id, 100, "", trust) && socket.GetLastError() == "Cannot enable TLS: not connected",
			"TLS is not enabled on a socket that is not connected");
	}
}

//------------------------------------------------------------------------------
// What the cases above leave: moves, SendPacket, the framing's bounds, failures after the dial
//------------------------------------------------------------------------------
void TestTheRest(const ServerIdentity &identity) {
	std::cout << "Moves, SendPacket, failures after the dial" << std::endl;
	{
		// A moved socket is the connection; the one moved from is closed and owns nothing.
		Pair pair;
		TdsSocket moved(std::move(pair.socket));
		Check(moved.IsConnected() && moved.GetPort() == pair.listener.port && !pair.socket.IsConnected() &&
				  pair.socket.GetSocketFd() == -1,
			  "a move-constructed socket has the connection, the source none");
		const Bytes out = Pattern(10, 2);
		Check(moved.Send(out) && ReadUpTo(pair.peer, 10) == out, "and sends on it");

		// Move-assigned over a connected socket: that one's connection is closed first.
		Pair other;
		other.socket = std::move(moved);
		Check(ReadUpTo(other.peer, 1).empty(), "move assignment closes the connection the target had");
		Check(other.socket.IsConnected() && !moved.IsConnected() && other.socket.Send(out) &&
				  ReadUpTo(pair.peer, 10) == out,
			  "and the target is the moved connection");
		TdsSocket &same = other.socket;
		other.socket = std::move(same);
		Check(other.socket.IsConnected(), "a socket moved onto itself keeps its connection");
	}
	{
		Pair pair;
		Check(pair.socket.GetTlsVersion().empty() && pair.socket.GetTlsCipherSuite().empty() &&
				  !pair.socket.IsTlsEnabled(),
			  "a plain connection reports no TLS version or cipher");

		// SendPacket writes the packet's header and payload.
		TdsPacket packet(duckdb::tds::PacketType::SQL_BATCH);
		const Bytes payload = Pattern(33, 4);
		packet.AppendPayload(payload);
		packet.SetPacketId(9);
		const bool sent = pair.socket.SendPacket(packet);
		const Bytes wire = ReadUpTo(pair.peer, 8 + payload.size());
		Check(sent && wire.size() == 41 && wire[0] == 0x01 && wire[1] == 0x01 && wire[2] == 0 && wire[3] == 41 &&
				  wire[6] == 9 && Bytes(wire.begin() + 8, wire.end()) == payload,
			  "SendPacket sends the header (type, end of message, length, id) and the payload");

		// A framing below the smallest packet, or of no frames, falls back to the default packet and one frame.
		pair.socket.SetReceiveFraming(100, 0);
		const Bytes big = Pattern(6000, 5);
		WriteAll(pair.peer, Packet(0x04, 0x01, big));
		TdsPacket got;
		Check(pair.socket.ReceivePacket(got, 2000) && got.GetPayload() == big,
			  "a framing of 100 bytes x 0 frames still reads a packet larger than its read");

		// A message cut off after its first packet, then a payload view on the closed connection.
		WriteAll(pair.peer, Packet(0x04, 0x00, Pattern(10, 1)));
		pair.ClosePeer();
		Bytes message;
		Check(!pair.socket.ReceiveMessage(message, 2000) && !pair.socket.IsConnected(),
			  "a message whose last packet never comes is no message, the peer having closed");
		const uint8_t *view = nullptr;
		size_t view_length = 0;
		Check(!pair.socket.ReceivePayloadView(view, view_length, 100), "and no payload view follows");
	}
	{
		// Sending to a peer that is gone: the first bytes may still be taken; then the send fails.
		Pair pair;
		pair.ClosePeer();
		const Bytes out = Pattern(65536, 1);
		bool failed = false;
		for (int i = 0; i < 100 && !failed; i++) {
			failed = !pair.socket.Send(out);
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
		}
		Check(failed && !pair.socket.IsConnected() && pair.socket.GetLastError().find("Send failed: ") == 0,
			  "a send to a peer that is gone fails and ends the connection (" + pair.socket.GetLastError() + ")");
	}
	TlsOptions trust;
	trust.verify_certificate = false;
	struct After {
		ServerFault fault;
		const char *what;
	};
	const After after[] = {{ServerFault::ShutdownAfterHandshake, "the server closing TLS after the handshake"},
						   {ServerFault::DropAfterHandshake, "the server dropping the socket after the handshake"}};
	for (const auto &a : after) {
		Pair pair;
		ServerSeen seen;
		std::thread server(ServeTls, pair.peer, std::cref(identity), a.fault, size_t(1), TLS1_2_VERSION,
						   std::ref(seen));
		pair.peer = -1;
		uint8_t packet_id = 1;
		const bool enabled = pair.socket.EnableTls(packet_id, 3000, "", trust);
		uint8_t buffer[64];
		const ssize_t n = enabled ? pair.socket.Receive(buffer, sizeof(buffer), 3000) : 0;
		const std::string error = pair.socket.GetLastError();
		Check(enabled && n == -1 && !pair.socket.IsConnected() &&
				  (error.find("Connection closed by server: ") == 0 || error.find("TLS receive failed: ") == 0),
			  std::string(a.what) + " is -1 on the next read, the connection done (" + error + ")");
		Note(a.what, error);
		pair.socket.Close();
		server.join();
	}
}

//------------------------------------------------------------------------------
// TlsHandshakeTransport on its own
//------------------------------------------------------------------------------
#ifndef TDS_SOCKET_TEST_NO_TRANSPORT
struct SocketPair {
	int a = -1, b = -1;
	SocketPair() {
		int fds[2];
		if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
			std::cerr << "socketpair failed" << std::endl;
			std::exit(2);
		}
		a = fds[0];
		b = fds[1];
	}
	~SocketPair() {
		::close(a);
		if (b >= 0) {
			::close(b);
		}
	}
	void CloseB() {
		::close(b);
		b = -1;
	}
};

void TestTransport() {
	using duckdb::tds::TlsHandshakeTransport;
	std::cout << "TlsHandshakeTransport" << std::endl;
	{
		SocketPair pair;
		uint8_t packet_id = 254;
		TlsHandshakeTransport transport(pair.a, packet_id);
		Check(transport.Flush() && packet_id == 254, "nothing kept: Flush sends nothing");
		const Bytes one = Pattern(5, 1), two = Pattern(300, 2);
		Check(transport.Buffer(one.data(), one.size()) == 5 && transport.Buffer(two.data(), two.size()) == 300 &&
				  transport.PendingSendBytes() == 305,
			  "Buffer keeps what it is given and answers its length");
		Check(transport.Flush() && transport.PendingSendBytes() == 0 && packet_id == 255,
			  "Flush sends it and advances the packet id");
		Bytes expected = {0x12, 0x01, 0x01, 0x39, 0x00, 0x00, 254, 0x00};  // 8 + 305 = 0x139
		expected.insert(expected.end(), one.begin(), one.end());
		expected.insert(expected.end(), two.begin(), two.end());
		Check(ReadUpTo(pair.b, expected.size()) == expected,
			  "as one PRELOGIN packet: type 0x12, end of message, the length, the id, the bytes in order");
		transport.Buffer(one.data(), one.size());
		transport.Flush();
		transport.Buffer(one.data(), one.size());
		transport.Flush();
		const Bytes next = ReadUpTo(pair.b, 26);  // two packets of 8 + 5
		Check(packet_id == 1 && next.size() == 26 && next[6] == 255 && next[13 + 6] == 0,
			  "the id wraps from 255 to 0 like the connection's: the packets carry 255, then 0");
	}
	{
		SocketPair pair;
		uint8_t packet_id = 1;
		TlsHandshakeTransport transport(pair.a, packet_id);
		uint8_t buffer[512];
		Check(transport.Receive(buffer, sizeof(buffer), 30) == 0, "nothing within the timeout: 0");

		const Bytes payload = Pattern(100, 5);
		WriteAll(pair.b, Packet(0x12, 0x01, payload));
		Check(transport.Receive(buffer, 5, 1000) == 5 &&
				  Bytes(buffer, buffer + 5) == Bytes(payload.begin(), payload.begin() + 5) &&
				  transport.PendingReceiveBytes() == 95,
			  "a record header's five bytes of a packet, the rest kept");
		Check(transport.Receive(buffer, 60, 1000) == 60 &&
				  Bytes(buffer, buffer + 60) == Bytes(payload.begin() + 5, payload.begin() + 65),
			  "the next bytes from what was kept, without reading the socket");
		Check(transport.Receive(buffer, 500, 1000) == 35 && transport.PendingReceiveBytes() == 0,
			  "no more than the packet holds");
		Check(transport.Receive(buffer, 500, 30) == 0, "and then the next packet is waited for");

		WriteAll(pair.b, Packet(0x04, 0x01, Pattern(10, 1)));
		Check(transport.Receive(buffer, 500, 1000) == 10, "a reply packet (0x04) is taken as a PRELOGIN is");
		WriteAll(pair.b, Packet(0x04, 0x01, Bytes()));
		Check(transport.Receive(buffer, 500, 1000) == 0, "a packet of a header alone gives no bytes");

		// What Buffer kept goes out before anything is read.
		const Bytes hello = Pattern(40, 7);
		transport.Buffer(hello.data(), hello.size());
		WriteAll(pair.b, Packet(0x04, 0x01, Pattern(3, 1)));
		Check(transport.Receive(buffer, 500, 1000) == 3 && transport.PendingSendBytes() == 0 && packet_id == 2,
			  "Receive sends what was kept first");
		const Bytes sent = ReadUpTo(pair.b, 48);
		Check(sent.size() == 48 && sent[0] == 0x12 && sent[6] == 1 && Bytes(sent.begin() + 8, sent.end()) == hello,
			  "as one PRELOGIN packet");
	}
	{
		// Regression: a packet that arrives in pieces slower than the timeout. Each call gives up at its
		// timeout (it used to block in the read until the rest came), and the packet is not lost for it.
		SocketPair pair;
		uint8_t packet_id = 1;
		TlsHandshakeTransport transport(pair.a, packet_id);
		const Bytes payload = Pattern(40, 3);
		const Bytes whole = Packet(0x12, 0x01, payload);
		uint8_t buffer[64];
		const auto start = std::chrono::steady_clock::now();
		WriteAll(pair.b, whole.data(), 3);
		const int in_header = transport.Receive(buffer, sizeof(buffer), 100);
		WriteAll(pair.b, whole.data() + 3, 5 + 10);
		const int in_payload = transport.Receive(buffer, sizeof(buffer), 100);
		const auto took =
			std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
		Check(in_header == 0 && in_payload == 0 && took >= 180 && took < 2000,
			  "a packet that stops inside its header, then inside its payload: 0 at each timeout");
		Check(transport.PendingReceiveBytes() == 0, "and nothing of the unfinished packet is handed out");
		WriteAll(pair.b, whole.data() + 18, whole.size() - 18);
		Check(transport.Receive(buffer, sizeof(buffer), 1000) == 40 && Bytes(buffer, buffer + 40) == payload,
			  "the rest arriving, the packet is whole: every byte, in order");
		WriteAll(pair.b, Packet(0x04, 0x01, Pattern(7, 9)));
		Check(transport.Receive(buffer, sizeof(buffer), 1000) == 7 && Bytes(buffer, buffer + 7) == Pattern(7, 9),
			  "and the packet after it is read from its own header");
	}
	struct Bad {
		Bytes bytes;
		bool close;
		const char *what;
	};
	const Bad bad[] = {{Packet(0x01, 0x01, Pattern(10, 1)), false, "a packet that is neither a reply nor a PRELOGIN"},
					   {{0x12, 0x01, 0x00, 0x07, 0, 0, 1, 0}, false, "a length of 7, shorter than the header"},
					   {{0x12, 0x01, 0x00, 0x00, 0, 0, 1, 0}, false, "a length of 0"},
					   {{}, true, "the peer closing before a packet"},
					   {{0x12, 0x01, 0x00}, true, "the peer closing inside a header"},
					   {{0x12, 0x01, 0x00, 0x20, 0, 0, 1, 0, 1, 2, 3}, true, "the peer closing inside a payload"}};
	for (const auto &b : bad) {
		SocketPair pair;
		uint8_t packet_id = 1;
		TlsHandshakeTransport transport(pair.a, packet_id);
		WriteAll(pair.b, b.bytes);
		if (b.close) {
			pair.CloseB();
		}
		uint8_t buffer[64];
		Check(transport.Receive(buffer, sizeof(buffer), 1000) == -1, std::string(b.what) + " is an error");
	}
	{
		// Sending to a peer that is gone.
		SocketPair pair;
		uint8_t packet_id = 1;
		TlsHandshakeTransport transport(pair.a, packet_id);
		pair.CloseB();
		const Bytes data = Pattern(100, 1);
		transport.Buffer(data.data(), data.size());
		uint8_t buffer[8];
		Check(!transport.Flush() || transport.Receive(buffer, sizeof(buffer), 100) == -1,
			  "a peer that is gone: the send or the read after it fails");
	}
}
#endif

//------------------------------------------------------------------------------
// A signal during a wait
//------------------------------------------------------------------------------
volatile std::sig_atomic_t signals_delivered = 0;

void OnSignal(int) {
	signals_delivered = signals_delivered + 1;
}

//! Runs `wait` on this thread while another sends it SIGUSR1 after 100 ms and, when `bytes` is not empty,
//! writes them to `peer` 200 ms after that. The handler is installed without SA_RESTART -- poll() is not
//! restarted either way -- so the wait under way is cut short with EINTR.
template <class WAIT>
void WithSignal(int peer, const Bytes &bytes, WAIT wait) {
	struct sigaction action;
	std::memset(&action, 0, sizeof(action));
	action.sa_handler = OnSignal;
	sigemptyset(&action.sa_mask);
	struct sigaction before;
	sigaction(SIGUSR1, &action, &before);
	const pthread_t waiter = pthread_self();
	std::thread other([&]() {
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		pthread_kill(waiter, SIGUSR1);
		if (!bytes.empty()) {
			std::this_thread::sleep_for(std::chrono::milliseconds(200));
			WriteAll(peer, bytes);
		}
	});
	wait();
	other.join();
	sigaction(SIGUSR1, &before, nullptr);
}

long long MillisSince(std::chrono::steady_clock::time_point start) {
	return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
}

//! Regressions: a signal ends poll() with EINTR, and the wait reported that as its own answer -- a timeout
//! before the time was up (Receive, and through it a query that "timed out" or "lost its connection"), an
//! error (the handshake's transport).
void TestSignals(const ServerIdentity &identity) {
	std::cout << "A signal during a wait" << std::endl;
	signals_delivered = 0;
	{
		Pair pair;
		const Bytes in = Pattern(24, 5);
		uint8_t buffer[64];
		ssize_t n = -2;
		WithSignal(pair.peer, in, [&]() { n = pair.socket.Receive(buffer, sizeof(buffer), 3000); });
		Check(n == 24 && Bytes(buffer, buffer + 24) == in && pair.socket.IsConnected(),
			  "Receive waits on through a signal and returns what arrives after it (" + std::to_string(n) + ")");
	}
	{
		Pair pair;
		uint8_t buffer[64];
		ssize_t n = -2;
		const auto start = std::chrono::steady_clock::now();
		WithSignal(pair.peer, Bytes(), [&]() { n = pair.socket.Receive(buffer, sizeof(buffer), 400); });
		const auto took = MillisSince(start);
		Check(n == 0 && pair.socket.IsConnected() && took >= 350 && took < 2500,
			  "with nothing arriving the timeout is the whole of it, counted once (" + std::to_string(took) + " ms)");
	}
	{
		Pair pair;
		const Bytes payload = Pattern(30, 2);
		TdsPacket packet;
		bool got = false;
		WithSignal(pair.peer, Packet(0x04, 0x01, payload), [&]() { got = pair.socket.ReceivePacket(packet, 3000); });
		Check(got && packet.GetPayload() == payload, "ReceivePacket too: the packet that arrives after the signal");
	}
#ifndef TDS_SOCKET_TEST_NO_TRANSPORT
	{
		using duckdb::tds::TlsHandshakeTransport;
		SocketPair pair;
		uint8_t packet_id = 1;
		TlsHandshakeTransport transport(pair.a, packet_id);
		const Bytes payload = Pattern(12, 4);
		uint8_t buffer[64];
		int n = -2;
		WithSignal(pair.b, Packet(0x12, 0x01, payload), [&]() { n = transport.Receive(buffer, sizeof(buffer), 3000); });
		Check(n == 12 && Bytes(buffer, buffer + 12) == payload,
			  "the handshake's transport waits on through a signal, where it failed the handshake (" +
				  std::to_string(n) + ")");
	}
#endif
	{
		// After the handshake, over TLS (review of #617): the read is an SSL_read under a receive timeout,
		// which a signal cuts short as it does poll(); OpenSSL says "read again", and that was answered as 0,
		// a timeout.
		TlsOptions trust;
		trust.verify_certificate = false;
		Pair pair;
		ServerSeen seen;
		std::thread server(ServeTls, pair.peer, std::cref(identity), ServerFault::SlowEcho, size_t(1), TLS1_2_VERSION,
						   std::ref(seen));
		pair.peer = -1;
		uint8_t packet_id = 1;
		const bool enabled = pair.socket.EnableTls(packet_id, 5000, "", trust);
		const Bytes out = {10, 20, 30, 40};
		uint8_t buffer[64];
		ssize_t n = -2;
		const bool sent = enabled && pair.socket.Send(out);
		// The answer comes 300 ms after the request; the signal at 100.
		WithSignal(-1, Bytes(), [&]() { n = pair.socket.Receive(buffer, sizeof(buffer), 3000); });
		Check(sent && n == 4 && Bytes(buffer, buffer + 4) == Bytes({11, 21, 31, 41}) && pair.socket.IsConnected(),
			  "over TLS, Receive waits on through a signal and returns the answer (" + std::to_string(n) + ")");
		const auto start = std::chrono::steady_clock::now();
		WithSignal(-1, Bytes(), [&]() { n = pair.socket.Receive(buffer, sizeof(buffer), 400); });
		const auto took = MillisSince(start);
		Check(n == 0 && pair.socket.IsConnected() && took >= 350 && took < 2500,
			  "and with nothing arriving its timeout is the whole of it (" + std::to_string(took) + " ms)");
		pair.socket.Close();
		server.join();
	}
	Check(signals_delivered == 6,
		  "every signal of these cases was delivered (" + std::to_string(signals_delivered) + ")");
}

}  // namespace

int main() {
	std::signal(SIGPIPE, SIG_IGN);
	const ServerIdentity identity;
	TestConnect();
	TestPlainIo();
	TestFraming();
	TestEnableTls(identity);
	TestTheRest(identity);
#ifndef TDS_SOCKET_TEST_NO_TRANSPORT
	TestTransport();
#endif
	TestSignals(identity);
	std::cout << (failures ? "FAILED" : "all passed") << std::endl;
	return failures ? 1 : 0;
}

#endif
