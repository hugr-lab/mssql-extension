#include "tds/tds_row_reader.hpp"
#include <cstring>
#include <initializer_list>
#include <stdexcept>
#include "tds/tds_types.hpp"

namespace duckdb {
namespace tds {

namespace {
//! Largest value a TEXT / IMAGE column can hold — SQL Server's own limit for the
//! type. A 4-byte wire length can declare far more than that, and no other
//! read-path framing field can: the 2-byte forms are bounded at 65535 and PLP's
//! chunk lengths are checked against the bytes actually present.
//!
//! Anything the staging layer would also refuse is caught slightly earlier, at
//! its own (lower) MAX_STAGING_PAYLOAD_BYTES, with its own message. This bound
//! exists for the values that are not merely too big to stage but impossible.
constexpr uint32_t MAX_LOB_VALUE_BYTES = 2147483647u;

//! How the per-value walks (RowReader::SkipValue and ReadValue, a ROW's and an NBCROW's) frame a value
//! of a TDS type.
enum class FramingKind : uint8_t {
	//! A type the walk does not know: it cannot be skipped.
	UNKNOWN = 0,
	//! `width` bytes, no prefix.
	FIXED,
	//! A 1-byte length, then that many bytes (0 is NULL).
	BYTE_PREFIX,
	//! A 2-byte length, then that many bytes (0xFFFF is NULL) -- or a PLP chunk list for a MAX column.
	SHORT_PREFIX,
	//! A PLP chunk list, always.
	PLP,
	//! TEXT / NTEXT / IMAGE: the text-pointer form.
	LEGACY_LOB,
	//! sql_variant (spec 093 G2): a u32 length, then that many bytes.
	VARIANT
};

struct ValueFraming {
	FramingKind kind;
	uint8_t width;	// FIXED: the value's bytes; other kinds: unused
};

//! The per-value walks' framing of every TDS type, by type id. It was a ~30-case switch on the type in
//! SkipValue and, arm for arm, in SkipValueNBC, ReadValue and ReadValueNBC; the rows below are those arms.
//!
//! Deliberately NOT ResolveSkipForm's switch, and not built from it: the fast walk is safe only while
//! the two agree, and test_skip_form_equivalence holds that by comparing them as two independent
//! statements of the wire's framing (spec 058 D1-alt). A width edited here and not there fails it.
struct LegacyFramings {
	ValueFraming by_type[256];

	LegacyFramings() : by_type() {
		// Fixed-length types
		Set(FramingKind::FIXED, 1, {TDS_TYPE_TINYINT, TDS_TYPE_BIT});
		Set(FramingKind::FIXED, 2, {TDS_TYPE_SMALLINT});
		Set(FramingKind::FIXED, 4, {TDS_TYPE_INT, TDS_TYPE_REAL, TDS_TYPE_SMALLMONEY, TDS_TYPE_SMALLDATETIME});
		Set(FramingKind::FIXED, 8, {TDS_TYPE_BIGINT, TDS_TYPE_FLOAT, TDS_TYPE_MONEY, TDS_TYPE_DATETIME});
		// Nullable fixed-length (1-byte length prefix), in a ROW and in an NBCROW alike
		Set(FramingKind::BYTE_PREFIX, 0,
			{TDS_TYPE_INTN, TDS_TYPE_BITN, TDS_TYPE_FLOATN, TDS_TYPE_MONEYN, TDS_TYPE_DATETIMEN, TDS_TYPE_DECIMAL,
			 TDS_TYPE_NUMERIC, TDS_TYPE_DATE, TDS_TYPE_TIME, TDS_TYPE_DATETIME2, TDS_TYPE_DATETIMEOFFSET,
			 TDS_TYPE_UNIQUEIDENTIFIER});
		// Legacy LOBs (TEXT/NTEXT/IMAGE)
		Set(FramingKind::LEGACY_LOB, 0, {TDS_TYPE_TEXT, TDS_TYPE_NTEXT, TDS_TYPE_IMAGE});
		// XML (always PLP)
		Set(FramingKind::PLP, 0, {TDS_TYPE_XML, TDS_TYPE_UDT});
		Set(FramingKind::VARIANT, 0, {TDS_TYPE_SQL_VARIANT});
		// Variable-length (2-byte length prefix, or PLP for MAX types); VECTOR per spec 093 D4
		Set(FramingKind::SHORT_PREFIX, 0,
			{TDS_TYPE_BIGCHAR, TDS_TYPE_BIGVARCHAR, TDS_TYPE_NCHAR, TDS_TYPE_NVARCHAR, TDS_TYPE_BIGBINARY,
			 TDS_TYPE_BIGVARBINARY, TDS_TYPE_VECTOR});
	}

