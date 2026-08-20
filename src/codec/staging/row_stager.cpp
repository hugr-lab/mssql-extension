//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// codec/staging/row_stager.cpp — spec 055 T5.
//===----------------------------------------------------------------------===//

#include "codec/staging/row_stager.hpp"

#include "codec/binary_codec.hpp"
#include "codec/datetime_codec.hpp"
#include "codec/decimal_codec.hpp"
#include "codec/money_codec.hpp"
#include "codec/string_codec.hpp"
#include "codec/uuid_codec.hpp"
#include "duckdb/common/exception.hpp"
#include "mssql_compat.hpp"
#include "tds/encoding/type_converter.hpp"

#include <chrono>
#include <cstring>

namespace duckdb {
namespace mssql {
namespace codec {
namespace staging {

namespace {

//! PLP total-length markers (MS-TDS): all bits set means NULL, one less means
//! the total is not declared and only the chunk terminator ends the value.
static const uint64_t PLP_NULL_MARKER = 0xFFFFFFFFFFFFFFFFULL;

//! Out of line and cold: a length prefix that is neither the declared width nor
//! the NULL marker cannot come from a conforming server, so it is a corrupt or
//! hostile stream. Keeping the throw out of the append body keeps the hot arm
//! down to a compare and a sized store.
//! Cold: the extension has no framing for this wire type, so no value of it can
//! be read. Thrown when a value ARRIVES, not at column resolution, so a query
//! that selects such a column and returns no rows still succeeds.
//!
//! A guard rather than a path anyone is expected to hit: the types with no
//! framing (TEXT/NTEXT/IMAGE uncast, UDT, SQL_VARIANT) break the COLMETADATA
//! parse first, upstream of here. Catalog scans never produce them at all —
//! BuildColumnExpression casts them server-side.
[[noreturn]] void ThrowUnsupportedType(const tds::ColumnMetadata &column) {
	throw InvalidInputException(
		"MSSQL: column '%s' arrives as TDS type 0x%02X, which this extension cannot decode. CAST it in the query to a "
		"type it can — NVARCHAR(MAX) for text-like values, VARBINARY(MAX) for binary ones.",
		column.name, column.type_id);
}

[[noreturn]] void ThrowBadPrefix(uint32_t expected, uint32_t actual) {
	throw InvalidInputException(
		"MSSQL: fixed-width column arrived with a %u-byte length prefix where %u or 0 (NULL) was required. The TDS "
		"stream is malformed.",
		actual, expected);
}

//! A UTF-16 value must be a whole number of code units. This is not a taste
//! check — it is what makes the batch decode's boundary walk provable.
//!
//! Each staged value carries a U+0000 code unit after it, and the walk relies on
//! that unit converting to exactly one 0x00 byte. At an ODD length the two zero
//! bytes straddle a code-unit boundary and become the low half of one unit and
//! the high half of the next, so they convert to no zero byte at all: the walk
//! then finds no delimiter for that value, runs to the end of the blob, and the
//! NEXT value searches from past the end — `limit - from` underflows and memchr
//! reads out of bounds. SkipValue accepts any 2-byte length, so a corrupt or
//! hostile stream reaches here; a conforming server never does.
//! In an NBC row a NULL is expressed by the bitmap, so a value the bitmap calls
//! present cannot also carry the 0xFFFF NULL prefix. SkipValueNBC is lenient
//! about it — it returns 2 and lets the row through — so the check has to be
//! here, and it has to be a throw: taking 0xFFFF as a LENGTH copies 65535 bytes
//! from beyond the row, and past the receive buffer for a row near its tail.
[[noreturn]] void ThrowNbcNullPrefix() {
	throw InvalidInputException(
		"MSSQL: a column marked present by an NBC row's null bitmap carries the 0xFFFF NULL length prefix. The TDS "
		"stream is malformed.");
}

[[noreturn]] void ThrowOddUtf16Length(uint32_t length) {
	throw InvalidInputException(
		"MSSQL: a UTF-16 column arrived with a %u-byte value, which is not a whole number of 2-byte code units. The "
		"TDS "
		"stream is malformed.",
		length);
}

//! One value on a 1-byte-prefixed direct column.
//!
//! `len == STRIDE` is the only fast case, and testing it first means the single
//! compare that selects it also rejects NULL — no separate null test on the
//! common path. Returns bytes consumed.
template <uint32_t STRIDE>
inline size_t AppendPrefixedDirect(ColumnStaging &st, const uint8_t *p) {
	const uint32_t len = p[0];
	if (len == STRIDE) {
		st.AppendDirect<STRIDE>(p + 1);
		return 1 + STRIDE;
	}
	if (len == 0) {
		st.AppendNull();
		return 1;
	}
	ThrowBadPrefix(STRIDE, len);
}

//! One value on a 1-byte-prefixed column staged for a batch kernel. Same shape
//! as AppendPrefixedDirect, but the bytes go to the staging buffer instead of
//! straight into the output vector.
template <uint32_t STRIDE>
inline size_t AppendPrefixedFixed(ColumnStaging &st, const uint8_t *p) {
	const uint32_t len = p[0];
	if (len == STRIDE) {
		st.AppendFixed<STRIDE>(p + 1);
		return 1 + STRIDE;
	}
	if (len == 0) {
		st.AppendNull();
		return 1;
	}
	ThrowBadPrefix(STRIDE, len);
}

//! Stage a fixed-width value whose width is a runtime property of the column.
//!
//! The switch is what keeps the copy compile-time sized — a memcpy with a
//! runtime length is a call into libc, which costs more than a perfectly
//! predicted branch on a value that never changes within a column. It lives in
//! one place, called from one arm in each walk, so the two walks cannot drift
//! apart: forgetting a width here is a compile error in neither, but forgetting
//! an ARM is caught by -Wswitch.
//!
//! `default` is unreachable: ResolveColumnOps only chooses these arms for a
//! width listed in IsStageableFixedWidth.
//! DECIMAL's append: like AppendPrefixedFixed, but a value shorter than the
//! declared width is zero-extended rather than rejected. The mantissa is
//! little-endian, so that is exactly value-preserving, and it keeps a server
//! that trims trailing zero bytes on the batch path.
template <uint32_t STRIDE>
inline size_t AppendPrefixedDecimal(ColumnStaging &st, const uint8_t *p) {
	const uint32_t len = p[0];
	if (len == STRIDE) {
		st.AppendFixed<STRIDE>(p + 1);
		return 1 + STRIDE;
	}
	if (len == 0) {
		st.AppendNull();
		return 1;
	}
	if (len < STRIDE) {
		st.AppendFixedPadded<STRIDE>(p + 1, len);
		return 1 + len;
	}
	ThrowBadPrefix(STRIDE, len);
}

inline size_t AppendStagedDecimal(ColumnStaging &st, const uint8_t *p) {
	switch (st.stride) {
	case 5:
		return AppendPrefixedDecimal<5>(st, p);
	case 9:
		return AppendPrefixedDecimal<9>(st, p);
	case 13:
		return AppendPrefixedDecimal<13>(st, p);
	default:
		return AppendPrefixedDecimal<17>(st, p);
	}
}

template <bool PREFIXED>
inline size_t AppendStagedFixed(ColumnStaging &st, const uint8_t *p) {
	switch (st.stride) {
	case 1:
		return PREFIXED ? AppendPrefixedFixed<1>(st, p) : (st.AppendFixed<1>(p), 1);
	case 2:
		return PREFIXED ? AppendPrefixedFixed<2>(st, p) : (st.AppendFixed<2>(p), 2);
	case 3:
		return PREFIXED ? AppendPrefixedFixed<3>(st, p) : (st.AppendFixed<3>(p), 3);
	case 4:
		return PREFIXED ? AppendPrefixedFixed<4>(st, p) : (st.AppendFixed<4>(p), 4);
	case 5:
		return PREFIXED ? AppendPrefixedFixed<5>(st, p) : (st.AppendFixed<5>(p), 5);
	case 6:
		return PREFIXED ? AppendPrefixedFixed<6>(st, p) : (st.AppendFixed<6>(p), 6);
	case 7:
		return PREFIXED ? AppendPrefixedFixed<7>(st, p) : (st.AppendFixed<7>(p), 7);
	case 8:
		return PREFIXED ? AppendPrefixedFixed<8>(st, p) : (st.AppendFixed<8>(p), 8);
	case 9:
		return PREFIXED ? AppendPrefixedFixed<9>(st, p) : (st.AppendFixed<9>(p), 9);
	case 10:
		return PREFIXED ? AppendPrefixedFixed<10>(st, p) : (st.AppendFixed<10>(p), 10);
	case 13:
		return PREFIXED ? AppendPrefixedFixed<13>(st, p) : (st.AppendFixed<13>(p), 13);
	case 16:
		return PREFIXED ? AppendPrefixedFixed<16>(st, p) : (st.AppendFixed<16>(p), 16);
	default:
		return PREFIXED ? AppendPrefixedFixed<17>(st, p) : (st.AppendFixed<17>(p), 17);
	}
}

//! Assemble one PLP value into the staging buffer. Returns bytes consumed.
//!
//! The value arrives as a chunk list whose total length may be declared UNKNOWN,
//! which is why it cannot use the sized AppendVar: the slot is opened, extended
//! per chunk, and only then given its length. `DELIMITED` appends the U+0000
//! separator the UTF-16 batch decode splits on.
//!
//! No bounds tests: the caller is handed a row only once SkipRow has proved the
//! whole of it is buffered, so every chunk header and every chunk body is here.
//! Assemble one legacy-LOB value (TEXT/NTEXT/IMAGE). Returns bytes consumed.
//!
//! Framing: a one-byte text-pointer length (0 means NULL), the pointer, an
//! 8-byte row timestamp, then a 4-byte data length. Only the data is the value.
//! As with PLP, no bounds tests — the row is handed over only once SkipRow has
//! proved all of it is buffered.
template <bool DELIMITED>
inline size_t AppendLob(ColumnStaging &st, const uint8_t *p) {
	const uint32_t pointer_len = p[0];
	if (pointer_len == 0) {
		st.AppendNull();
		return 1;
	}
	const size_t header = 1 + pointer_len + 8 + 4;
	uint32_t data_length;
	std::memcpy(&data_length, p + 1 + pointer_len + 8, 4);
	if (DELIMITED) {
		if (data_length & 1) {
			ThrowOddUtf16Length(data_length);
		}
		st.BeginVar();
		std::memcpy(st.ExtendVar(data_length), p + header, data_length);
		st.FinishVarDelimited();
	} else {
		st.AppendVar(p + header, data_length);
	}
	return header + data_length;
}

template <bool DELIMITED>
inline size_t AppendPlp(ColumnStaging &st, const uint8_t *p) {
	uint64_t total;
	std::memcpy(&total, p, 8);
	if (total == PLP_NULL_MARKER) {
		st.AppendNull();
		return 8;
	}
	size_t offset = 8;
	st.BeginVar();
	while (true) {
		uint32_t chunk_length;
		std::memcpy(&chunk_length, p + offset, 4);
		offset += 4;
		if (chunk_length == 0) {
			break;
		}
		std::memcpy(st.ExtendVar(chunk_length), p + offset, chunk_length);
		offset += chunk_length;
	}
	if (DELIMITED) {
		if (st.PendingVarLength() & 1) {
			ThrowOddUtf16Length(static_cast<uint32_t>(st.PendingVarLength()));
		}
		st.FinishVarDelimited();
	} else {
		st.FinishVar();
	}
	return offset;
}

//! Issue #89: the catalog's type and the wire type disagree, so no family
//! kernel applies — the wire bytes would land in a vector of a different
//! physical type. The shipped behaviour renders each value as text when the
//! destination is VARCHAR, which is what a view with an inline CAST produces.
//!
//! Rendering a value as text has no bulk primitive, but this is still one loop
//! per column with the dispatch resolved once, and the row walk keeps a single
//! shape: the column stages exactly like any other.
void FinalizeFallbackColumn(const ColumnStaging &st, idx_t count, const tds::ColumnMetadata &col, Vector &out) {
	if (out.GetType().id() != LogicalTypeId::VARCHAR) {
		// Anything else would be writing one physical type into another. The
		// pre-staging path dispatched on the TDS type here and corrupted the
		// vector; naming the mismatch is strictly better.
		throw InvalidInputException(
			"MSSQL: column '%s' is declared %s in the catalog but SQL Server returned TDS type 0x%02X. This happens "
			"when a VIEW casts a column and the cached catalog is stale. Re-attach the database, or use mssql_scan() "
			"with an explicit CAST.",
			col.name, out.GetType().ToString(), col.type_id);
	}
	for (idx_t row = 0; row < count; row++) {
		if (!st.IsValid(row)) {
			continue;
		}
		tds::encoding::TypeConverter::WriteAsStringFallback(st.ValueAt(row), st.LengthAt(row), col, out, row);
	}
}

}  // namespace

void RowStager::Configure(const std::vector<tds::ColumnMetadata> &metadata, const std::vector<Vector *> &targets) {
	metadata_ = &metadata;
	reader_ = make_uniq<tds::RowReader>(metadata);
	reader_ptr_ = reader_.get();

	const idx_t column_count = metadata.size();
	ops_.assign(column_count, ColumnOps());
	chunk_nulls_.assign(column_count, 0);
	unbounded_columns_.clear();
	arena_.Configure(column_count);
	// Bind the staging addresses ONCE. They are stable for as long as the arena
	// keeps this column count, which is exactly why it owns its columns through
	// unique_ptr — see the member's comment for what reaching through the arena
	// per value actually costs.
	staging_.resize(column_count);
	for (idx_t i = 0; i < column_count; i++) {
		staging_[i] = &arena_.Column(i);
	}

	for (idx_t i = 0; i < column_count; i++) {
		Vector *target = i < targets.size() ? targets[i] : nullptr;
		if (target == nullptr) {
			// Parsed for its length only.
			ops_[i].arm = AppendArm::Skip;
			continue;
		}
		ops_[i] = ResolveColumnOps(metadata[i], target->GetType());
		if (ops_[i].arm < AppendArm::Unsupported) {
			arena_.Column(i).Configure(ops_[i].kind, ops_[i].stride, ops_[i].max_value_bytes);
			if (counters_enabled_ && ops_[i].kind == StagingKind::Var) {
				// How the column's payload got sized. Only Var columns have a
				// choice — Fixed takes its exact chunk size and Direct stages
				// nothing at all.
				if (ops_[i].max_value_bytes == 0) {
					counters_.unbounded_columns++;
				} else if (arena_.Column(i).payload_bounded) {
					counters_.prealloc_bounded_columns++;
				} else {
					counters_.prealloc_capped_columns++;
				}
			}
		}
		if (ops_[i].arm >= AppendArm::PlpStageString && ops_[i].arm <= AppendArm::LobStageBinary) {
			unbounded_columns_.push_back(staging_[i]);
		}
	}
	has_unbounded_column_ = !unbounded_columns_.empty();
	configured_ = true;
}

void RowStager::BeginChunk(const std::vector<Vector *> &targets) {
	targets_.assign(targets.begin(), targets.end());
	targets_.resize(ops_.size(), nullptr);
	for (idx_t i = 0; i < ops_.size(); i++) {
		if (ops_[i].arm >= AppendArm::Unsupported) {
			continue;
		}
		// Direct columns write through the output vector's own storage, so the
		// data pointer has to be re-taken every chunk: DataChunk::Reset restores
		// each vector from its cache and is free to hand back different memory.
		// Columns that stage into their own buffer take no pointer at all.
		// A chunk always starts FLAT: MSSQLResultStream::FillChunk calls
		// DataChunk::Reset, whose ResetFromCache restores the type and validity.
		// The constant path (spec 056) leaves vectors CONSTANT, and a CONSTANT one
		// here would hand `direct_dst` a single-value view to write 2048 rows into.
		D_ASSERT(targets_[i]->GetVectorType() == VectorType::FLAT_VECTOR);
		uint8_t *direct_dst =
			ops_[i].direct_write ? reinterpret_cast<uint8_t *>(FlatVector::GetDataMutable(*targets_[i])) : nullptr;
		staging_[i]->BeginChunk(direct_dst);
	}
}

size_t RowStager::StageRow(const uint8_t *row, size_t row_length, idx_t row_idx) {
	const uint8_t *p = row;
	const uint8_t *const end = row + row_length;
	const idx_t column_count = ops_.size();

	for (idx_t c = 0; c < column_count; c++) {
		switch (ops_[c].arm) {
		case AppendArm::RawDirect1:
			staging_[c]->AppendDirect<1>(p);
			p += 1;
			break;
		case AppendArm::RawDirect2:
			staging_[c]->AppendDirect<2>(p);
			p += 2;
			break;
		case AppendArm::RawDirect4:
			staging_[c]->AppendDirect<4>(p);
			p += 4;
			break;
		case AppendArm::RawDirect8:
			staging_[c]->AppendDirect<8>(p);
			p += 8;
			break;
		case AppendArm::P1Direct1:
			p += AppendPrefixedDirect<1>(*staging_[c], p);
			break;
		case AppendArm::P1Direct2:
			p += AppendPrefixedDirect<2>(*staging_[c], p);
			break;
		case AppendArm::P1Direct4:
			p += AppendPrefixedDirect<4>(*staging_[c], p);
			break;
		case AppendArm::P1Direct8:
			p += AppendPrefixedDirect<8>(*staging_[c], p);
			break;
		case AppendArm::P1StageFixed:
			p += AppendStagedFixed<true>(*staging_[c], p);
			break;
		case AppendArm::RawStageFixed:
			p += AppendStagedFixed<false>(*staging_[c], p);
			break;
		case AppendArm::P1StageDecimal:
			p += AppendStagedDecimal(*staging_[c], p);
			break;
		case AppendArm::P2StageString: {
			const uint32_t length = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8);
			if (length == 0xFFFF) {
				staging_[c]->AppendNull();
				p += 2;
				break;
			}
			if (length & 1) {
				ThrowOddUtf16Length(length);
			}
			staging_[c]->AppendVarDelimited(p + 2, length);
			p += 2 + length;
			break;
		}
		case AppendArm::P2StageBinary: {
			// Its own arm rather than sharing the string one: binary needs no
			// separator, because its output offsets ARE its input offsets. Two wasted
			// bytes per value is 12% of a 16-byte blob, which is more than a
			// perfectly predicted extra switch arm costs.
			const uint32_t length = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8);
			if (length == 0xFFFF) {
				staging_[c]->AppendNull();
				p += 2;
				break;
			}
			staging_[c]->AppendVar(p + 2, length);
			p += 2 + length;
			break;
		}
		case AppendArm::PlpStageString:
			p += AppendPlp<true>(*staging_[c], p);
			break;
		case AppendArm::PlpStageBinary:
			p += AppendPlp<false>(*staging_[c], p);
			break;
		case AppendArm::LobStageString:
			p += AppendLob<true>(*staging_[c], p);
			break;
		case AppendArm::LobStageBinary:
			p += AppendLob<false>(*staging_[c], p);
			break;
		case AppendArm::Unsupported:
			ThrowUnsupportedType((*metadata_)[c]);
		case AppendArm::Skip:
			p += reader_ptr_->SkipValue(p, static_cast<size_t>(end - p), c);
			break;
		}
	}
	D_ASSERT(p == end);
	return static_cast<size_t>(p - row);
}

size_t RowStager::StageNBCRow(const uint8_t *row, size_t row_length, idx_t row_idx) {
	if (counters_enabled_) {
		counters_.nbc_rows++;
	}
	const idx_t column_count = ops_.size();
	const uint8_t *const bitmap = row;
	const uint8_t *p = row + (column_count + 7) / 8;
	const uint8_t *const end = row + row_length;

	for (idx_t c = 0; c < column_count; c++) {
		// An NBC row carries no bytes at all for a NULL column, so the bitmap has
		// to be consulted before the arm — this is why the NBC walk is a separate
		// function instead of a flag inside the regular one.
		if ((bitmap[c >> 3] & (1u << (c & 7))) != 0) {
			// A NULL in an NBC row carries no bytes at all, so even an
			// unsupported type costs nothing here — it is only a value of one
			// that cannot be read.
			if (ops_[c].arm < AppendArm::Unsupported) {
				staging_[c]->AppendNull();
			}
			continue;
		}

		switch (ops_[c].arm) {
		case AppendArm::RawDirect1:
			staging_[c]->AppendDirect<1>(p);
			p += 1;
			break;
		case AppendArm::RawDirect2:
			staging_[c]->AppendDirect<2>(p);
			p += 2;
			break;
		case AppendArm::RawDirect4:
			staging_[c]->AppendDirect<4>(p);
			p += 4;
			break;
		case AppendArm::RawDirect8:
			staging_[c]->AppendDirect<8>(p);
			p += 8;
			break;
		// The *N variants keep their length prefix in an NBC row; the bitmap only
		// says whether the value is there at all.
		case AppendArm::P1Direct1:
			p += AppendPrefixedDirect<1>(*staging_[c], p);
			break;
		case AppendArm::P1Direct2:
			p += AppendPrefixedDirect<2>(*staging_[c], p);
			break;
		case AppendArm::P1Direct4:
			p += AppendPrefixedDirect<4>(*staging_[c], p);
			break;
		case AppendArm::P1Direct8:
			p += AppendPrefixedDirect<8>(*staging_[c], p);
			break;
		case AppendArm::P1StageFixed:
			p += AppendStagedFixed<true>(*staging_[c], p);
			break;
		case AppendArm::RawStageFixed:
			p += AppendStagedFixed<false>(*staging_[c], p);
			break;
		case AppendArm::P1StageDecimal:
			p += AppendStagedDecimal(*staging_[c], p);
			break;
		case AppendArm::P2StageString: {
			// The length prefix stays in an NBC row; the bitmap only decided that
			// a value is present at all, which the branch above already handled.
			const uint32_t length = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8);
			if (length == 0xFFFF) {
				// Explicit, not left to the parity test below: 0xFFFF happens to be
				// odd, so the UTF-16 check would catch it today, but that is luck.
				ThrowNbcNullPrefix();
			}
			if (length & 1) {
				ThrowOddUtf16Length(length);
			}
			staging_[c]->AppendVarDelimited(p + 2, length);
			p += 2 + length;
			break;
		}
		case AppendArm::P2StageBinary: {
			const uint32_t length = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8);
			if (length == 0xFFFF) {
				ThrowNbcNullPrefix();
			}
			staging_[c]->AppendVar(p + 2, length);
			p += 2 + length;
			break;
		}
		case AppendArm::PlpStageString:
			p += AppendPlp<true>(*staging_[c], p);
			break;
		case AppendArm::PlpStageBinary:
			p += AppendPlp<false>(*staging_[c], p);
			break;
		case AppendArm::LobStageString:
			p += AppendLob<true>(*staging_[c], p);
			break;
		case AppendArm::LobStageBinary:
			p += AppendLob<false>(*staging_[c], p);
			break;
		case AppendArm::Unsupported:
			ThrowUnsupportedType((*metadata_)[c]);
		case AppendArm::Skip:
			p += reader_ptr_->SkipValueNBC(p, static_cast<size_t>(end - p), c);
			break;
		}
	}
	D_ASSERT(p == end);
	return static_cast<size_t>(p - row);
}

