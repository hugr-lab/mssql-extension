//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// codec/staging/column_staging.cpp
//
// Out-of-line parts of the column staging structures (spec 055 D3).
// Everything on the per-value path lives in the header so it inlines; only the
// error path is here, deliberately, to keep it out of the append body.
//===----------------------------------------------------------------------===//

#include "codec/staging/column_staging.hpp"

#include <cstring>

namespace duckdb {
namespace mssql {
namespace codec {
namespace staging {

const char *BoundaryStrategyName(BoundaryStrategy strategy) {
	switch (strategy) {
	case BoundaryStrategy::None:
		return "none";
	case BoundaryStrategy::AsciiOffsets:
		return "ascii";
	case BoundaryStrategy::Sweep:
		return "sweep";
	case BoundaryStrategy::Memchr:
		return "memchr";
	case BoundaryStrategy::EmbeddedNul:
		return "embedded-nul";
	}
	return "?";
}

void ColumnStaging::GrowPayload(idx_t needed, bool exact) {
	grow_events++;
	// `needed` derives from a length prefix read off the wire. The cap is the one
	// place that is bounded: this repo already fuzzes the token parser for
	// exactly this shape of input (test_token_parser_security), and without the
	// bound a corrupt prefix would both wrap the uint32_t offsets and ask the
	// allocator for an arbitrary size.
	if (needed > payload_cap) {
		throw InvalidInputException(
			"MSSQL: staged column payload would reach %llu bytes, past the %llu-byte cap. This indicates a corrupt "
			"length prefix on the wire rather than legitimate data.",
			static_cast<unsigned long long>(needed), static_cast<unsigned long long>(payload_cap));
	}
	if (exact) {
		// Exactly `needed`, of which only the staged bytes are copied: std::vector's own growth would
		// double the capacity and copy the whole old size (review of PR 3 S4).
		PayloadBuffer next;
		next.reserve(needed);
		next.resize(needed);
		if (payload_used > 0) {
			std::memcpy(next.data(), buffer.data(), payload_used);
		}
		buffer.swap(next);
		if (needed > payload_bound) {
			payload_bound = 0;
			payload_bounded = false;
		}
		return;
	}
	// Double, so a steady stream allocates once and then never again — capacity
	// is retained across chunks by the arena.
	idx_t target = buffer.empty() ? STAGING_INITIAL_PAYLOAD_BYTES : buffer.size() * 2;
	if (target < needed) {
		target = needed;
	}
	if (target > payload_cap) {
		target = payload_cap;  // doubling stops at the cap; `needed` is within it
	}
	// A value longer than its declared type allows (the wire's length is not
	// checked against COLMETADATA): there is no bound after all, and clamping
	// would turn every later growth into an exact-size reallocation.
	if (needed > payload_bound) {
		payload_bound = 0;
		payload_bounded = false;
	}
	// Never past the column's provable worst case: once there it cannot grow
	// again, and the arena leaves it alone.
	if (payload_bound > 0 && target >= payload_bound) {
		target = payload_bound;
		payload_bounded = true;
	}
	buffer.resize(target);
}

}  // namespace staging
}  // namespace codec
}  // namespace mssql
}  // namespace duckdb