	void Set(FramingKind kind, uint8_t width, std::initializer_list<uint8_t> types) {
		for (const uint8_t type_id : types) {
			by_type[type_id] = ValueFraming{kind, width};
		}
	}

	ValueFraming Of(uint8_t type_id) const {
		return by_type[type_id];
	}
};

//! Built when the library loads. Nothing skips a row during another file's static initialisation, so
//! no reader can see it unbuilt; a function-local static would put a guard test on every value.
const LegacyFramings LEGACY_FRAMINGS;

//! A 1-byte length, then that many bytes.
size_t SkipBytePrefixed(const uint8_t *data, size_t length) {
	if (length < 1) {
		return 0;
	}
	const uint8_t data_length = data[0];
	return length >= 1 + data_length ? 1 + data_length : 0;
}

//! `width` bytes with no prefix, read into `value`; 0 when they have not all arrived.
size_t ReadFixed(const uint8_t *data, size_t length, size_t width, std::vector<uint8_t> &value) {
	if (length < width) {
		return 0;  // Need more data
	}
	value.assign(data, data + width);
	return width;
}

//! A 1-byte length, then that many bytes, read into `value`: the nullable fixed-length types, DECIMAL /
//! NUMERIC, DATE, TIME, DATETIME2, DATETIMEOFFSET (each had a reader of its own, the same lines). With
//! `zero_is_null` (a ROW) a length of 0 is NULL; without (an NBCROW's present value) it is an empty
//! value. The length is taken as sent: a DATE of another length than 3 is the decoder's to refuse
//! (datetime_codec.cpp).
size_t ReadBytePrefixed(const uint8_t *data, size_t length, std::vector<uint8_t> &value, bool &is_null,
						bool zero_is_null) {
	if (length < 1) {
		return 0;  // Need length byte
	}
	const uint8_t data_length = data[0];
	if (data_length == 0 && zero_is_null) {
		is_null = true;
		value.clear();
		return 1;
	}
	if (length < 1 + data_length) {
		return 0;  // Need more data
	}
	value.assign(data + 1, data + 1 + data_length);
	return 1 + data_length;
}

//! A 2-byte length, then that many bytes; 0xFFFF is NULL, the two bytes alone.
size_t SkipShortPrefixed(const uint8_t *data, size_t length) {
	if (length < 2) {
		return 0;
	}
	const uint16_t data_length = static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8);
	if (data_length == 0xFFFF) {
		return 2;  // NULL
	}
	return length >= 2 + data_length ? 2 + data_length : 0;
}

//! Legacy LOBs (TEXT/NTEXT/IMAGE): 1-byte text-pointer length (0 = NULL), the
//! pointer itself, an 8-byte timestamp, then a 4-byte data length. Nothing
//! else on the wire is framed this way.
size_t SkipLegacyLob(const uint8_t *data, size_t length) {
	if (length < 1) {
		return 0;
	}
	const size_t pointer_len = data[0];
	if (pointer_len == 0) {
		return 1;  // NULL
	}
	const size_t header = 1 + pointer_len + 8 + 4;
	if (length < header) {
		return 0;
	}
	uint32_t data_length;
	std::memcpy(&data_length, data + 1 + pointer_len + 8, 4);
	if (data_length > MAX_LOB_VALUE_BYTES) {
		// A 4-byte length is the only framing field on the read path that can
		// declare more than a value could ever be. Returning 0 here would mean
		// "need more data", and the parser would buffer the whole rest of the
		// stream waiting for bytes that are never coming.
		// std::runtime_error, not a DuckDB exception: this layer is compiled
		// standalone by the fuzz harness (fuzz/build.sh) with no DuckDB include
		// path, which is what makes it fuzzable at all. Every other throw in
		// this file is the same for the same reason.
		throw std::runtime_error("TEXT/NTEXT/IMAGE value declares " + std::to_string(data_length) +
								 " bytes, past the " + std::to_string(MAX_LOB_VALUE_BYTES) +
								 "-byte maximum for the type. The TDS stream is malformed.");
	}
	const size_t total = header + data_length;
	return length >= total ? total : 0;
}
}  // namespace

RowReader::RowReader(const std::vector<ColumnMetadata> &columns) : columns_(columns) {}

// Spec 058 D1-alt: one value of the fast walk. Byte-identical semantics to the
// corresponding SkipValue arms — same prefix reads, same bounds tests, 0 still
// means "need more data" — only the per-value lookup by type_id is gone. Shared by
// the ROW and NBC walks because the wire forms are identical for both.
static inline size_t SkipByForm(const uint8_t *data, size_t length, SkipDesc desc) {
	switch (desc.form) {
	case SkipForm::FIXED:
		return length >= desc.width ? desc.width : 0;
	case SkipForm::PREFIX1: {
		if (length < 1) {
			return 0;
		}
		const size_t data_length = data[0];
		return length >= 1 + data_length ? 1 + data_length : 0;
	}
	case SkipForm::PREFIX2: {
		if (length < 2) {
			return 0;
		}
		const uint16_t data_length = static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8);
		if (data_length == 0xFFFF) {
			return 2;  // NULL
		}
		return length >= 2 + static_cast<size_t>(data_length) ? 2 + data_length : 0;
	}
	default:
		return 0;  // SLOW is handled by the caller; unreachable here
	}
}