void RowStager::FinalizeChunk(idx_t row_count) {
	const idx_t words = (row_count + 63) / 64;
	for (idx_t c = 0; c < ops_.size(); c++) {
		if (ops_[c].arm >= AppendArm::Unsupported) {
			chunk_nulls_[c] = 0;
			continue;
		}
		const ColumnStaging &st = *staging_[c];
		const tds::ColumnMetadata &meta = (*metadata_)[c];
		if (TryEmitConstant(c, st, meta, row_count)) {
			chunk_nulls_[c] = st.null_count;
			continue;
		}
		std::chrono::steady_clock::time_point started;
		if (counters_enabled_) {
			started = std::chrono::steady_clock::now();
		}
		// Which kernel is a property of the column, resolved with the append arm
		// after COLMETADATA, so this is one switch on one invariant value and the
		// kernel's own loop carries no dispatch at all.
		switch (ops_[c].kernel) {
		case FinalizeKernel::None:
			break;
		case FinalizeKernel::String:
			string::DecodeChunkFromStaging(st, row_count, meta, *targets_[c]);
			break;
		case FinalizeKernel::Binary:
			binary::DecodeChunkFromStaging(st, row_count, meta, *targets_[c]);
			break;
		case FinalizeKernel::Uuid:
			uuid::DecodeChunkFromStaging(st, row_count, meta, *targets_[c]);
			break;
		case FinalizeKernel::Decimal:
			decimal::DecodeChunkFromStaging(st, row_count, meta, *targets_[c]);
			break;
		case FinalizeKernel::Money:
			money::DecodeChunkFromStaging(st, row_count, meta, *targets_[c]);
			break;
		case FinalizeKernel::Datetime:
			datetime::DecodeChunkFromStaging(st, row_count, meta, *targets_[c]);
			break;
		case FinalizeKernel::Text:
			FinalizeFallbackColumn(st, row_count, meta, *targets_[c]);
			break;
		}
		if (counters_enabled_) {
			const auto elapsed = std::chrono::steady_clock::now() - started;
			CountColumn(c, st, row_count,
						static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()));
		}
		// Values went straight into the vector; only validity is still ours. An
		// all-valid column — the overwhelmingly common one — must not force
		// DuckDB to allocate a mask it does not need, and the append arm already
		// counted the NULLs, so that costs one test rather than a scan.
		chunk_nulls_[c] = st.null_count;
		if (st.null_count == 0) {
			continue;
		}
		ValidityMask &mask = FlatVector::ValidityMutable(*targets_[c]);
		mask.EnsureWritable();
		// AND, not memcpy: a kernel may have set NULLs of its own before this runs
		// — datetime does, for a datetime2 whose value does not fit the target
		// variant (issue #168) — and overwriting the mask would silently republish
		// those rows as valid, with a slot the kernel never wrote. Same layout on
		// both sides (64-bit words, 1 = valid), which is why ColumnStaging keeps
		// validity in this shape. Bits past row_count are set from BeginChunk and
		// are ignored downstream.
		uint64_t *published = mask.GetData();
		for (idx_t w = 0; w < words; w++) {
			published[w] &= st.validity_words[w];
		}
	}
	arena_.EndChunk();
}

