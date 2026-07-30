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
	case BoundaryStrategy::SkipSweep:
		return "skip+sweep";
	case BoundaryStrategy::SkipMemchr:
		return "skip+memchr";
	case BoundaryStrategy::EmbeddedNul:
		return "embedded-nul";
	}
	return "?";
}

void ColumnStaging::GrowPayload(idx_t needed) {
	grow_events++;
	// `needed` derives from a length prefix read off the wire. The cap is the one
	// place that is bounded: this repo already fuzzes the token parser for
	// exactly this shape of input (test_token_parser_security), and without the
	// bound a corrupt prefix would both wrap the uint32_t offsets and ask the
	// allocator for an arbitrary size.
	if (needed > MAX_STAGING_PAYLOAD_BYTES) {
		throw InvalidInputException(
			"MSSQL: staged column payload would reach %llu bytes, past the %llu-byte cap. This indicates a corrupt "
			"length prefix on the wire rather than legitimate data.",
			static_cast<unsigned long long>(needed), static_cast<unsigned long long>(MAX_STAGING_PAYLOAD_BYTES));
	}
	// Double, so a steady stream allocates once and then never again — capacity
	// is retained across chunks by the arena.
	idx_t target = buffer.size() * 2;
	if (target < needed) {
		target = needed;
	}
	buffer.resize(target);
}

}  // namespace staging
}  // namespace codec
}  // namespace mssql
}  // namespace duckdb
