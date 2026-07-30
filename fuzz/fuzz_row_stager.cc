// Fuzz harness: the STAGED read path (spec 055) — the walk that replaced the
// one fuzz_tds_tokens covers.
//
// The parser's raw-row mode hands whole ROW / NBCROW payloads to
// codec::staging::RowStager, which walks them column by column with NO bounds
// test per value: its safety rests entirely on the parser having established the
// row's exact length first. That argument is what this fuzzes. Three
// out-of-bounds defects were found in this walk by reading code, none by a test,
// and until spec 059 D4 nothing fuzzed it at all — while the per-value path it
// replaced is fuzzed weekly.
//
// Unlike the other harnesses here this one links libduckdb: the stager writes
// into duckdb::Vector. It is therefore NOT in the default TARGETS; build it with
//
//   MSSQL_DUCKDB_INC=<duckdb/src/include> MSSQL_DUCKDB_LIB=<dir with libduckdb>
//   TARGETS=row_stager fuzz/build.sh
//
// The input is a raw token stream, exactly as fuzz_tds_tokens takes it: the
// bytes after the 8-byte TDS packet header. A COLMETADATA token has to come
// first for anything interesting to happen, which is what the seed corpus is
// for.
//
// A throw is a PASS. This code rejects malformed framing by design
// (ThrowBadPrefix, ThrowOddUtf16Length, ThrowNbcNullPrefix, ThrowUnsupportedType,
// and the LOB length cap), so only a sanitizer report, a crash or a hang is a
// finding.

#include "codec/staging/row_stager.hpp"
#include "copy/target_resolver.hpp"
#include "duckdb/common/types/vector.hpp"
#include "tds/encoding/type_converter.hpp"
#include "tds/tds_token_parser.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <memory>
#include <vector>

namespace duckdb {
namespace mssql {
//! The family codecs call BCPRowEncoder::EncodeDecimal, which drags in
//! bcp_row_encoder.cpp, which references this one method — and it is defined in
//! target_resolver.cpp, a file that pulls the whole catalog layer in behind it.
//! Nothing on the READ path can reach it, so it aborts rather than answering:
//! a stub that returned a plausible value could hide a wrong turn.
bool BCPColumnMetadata::IsVariableLengthUSHORT() const {
	std::abort();
}
}  // namespace mssql
}  // namespace duckdb

namespace {

//! Output vectors for one result set, kept alive for as long as the stager is
//! configured against them.
struct Targets {
	std::vector<std::unique_ptr<duckdb::Vector>> owned;
	std::vector<duckdb::Vector *> pointers;
};

//! Build one output vector per column, of the type the wire metadata implies.
//! A column whose type GetDuckDBType cannot name gets a null target, which is
//! how the stream marks a column it parses for its length and discards — the
//! Skip arm, itself worth fuzzing.
bool BuildTargets(const std::vector<duckdb::tds::ColumnMetadata> &columns, Targets &out) {
	out.owned.clear();
	out.pointers.clear();
	if (columns.empty() || columns.size() > 1024) {
		return false;
	}
	for (const auto &col : columns) {
		try {
			out.owned.push_back(std::unique_ptr<duckdb::Vector>(
				new duckdb::Vector(duckdb::tds::encoding::TypeConverter::GetDuckDBType(col))));
			out.pointers.push_back(out.owned.back().get());
		} catch (const std::exception &) {
			out.owned.push_back(nullptr);
			out.pointers.push_back(nullptr);
		}
	}
	return true;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
	if (size > 1 << 20) {
		return 0;
	}
	try {
		duckdb::tds::TokenParser parser;
		parser.SetRawRowMode(true);
		parser.Feed(data, size);

		duckdb::mssql::codec::staging::RowStager stager;
		Targets targets;
		duckdb::idx_t row_in_chunk = 0;

		for (int i = 0; i < 100000; ++i) {
			const duckdb::tds::ParsedTokenType token = parser.TryParseNext();
			if (token == duckdb::tds::ParsedTokenType::NeedMoreData || token == duckdb::tds::ParsedTokenType::None) {
				break;
			}
			if (token == duckdb::tds::ParsedTokenType::ColMetadata) {
				// A new result set: everything staged so far is abandoned, which is
				// what the stream does too.
				stager.Invalidate();
				row_in_chunk = 0;
				if (BuildTargets(parser.GetColumnMetadata(), targets)) {
					stager.Configure(parser.GetColumnMetadata(), targets.pointers);
					stager.BeginChunk(targets.pointers);
				}
				continue;
			}
			if (token != duckdb::tds::ParsedTokenType::Row || !stager.IsConfigured()) {
				continue;
			}
			const uint8_t *row = parser.GetRawRow();
			const size_t row_length = parser.GetRawRowLength();
			if (parser.IsRawRowNBC()) {
				stager.StageNBCRow(row, row_length, row_in_chunk);
			} else {
				stager.StageRow(row, row_length, row_in_chunk);
			}
			row_in_chunk++;
			if (row_in_chunk == STANDARD_VECTOR_SIZE) {
				stager.FinalizeChunk(row_in_chunk);
				row_in_chunk = 0;
				stager.BeginChunk(targets.pointers);
			}
		}
		if (stager.IsConfigured() && row_in_chunk > 0) {
			stager.FinalizeChunk(row_in_chunk);
		}
	} catch (const std::exception &) {
		// Malformed framing, rejected by design. Not a finding.
	}
	return 0;
}
