#pragma once

#include <atomic>
#include <cstdint>
#include <cstdlib>

namespace duckdb {
namespace mssql {

//===----------------------------------------------------------------------===//
// Performance counters — their own switch, separate from MSSQL_DEBUG
//
// The counters report per-phase nanoseconds, and MSSQL_DEBUG turns on logging
// that sits INSIDE the phases they time. Both sides of the wire have been
// measured lying because of it:
//
//   read   at MSSQL_DEBUG=2 the parser fprintf's every token from inside the
//          function the parse timer wraps — `parse` went 22 -> 1133 ns/row and
//          the socket wait collapsed 128 -> 12. The numbers did not get noisy,
//          they inverted, naming parsing as 95% of the work when it is ~11%.
//          Spec 058's `parse=577 ns/row` reading was withdrawn for this reason.
//
//   write  BCPCopySink logged a DONE line per CHUNK at level 1, and the logger
//          fflush'es. At 500k rows that is ~245 fprintf+fflush inside the timed
//          path; client CPU for the same statement measured 0.047 s clean vs
//          0.194 s at MSSQL_DEBUG=1 — a 4x inflation of the very quantity the
//          write-path phase split is trying to attribute.
//
// So: MSSQL_COUNTERS=1 enables the counters with logging OFF, which is the only
// configuration whose numbers mean anything. MSSQL_DEBUG>=1 still enables them,
// because that is where they lived and scripts depend on it — but a run that
// wants trustworthy timings must use MSSQL_COUNTERS and leave MSSQL_DEBUG unset.
//
// Latched once: a benchmark must not pay a getenv per chunk, and the answer
// cannot change mid-process.
//===----------------------------------------------------------------------===//

inline bool CountersEnabled() {
	static const bool enabled = []() {
		const char *counters = std::getenv("MSSQL_COUNTERS");
		if (counters) {
			return std::atoi(counters) > 0;
		}
		const char *debug = std::getenv("MSSQL_DEBUG");
		return debug && std::atoi(debug) >= 1;
	}();
	return enabled;
}

//! True when logging is on loudly enough to distort what the counters measure.
//! Used to print the warning next to the numbers rather than only in a comment.
inline bool CountersConfoundedByLogging() {
	static const bool confounded = []() {
		const char *debug = std::getenv("MSSQL_DEBUG");
		return debug && std::atoi(debug) >= 1;
	}();
	return confounded;
}

//===----------------------------------------------------------------------===//
// Encode-path attribution (spec 064 reconnaissance)
//
// The write path has three shapes and picks one PER CHUNK, and nothing reported
// which. That makes the central question of the columnar work unanswerable from
// the outside: "what fraction of this table's chunks fell off the fast path, and
// which column pushed them?" Every number in the spec-057 series was a total,
// and a total cannot distinguish a slow kernel from a chunk that never reached
// one.
//
// Process-wide rather than per-statement on purpose: this is reconnaissance, the
// benchmark runs one load per process, and threading it through four state
// structs to answer a question about a resolver would be a lot of machinery for
// a measurement. Relaxed ordering — these are counts, never a decision.
//===----------------------------------------------------------------------===//

struct EncodePathCounters {
	//! Every column fixed-width and no NULLs: one kernel call per column per
	//! block, constant stride. The fast path.
	std::atomic<uint64_t> chunks_strided{0};
	//! A variable-length column, or a NULL in a fixed-width one. One kernel call
	//! per column per block for every arm — decimal, uuid and datetime included
	//! since spec 064 D1 (this counter was born measuring their per-value era).
	std::atomic<uint64_t> chunks_cursor{0};
	//! Some column resolved to RowFallback, or a string column could not be
	//! planned: no kernels at all, for ANY column in the chunk.
	std::atomic<uint64_t> chunks_row_major{0};

	//! Why the row path was taken, counted separately because the two have
	//! different fixes: an unsupported (source, target) PAIR is resolver work,
	//! an unplannable string column is codec work.
	std::atomic<uint64_t> fallback_unsupported_pair{0};
	std::atomic<uint64_t> fallback_string_plan{0};

	//! The column index that most recently forced the row path, and the arm it
	//! resolved to. Last-writer-wins: enough to name a culprit in a benchmark,
	//! not enough to attribute a mixed workload.
	std::atomic<uint64_t> last_fallback_column{0};
	std::atomic<uint64_t> last_fallback_arm{0};
};

inline EncodePathCounters &GetEncodePathCounters() {
	static EncodePathCounters counters;
	return counters;
}

}  // namespace mssql
}  // namespace duckdb
