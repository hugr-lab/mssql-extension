// Spec 082 probe — what does connect() actually do on this platform?
//
// ANSWERED. Measured on both platforms; the table is in spec.md §4.
//
//   bound, never listen()   nothing bound      192.0.2.1
//   macOS: SYN DROPPED      REFUSED, 0ms       no answer within 3s
//          (7.8s, ETIMEDOUT)
//   Linux: REFUSED, 0ms     REFUSED, 0ms       no answer within 3s
//
// So macOS drops and Linux refuses, which is what spec 082's draft said. The
// comment in TestUnreachableRoutedTargetFails (test/cpp/test_login_routing_hops.cpp)
// is consistent with that once its "That is a Linux behaviour" is read as
// referring to getting an RST — it is ambiguously worded, not wrong. Rewording it
// is on §4's work list.
//
// Kept as a spec artifact because the answer is load-bearing: CI runs on Linux,
// where a bound-but-unlistening port REFUSES, so W1's blackhole cases cannot get
// one that way and need an address supplied to them. Rerun with ./run_probe.sh
// if that is ever doubted.
//
// WHAT THIS DOES AND DOES NOT MIRROR. The dial below reproduces the TIMING
// MECHANICS of TdsSocket::Connect — non-blocking connect(), poll(), then
// getsockopt(SO_ERROR) — which is what the measurement needs. It deliberately
// does NOT reproduce TdsSocket::WaitForReady's revents handling, and the
// difference is not cosmetic:
//
//   * WaitForReady returns false on POLLERR | POLLNVAL (and on POLLHUP) BEFORE
//     Connect reaches its getsockopt, and Connect's else-branch then overwrites
//     the reason with the literal string "Connection timed out". So an async
//     failure is reported as a timeout whatever it was.
//   * A SYNCHRONOUS refusal — what Linux loopback gives, measured above —
//     returns from connect() with ECONNREFUSED, which is neither EINPROGRESS nor
//     EWOULDBLOCK, so Connect falls to a branch that only debug-logs and records
//     NO reason at all. With every candidate refusing that way, last_error_ is
//     still empty at the end and the caller gets the generic
//     "Failed to connect to <host>:<port>".
//
// Only the third path — poll reports writability without POLLERR — reaches
// getsockopt and reports the true reason. This probe takes that path on purpose,
// so it can SHOW the per-candidate reason the production dial usually discards.
// spec.md W1.2/W1.3 record it as a gap W1 has to close, not a property to keep.
//
// POSIX only (macOS + Linux) — which is the whole question; the Windows half of
// #122 is about the candidate LIST, not about RST-vs-drop.
//
// Build & run:
//   c++ -std=c++11 -O0 -o /tmp/probe082 probe_dial_outcomes.cpp && /tmp/probe082
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
#include <cstdlib>
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

// A measurement artifact must fail loudly rather than print a plausible wrong
// row, so every input is checked and every syscall return is consulted.
void Fatal(const char *what, const char *detail) {
	std::fprintf(stderr, "probe082: FATAL %s: %s\n", what, detail);
	std::exit(2);
}

void FillAddr(sockaddr_in *addr, const char *ip, uint16_t port) {
	std::memset(addr, 0, sizeof(*addr));
	addr->sin_family = AF_INET;
	addr->sin_port = htons(port);
	// Unchecked, a typo'd literal leaves sin_addr at 0.0.0.0 and the probe
	// silently dials the wildcard instead of the address it names.
	if (::inet_pton(AF_INET, ip, &addr->sin_addr) != 1) {
		Fatal("not a dotted-quad IPv4 literal", ip);
	}
}