SkipDesc ResolveSkipForm(const ColumnMetadata &col) {
	switch (col.type_id) {
	case TDS_TYPE_TINYINT:
	case TDS_TYPE_BIT:
		return {SkipForm::FIXED, 1};
	case TDS_TYPE_SMALLINT:
		return {SkipForm::FIXED, 2};
	case TDS_TYPE_INT:
	case TDS_TYPE_REAL:
	case TDS_TYPE_SMALLMONEY:
	case TDS_TYPE_SMALLDATETIME:
		return {SkipForm::FIXED, 4};
	case TDS_TYPE_BIGINT:
	case TDS_TYPE_FLOAT:
	case TDS_TYPE_MONEY:
	case TDS_TYPE_DATETIME:
		return {SkipForm::FIXED, 8};
	case TDS_TYPE_INTN:
	case TDS_TYPE_BITN:
	case TDS_TYPE_FLOATN:
	case TDS_TYPE_MONEYN:
	case TDS_TYPE_DATETIMEN:
	case TDS_TYPE_DECIMAL:
	case TDS_TYPE_NUMERIC:
	case TDS_TYPE_DATE:
	case TDS_TYPE_TIME:
	case TDS_TYPE_DATETIME2:
	case TDS_TYPE_DATETIMEOFFSET:
	case TDS_TYPE_UNIQUEIDENTIFIER:
		return {SkipForm::PREFIX1, 0};
	case TDS_TYPE_BIGCHAR:
	case TDS_TYPE_BIGVARCHAR:
	case TDS_TYPE_NCHAR:
	case TDS_TYPE_NVARCHAR:
	case TDS_TYPE_BIGBINARY:
	case TDS_TYPE_BIGVARBINARY:
	case TDS_TYPE_VECTOR:  // spec 093 D4: u16 length, 0xFFFF NULL
		// PLP (MAX) framing is a chunk list, not a prefix — legacy code owns it.
		return col.IsPLPType() ? SkipDesc{SkipForm::SLOW, 0} : SkipDesc{SkipForm::PREFIX2, 0};
	default:
		// XML (always PLP), TEXT/NTEXT/IMAGE (text-pointer form + MAX_LOB
		// guard), and anything unknown: the legacy walk (SkipValue) stays the one
		// implementation of the hard forms.
		return {SkipForm::SLOW, 0};
	}
}

