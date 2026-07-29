//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// codec/staging/row_stager.cpp — spec 055 T5.
//===----------------------------------------------------------------------===//

#include "codec/staging/row_stager.hpp"

#include "codec/datetime_codec.hpp"
#include "codec/decimal_codec.hpp"
#include "codec/money_codec.hpp"
#include "codec/staging/binary_finalize.hpp"
#include "codec/staging/string_finalize.hpp"
#include "codec/staging/uuid_finalize.hpp"
#include "duckdb/common/exception.hpp"
#include "mssql_compat.hpp"
#include "tds/encoding/type_converter.hpp"

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
[[noreturn]] void ThrowUnsupportedType(const tds::ColumnMetadata &column) {
	throw InvalidInputException(
		"MSSQL: column '%s' arrives as TDS type 0x%02X, which this extension cannot decode. TEXT, NTEXT and IMAGE are "
		"readable through a catalog scan (which casts them server-side) but not through raw mssql_scan(); UDT and "
		"SQL_VARIANT are not supported at all. Cast the column in the query instead.",
		column.name, column.type_id);
}

[[noreturn]] void ThrowBadPrefix(uint32_t expected, uint32_t actual) {
	throw InvalidInputException(
		"MSSQL: fixed-width column arrived with a %u-byte length prefix where %u or 0 (NULL) was required. The TDS "
		"stream is malformed.",
		actual, expected);
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

	const idx_t column_count = metadata.size();
	ops_.assign(column_count, ColumnOps());
	chunk_nulls_.assign(column_count, 0);
	unbounded_columns_.clear();
	arena_.Configure(column_count);

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
		}
		if (ops_[i].arm == AppendArm::PlpStageString || ops_[i].arm == AppendArm::PlpStageBinary) {
			unbounded_columns_.push_back(i);
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
		uint8_t *direct_dst =
			ops_[i].direct_write ? reinterpret_cast<uint8_t *>(FlatVector::GetData(*targets_[i])) : nullptr;
		arena_.Column(i).BeginChunk(direct_dst);
	}
}

void RowStager::StageRow(const uint8_t *row, size_t row_length, idx_t row_idx) {
	const uint8_t *p = row;
	const uint8_t *const end = row + row_length;
	const idx_t column_count = ops_.size();

	for (idx_t c = 0; c < column_count; c++) {
		switch (ops_[c].arm) {
		case AppendArm::RawDirect1:
			arena_.Column(c).AppendDirect<1>(p);
			p += 1;
			break;
		case AppendArm::RawDirect2:
			arena_.Column(c).AppendDirect<2>(p);
			p += 2;
			break;
		case AppendArm::RawDirect4:
			arena_.Column(c).AppendDirect<4>(p);
			p += 4;
			break;
		case AppendArm::RawDirect8:
			arena_.Column(c).AppendDirect<8>(p);
			p += 8;
			break;
		case AppendArm::P1Direct1:
			p += AppendPrefixedDirect<1>(arena_.Column(c), p);
			break;
		case AppendArm::P1Direct2:
			p += AppendPrefixedDirect<2>(arena_.Column(c), p);
			break;
		case AppendArm::P1Direct4:
			p += AppendPrefixedDirect<4>(arena_.Column(c), p);
			break;
		case AppendArm::P1Direct8:
			p += AppendPrefixedDirect<8>(arena_.Column(c), p);
			break;
		case AppendArm::P1StageFixed:
			p += AppendStagedFixed<true>(arena_.Column(c), p);
			break;
		case AppendArm::RawStageFixed:
			p += AppendStagedFixed<false>(arena_.Column(c), p);
			break;
		case AppendArm::P1StageDecimal:
			p += AppendStagedDecimal(arena_.Column(c), p);
			break;
		case AppendArm::P2StageString: {
			const uint32_t length = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8);
			if (length == 0xFFFF) {
				arena_.Column(c).AppendNull();
				p += 2;
				break;
			}
			arena_.Column(c).AppendVarDelimited(p + 2, length);
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
				arena_.Column(c).AppendNull();
				p += 2;
				break;
			}
			arena_.Column(c).AppendVar(p + 2, length);
			p += 2 + length;
			break;
		}
		case AppendArm::PlpStageString:
			p += AppendPlp<true>(arena_.Column(c), p);
			break;
		case AppendArm::PlpStageBinary:
			p += AppendPlp<false>(arena_.Column(c), p);
			break;
		case AppendArm::Unsupported:
			ThrowUnsupportedType((*metadata_)[c]);
		case AppendArm::Skip:
			p += reader_->SkipValue(p, static_cast<size_t>(end - p), c);
			break;
		}
	}
	D_ASSERT(p == end);
}

