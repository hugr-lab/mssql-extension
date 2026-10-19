// Fuzz harness: the BCP/PLP WRITE path — the first harness here pointed
// outbound.
//
// Every other harness in this directory decodes bytes a hostile or MITM'd SQL
// Server sends us. None of them touches the encoders, and that is where the
// framing bugs have actually been: #489 (an nvarchar(max) chunk length declared
// `length mod 2^32` while every payload byte was still written — a session
// desync), the review of #490 (the same arithmetic narrowed BEFORE the UTF-16
// doubling multiply, so the columnar path wrote a full payload into a buffer
// sized from a wrapped length — a client-side heap overflow), #181 (varchar
// declared as nvarchar at 1x bytes) and #91 (the non-PLP bound in the wrong
// unit). All four were found by reading code. Nothing automated could see them.
//
// WHY A PARSER-STYLE HARNESS WOULD NOT HAVE FOUND THEM
//
// ASan is the whole oracle for a decoder: feed it garbage, see if it reads out
// of bounds. On the encode side the inputs are OUR OWN values, so ASan alone
// catches only the subset that overruns a buffer (#490's non-ASCII arm). A
// declared length that disagrees with the bytes written (#489) is memory-safe
// and silent — the server desyncs, or stores a truncated value, long after the
// process that produced it exited cleanly. So this harness carries three
// explicit oracles of its own:
//
//   A. FRAMING. Walk the emitted bytes back as SQL Server's tokenizer would and
//      require the frame to account for itself exactly: a PLP value is an
//      8-byte UNKNOWN-length header, a 4-byte chunk length, that many payload
//      bytes and a 4-byte terminator; the USHORT form is a 2-byte length and
//      that many bytes. The declared length must equal `plan.lengths[r]` and
//      the frame must consume exactly `plan.wire[r]`. This is the #489 class at
//      any size: nothing about it needs a 2 GiB value to express.
//
//   B. DIFFERENTIAL. The row path and the columnar path are two
//      implementations of one wire format, and string_codec.cpp says so in as
//      many words — "the columnar path clamps during planning; this keeps the
//      two producing the same bytes". So for every non-NULL row the two must
//      agree byte for byte. This is exactly the asymmetry that produced the
//      review of #490: the guards went onto the row path, which a valid
//      nvarchar(max) value never takes, and the columnar path kept the
//      narrowing. A differential oracle does not care which of the two is
//      wrong, which is the point — it fails on the disagreement.
//
//   C. ROUND TRIP. Decode the payload back and require it to be a PREFIX of the
//      source value (a prefix, not equal: clamping to a bounded column's
//      declared length is documented truncation, spec 057 3g). This catches the
//      silent-truncation half of #490, where the declaration and the payload
//      agreed with each other and both were wrong, and it puts
//      Utf8PrefixForUtf16Limit under test at every bound — including bounds that
//      fall inside a surrogate pair, which is where #91 lived.
//
// WHAT THIS HARNESS CANNOT DO, STATED PLAINLY
//
// It cannot build the >2 GiB value that #489 and #490 needed, so it will not
// rediscover those two instances. It fuzzes their CLASS at reachable sizes. The
// 2 GiB arithmetic itself stays covered by the unit tests on PlanOneLength /
// PlannedNVarcharBytes / RequirePlpWireBytes, which drive it with plain numbers
// precisely because no test can allocate the value. The two kinds of coverage
// are complements, not substitutes.
//
// Like fuzz_row_stager this links libduckdb (the encoders read duckdb::Vector),
// so it is NOT in the default TARGETS and does not run in the per-PR
// ClusterFuzzLite job. Build it where a libduckdb already exists:
//
//   MSSQL_DUCKDB_INC=duckdb/src/include MSSQL_DUCKDB_LIB=build/release/src \
//   TARGETS=bcp_encode fuzz/build.sh
//
// A THROW IS A PASS. The encoders refuse values by design — RequirePlpWireBytes
// for a MAX value past 2^31-1, NotImplementedException for a type the planner
// does not handle. Only a sanitizer report, a signal, or one of the three
// oracles above firing is a finding.
//
// INPUT LAYOUT
//
//   byte 0   config
//              bits 0-1  column kind: 0,3 = nvarchar   (TDS_TYPE_NVARCHAR)
//                                     1   = utf8 varchar (TDS_TYPE_BIGVARCHAR)
//                                     2   = varbinary  (TDS_TYPE_BIGVARBINARY)
//              bit  2    PLP: declare the column MAX (max_length = 0xFFFF)
//              bits 3-7  index into BOUNDS[] for the declared byte bound,
//                        used when the column is not MAX
//   byte 1..  values, each  [LE16 length][length bytes]
//              length 0xFFFF marks a SQL NULL and is followed by no bytes
//
// The bound table deliberately includes odd and tiny values. max_length reaches
// BCPColumnMetadata from the server's own sys.columns (FromServerColumn), so it
// is not a constant we choose, and a clamp that assumes an even UTF-16 bound is
// an assumption worth testing rather than trusting.