bool RowReader::ReadRow(const uint8_t *data, size_t length, size_t &bytes_consumed, RowData &row) {
	// Use Prepare to allocate/clear efficiently (preserves capacity)
	row.Prepare(columns_.size());

	size_t offset = 0;

	for (size_t col_idx = 0; col_idx < columns_.size(); col_idx++) {
		bool is_null = false;

		// Read directly into the pre-allocated vector (clears it first, preserves capacity)
		row.values[col_idx].clear();
		size_t consumed = ReadValue(data + offset, length - offset, col_idx, row.values[col_idx], is_null);
		if (consumed == 0) {
			return false;  // Need more data
		}

		row.null_mask[col_idx] = is_null;
		offset += consumed;
	}

	bytes_consumed = offset;
	return true;
}

bool RowReader::SkipRow(const uint8_t *data, size_t length, size_t &bytes_consumed) {
	// Fast path: just calculate byte size without copying data
	size_t offset = 0;

	if (skip_descs_ != nullptr) {
		for (size_t col_idx = 0; col_idx < columns_.size(); col_idx++) {
			const SkipDesc desc = skip_descs_[col_idx];
			size_t consumed;
			if (desc.form == SkipForm::SLOW) {
				consumed = SkipValue(data + offset, length - offset, col_idx);
			} else {
				consumed = SkipByForm(data + offset, length - offset, desc);
			}
			if (consumed == 0) {
				return false;  // Need more data
			}
			offset += consumed;
		}
		bytes_consumed = offset;
		return true;
	}

	for (size_t col_idx = 0; col_idx < columns_.size(); col_idx++) {
		size_t consumed = SkipValue(data + offset, length - offset, col_idx);
		if (consumed == 0) {
			return false;  // Need more data
		}
		offset += consumed;
	}

	bytes_consumed = offset;
	return true;
}

bool RowReader::ReadNBCRow(const uint8_t *data, size_t length, size_t &bytes_consumed, RowData &row) {
	// Use Prepare to allocate/clear efficiently (preserves capacity)
	row.Prepare(columns_.size());

	// Calculate null bitmap size: ceil(columns / 8)
	size_t bitmap_bytes = (columns_.size() + 7) / 8;

	if (length < bitmap_bytes) {
		return false;  // Need more data for bitmap
	}

	// Read null bitmap and set null_mask
	for (size_t col_idx = 0; col_idx < columns_.size(); col_idx++) {
		size_t byte_idx = col_idx / 8;
		size_t bit_idx = col_idx % 8;
		bool is_null = (data[byte_idx] & (1 << bit_idx)) != 0;
		row.null_mask[col_idx] = is_null;
	}

	size_t offset = bitmap_bytes;

	// Read values for non-NULL columns only
	for (size_t col_idx = 0; col_idx < columns_.size(); col_idx++) {
		if (row.null_mask[col_idx]) {
			// Column is NULL - no data to read (already cleared by Prepare)
			continue;
		}

		bool is_null = false;

		// Read directly into the pre-allocated vector
		row.values[col_idx].clear();
		size_t consumed = ReadValueNBC(data + offset, length - offset, col_idx, row.values[col_idx], is_null);
		if (consumed == 0) {
			return false;  // Need more data
		}

		// The bitmap says present; a value can still say NULL by itself, and that NULL is honoured
		// whatever the bitmap said (spec 093 T0; review of #557): a sql_variant of length 0, a PLP
		// value (xml, a UDT, a MAX string or binary) carrying PLP_NULL, a non-MAX string or binary of
		// length 0xFFFF. Before T0 it was dropped, and such a value read as an empty one. The other
		// types' NBC arms take no NULL form of their own.
		if (is_null) {
			row.null_mask[col_idx] = true;
		}
		offset += consumed;
	}

	bytes_consumed = offset;
	return true;
}