// Non-blocking connect + poll + SO_ERROR. See the header on what this does and
// does not mirror.
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
	const int flags = ::fcntl(fd, F_GETFL, 0);
	if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
		const int e = errno;
		::close(fd);
		return {Outcome::OtherError, e, elapsed()};
	}

	sockaddr_in addr;
	FillAddr(&addr, ip, port);

	if (::connect(fd, (sockaddr *)&addr, sizeof(addr)) == 0) {
		// Loopback can complete synchronously.
		const long t = elapsed();
		::close(fd);
		return {Outcome::Connected, 0, t};
	}
	if (errno != EINPROGRESS) {
		// Refused (or unreachable) before poll — the fast case, and the one the
		// production dial records no reason for.
		const int e = errno;
		const long t = elapsed();
		::close(fd);
		return {Classify(e), e, t};
	}

	// poll() against a deadline, retrying EINTR rather than reporting it as a
	// failure of the dial.
	const auto deadline = started + std::chrono::milliseconds(timeout_ms);
	for (;;) {
		const auto now = std::chrono::steady_clock::now();
		if (now >= deadline) {
			const long t = elapsed();
			::close(fd);
			return {Outcome::TimedOut, ETIMEDOUT, t};
		}
		const int remaining =
			(int)std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
		pollfd pfd;
		pfd.fd = fd;
		pfd.events = POLLOUT;
		pfd.revents = 0;
		const int pr = ::poll(&pfd, 1, remaining);
		if (pr < 0) {
			if (errno == EINTR) {
				continue;
			}
			const int e = errno;
			const long t = elapsed();
			::close(fd);
			return {Outcome::OtherError, e, t};
		}
		if (pr == 0) {
			// Nothing happened within the budget: the SYN went unanswered.
			const long t = elapsed();
			::close(fd);
			return {Outcome::TimedOut, ETIMEDOUT, t};
		}
		int so_error = 0;
		socklen_t len = sizeof(so_error);
		if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &len) != 0) {
			// Unchecked, a failed getsockopt leaves so_error at 0 and the dial
			// is misreported as CONNECTED.
			const int e = errno;
			const long t = elapsed();
			::close(fd);
			return {Outcome::OtherError, e, t};
		}
		const long t = elapsed();
		::close(fd);
		return {Classify(so_error), so_error, t};
	}
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
	FillAddr(&addr, "127.0.0.1", 0);
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
		std::printf("spec 082 dial probe — %s %s (%s)\n\n", u.sysname, u.release, u.machine);
	} else {
		std::printf("spec 082 dial probe\n\n");
	}

	// ---- Control: a LISTENING port must connect, or nothing below means anything.
	{
		uint16_t port = 0;
		int fd = BindEphemeral(&port);
		if (fd < 0 || ::listen(fd, 1) != 0) {
			std::fprintf(stderr, "  control setup FAILED (%s)\n", std::strerror(errno));
			if (fd >= 0) {
				::close(fd);
			}
			return 1;
		}
		std::printf("CONTROL — the probe's dial works\n");
		const DialResult r = Dial("127.0.0.1", port, 2000);
		Report("bound + LISTENING", r);
		::close(fd);
		if (r.outcome != Outcome::Connected) {
			std::fprintf(stderr, "\n  CONTROL VIOLATED: a listening port did not connect. Nothing below is\n");
			std::fprintf(stderr, "  trustworthy — the probe's own dial is broken in this environment.\n");
			return 1;
		}
	}

	// ---- Control: a port with NOTHING bound. This is the trick
	// TestUnreachableRoutedTargetFails settled on, and it should be a refusal.
	//
	// The premise is that nothing took the port back. The repo's own test enforces
	// that by assertion for the same reason: a reassignment — or a loopback
	// self-connect, where the dialling socket draws the destination port as its
	// own ephemeral source — would answer CONNECTED and turn the control into a
	// meaningless row. So CONNECTED here is a hard failure, not a result.
	{
		uint16_t port = 0;
		int fd = BindEphemeral(&port);
		if (fd < 0) {
			// Same doctrine as QUESTION A below: a setup failure must not print a
			// row. Falling through with fd < 0 leaves port == 0 and dials
			// 127.0.0.1:0, which answers REFUSED on both platforms and would be
			// reported as the control's result.
			std::fprintf(stderr, "\nCONTROL setup FAILED (nothing bound): %s\n", std::strerror(errno));
			return 1;
		}
		::close(fd); // free it: nothing is bound to `port` now
		std::printf("\nCONTROL — nothing bound (what the repo's test uses)\n");
		const DialResult r = Dial("127.0.0.1", port, 12000);
		Report("freed port", r);
		if (r.outcome == Outcome::Connected) {
			std::fprintf(stderr, "\n  CONTROL VIOLATED: port %u was reassigned (or self-connected) between\n", port);
			std::fprintf(stderr, "  freeing it and dialling it. This run proves nothing — rerun.\n");
			return 1;
		}
	}

	// ---- QUESTION A: bound, never listen(), fd kept OPEN for the whole dial.
	{
		uint16_t port = 0;
		int fd = BindEphemeral(&port);
		if (fd < 0) {
			std::fprintf(stderr, "\nQUESTION A setup FAILED: %s\n", std::strerror(errno));
			return 1;
		}
		std::printf("\nQUESTION A — bound but never listen()\n");
		Report("bound, NOT listening", Dial("127.0.0.1", port, 12000));
		::close(fd);
	}

	// ---- QUESTION B: does 192.0.2.1 blackhole in THIS environment?
	{
		std::printf("\nQUESTION B — 192.0.2.1:1433, RFC 5737 TEST-NET-1\n");
		Report("192.0.2.1 (3s cap)", Dial("192.0.2.1", 1433, 3000));
	}

	std::printf("\nReading it:\n");
	std::printf("  QUESTION A \"DROPPED\" / \"STILL PENDING\" => this platform DROPS the SYN, so an\n");
	std::printf("             in-process blackhole is available here (measured: macOS).\n");
	std::printf("  QUESTION A \"REFUSED\"       => this platform sends RST, so the trick gives the\n");
	std::printf("             wrong branch and W1 must be handed an address (measured: Linux).\n");
	std::printf("  QUESTION B \"STILL PENDING\" => 192.0.2.1 blackholes in THIS environment. It is\n");
	std::printf("             not evidence about any other one, a CI runner included.\n");
	std::printf("  QUESTION B \"UNREACHABLE\"   => it fails instantly here, so a test that only\n");
	std::printf("             bounds elapsed time from ABOVE passes without ever reaching the\n");
	std::printf("             timeout branch.\n");
	return 0;
}