#include "codec/string_codec.hpp"
#include "copy/target_resolver.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/string_vector.hpp"
#include "tds/encoding/utf16.hpp"
#include "tds/tds_types.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>
#include <vector>

namespace duckdb {
namespace mssql {
//! string_codec.cpp does not call this, but the family codecs it is compiled
//! beside drag in bcp_row_encoder.cpp, which references it — and the definition
//! lives in target_resolver.cpp, a file that pulls the whole catalog layer in
//! behind it. Nothing this harness drives can reach it, so it aborts rather
//! than answering: a stub returning a plausible value could hide a wrong turn.
//! Same reasoning, same stub, as fuzz_row_stager.cc.
bool BCPColumnMetadata::IsVariableLengthUSHORT() const {
	std::abort();
}
}  // namespace mssql
}  // namespace duckdb

namespace {

//! An oracle failing is a BUG REPORT, not a rejected input, so it must not be
//! swallowed by the catch below. Print what disagreed and abort — libFuzzer and
//! AFL++ both treat the signal as a crash and save the input.
//! `#cond` goes through as an ARGUMENT, never as the format string: a condition
//! containing a `%` (`size % 2 == 0`) would otherwise be read as a conversion
//! specifier and the report itself would be undefined behaviour.
#define ORACLE(cond, ...)                                       \
	do {                                                        \
		if (!(cond)) {                                          \
			std::fprintf(stderr, "ORACLE FAILED: %s\n", #cond); \
			std::fprintf(stderr, __VA_ARGS__);                  \
			std::fprintf(stderr, "\n");                         \
			std::abort();                                       \
		}                                                       \
	} while (0)

//! Declared byte bounds for the non-MAX column. Real nvarchar(n) always reports
//! an even 2n, so the odd entries are the adversarial half: they ask whether the
//! clamp can be made to split a UTF-16 code unit or a surrogate pair.
const uint16_t BOUNDS[32] = {1,	   2,	  3,	 4,		6,	   7,	  8,	 10,	16,	   20,	 30,
							 40,   50,	  100,	 200,	255,   256,	  510,	 1000,	2000,  4000, 8000,
							 8001, 16000, 24000, 32000, 40000, 50000, 60000, 65000, 65534, 12};

const size_t MAX_ROWS = 64;
const size_t MAX_VALUE_BYTES = 4096;
//! A ceiling on the whole emitted block, so a pathological plan cannot turn the
//! campaign into an allocator benchmark.
const size_t MAX_WIRE_BYTES = 8u << 20;

constexpr uint64_t UNKNOWN_PLP_LEN = 0xFFFFFFFFFFFFFFFEULL;

uint32_t ReadLe32(const uint8_t *p) {
	return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
		   (static_cast<uint32_t>(p[3]) << 24);
}

uint16_t ReadLe16(const uint8_t *p) {
	return static_cast<uint16_t>(static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8));
}

//! One parsed input value: `is_null`, or `bytes`.
struct InValue {
	bool is_null = false;
	std::string bytes;
};

//! ORACLE A — walk one value's frame exactly as the server's tokenizer would
//! and require it to account for itself. Returns the payload so the caller can
//! run oracle C on it.
//!
//! `frame` is the slice the plan reserved for this row: `plan.wire[r]` bytes at
//! the row's cursor. Every length in here is one the ENCODER wrote; the only
//! inputs from the plan are the two numbers the frame is checked against, which
//! is what makes a disagreement between them a finding.
std::string CheckFrame(const uint8_t *frame, size_t frame_len, bool plp, bool is_null, uint32_t planned_len,
					   uint32_t planned_wire, size_t row) {
	ORACLE(frame_len == planned_wire, "row %zu: frame slice %zu != plan.wire %u", row, frame_len, planned_wire);

	if (is_null) {
		// PLP NULL is eight 0xFF bytes; the USHORT form is 0xFFFF.
		const size_t want = plp ? 8u : 2u;
		ORACLE(frame_len == want, "row %zu: NULL frame is %zu bytes, want %zu", row, frame_len, want);
		for (size_t i = 0; i < want; i++) {
			ORACLE(frame[i] == 0xFF, "row %zu: NULL frame byte %zu is 0x%02X, want 0xFF", row, i, frame[i]);
		}
		ORACLE(planned_len == 0, "row %zu: NULL planned a length of %u", row, planned_len);
		return std::string();
	}

	if (!plp) {
		ORACLE(frame_len >= 2, "row %zu: USHORT frame too short (%zu)", row, frame_len);
		const uint16_t declared = ReadLe16(frame);
		ORACLE(declared != 0xFFFF, "row %zu: a non-NULL value declared the NULL sentinel 0xFFFF", row);
		ORACLE(declared == planned_len, "row %zu: declared %u != plan.lengths %u", row, declared, planned_len);
		ORACLE(static_cast<size_t>(2) + declared == frame_len,
			   "row %zu: USHORT frame declares %u payload bytes in a %zu-byte slice", row, declared, frame_len);
		return std::string(reinterpret_cast<const char *>(frame + 2), declared);
	}

	// PLP: 8-byte UNKNOWN-length header, then chunks, then a 4-byte terminator.
	ORACLE(frame_len >= 12, "row %zu: PLP frame too short (%zu)", row, frame_len);
	for (int i = 0; i < 8; i++) {
		const uint8_t want = static_cast<uint8_t>((UNKNOWN_PLP_LEN >> (i * 8)) & 0xFF);
		ORACLE(frame[i] == want, "row %zu: PLP header byte %d is 0x%02X, want 0x%02X", row, i, frame[i], want);
	}
	const uint32_t chunk = ReadLe32(frame + 8);
	ORACLE(chunk == planned_len, "row %zu: PLP chunk length %u != plan.lengths %u", row, chunk, planned_len);
	if (chunk == 0) {
		// An empty value is the header plus the terminator and carries no chunk.
		ORACLE(frame_len == 12, "row %zu: empty PLP frame is %zu bytes, want 12", row, frame_len);
		return std::string();
	}
	// The single-chunk form every encoder here writes: header, length, payload,
	// terminator. A declared length that overruns its own slice is #489 exactly.
	ORACLE(static_cast<size_t>(16) + chunk == frame_len,
		   "row %zu: PLP declares %u payload bytes in a %zu-byte slice (#489 shape)", row, chunk, frame_len);
	const uint32_t terminator = ReadLe32(frame + 12 + chunk);
	ORACLE(terminator == 0, "row %zu: PLP terminator is %u, want 0", row, terminator);
	return std::string(reinterpret_cast<const char *>(frame + 12), chunk);
}

//! ORACLE C — the payload must decode back to a PREFIX of what went in.
//!
//! A prefix rather than equality because clamping a value to a bounded column's
//! declared length is documented truncation. Only run for input that was valid
//! UTF-8: invalid input is substituted with U+FFFD by design, so it does not
//! round-trip and must not be asserted on.
void CheckRoundTrip(const std::string &payload, const InValue &in, bool is_nvarchar, size_t row) {
	if (!is_nvarchar) {
		// The payload IS the source bytes for a UTF-8 varchar and for binary.
		ORACLE(payload.size() <= in.bytes.size(), "row %zu: payload %zu bytes longer than the %zu-byte source", row,
			   payload.size(), in.bytes.size());
		ORACLE(std::memcmp(payload.data(), in.bytes.data(), payload.size()) == 0,
			   "row %zu: payload is not a prefix of the source", row);
		return;
	}
	// nvarchar: UTF-16LE on the wire. An odd payload length cannot be a
	// sequence of 16-bit code units at all.
	ORACLE(payload.size() % 2 == 0, "row %zu: nvarchar payload is %zu bytes — not whole UTF-16 code units", row,
		   payload.size());
	const std::string back =
		duckdb::tds::encoding::Utf16LEDecode(reinterpret_cast<const uint8_t *>(payload.data()), payload.size());
	ORACLE(back.size() <= in.bytes.size(), "row %zu: decoded %zu bytes from a %zu-byte source", row, back.size(),
		   in.bytes.size());
	ORACLE(std::memcmp(back.data(), in.bytes.data(), back.size()) == 0,
		   "row %zu: decoded payload is not a prefix of the source", row);
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
	if (size < 2 || size > (1u << 18)) {
		return 0;
	}

	// --- configuration -------------------------------------------------------
	const uint8_t config = data[0];
	const unsigned kind_sel = config & 0x03;
	const bool plp = (config & 0x04) != 0;
	const uint16_t bound = BOUNDS[(config >> 3) & 0x1F];

	uint8_t tds_type = duckdb::tds::TDS_TYPE_NVARCHAR;
	bool is_nvarchar = true;
	bool is_binary = false;
	if (kind_sel == 1) {
		tds_type = duckdb::tds::TDS_TYPE_BIGVARCHAR;
		is_nvarchar = false;
	} else if (kind_sel == 2) {
		tds_type = duckdb::tds::TDS_TYPE_BIGVARBINARY;
		is_nvarchar = false;
		is_binary = true;
	}

	// --- values --------------------------------------------------------------
	std::vector<InValue> values;
	size_t at = 1;
	while (at + 2 <= size && values.size() < MAX_ROWS) {
		const uint16_t declared = ReadLe16(data + at);
		at += 2;
		InValue v;
		if (declared == 0xFFFF) {
			v.is_null = true;
			values.push_back(v);
			continue;
		}
		size_t take = declared;
		if (take > MAX_VALUE_BYTES) {
			take = MAX_VALUE_BYTES;
		}
		if (at + take > size) {
			take = size - at;
		}
		v.bytes.assign(reinterpret_cast<const char *>(data + at), take);
		at += take;
		values.push_back(v);
	}
	if (values.empty()) {
		return 0;
	}
	const duckdb::idx_t rows = static_cast<duckdb::idx_t>(values.size());

	try {
		// --- the source vector ------------------------------------------------
		duckdb::Vector vec(is_binary ? duckdb::LogicalType::BLOB : duckdb::LogicalType::VARCHAR, rows);
		// The 2.0 contract (spec 069): a producer writes through the Mutable
		// accessors, which are checked in release builds.
		auto *slots = duckdb::FlatVector::GetDataMutable<duckdb::string_t>(vec);
		auto &validity = duckdb::FlatVector::ValidityMutable(vec);
		for (duckdb::idx_t r = 0; r < rows; r++) {
			if (values[r].is_null) {
				validity.SetInvalid(r);
				continue;
			}
			const auto &b = values[r].bytes;
			slots[r] = is_binary ? duckdb::StringVector::AddStringOrBlob(vec, b.data(), b.size())
								 : duckdb::StringVector::AddString(vec, b.data(), b.size());
		}
		duckdb::UnifiedVectorFormat fmt;
		vec.ToUnifiedFormat(fmt);

		// --- the target column ------------------------------------------------
		duckdb::mssql::BCPColumnMetadata col;
		col.name = "c0";
		col.tds_type_token = tds_type;
		col.max_length = plp ? 0xFFFF : bound;
		col.duckdb_type = is_binary ? duckdb::LogicalType::BLOB : duckdb::LogicalType::VARCHAR;
		col.nullable = true;

		// --- the columnar path ------------------------------------------------
		duckdb::mssql::codec::string::StringColumnPlan plan;
		const bool planned = duckdb::mssql::codec::string::PlanColumn(vec, fmt, 0, rows, col, plan);

		if (!planned) {
			// The documented contract: the caller falls back to the row path.
			// Exercise that fallback — it is where the cold-path encoders live
			// (AppendNVarcharPlp, whose #489 guard runs only AFTER the encode has
			// written into an already-sized buffer) — and frame-check each value.
			if (is_binary) {
				return 0;  // no row-path partner for binary in this module
			}
			for (duckdb::idx_t r = 0; r < rows; r++) {
				if (values[r].is_null) {
					continue;
				}
				duckdb::vector<uint8_t> one;
				if (is_nvarchar) {
					duckdb::mssql::codec::string::EncodeToBcp(vec, fmt, r, col, one);
				} else {
					duckdb::mssql::codec::string::EncodeToBcpUtf8(vec, fmt, r, col, one);
				}
				// The row path appends exactly one frame, so its whole buffer IS
				// the frame; its own declared length is the only thing to check
				// it against.
				ORACLE(!one.empty(), "row %llu: row path emitted nothing", (unsigned long long)r);
				const uint32_t declared = plp ? ReadLe32(one.data() + 8) : static_cast<uint32_t>(ReadLe16(one.data()));
				CheckFrame(one.data(), one.size(), plp, false, declared, static_cast<uint32_t>(one.size()), r);
			}
			return 0;
		}

		// Lay the block out exactly as BCPRowEncoder::EncodeChunk does: a cursor
		// per row, each advanced by that row's planned wire size.
		std::vector<size_t> cursor(rows);
		size_t total = 0;
		for (duckdb::idx_t r = 0; r < rows; r++) {
			cursor[r] = total;
			total += plan.wire[r];
			if (total > MAX_WIRE_BYTES) {
				return 0;
			}
		}
		ORACLE(plan.lengths.size() == rows, "plan.lengths holds %zu entries for %llu rows", plan.lengths.size(),
			   (unsigned long long)rows);
		ORACLE(plan.wire.size() == rows, "plan.wire holds %zu entries for %llu rows", plan.wire.size(),
			   (unsigned long long)rows);

		// A canary byte each side of the block: ASan catches a write past the
		// end, but a write that lands INSIDE the block at the wrong offset is
		// what the per-row frame checks below are for.
		std::vector<uint8_t> block(total, 0xAA);
		const std::vector<size_t> start = cursor;
		duckdb::mssql::codec::string::ScatterVarBlock(block.data(), cursor.data(), 0, rows, fmt, col, plan);

		for (duckdb::idx_t r = 0; r < rows; r++) {
			// The scatter must have advanced each cursor by exactly the planned
			// wire size — the accounting the chunk's total was computed from.
			ORACLE(cursor[r] == start[r] + plan.wire[r], "row %llu: cursor advanced %zu, plan.wire said %u",
				   (unsigned long long)r, cursor[r] - start[r], plan.wire[r]);

			// ORACLE A.
			const std::string payload = CheckFrame(block.data() + start[r], plan.wire[r], plan.plp, values[r].is_null,
												   plan.lengths[r], plan.wire[r], r);
			if (values[r].is_null) {
				continue;
			}

			// ORACLE C.
			CheckRoundTrip(payload, values[r], is_nvarchar, r);

			// ORACLE B — the row path must produce the same frame, byte for
			// byte. Binary has no row-path partner in this module.
			if (is_binary) {
				continue;
			}
			duckdb::vector<uint8_t> one;
			if (is_nvarchar) {
				duckdb::mssql::codec::string::EncodeToBcp(vec, fmt, r, col, one);
			} else {
				duckdb::mssql::codec::string::EncodeToBcpUtf8(vec, fmt, r, col, one);
			}
			ORACLE(one.size() == plan.wire[r], "row %llu: row path wrote %zu bytes, columnar plan reserved %u",
				   (unsigned long long)r, one.size(), plan.wire[r]);
			ORACLE(std::memcmp(one.data(), block.data() + start[r], one.size()) == 0,
				   "row %llu: row path and columnar path disagree on the bytes (the #490 shape)",
				   (unsigned long long)r);
		}
	} catch (const std::exception &) {
		// By design: RequirePlpWireBytes refusing a MAX value past 2^31-1, a
		// NotImplementedException for a type the planner does not handle, a bad
		// allocation on a hostile size. Not a finding — the oracles above abort
		// rather than throw, so they are never caught here.
	}
	return 0;
}