bool RowReader::SkipNBCRow(const uint8_t *data, size_t length, size_t &bytes_consumed) {
	// Calculate null bitmap size
	size_t bitmap_bytes = (columns_.size() + 7) / 8;

	if (length < bitmap_bytes) {
		return false;
	}

	size_t offset = bitmap_bytes;

	// Skip values for non-NULL columns only
	for (size_t col_idx = 0; col_idx < columns_.size(); col_idx++) {
		size_t byte_idx = col_idx / 8;
		size_t bit_idx = col_idx % 8;
		bool is_null = (data[byte_idx] & (1 << bit_idx)) != 0;

		if (is_null) {
			continue;  // No data for NULL column
		}

		size_t consumed;
		if (skip_descs_ != nullptr && skip_descs_[col_idx].form != SkipForm::SLOW) {
			consumed = SkipByForm(data + offset, length - offset, skip_descs_[col_idx]);
		} else {
			consumed = SkipValueNBC(data + offset, length - offset, col_idx);
		}
		if (consumed == 0) {
			return false;
		}
		offset += consumed;
	}

	bytes_consumed = offset;
	return true;
}

size_t RowReader::SkipValue(const uint8_t *data, size_t length, size_t col_idx) {
	const ColumnMetadata &col = columns_[col_idx];
	const ValueFraming framing = LEGACY_FRAMINGS.Of(col.type_id);

	// The forms in the order rows meet them: a chain of tests on one byte, not a second jump.
	if (framing.kind == FramingKind::FIXED) {
		return length >= framing.width ? framing.width : 0;
	}
	if (framing.kind == FramingKind::BYTE_PREFIX) {
		return SkipBytePrefixed(data, length);
	}
	if (framing.kind == FramingKind::SHORT_PREFIX) {
		// Check for MAX types (PLP encoding)
		return col.IsPLPType() ? SkipPLPType(data, length) : SkipShortPrefixed(data, length);
	}
	return SkipRareForm(data, length, static_cast<uint8_t>(framing.kind));
}

//! The forms few columns have: a PLP chunk list, a legacy LOB, a sql_variant -- or a type the walk
//! does not know, which cannot be skipped.
size_t RowReader::SkipRareForm(const uint8_t *data, size_t length, uint8_t kind) {
	switch (static_cast<FramingKind>(kind)) {
	case FramingKind::PLP:
		return SkipPLPType(data, length);
	case FramingKind::LEGACY_LOB:
		return SkipLegacyLob(data, length);
	case FramingKind::VARIANT:
		return SkipVariant(data, length);
	default:
		return 0;  // Unknown type - can't skip
	}
}

size_t RowReader::ReadValue(const uint8_t *data, size_t length, size_t col_idx, std::vector<uint8_t> &value,
							bool &is_null) {
	const ColumnMetadata &col = columns_[col_idx];
	is_null = false;
	// A ROW's UNIQUEIDENTIFIER is read as 16 bytes whatever its prefix says (ReadGuidType); every
	// other type by its framing.
	if (col.type_id == TDS_TYPE_UNIQUEIDENTIFIER) {
		return ReadGuidType(data, length, value, is_null);
	}
	// The two forms most values have are read here, the 1-byte prefix (every nullable fixed type) first;
	// the rest by ReadFramed. Through ReadFramed alone a 1-byte-prefixed value read 15-17% slower than
	// the per-type switch before #648, and here no slower (an A/B of the two builds, interleaved).
	const ValueFraming framing = LEGACY_FRAMINGS.Of(col.type_id);
	if (framing.kind == FramingKind::BYTE_PREFIX) {
		return ReadBytePrefixed(data, length, value, is_null, true);
	}
	if (framing.kind == FramingKind::FIXED) {
		return ReadFixed(data, length, framing.width, value);
	}
	return ReadFramed(data, length, col, value, is_null, false);
}

