#pragma once

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

}  // namespace mssql
}  // namespace duckdb