void RowStager::StageNBCRow(const uint8_t *row, size_t row_length, idx_t row_idx) {
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
				arena_.Column(c).AppendNull();
			}
			continue;
		}

		switch (ops_[c].arm) {
		case AppendArm::RawDirect1:
			arena_.Column(c).AppendDirect<1>(p);
			p += 1;
			break;
		case AppendArm::RawDirect2:
			arena_.Column(c).AppendDirect<2>(p);
			p += 2;
			break;
		case AppendArm::RawDirect4:
			arena_.Column(c).AppendDirect<4>(p);
			p += 4;
			break;
		case AppendArm::RawDirect8:
			arena_.Column(c).AppendDirect<8>(p);
			p += 8;
			break;
		// The *N variants keep their length prefix in an NBC row; the bitmap only
		// says whether the value is there at all.
		case AppendArm::P1Direct1:
			p += AppendPrefixedDirect<1>(arena_.Column(c), p);
			break;
		case AppendArm::P1Direct2:
			p += AppendPrefixedDirect<2>(arena_.Column(c), p);
			break;
		case AppendArm::P1Direct4:
			p += AppendPrefixedDirect<4>(arena_.Column(c), p);
			break;
		case AppendArm::P1Direct8:
			p += AppendPrefixedDirect<8>(arena_.Column(c), p);
			break;
		case AppendArm::P1StageFixed:
			p += AppendStagedFixed<true>(arena_.Column(c), p);
			break;
		case AppendArm::RawStageFixed:
			p += AppendStagedFixed<false>(arena_.Column(c), p);
			break;
		case AppendArm::P1StageDecimal:
			p += AppendStagedDecimal(arena_.Column(c), p);
			break;
		case AppendArm::P2StageString: {
			// The length prefix stays in an NBC row; the bitmap only decided that
			// a value is present at all, which the branch above already handled.
			const uint32_t length = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8);
			arena_.Column(c).AppendVarDelimited(p + 2, length);
			p += 2 + length;
			break;
		}
		case AppendArm::P2StageBinary: {
			const uint32_t length = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8);
			arena_.Column(c).AppendVar(p + 2, length);
			p += 2 + length;
			break;
		}
		case AppendArm::PlpStageString:
			p += AppendPlp<true>(arena_.Column(c), p);
			break;
		case AppendArm::PlpStageBinary:
			p += AppendPlp<false>(arena_.Column(c), p);
			break;
		case AppendArm::Unsupported:
			ThrowUnsupportedType((*metadata_)[c]);
		case AppendArm::Skip:
			p += reader_->SkipValueNBC(p, static_cast<size_t>(end - p), c);
			break;
		}
	}
	D_ASSERT(p == end);
}

void RowStager::FinalizeChunk(idx_t row_count, bool collect_nulls) {
	const idx_t words = (row_count + 63) / 64;
	for (idx_t c = 0; c < ops_.size(); c++) {
		if (ops_[c].arm >= AppendArm::Unsupported) {
			chunk_nulls_[c] = 0;
			continue;
		}
		const ColumnStaging &st = arena_.Column(c);
		if (ops_[c].needs_value_fallback) {
			FinalizeFallbackColumn(st, row_count, (*metadata_)[c], *targets_[c]);
		} else if (ops_[c].arm == AppendArm::P2StageString || ops_[c].arm == AppendArm::PlpStageString) {
			// One conversion for the whole column (D5).
			FinalizeStringColumn(st, row_count, *targets_[c], ops_[c].trim_trailing_spaces);
		} else if (ops_[c].arm == AppendArm::P2StageBinary || ops_[c].arm == AppendArm::PlpStageBinary) {
			FinalizeBinaryColumn(st, row_count, *targets_[c], ops_[c].trim_trailing_spaces);
		} else if (ops_[c].arm == AppendArm::P1StageDecimal) {
			decimal::DecodeChunkFromStaging(st, row_count, (*metadata_)[c], *targets_[c]);
		} else if (ops_[c].arm == AppendArm::P1StageFixed || ops_[c].arm == AppendArm::RawStageFixed) {
			// Which kernel is a property of the column, so it is decided here,
			// once, and the kernel's own loop carries no dispatch at all.
			const uint8_t type_id = (*metadata_)[c].type_id;
			if (type_id == tds::TDS_TYPE_UNIQUEIDENTIFIER) {
				FinalizeUuidColumn(st, row_count, *targets_[c]);
			} else if (type_id == tds::TDS_TYPE_MONEY || type_id == tds::TDS_TYPE_SMALLMONEY ||
					   type_id == tds::TDS_TYPE_MONEYN) {
				money::DecodeChunkFromStaging(st, row_count, (*metadata_)[c], *targets_[c]);
			} else {
				datetime::DecodeChunkFromStaging(st, row_count, (*metadata_)[c], *targets_[c]);
			}
		}
		// Values went straight into the vector; only validity is still ours. Scan
		// first so an all-valid column — the overwhelmingly common one — never
		// forces DuckDB to allocate a mask it does not need.
		bool any_null = false;
		for (idx_t w = 0; w < words; w++) {
			if (st.validity_words[w] != ~static_cast<uint64_t>(0)) {
				any_null = true;
				break;
			}
		}
		chunk_nulls_[c] = 0;
		if (!any_null) {
			continue;
		}
		ValidityMask &mask = FlatVector::Validity(*targets_[c]);
		mask.EnsureWritable();
		// Same layout on both sides (64-bit words, 1 = valid), which is why
		// ColumnStaging keeps validity in this shape to begin with. Bits past
		// row_count are still set from BeginChunk and are ignored downstream.
		std::memcpy(mask.GetData(), st.validity_words.data(), words * sizeof(uint64_t));
		if (collect_nulls) {
			for (idx_t r = 0; r < row_count; r++) {
				chunk_nulls_[c] += st.IsValid(r) ? 0 : 1;
			}
		}
	}
	arena_.EndChunk();
}

}  // namespace staging
}  // namespace codec
}  // namespace mssql
}  // namespace duckdb