//! One value by its type's framing, for a ROW or (`nbc`) for a column an NBCROW's bitmap says is
//! present. The two differ in one thing: a 1-byte prefix of 0 is NULL in a ROW and an empty value in
//! an NBCROW, where the bitmap alone says NULL. ReadValue / ReadValueNBC read the fixed and 1-byte
//! forms before calling this, through the same ReadFixed / ReadBytePrefixed; the arms stay here so
//! this reads every form (and measured: without them the 2-byte form read 5% slower, by code layout).
size_t RowReader::ReadFramed(const uint8_t *data, size_t length, const ColumnMetadata &col, std::vector<uint8_t> &value,
							 bool &is_null, bool nbc) {
	const ValueFraming framing = LEGACY_FRAMINGS.Of(col.type_id);

	// The forms in the order rows meet them, as in SkipValue.
	if (framing.kind == FramingKind::FIXED) {
		return ReadFixed(data, length, framing.width, value);
	}
	if (framing.kind == FramingKind::BYTE_PREFIX) {
		return ReadBytePrefixed(data, length, value, is_null, !nbc);
	}
	if (framing.kind == FramingKind::SHORT_PREFIX) {
		// Check for MAX types (PLP encoding)
		return col.IsPLPType() ? ReadPLPType(data, length, value, is_null)
							   : ReadVariableLengthType(data, length, value, is_null);
	}
	if (framing.kind == FramingKind::PLP) {
		return ReadPLPType(data, length, value, is_null);
	}
	if (framing.kind == FramingKind::VARIANT) {
		// sql_variant (spec 093 G2): a u32 length, then that many bytes; 0 is NULL in a ROW.
		return ReadVariant(data, length, value, is_null);
	}
	// A legacy LOB (TEXT / NTEXT / IMAGE) has a skip walk and no reader here, and an unknown type neither.
	throw std::runtime_error(
		std::string(nbc ? "Unsupported type in NBC RowReader: " : "Unsupported type in RowReader: ") +
		col.GetTypeName());
}

size_t RowReader::ReadVariableLengthType(const uint8_t *data, size_t length, std::vector<uint8_t> &value,
										 bool &is_null) {
	if (length < 2) {
		return 0;  // Need length field
	}

	uint16_t data_length = static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8);

	// 0xFFFF indicates NULL
	if (data_length == 0xFFFF) {
		is_null = true;
		value.clear();
		return 2;
	}

	if (length < 2 + data_length) {
		return 0;  // Need more data
	}

	value.assign(data + 2, data + 2 + data_length);
	return 2 + data_length;
}

size_t RowReader::ReadGuidType(const uint8_t *data, size_t length, std::vector<uint8_t> &value, bool &is_null) {
	if (length < 1) {
		return 0;
	}

	uint8_t data_length = data[0];

	if (data_length == 0) {
		is_null = true;
		value.clear();
		return 1;
	}

	// GUID is always 16 bytes
	if (length < 1 + 16) {
		return 0;
	}

	value.assign(data + 1, data + 1 + 16);
	return 1 + 16;
}