//! Is every value the same? Callers reach these only for an all-valid column, so
//! there are no NULL slots to step over.
//!
//! Each returns on the first difference, which is what makes the miss free: a
//! column of distinct values costs one comparison, not a scan.
namespace {

//! One slot compared as a machine word rather than through memcmp, which is a
//! call into libc per value — measured at ~1.7 ns/value against ~0.3 for this.
//! memcpy for the load, not a cast: the staging buffer is byte-addressed.
template <typename T>
inline bool SlotsRepeat(const uint8_t *data, idx_t count) {
	T first;
	std::memcpy(&first, data, sizeof(T));
	for (idx_t row = 1; row < count; row++) {
		T value;
		std::memcpy(&value, data + row * sizeof(T), sizeof(T));
		if (value != first) {
			return false;
		}
	}
	return true;
}

bool BytesRepeat(const uint8_t *data, uint32_t stride, idx_t count) {
	switch (stride) {
	case 1:
		return SlotsRepeat<uint8_t>(data, count);
	case 2:
		return SlotsRepeat<uint16_t>(data, count);
	case 4:
		return SlotsRepeat<uint32_t>(data, count);
	case 8:
		return SlotsRepeat<uint64_t>(data, count);
	default:
		// The odd widths — 3, 5-7, 9, 10, 13, 16, 17 — belong to the temporal,
		// GUID and DECIMAL families, where the decode this saves dwarfs the
		// comparison either way.
		for (idx_t row = 1; row < count; row++) {
			if (std::memcmp(data + row * stride, data, stride) != 0) {
				return false;
			}
		}
		return true;
	}
}

bool StagedVarRepeats(const ColumnStaging &st, idx_t count) {
	const uint32_t length = st.lengths[0];
	const uint8_t *const first = st.buffer.data() + st.offsets[0];
	for (idx_t row = 1; row < count; row++) {
		// Length first: it rejects most columns before a byte is compared.
		if (st.lengths[row] != length || std::memcmp(st.buffer.data() + st.offsets[row], first, length) != 0) {
			return false;
		}
	}
	return true;
}

}  // namespace

