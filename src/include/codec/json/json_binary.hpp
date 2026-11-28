//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// codec/json/json_binary.hpp
//
// Spec 086 PR 6: SQL Server 2025's json type in its stored binary form (E9, E9b: measured/E9-json.md) -- what
// a record holds; over the wire the server sends the text, with JSONSUPPORT too (J1) -- a header, a pool of
// items, the container nodes, a key table and a 16-byte trailer, little-endian. DuckDB-free. Read
// with every offset and count bounded; a form E9 did not see (a tag, a flag bit, a constant) is refused
// as malformed with its reason, never guessed. Two outputs: the server's own text (what CAST(j AS
// nvarchar(max)) gives, TYPE mssql's reading of the column) and a walk the VARIANT emitter drives.
//===----------------------------------------------------------------------===//

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace duckdb {
namespace mssql {
namespace codec {
namespace json {

//! The nesting a document may hold: the server's own limit (E9: 129 is refused).
constexpr uint32_t JSON_MAX_DEPTH = 128;
//! The longest text a document renders to: the backup reader's value limit (2^31 - 1). A key is stored once and
//! referenced by every member that has it, so a document of a few MB can name a 7,998-byte key often enough to
//! render past it -- read as malformed, not truncated.
constexpr size_t JSON_MAX_TEXT_BYTES = 0x7FFFFFFF;

class JsonDocument {
public:
	enum class Kind : uint8_t { Null, False, True, Integer, Decimal, String, Object, Array };

	//! A decimal item: precision, scale, sign and the mantissa, an unsigned integer of up to 128 bits.
	struct Decimal {
		uint8_t precision = 0;
		uint8_t scale = 0;
		bool negative = false;
		uint64_t low = 0;
		uint64_t high = 0;
	};

	//! An object or an array node: `count` slots, some of an object's deleted in place (Member says).
	struct Container {
		bool object = false;
		uint32_t count = 0;
		uint32_t refs = 0;	  // an object's key references: their offset
		uint32_t values = 0;  // the value words: their offset
	};

	//! The document `data` / `size`: its header, trailer and key table checked. False with `why` when they
	//! are not the form E9 measured.
	bool Open(const uint8_t *data, size_t size, std::string &why);
	//! The root value word (an object or an array: a bare scalar is refused by the server).
	uint32_t Root() const {
		return root_;
	}

	bool KindOf(uint32_t value, Kind &kind, std::string &why) const;
	//! An integer: one in the value word (30 bits) or an int64 item.
	bool IntegerOf(uint32_t value, int64_t &out, std::string &why) const;
	bool DecimalOf(uint32_t value, Decimal &out, std::string &why) const;
	//! A string value's UTF-8 bytes, unescaped (""'s too: `length` 0).
	bool StringOf(uint32_t value, const uint8_t *&bytes, uint32_t &length, std::string &why) const;
	bool ContainerOf(uint32_t value, Container &out, std::string &why) const;
	//! Slot `i` of `c`: an array's element, or an object's member -- its key bytes as stored (escapes as
	//! written; the empty key empty) -- unless `deleted` (a member removed in place, left out of the text).
	bool Member(const Container &c, uint32_t i, bool &deleted, const uint8_t *&key, uint32_t &key_length,
				uint32_t &value, std::string &why) const;

private:
	bool Fits(uint64_t offset, uint64_t length) const {
		return offset <= size_ && length <= size_ - offset;
	}
	bool U16(uint64_t at, uint32_t &out) const;
	bool U32(uint64_t at, uint32_t &out) const;
	//! A variable length (E9: 1-4 bytes sized by its low bits) at `at`: its value and its own size.
	bool Length(uint64_t at, uint32_t &length, uint32_t &width) const;
	//! A node's header at `at`: its count and flag byte, and where its slot arrays begin.
	bool NodeHeader(uint64_t at, uint32_t &count, uint8_t &flag, uint64_t &slots) const;
	bool KeyAt(uint32_t index, const uint8_t *&key, uint32_t &length, std::string &why) const;

	const uint8_t *data_ = nullptr;
	uint64_t size_ = 0;
	bool wide_ = false;
	uint32_t root_ = 0;
	//! The key table: its count and where its key offsets are; none when the document has no key.
	bool has_keys_ = false;
	uint32_t key_count_ = 0;
	uint64_t key_offsets_ = 0;
};

//! The server's text of a stored json value: no whitespace, keys as stored, strings escaped by the
//! server's rules (E9), numbers as stored. False with `why` when the document is malformed.
//! `check_keys`: also refuse a key whose escapes do not unescape (a lone surrogate the server takes and
//! keeps as written, measured; DuckDB's JSON parse refuses it) -- the VARIANT's reading. The text alone
//! is the server's, such a key included.
bool RenderJsonText(const uint8_t *data, size_t size, std::string &out, std::string &why, bool check_keys = false);
//! A decimal's text as the server writes it: the mantissa with `scale` digits after the point.
void AppendDecimalText(const JsonDocument::Decimal &d, std::string &out);
//! A key as stored (its escapes as written, E9b) unescaped as a JSON parser reads it -- what DuckDB's
//! JSON -> VARIANT keys it by. False when an escape is malformed.
bool UnescapeJsonKey(const uint8_t *key, uint32_t length, std::string &out);

}  // namespace json
}  // namespace codec
}  // namespace mssql
}  // namespace duckdb