// A sql_variant value: a u32 length, then the value (its header included). A length of 0 is NULL; past
// SQL_VARIANT_MAX_BYTES the stream is malformed. Returns the bytes consumed, 0 when they have not all
// arrived yet.
static size_t VariantFrame(const uint8_t *data, size_t length, uint32_t &value_length) {
	if (length < 4)
		return 0;
	value_length = static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
				   (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
	if (value_length > SQL_VARIANT_MAX_BYTES) {
		throw std::runtime_error("sql_variant value declares " + std::to_string(value_length) + " bytes, past the " +
								 std::to_string(SQL_VARIANT_MAX_BYTES) + "-byte maximum. The TDS stream is malformed.");
	}
	return length >= 4u + value_length ? 4u + value_length : 0;
}

size_t RowReader::SkipVariant(const uint8_t *data, size_t length) {
	uint32_t value_length = 0;
	return VariantFrame(data, length, value_length);
}

size_t RowReader::ReadVariant(const uint8_t *data, size_t length, std::vector<uint8_t> &value, bool &is_null) {
	uint32_t value_length = 0;
	const size_t consumed = VariantFrame(data, length, value_length);
	if (consumed == 0) {
		return 0;
	}
	is_null = value_length == 0;
	value.assign(data + 4, data + 4 + value_length);
	return consumed;
}

// PLP_NULL marker: 0xFFFFFFFFFFFFFFFF (all bits set = null value)
static constexpr uint64_t PLP_NULL_MARKER = 0xFFFFFFFFFFFFFFFFULL;
// PLP_UNKNOWN marker: 0xFFFFFFFFFFFFFFFE (unknown total length, chunks follow)
static constexpr uint64_t PLP_UNKNOWN_MARKER = 0xFFFFFFFFFFFFFFFEULL;

// Debug logging for PLP parsing
static int GetPLPDebugLevel() {
	static const int level = []() {
		const char *env = std::getenv("MSSQL_DEBUG_PLP");
		return env ? std::atoi(env) : 0;
	}();
	return level;
}

#define PLP_DEBUG(level, fmt, ...)                             \
	do {                                                       \
		if (GetPLPDebugLevel() >= level) {                     \
			fprintf(stderr, "[PLP] " fmt "\n", ##__VA_ARGS__); \
		}                                                      \
	} while (0)

size_t RowReader::ReadPLPType(const uint8_t *data, size_t length, std::vector<uint8_t> &value, bool &is_null) {
	// PLP format:
	// 8 bytes: total length (PLP_NULL=null, PLP_UNKNOWN=unknown length, else actual length)
	// Then chunks: 4-byte chunk length + chunk data, until chunk length == 0

	PLP_DEBUG(1, "ReadPLPType: buffer_length=%zu", length);

	// Hex dump first 16 bytes for debugging
	if (GetPLPDebugLevel() >= 2 && length >= 16) {
		fprintf(stderr, "[PLP] ReadPLPType: first 16 bytes: ");
		for (size_t i = 0; i < 16; i++) {
			fprintf(stderr, "%02x ", data[i]);
		}
		fprintf(stderr, "\n");
	}

	if (length < 8) {
		PLP_DEBUG(1, "ReadPLPType: need more data for header (have %zu, need 8)", length);
		return 0;  // Need total length field
	}

	// Read 8-byte total length (little-endian)
	uint64_t total_length = static_cast<uint64_t>(data[0]) | (static_cast<uint64_t>(data[1]) << 8) |
							(static_cast<uint64_t>(data[2]) << 16) | (static_cast<uint64_t>(data[3]) << 24) |
							(static_cast<uint64_t>(data[4]) << 32) | (static_cast<uint64_t>(data[5]) << 40) |
							(static_cast<uint64_t>(data[6]) << 48) | (static_cast<uint64_t>(data[7]) << 56);

	size_t offset = 8;

	PLP_DEBUG(1, "ReadPLPType: total_length=0x%llx (%llu)", (unsigned long long)total_length,
			  (unsigned long long)total_length);

	// Check for NULL
	if (total_length == PLP_NULL_MARKER) {
		PLP_DEBUG(1, "ReadPLPType: NULL value");
		is_null = true;
		value.clear();
		return offset;
	}

	is_null = false;

	// For PLP_UNKNOWN or known length, read chunks
	//
	// Pre-allocate if we know the total length -- but never more than the buffer
	// in hand. A parse only SUCCEEDS with the whole value buffered (every chunk
	// below returns 0, "need more data", unless `offset + chunk_length <=
	// length`), so a reserve beyond `length` can never be the allocation this
	// call ends up needing: either the value fits and `length` covers it, or the
	// caller comes back with more bytes and reserves again.
	//
	// Without the cap, `total_length` is the SERVER's declaration bounded only by
	// the 0x7FFFFFFF below: one small response declaring ~2 GB and then sending
	// the terminator chunk immediately buys a 2 GB allocation before a single
	// payload byte arrives. Nothing is written out of bounds and the memory is
	// released, so this was remote memory amplification rather than corruption --
	// but the LOB path next door already caps (MAX_LOB_VALUE_BYTES, "declares N
	// bytes, past the N-byte maximum"), and the inconsistency was the bug. Found
	// by the q4 query in joern/ ("allocation sized by a declared length").
	if (total_length != PLP_UNKNOWN_MARKER && total_length < 0x7FFFFFFF) {
		const size_t declared = static_cast<size_t>(total_length);
		value.reserve(declared < length ? declared : length);
	}
	value.clear();

	// Read chunks until terminator (chunk length == 0)
	int chunk_num = 0;
	while (true) {
		if (offset + 4 > length) {
			PLP_DEBUG(1, "ReadPLPType: need more data for chunk header (have %zu, need %zu)", length, offset + 4);
			return 0;  // Need chunk length
		}

		// Read 4-byte chunk length (little-endian)
		uint32_t chunk_length = static_cast<uint32_t>(data[offset]) | (static_cast<uint32_t>(data[offset + 1]) << 8) |
								(static_cast<uint32_t>(data[offset + 2]) << 16) |
								(static_cast<uint32_t>(data[offset + 3]) << 24);
		offset += 4;

		PLP_DEBUG(2, "ReadPLPType: chunk[%d] length=%u, offset=%zu, buffer_length=%zu", chunk_num, chunk_length, offset,
				  length);

		// Terminator: chunk length == 0
		if (chunk_length == 0) {
			PLP_DEBUG(1, "ReadPLPType: terminator found, total_read=%zu bytes in %d chunks", value.size(), chunk_num);
			break;
		}

		// Read chunk data
		if (offset + chunk_length > length) {
			PLP_DEBUG(1, "ReadPLPType: need more data for chunk (have %zu, need %zu)", length, offset + chunk_length);
			return 0;  // Need more chunk data
		}

		// Append chunk data to value
		value.insert(value.end(), data + offset, data + offset + chunk_length);
		offset += chunk_length;
		chunk_num++;
	}

	PLP_DEBUG(1, "ReadPLPType: complete, consumed=%zu bytes, value_size=%zu", offset, value.size());
	return offset;
}

size_t RowReader::SkipPLPType(const uint8_t *data, size_t length) {
	// Same structure as ReadPLPType but without copying data

	if (length < 8) {
		return 0;
	}

	// Read 8-byte total length
	uint64_t total_length = static_cast<uint64_t>(data[0]) | (static_cast<uint64_t>(data[1]) << 8) |
							(static_cast<uint64_t>(data[2]) << 16) | (static_cast<uint64_t>(data[3]) << 24) |
							(static_cast<uint64_t>(data[4]) << 32) | (static_cast<uint64_t>(data[5]) << 40) |
							(static_cast<uint64_t>(data[6]) << 48) | (static_cast<uint64_t>(data[7]) << 56);

	size_t offset = 8;

	// Check for NULL
	if (total_length == PLP_NULL_MARKER) {
		return offset;
	}

	// Skip chunks
	while (true) {
		if (offset + 4 > length) {
			return 0;
		}

		uint32_t chunk_length = static_cast<uint32_t>(data[offset]) | (static_cast<uint32_t>(data[offset + 1]) << 8) |
								(static_cast<uint32_t>(data[offset + 2]) << 16) |
								(static_cast<uint32_t>(data[offset + 3]) << 24);
		offset += 4;

		if (chunk_length == 0) {
			break;
		}

		if (offset + chunk_length > length) {
			return 0;
		}

		offset += chunk_length;
	}

	return offset;
}

// NBC row value reading - for the columns the bitmap says are present. Framed as in a ROW; what
// differs is in ReadFramed (it was a second copy of ReadValue's switch).
size_t RowReader::ReadValueNBC(const uint8_t *data, size_t length, size_t col_idx, std::vector<uint8_t> &value,
							   bool &is_null) {
	is_null = false;  // NBC rows don't include data for NULL columns
	// As ReadValue: the two common forms here, the rest by ReadFramed.
	const ColumnMetadata &col = columns_[col_idx];
	const ValueFraming framing = LEGACY_FRAMINGS.Of(col.type_id);
	if (framing.kind == FramingKind::BYTE_PREFIX) {
		return ReadBytePrefixed(data, length, value, is_null, false);
	}
	if (framing.kind == FramingKind::FIXED) {
		return ReadFixed(data, length, framing.width, value);
	}
	return ReadFramed(data, length, col, value, is_null, true);
}

size_t RowReader::SkipValueNBC(const uint8_t *data, size_t length, size_t col_idx) {
	// A value present in an NBCROW is framed as it is in a ROW -- the bitmap only leaves the NULL
	// columns out, and a present value keeps its length prefix -- so this is the same walk. (It was a
	// second copy of SkipValue's switch, arm for arm.) A 0xFFFF prefix should not happen here, for a
	// value the bitmap says is present; it is skipped as the two bytes it is, as before.
	return SkipValue(data, length, col_idx);
}

}  // namespace tds
}  // namespace duckdb