bool RowStager::TryEmitConstant(idx_t c, const ColumnStaging &st, const tds::ColumnMetadata &meta, idx_t row_count) {
	if (row_count < 2) {
		// One row is trivially uniform, but a one-row chunk has nothing to save
		// and the vector is already correct as a flat one.
		return false;
	}
	Vector &out = *targets_[c];

	// All NULL. Free to detect — two numbers already in hand — and it skips the
	// kernel entirely, which is the whole win for a column a query does not
	// populate. Common in wide tables.
	if (st.null_count == row_count) {
		out.SetVectorType(VectorType::CONSTANT_VECTOR);
		ConstantVector::SetNull(out, true);
		if (counters_enabled_) {
			counters_.constant_null_columns++;
		}
		return true;
	}
	// A NULL among values cannot be uniform, and this rejects it before any value
	// is looked at.
	if (st.null_count != 0) {
		return false;
	}

	// Only columns whose values still have to be DECODED are scanned for
	// uniformity. A direct-write column has no decode to collapse — its values
	// were stored into the output vector as they arrived — so the scan buys
	// nothing here and only a downstream constant, which measured at +0.26
	// ns/value on a uniform BIGINT column against a decode saving of zero. The
	// families that do decode measured -14% (DECIMAL) and -16% (NVARCHAR).
	//
	// The all-NULL case above deliberately still applies to them: it is detected
	// by comparing two counters, never looks at a value, and skips publishing the
	// validity mask.
	if (ops_[c].direct_write) {
		return false;
	}
	if (ops_[c].kind == StagingKind::Var) {
		if (!StagedVarRepeats(st, row_count)) {
			return false;
		}
	} else if (!BytesRepeat(st.buffer.data(), st.stride, row_count)) {
		return false;
	}

	// Decode row 0 alone. This is the family's per-VALUE entry point, called once
	// per column per chunk — not a per-value path — and it is preferred to calling
	// the batch kernel with count = 1 because the string kernel sizes its work
	// from the whole staged payload and would convert all 2048 values to publish
	// one.
	DecodeFirstValue(c, st, meta, out);
	out.SetVectorType(VectorType::CONSTANT_VECTOR);
	if (counters_enabled_) {
		counters_.constant_columns++;
	}
	return true;
}

