//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// codec/code_page/code_page_codec.cpp
//
// See code_page_codec.hpp.
//===----------------------------------------------------------------------===//

#include "codec/code_page/code_page_codec.hpp"

#include "codec/code_page/code_page.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/string_vector.hpp"
#include "tds/tds_types.hpp"

#include <cstring>

namespace duckdb {
namespace mssql {
namespace codec {
namespace code_page {

// DecodeColumn writes each row's string_t itself. The field order is checked by
// test_row_stager.cpp, on the views this wrapper publishes: string_t's fields are
// private, so nothing here can name them.
static_assert(sizeof(string_t) == sizeof(StringView16), "StringView16 must be string_t's layout");
static_assert(string_t::INLINE_LENGTH == 12 || string_t::INLINE_LENGTH == 0,
			  "DecodeColumn takes no other inline length");

//! The low 20 bits of ColumnMetadata::collation: the wire's LCID.
static constexpr uint32_t WIRE_LCID_MASK = 0xFFFFFu;

void ResolveWireCodePages(std::vector<tds::ColumnMetadata> &columns) {
	for (auto &col : columns) {
		if (!col.IsSingleByteTextColumn() || col.IsUtf8Collation()) {
			continue;
		}
		col.code_page =
			FindCodePage(CodePageForWireCollation(col.collation & WIRE_LCID_MASK, false, col.collation_sort_id));
	}
}

//! A DuckDB string holds at most MAX_STRING_SIZE bytes, and a chunk's output is
//! one allocation. 3 bytes per input byte is the bound while it fits.
static size_t OutputCapacity(size_t input_bytes) {
	return (string_t::MAX_STRING_SIZE - STORE_SLACK) / MAX_UTF8_PER_BYTE >= input_bytes ? Utf8Bound(input_bytes) : 0;
}

[[noreturn]] static void ThrowTooLarge(const tds::ColumnMetadata &col, size_t bytes) {
	throw InvalidInputException(
		"MSSQL: column \"%s\" decodes to %llu bytes of UTF-8 in one chunk, past DuckDB's "
		"%llu-byte string limit",
		col.name, static_cast<unsigned long long>(bytes), static_cast<unsigned long long>(string_t::MAX_STRING_SIZE));
}

//! What the bound cannot size, measured: value by value, as the decode reads them
//! (on a double-byte page a pair must not cross two values).
static size_t ExactCapacity(const CodePageTable &page, const staging::ColumnStaging &st, idx_t count,
							const tds::ColumnMetadata &col) {
	size_t bytes = 0;
	for (idx_t row = 0; row < count; row++) {
		if (st.IsValid(row)) {
			bytes += ExactUtf8Length(page, st.ValueAt(row), st.LengthAt(row));
		}
	}
	if (bytes > string_t::MAX_STRING_SIZE - STORE_SLACK) {
		ThrowTooLarge(col, bytes);
	}
	return bytes + STORE_SLACK;
}

void DecodeChunkFromStaging(const staging::ColumnStaging &st, idx_t count, const tds::ColumnMetadata &col,
							Vector &out) {
	string_t *const result = FlatVector::GetDataMutable<string_t>(out);
	auto *const views = reinterpret_cast<StringView16 *>(result);
	const size_t payload = st.PayloadSize();
	if (payload == 0) {
		// All NULL or all empty: an empty string_t is all zero, and needs no storage.
		std::memset(views, 0, count * sizeof(StringView16));
		return;
	}

	const CodePageTable &page = *col.code_page;
	size_t capacity = OutputCapacity(payload);
	if (capacity == 0) {
		capacity = ExactCapacity(page, st, count, col);
	}
	string_t output = StringVector::EmptyString(out, capacity);
	// No bit tests per row in the chunk that has no NULL (DecodeColumn's contract).
	const uint64_t *const validity = st.null_count == 0 ? nullptr : st.validity_words.data();
	DecodeColumn(page, st.buffer.data(), payload, st.offsets.data(), st.lengths.data(), validity, count,
				 output.GetDataWriteable(), views, string_t::INLINE_LENGTH);

	if (col.type_id != tds::TDS_TYPE_BIGCHAR) {
		return;
	}
	// The padding comes off the output: on a double-byte page an input 0x20 can be
	// a trail byte. A NULL row's view is empty, so it asks for no test of its own.
	for (idx_t row = 0; row < count; row++) {
		// A copy: an inlined value's bytes live in the slot being rewritten.
		const string_t value = result[row];
		const char *const data = value.GetData();
		uint32_t length = value.GetSize();
		while (length > 0 && data[length - 1] == ' ') {
			length--;
		}
		if (length != value.GetSize()) {
			result[row] = string_t(data, length);
		}
	}
}

void DecodeFromTds(const std::vector<uint8_t> &bytes, const tds::ColumnMetadata &col, Vector &out, idx_t row) {
	const CodePageTable &page = *col.code_page;
	size_t capacity = OutputCapacity(bytes.size());
	if (capacity == 0) {
		const size_t exact = ExactUtf8Length(page, bytes.data(), bytes.size());
		if (exact > string_t::MAX_STRING_SIZE - STORE_SLACK) {
			ThrowTooLarge(col, exact);
		}
		capacity = exact + STORE_SLACK;
	}
	string_t output = StringVector::EmptyString(out, capacity);
	const char *const data = output.GetDataWriteable();
	size_t length = DecodeToUtf8(page, bytes.data(), bytes.size(), output.GetDataWriteable());
	if (col.type_id == tds::TDS_TYPE_BIGCHAR) {
		while (length > 0 && data[length - 1] == ' ') {
			length--;
		}
	}
	FlatVector::GetDataMutableUnsafe<string_t>(out)[row] = string_t(data, static_cast<uint32_t>(length));
}

}  // namespace code_page
}  // namespace codec
}  // namespace mssql
}  // namespace duckdb
