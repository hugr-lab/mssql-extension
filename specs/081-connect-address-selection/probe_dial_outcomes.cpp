// Spec 081 probe — what does connect() actually do on this platform?
//
// Settles two questions the spec cannot answer by reading code:
//
//   A. Does a bound-but-UNLISTENING 127.0.0.1 port DROP the SYN (connect hangs)
//      or REFUSE it (RST, immediate ECONNREFUSED)? The spec's draft claimed
//      macOS drops and Linux refuses; the comment it cited
//      (TestUnreachableRoutedTargetFails in test/cpp/test_login_routing_hops.cpp)
//      closes by attributing the drop to Linux instead. One is wrong, and which
//      one decides whether W1's hardest test needs an injected candidate list or
//      gets an in-process blackhole for free.
//
//   B. Does 192.0.2.1 (RFC 5737 TEST-NET-1) blackhole here, or answer
//      EHOSTUNREACH instantly? TestHopHonoursCallerConnectTimeout already dials
//      it in CI, but asserts only an UPPER time bound, so it passes either way
//      and does not witness which branch it took.
//
// The dial mirrors TdsSocket::Connect: non-blocking connect(), poll() for
// writability, getsockopt(SO_ERROR) as the only success oracle. So what this
// measures is what the extension's own dial sees, not what a blocking connect
// would see.
//
// POSIX only (macOS + Linux) — which is the whole question; the Windows half of
// #122 is about the candidate LIST, not about RST-vs-drop.
//
// Build & run:
//   c++ -std=c++11 -O0 -o /tmp/probe081 probe_dial_outcomes.cpp && /tmp/probe081
// Or both platforms from a Mac, via run_probe.sh in this directory.

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

enum class Outcome { Connected, Refused, Unreachable, Dropped, TimedOut, OtherError };

const char *Name(Outcome o) {
	switch (o) {
	case Outcome::Connected:
		return "CONNECTED";
	case Outcome::Refused:
		return "REFUSED";
	case Outcome::Unreachable:
		return "UNREACHABLE";
	case Outcome::Dropped:
		return "DROPPED (SYN unanswered)";
	case Outcome::TimedOut:
		return "STILL PENDING (blackhole)";
	default:
		return "ERROR";
	}
}

struct DialResult {
	Outcome outcome;
	int err;
	long ms;
};

Outcome Classify(int e) {
	switch (e) {
	case 0:
		return Outcome::Connected;
	case ECONNREFUSED:
		return Outcome::Refused;
	case EHOSTUNREACH:
	case ENETUNREACH:
		return Outcome::Unreachable;
	case ETIMEDOUT:
		// The kernel exhausted its own SYN retries: nothing ever answered. Same
		// verdict as poll() running out of budget, just reached by the kernel
		// first (macOS gives up at ~7.8s, inside a 12s budget).
		return Outcome::Dropped;
	default:
		return Outcome::OtherError;
	}
}

// Non-blocking connect + poll + SO_ERROR, exactly as TdsSocket::Connect does it.
DialResult Dial(const char *ip, uint16_t port, int timeout_ms) {
	const auto started = std::chrono::steady_clock::now();
	auto elapsed = [&started]() -> long {
		return (long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started)
			.count();
	};

	int fd = ::socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		return {Outcome::OtherError, errno, elapsed()};
	}
	int flags = ::fcntl(fd, F_GETFL, 0);
	::fcntl(fd, F_SETFL, flags | O_NONBLOCK);

	sockaddr_in addr;
	std::memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	::inet_pton(AF_INET, ip, &addr.sin_addr);

	const int rc = ::connect(fd, (sockaddr *)&addr, sizeof(addr));
	if (rc == 0) {
		// Loopback can complete synchronously.
		const long t = elapsed();
		::close(fd);
		return {Outcome::Connected, 0, t};
	}
	if (errno != EINPROGRESS) {
		// Refused (or unreachable) before poll — the fast, self-correcting case.
		const int e = errno;
		const long t = elapsed();
		::close(fd);
		return {Classify(e), e, t};
	}

	pollfd pfd;
	pfd.fd = fd;
	pfd.events = POLLOUT;
	pfd.revents = 0;
	const int pr = ::poll(&pfd, 1, timeout_ms);
	if (pr == 0) {
		// Nothing happened within the budget: the SYN went unanswered.
		const long t = elapsed();
		::close(fd);
		return {Outcome::TimedOut, ETIMEDOUT, t};
	}
	if (pr < 0) {
		const int e = errno;
		const long t = elapsed();
		::close(fd);
		return {Outcome::OtherError, e, t};
	}

	int so_error = 0;
	socklen_t len = sizeof(so_error);
	::getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &len);
	const long t = elapsed();
	::close(fd);
	return {Classify(so_error), so_error, t};
}