//! Publish row 0 into slot 0, by family. Only the staged kernels come here;
//! a direct-write column already has its value in place.
void RowStager::DecodeFirstValue(idx_t c, const ColumnStaging &st, const tds::ColumnMetadata &meta, Vector &out) {
	if (ops_[c].kernel == FinalizeKernel::Text) {
		// The issue-#89 fallback is row-indexed already, so one row is one row.
		FinalizeFallbackColumn(st, 1, meta, out);
		return;
	}
	const uint8_t *const value = st.ValueAt(0);
	const std::vector<uint8_t> bytes(value, value + st.LengthAt(0));
	switch (ops_[c].kernel) {
	case FinalizeKernel::String:
		string::DecodeFromTds(bytes, meta, out, 0);
		break;
	case FinalizeKernel::Binary:
		binary::DecodeFromTds(bytes, meta, out, 0);
		break;
	case FinalizeKernel::Uuid:
		uuid::DecodeFromTds(bytes, meta, out, 0);
		break;
	case FinalizeKernel::Decimal:
		decimal::DecodeFromTds(bytes, meta, out, 0);
		break;
	case FinalizeKernel::Money:
		money::DecodeFromTds(bytes, meta, out, 0);
		break;
	case FinalizeKernel::Datetime:
		datetime::DecodeFromTds(bytes, meta, out, 0);
		break;
	case FinalizeKernel::None:
	case FinalizeKernel::Text:
		break;
	}
}

void RowStager::CountColumn(idx_t c, const ColumnStaging &st, idx_t row_count, uint64_t elapsed_ns) {
	const uint8_t kernel = static_cast<uint8_t>(ops_[c].kernel);
	const idx_t values = row_count - st.null_count;
	counters_.kernel_ns[kernel] += elapsed_ns;
	counters_.kernel_values[kernel] += values;

	if (ops_[c].direct_write) {
		// The bypass: no staging buffer was touched, so there are no staged
		// bytes to attribute — only values that skipped the whole mechanism.
		counters_.direct_values += values;
	} else if (ops_[c].kind == StagingKind::Var) {
		counters_.staged_bytes[kernel] += st.PayloadSize();
	} else {
		// Fixed is positional, so a NULL row occupies a slot but copies nothing.
		counters_.staged_bytes[kernel] += values * st.stride;
	}

	if (st.boundary != BoundaryStrategy::None) {
		counters_.boundary[static_cast<uint8_t>(st.boundary)]++;
	}
	counters_.replaced_units += st.replaced_units;
}

}  // namespace staging
}  // namespace codec
}  // namespace mssql
}  // namespace duckdb
