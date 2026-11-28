// test/cpp/test_result_stream_drain.cpp
// Issue #682: a row the reader refuses (a sql_variant value declaring more than 8,009 bytes) throws out
// of FillChunk with the stream still Streaming. The destructor then cancels and drains, and the drain met
// the same row again: the reader's exception left a destructor, which is std::terminate.
//
// A stream is driven with no SQL Server: a TdsConnection on a loopback socket is brought to Idle (as
// test_pool_prewarm does), and a thread answers its request with the bytes of the malformed response.
//
// What is checked: FillChunk reports the malformed row as an exception; destroying the stream, or
// cancelling it, returns; and the connection is closed, not handed back as reusable.
//
// Does NOT require a running SQL Server instance.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "duckdb.hpp"
#include "query/mssql_result_stream.hpp"
#include "tds/tds_connection.hpp"
#include "tds/tds_request.hpp"

using namespace duckdb;

namespace {

int failures = 0;

#define CHECK(condition)                                                  \
	do {                                                                  \
		if (!(condition)) {                                               \
			++failures;                                                   \
			std::cerr << "FAIL [" << __LINE__ << "] " #condition << "\n"; \
		}                                                                 \
	} while (0)

struct Bytes {
	std::vector<uint8_t> b;
	void U8(uint8_t v) {
		b.push_back(v);
	}
	void U16(uint16_t v) {
		U8(static_cast<uint8_t>(v & 0xFF));
		U8(static_cast<uint8_t>(v >> 8));
	}
	void U32(uint32_t v) {
		U16(static_cast<uint16_t>(v & 0xFFFF));
		U16(static_cast<uint16_t>(v >> 16));
	}
	void U64(uint64_t v) {
		U32(static_cast<uint32_t>(v & 0xFFFFFFFFu));
		U32(static_cast<uint32_t>(v >> 32));
	}
};

// One tabular-result packet (type 0x04).
std::vector<uint8_t> Packet(const std::vector<uint8_t> &payload, bool end_of_message) {
	const size_t size = 8 + payload.size();
	std::vector<uint8_t> p(size);
	p[0] = 0x04;
	p[1] = end_of_message ? 0x01 : 0x00;
	p[2] = static_cast<uint8_t>(size >> 8);
	p[3] = static_cast<uint8_t>(size & 0xFF);
	p[6] = 1;
	std::memcpy(p.data() + 8, payload.data(), payload.size());
	return p;
}

// COLMETADATA of one sql_variant column "v".
void WriteColumns(Bytes &w) {
	w.U8(0x81);		// COLMETADATA
	w.U16(1);		// one column
	w.U32(0);		// user type
	w.U16(0x0009);	// flags: nullable, updatable
	w.U8(0x62);		// sql_variant
	w.U32(8009);	// its maximum length
	w.U8(1);		// name: "v"
	w.U8('v');
	w.U8(0);
}

// A ROW whose value declares `declared` bytes and carries `value`.
void WriteRow(Bytes &w, uint32_t declared, const std::vector<uint8_t> &value) {
	w.U8(0xD1);
	w.U32(declared);
	w.b.insert(w.b.end(), value.begin(), value.end());
}

void WriteFinalDone(Bytes &w) {
	w.U8(0xFD);
	w.U16(0x0010);
	w.U16(0xC1);
	w.U64(1);
}

// An int 42 as a sql_variant value: base type 0x38, no property bytes.
const std::vector<uint8_t> INT_42 = {0x38, 0x00, 0x2A, 0x00, 0x00, 0x00};

// One result set of one row.
std::vector<uint8_t> Response(uint32_t declared, const std::vector<uint8_t> &value) {
	Bytes w;
	WriteColumns(w);
	WriteRow(w, declared, value);
	WriteFinalDone(w);
	return w.b;
}

// A good row, then a second result set whose row is malformed.
std::vector<uint8_t> TwoResultSets() {
	Bytes w;
	WriteColumns(w);
	WriteRow(w, 6, INT_42);
	WriteColumns(w);
	WriteRow(w, 8010, std::vector<uint8_t>(16, 0x38));
	WriteFinalDone(w);
	return w.b;
}

bool ReadPacket(int fd, std::vector<uint8_t> &packet) {
	uint8_t head[8];
	size_t got = 0;
	while (got < 8) {
		const ssize_t n = recv(fd, head + got, 8 - got, 0);
		if (n <= 0) {
			return false;
		}
		got += static_cast<size_t>(n);
	}
	const size_t size = (static_cast<size_t>(head[2]) << 8) | head[3];
	if (size < 8) {
		return false;
	}
	packet.assign(head, head + 8);
	packet.resize(size);
	while (got < size) {
		const ssize_t n = recv(fd, packet.data() + got, size - got, 0);
		if (n <= 0) {
			return false;
		}
		got += static_cast<size_t>(n);
	}
	return true;
}

void SendAll(int fd, const std::vector<uint8_t> &bytes) {
	size_t at = 0;
	while (at < bytes.size()) {
		const ssize_t n = send(fd, bytes.data() + at, bytes.size() - at, 0);
		if (n <= 0) {
			return;
		}
		at += static_cast<size_t>(n);
	}
}

// The server's side: read the request, answer with `response`, acknowledge an ATTENTION.
void Serve(int listen_fd, std::vector<uint8_t> response) {
	const int fd = accept(listen_fd, nullptr, nullptr);
	if (fd < 0) {
		return;
	}
	int one = 1;
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#ifdef SO_NOSIGPIPE
	setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
	std::vector<uint8_t> packet;
	bool open = true;
	while (open) {
		open = ReadPacket(fd, packet);
		if (open && (packet[1] & 0x01)) {
			break;
		}
	}
	if (open) {
		SendAll(fd, Packet(response, true));
	}
	while (open && ReadPacket(fd, packet)) {
		if (packet[0] == 0x06) {  // ATTENTION: a DONE carrying the acknowledgement
			Bytes w;
			w.U8(0xFD);
			w.U16(0x0020);
			w.U16(0);
			w.U64(0);
			SendAll(fd, Packet(w.b, true));
		}
	}
	close(fd);
}

struct Listener {
	int fd = -1;
	uint16_t port = 0;
	Listener() {
		fd = socket(AF_INET, SOCK_STREAM, 0);
		sockaddr_in addr{};
		addr.sin_family = AF_INET;
		addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		addr.sin_port = 0;
		if (fd < 0 || bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0 || listen(fd, 4) != 0) {
			std::cerr << "cannot listen on loopback\n";
			std::exit(2);
		}
		socklen_t len = sizeof(addr);
		getsockname(fd, reinterpret_cast<sockaddr *>(&addr), &len);
		port = ntohs(addr.sin_port);
	}
	~Listener() {
		close(fd);
	}
};

std::shared_ptr<tds::TdsConnection> IdleConnection(uint16_t port) {
	auto connection = std::make_shared<tds::TdsConnection>();
	if (!connection->Connect("127.0.0.1", port, 5) ||
		!connection->TransitionState(tds::ConnectionState::Authenticating, tds::ConnectionState::Idle)) {
		std::cerr << "cannot connect to the loopback server\n";
		std::exit(2);
	}
	return connection;
}

// Runs a stream over `response` up to its first FillChunk, then ends it by `cancel_first` or by its
// destructor alone. Returns what FillChunk threw ("" when it returned).
std::string RunStream(const std::vector<uint8_t> &response, bool cancel_first,
					  std::shared_ptr<tds::TdsConnection> &connection_out) {
	Listener listener;
	std::thread server(Serve, listener.fd, response);
	auto connection = IdleConnection(listener.port);
	std::string thrown;
	{
		MSSQLResultStream stream(connection, tds::Request("select v from t"), "ctx", /*client_transcode*/ true,
								 tds::GeometryAs::Geometry);
		CHECK(stream.Initialize());
		DataChunk chunk;
		chunk.Initialize(Allocator::DefaultAllocator(), stream.GetColumnTypes());
		try {
			stream.FillChunk(chunk);
		} catch (const std::exception &e) {
			thrown = e.what();
		}
		if (cancel_first) {
			stream.Cancel();
		}
		// The destructor runs here. Before the fix it ended the process.
	}
	connection_out = connection;
	connection->Close();
	server.join();
	return thrown;
}

const std::vector<uint8_t> MALFORMED = Response(8010, std::vector<uint8_t>(16, 0x38));

void TestMalformedVariantLengthThenDestroy() {
	std::cout << "Test: a sql_variant value declaring 8010 bytes, the stream destroyed\n";
	std::shared_ptr<tds::TdsConnection> connection;
	const std::string thrown = RunStream(MALFORMED, /*cancel_first*/ false, connection);
	CHECK(thrown.find("sql_variant value declares 8010 bytes") != std::string::npos);
	// A connection whose stream could not be parsed is not reusable.
	CHECK(connection->GetState() != tds::ConnectionState::Idle);
}

void TestMalformedVariantLengthThenCancel() {
	std::cout << "Test: a sql_variant value declaring 8010 bytes, the stream cancelled\n";
	std::shared_ptr<tds::TdsConnection> connection;
	const std::string thrown = RunStream(MALFORMED, /*cancel_first*/ true, connection);
	CHECK(thrown.find("sql_variant value declares 8010 bytes") != std::string::npos);
	CHECK(connection->GetState() != tds::ConnectionState::Idle);
}

// The other drain: a second result set is refused by name, and the drain that follows meets the
// malformed row. The refusal is still what the caller hears.
void TestMalformedRowInARefusedSecondResultSet() {
	std::cout << "Test: a malformed row in a second result set, which is refused\n";
	std::shared_ptr<tds::TdsConnection> connection;
	const std::string thrown = RunStream(TwoResultSets(), /*cancel_first*/ false, connection);
	CHECK(thrown.find("multiple result sets") != std::string::npos);
	CHECK(connection->GetState() != tds::ConnectionState::Idle);
}

// The control: a well-formed value reads, and the stream ends without an exception.
void TestWellFormedVariantReads() {
	std::cout << "Test: a well-formed sql_variant value reads\n";
	std::shared_ptr<tds::TdsConnection> connection;
	const std::string thrown = RunStream(Response(6, INT_42), /*cancel_first*/ false, connection);
	CHECK(thrown.empty());
}

}  // namespace

int main() {
	TestWellFormedVariantReads();
	TestMalformedVariantLengthThenDestroy();
	TestMalformedVariantLengthThenCancel();
	TestMalformedRowInARefusedSecondResultSet();

	if (failures > 0) {
		std::cerr << "\n" << failures << " assertion(s) failed.\n";
		return 1;
	}
	std::cout << "\nAll result-stream drain assertions passed.\n";
	return 0;
}