void Report(const char *label, const DialResult &r) {
	std::printf("  %-34s %-26s %6ld ms", label, Name(r.outcome), r.ms);
	if (r.err != 0 && r.outcome != Outcome::TimedOut && r.outcome != Outcome::Dropped) {
		std::printf("  (errno %d: %s)", r.err, std::strerror(r.err));
	}
	std::printf("\n");
}

// Bind 127.0.0.1:0 and return the port the kernel assigned. The caller decides
// whether to listen(), and whether to keep the fd open.
int BindEphemeral(uint16_t *port_out) {
	int fd = ::socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		return -1;
	}
	sockaddr_in addr;
	std::memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = 0;
	::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
	if (::bind(fd, (sockaddr *)&addr, sizeof(addr)) != 0) {
		::close(fd);
		return -1;
	}
	sockaddr_in bound;
	socklen_t len = sizeof(bound);
	if (::getsockname(fd, (sockaddr *)&bound, &len) != 0) {
		::close(fd);
		return -1;
	}
	*port_out = ntohs(bound.sin_port);
	return fd;
}

} // namespace

int main() {
	utsname u;
	if (::uname(&u) == 0) {
		std::printf("spec 081 dial probe — %s %s (%s)\n\n", u.sysname, u.release, u.machine);
	} else {
		std::printf("spec 081 dial probe\n\n");
	}

	// ---- Control: a LISTENING port must connect, or nothing below means anything.
	{
		uint16_t port = 0;
		int fd = BindEphemeral(&port);
		if (fd < 0 || ::listen(fd, 1) != 0) {
			std::printf("  control setup FAILED (%s) — results below are not trustworthy\n", std::strerror(errno));
			if (fd >= 0) {
				::close(fd);
			}
			return 1;
		}
		std::printf("CONTROL — the probe's dial works\n");
		Report("bound + LISTENING", Dial("127.0.0.1", port, 2000));
		::close(fd);
	}

	// ---- Control: a port with NOTHING bound. This is the trick
	// TestUnreachableRoutedTargetFails settled on, and it should be a refusal.
	{
		uint16_t port = 0;
		int fd = BindEphemeral(&port);
		if (fd >= 0) {
			::close(fd); // free it: nothing is bound to `port` now
		}
		std::printf("\nCONTROL — nothing bound (what the repo's test uses)\n");
		Report("freed port", Dial("127.0.0.1", port, 12000));
	}

	// ---- QUESTION A: bound, never listen(), fd kept OPEN for the whole dial.
	{
		uint16_t port = 0;
		int fd = BindEphemeral(&port);
		if (fd < 0) {
			std::printf("\nQUESTION A setup FAILED: %s\n", std::strerror(errno));
			return 1;
		}
		std::printf("\nQUESTION A — bound but never listen() (the spec's disputed case)\n");
		Report("bound, NOT listening", Dial("127.0.0.1", port, 12000));
		::close(fd);
	}

	// ---- QUESTION B: does 192.0.2.1 blackhole in THIS environment?
	{
		std::printf("\nQUESTION B — 192.0.2.1:1433, RFC 5737 TEST-NET-1\n");
		Report("192.0.2.1 (3s cap)", Dial("192.0.2.1", 1433, 3000));
	}

	std::printf("\nReading it:\n");
	std::printf("  QUESTION A \"DROPPED\" / \"STILL PENDING\" => this platform DROPS the SYN; an\n");
	std::printf("             blackhole is available here and W1 case 2 needs no injected list.\n");
	std::printf("  QUESTION A \"REFUSED\"       => this platform sends RST; the trick gives the\n");
	std::printf("             wrong branch here and the injected candidate list is required.\n");
	std::printf("  QUESTION B \"STILL PENDING\" => 192.0.2.1 blackholes here, so\n");
	std::printf("             TestHopHonoursCallerConnectTimeout really does exercise the\n");
	std::printf("             timeout branch in this environment.\n");
	std::printf("  QUESTION B \"UNREACHABLE\"   => it fails instantly here, so that test passes\n");
	std::printf("             on its upper bound WITHOUT taking the timeout branch at all.\n");
	return 0;
}
