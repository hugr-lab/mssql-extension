// Spec 086 PR 6: SQL Server 2025's stored json value (codec/json/json_binary.hpp; E9 / E9b in
// specs/086-backup-row-formats-and-types/measured/E9-json.md). DuckDB-free (D15).

#include "codec/json/json_binary.hpp"

#include <cstring>

namespace duckdb {
namespace mssql {
namespace codec {
namespace json {

namespace {

// The value word's top two bits (E9): 10 an integer in the low 30 bits, 11 a constant, 00 an offset.
constexpr uint32_t VALUE_KIND_MASK = 0xC0000000u;
constexpr uint32_t VALUE_INTEGER = 0x80000000u;
constexpr uint32_t VALUE_CONSTANT = 0xC0000000u;
// The constants E9 saw: false, true, null, {}, [], "".
constexpr uint32_t CONST_FALSE = 0, CONST_TRUE = 1, CONST_NULL = 2, CONST_EMPTY_OBJECT = 4, CONST_EMPTY_ARRAY = 5,
				   CONST_EMPTY_STRING = 6;
// Item and node tags.
constexpr uint8_t TAG_OBJECT = 0x01, TAG_ARRAY = 0x02, TAG_STRING = 0x03, TAG_INT64 = 0x04, TAG_DECIMAL = 0x05,
				  TAG_KEY_TABLE = 0x06, TAG_KEY = 0x07;
// A node's flag byte is its number of spare slots (E9b, measured over 240 documents grown in place: an object or
// array of count c holds c + flag slots in each of its arrays; E9's 0x01 is one spare). The key table's the same
// in its low 7 bits, its top bit the sorted-hash permutation.
constexpr uint8_t KEY_TABLE_SPARE = 0x7F, FLAG_PERMUTED = 0x80;
constexpr uint32_t TRAILER_BYTES = 16;
constexpr uint32_t NO_KEY_TABLE = 0xFFFFFFFFu;

std::string Hex(uint32_t v) {
	static const char digits[] = "0123456789ABCDEF";
	std::string s = "0x";
	for (int shift = 28; shift >= 0; shift -= 4) {
		s.push_back(digits[(v >> shift) & 0xF]);
	}
	return s;
}

}  // namespace

bool JsonDocument::U16(uint64_t at, uint32_t &out) const {
	if (!Fits(at, 2)) {
		return false;
	}
	out = static_cast<uint32_t>(data_[at]) | (static_cast<uint32_t>(data_[at + 1]) << 8);
	return true;
}

bool JsonDocument::U32(uint64_t at, uint32_t &out) const {
	if (!Fits(at, 4)) {
		return false;
	}
	out = static_cast<uint32_t>(data_[at]) | (static_cast<uint32_t>(data_[at + 1]) << 8) |
		  (static_cast<uint32_t>(data_[at + 2]) << 16) | (static_cast<uint32_t>(data_[at + 3]) << 24);
	return true;
}

bool JsonDocument::Length(uint64_t at, uint32_t &length, uint32_t &width) const {
	if (!Fits(at, 1)) {
		return false;
	}
	const uint8_t first = data_[at];
	width = (first & 1) ? 1 : (first & 2) ? 2 : (first & 4) ? 3 : (first & 8) ? 4 : 0;
	if (width == 0 || !Fits(at, width)) {
		return false;
	}
	uint32_t raw = 0;
	for (uint32_t i = 0; i < width; i++) {
		raw |= static_cast<uint32_t>(data_[at + i]) << (8 * i);
	}
	length = raw >> width;
	return true;
}

bool JsonDocument::NodeHeader(uint64_t at, uint32_t &count, uint8_t &flag, uint64_t &slots) const {
	// A tag, a count (u16, or u32 when wide), a flag byte.
	const uint64_t count_at = at + 1;
	if (wide_ ? !U32(count_at, count) : !U16(count_at, count)) {
		return false;
	}
	const uint64_t flag_at = count_at + (wide_ ? 4 : 2);
	if (!Fits(flag_at, 1)) {
		return false;
	}
	flag = data_[flag_at];
	slots = flag_at + 1;
	return true;
}

bool JsonDocument::Open(const uint8_t *data, size_t size, std::string &why) {
	data_ = data;
	size_ = size;
	if (size < 2 + TRAILER_BYTES) {
		why = "a json value of " + std::to_string(size) + " bytes, shorter than its header and trailer";
		return false;
	}
	// The header's second byte: bit 0 says wide (u32 counts and key references); no other bit was seen.
	if (data[1] & ~1u) {
		why = "a json header whose second byte is " + Hex(data[1]) + " (only bit 0 is known)";
		return false;
	}
	wide_ = (data[1] & 1) != 0;
	const uint64_t trailer = size - TRAILER_BYTES;
	uint32_t key_table = 0;
	if (data[trailer] != 0x08 || data[size - 3] || data[size - 2] || data[size - 1] || !U32(trailer + 5, root_) ||
		!U32(trailer + 9, key_table)) {
		why = "a json trailer of another form than E9's";
		return false;
	}
	has_keys_ = key_table != NO_KEY_TABLE;
	if (has_keys_) {
		uint32_t count = 0;
		uint8_t flag = 0;
		uint64_t slots = 0;
		if (key_table >= trailer || data[key_table] != TAG_KEY_TABLE || !NodeHeader(key_table, count, flag, slots)) {
			why = "a json key table at " + std::to_string(key_table) + " that is none";
			return false;
		}
		const uint64_t slot_count = static_cast<uint64_t>(count) + (flag & KEY_TABLE_SPARE);
		// The hashes (u32), then with the permutation flag a permutation (u16, u32 when wide), then the offsets.
		uint64_t at = slots + slot_count * 4;
		if (flag & FLAG_PERMUTED) {
			at += slot_count * (wide_ ? 4 : 2);
		}
		if (!Fits(at, slot_count * 4)) {
			why = "a json key table of " + std::to_string(count) + " keys past its document";
			return false;
		}
		key_count_ = count;
		key_offsets_ = at;
	}
	Kind kind;
	if (!KindOf(root_, kind, why)) {
		return false;
	}
	if (kind != Kind::Object && kind != Kind::Array) {
		why = "a json document whose root is no object or array";
		return false;
	}
	return true;
}

bool JsonDocument::KindOf(uint32_t value, Kind &kind, std::string &why) const {
	switch (value & VALUE_KIND_MASK) {
	case VALUE_INTEGER:
		kind = Kind::Integer;
		return true;
	case VALUE_CONSTANT:
		switch (value & ~VALUE_KIND_MASK) {
		case CONST_FALSE:
			kind = Kind::False;
			return true;
		case CONST_TRUE:
			kind = Kind::True;
			return true;
		case CONST_NULL:
			kind = Kind::Null;
			return true;
		case CONST_EMPTY_OBJECT:
			kind = Kind::Object;
			return true;
		case CONST_EMPTY_ARRAY:
			kind = Kind::Array;
			return true;
		case CONST_EMPTY_STRING:
			kind = Kind::String;
			return true;
		default:
			why = "a json constant " + Hex(value) + " E9 did not see";
			return false;
		}
	case 0:
		break;
	default:
		why = "a json value word " + Hex(value) + " of no kind E9 saw";
		return false;
	}
	if (value >= size_ - TRAILER_BYTES) {
		why = "a json value at " + std::to_string(value) + ", past its document";
		return false;
	}
	switch (data_[value]) {
	case TAG_OBJECT:
		kind = Kind::Object;
		return true;
	case TAG_ARRAY:
		kind = Kind::Array;
		return true;
	case TAG_STRING:
		kind = Kind::String;
		return true;
	case TAG_INT64:
		kind = Kind::Integer;
		return true;
	case TAG_DECIMAL:
		kind = Kind::Decimal;
		return true;
	default:
		why = "a json item tag " + Hex(data_[value]) + " at " + std::to_string(value) + " where a value is";
		return false;
	}
}

bool JsonDocument::IntegerOf(uint32_t value, int64_t &out, std::string &why) const {
	if ((value & VALUE_KIND_MASK) == VALUE_INTEGER) {
		// Two's complement in 30 bits.
		const uint32_t low = value & 0x3FFFFFFFu;
		out = (low & 0x20000000u) ? static_cast<int64_t>(low) - (int64_t(1) << 30) : static_cast<int64_t>(low);
		return true;
	}
	uint32_t lo = 0, hi = 0;
	if (data_[value] != TAG_INT64 || !U32(uint64_t(value) + 1, lo) || !U32(uint64_t(value) + 5, hi)) {
		why = "a json int64 item at " + std::to_string(value) + " past its document";
		return false;
	}
	out = static_cast<int64_t>((static_cast<uint64_t>(hi) << 32) | lo);
	return true;
}

bool JsonDocument::DecimalOf(uint32_t value, Decimal &out, std::string &why) const {
	// A length n, then precision, scale, a sign byte (1 positive, 0 negative), a mantissa of 2 x (n - 1) bytes.
	uint32_t n = 0, width = 0;
	if (!Length(uint64_t(value) + 1, n, width) || n < 2) {
		why = "a json decimal at " + std::to_string(value) + " whose length is malformed";
		return false;
	}
	const uint64_t at = uint64_t(value) + 1 + width;
	const uint64_t mantissa_bytes = 2 * (uint64_t(n) - 1);
	if (mantissa_bytes > 16 || !Fits(at, 3 + mantissa_bytes)) {
		why = "a json decimal at " + std::to_string(value) + " of " + std::to_string(mantissa_bytes) +
			  " mantissa bytes (at most 16), or past its document";
		return false;
	}
	out.precision = data_[at];
	out.scale = data_[at + 1];
	if (data_[at + 2] > 1 || out.precision == 0 || out.precision > 38 || out.scale > out.precision) {
		why = "a json decimal at " + std::to_string(value) + " of precision " + std::to_string(out.precision) +
			  ", scale " + std::to_string(out.scale) + " and sign byte " + std::to_string(data_[at + 2]);
		return false;
	}
	out.negative = data_[at + 2] == 0;
	out.low = out.high = 0;
	for (uint64_t i = 0; i < mantissa_bytes; i++) {
		const uint64_t byte = data_[at + 3 + i];
		if (i < 8) {
			out.low |= byte << (8 * i);
		} else {
			out.high |= byte << (8 * (i - 8));
		}
	}
	return true;
}

bool JsonDocument::StringOf(uint32_t value, const uint8_t *&bytes, uint32_t &length, std::string &why) const {
	if (value == (VALUE_CONSTANT | CONST_EMPTY_STRING)) {
		bytes = data_;
		length = 0;
		return true;
	}
	uint32_t width = 0;
	if (data_[value] != TAG_STRING || !Length(uint64_t(value) + 1, length, width) ||
		!Fits(uint64_t(value) + 1 + width, length)) {
		why = "a json string at " + std::to_string(value) + " past its document";
		return false;
	}
	bytes = data_ + value + 1 + width;
	return true;
}

bool JsonDocument::ContainerOf(uint32_t value, Container &out, std::string &why) const {
	if ((value & VALUE_KIND_MASK) == VALUE_CONSTANT) {
		out.object = value == (VALUE_CONSTANT | CONST_EMPTY_OBJECT);
		out.count = 0;
		return true;
	}
	uint32_t count = 0;
	uint8_t flag = 0;
	uint64_t slots = 0;
	if (!NodeHeader(value, count, flag, slots)) {
		why = "a json node at " + std::to_string(value) + " past its document";
		return false;
	}
	out.object = data_[value] == TAG_OBJECT;
	out.count = count;
	const uint64_t slot_count = static_cast<uint64_t>(count) + flag;
	const uint64_t ref_bytes = out.object ? slot_count * (wide_ ? 4 : 2) : 0;
	if (!Fits(slots, ref_bytes + slot_count * 4)) {
		why = "a json node of " + std::to_string(count) + " slots past its document";
		return false;
	}
	out.refs = static_cast<uint32_t>(slots);
	out.values = static_cast<uint32_t>(slots + ref_bytes);
	if (out.object && count > 0 && !has_keys_) {
		why = "a json object with members in a document with no key table";
		return false;
	}
	return true;
}

bool JsonDocument::KeyAt(uint32_t index, const uint8_t *&key, uint32_t &length, std::string &why) const {
	uint32_t offset = 0;
	if (index >= key_count_ || !U32(key_offsets_ + uint64_t(index) * 4, offset)) {
		why = "a json key reference " + std::to_string(index) + " past its key table of " + std::to_string(key_count_);
		return false;
	}
	if (offset == (VALUE_CONSTANT | CONST_EMPTY_STRING)) {
		key = data_;
		length = 0;
		return true;
	}
	uint32_t width = 0;
	if (offset >= size_ - TRAILER_BYTES || data_[offset] != TAG_KEY || !Length(uint64_t(offset) + 1, length, width) ||
		!Fits(uint64_t(offset) + 1 + width, length)) {
		why = "a json key at " + std::to_string(offset) + " that is none";
		return false;
	}
	key = data_ + offset + 1 + width;
	return true;
}

bool JsonDocument::Member(const Container &c, uint32_t i, bool &deleted, const uint8_t *&key, uint32_t &key_length,
						  uint32_t &value, std::string &why) const {
	deleted = false;
	if (!U32(uint64_t(c.values) + uint64_t(i) * 4, value)) {
		why = "a json slot past its document";
		return false;
	}
	if (!c.object) {
		return true;
	}
	uint32_t ref = 0;
	if (wide_ ? !U32(uint64_t(c.refs) + uint64_t(i) * 4, ref) : !U16(uint64_t(c.refs) + uint64_t(i) * 2, ref)) {
		why = "a json key reference past its document";
		return false;
	}
	// A member deleted in place keeps its slot, its reference marked by the top bit (E9).
	const uint32_t deleted_bit = wide_ ? 0x80000000u : 0x8000u;
	if (ref & deleted_bit) {
		deleted = true;
		return true;
	}
	return KeyAt(ref, key, key_length, why);
}

void AppendDecimalText(const JsonDocument::Decimal &d, std::string &out) {
	// The mantissa's decimal digits, by repeated division of the 128-bit value by 10.
	uint64_t high = d.high, low = d.low;
	char digits[48];
	int n = 0;
	do {
		// (high:low) / 10 with the remainder, 32 bits at a time.
		uint64_t rem = 0;
		uint32_t parts[4] = {static_cast<uint32_t>(high >> 32), static_cast<uint32_t>(high),
							 static_cast<uint32_t>(low >> 32), static_cast<uint32_t>(low)};
		for (auto &p : parts) {
			const uint64_t cur = (rem << 32) | p;
			p = static_cast<uint32_t>(cur / 10);
			rem = cur % 10;
		}
		high = (static_cast<uint64_t>(parts[0]) << 32) | parts[1];
		low = (static_cast<uint64_t>(parts[2]) << 32) | parts[3];
		digits[n++] = static_cast<char>('0' + rem);
	} while ((high | low) != 0 && n < 47);
	// At least scale + 1 digits: zeros before the point when the mantissa is shorter.
	while (n < d.scale + 1) {
		digits[n++] = '0';
	}
	if (d.negative) {
		out.push_back('-');
	}
	for (int i = n - 1; i >= 0; i--) {
		out.push_back(digits[i]);
		if (i == d.scale && d.scale > 0) {
			out.push_back('.');
		}
	}
}

namespace {

void AppendEscaped(const uint8_t *bytes, uint32_t length, std::string &out) {
	static const char hex[] = "0123456789ABCDEF";
	out.push_back('"');
	for (uint32_t i = 0; i < length; i++) {
		const uint8_t c = bytes[i];
		switch (c) {
		case '"':
			out += "\\\"";
			break;
		case '\\':
			out += "\\\\";
			break;
		case '\b':
			out += "\\b";
			break;
		case '\f':
			out += "\\f";
			break;
		case '\n':
			out += "\\n";
			break;
		case '\r':
			out += "\\r";
			break;
		case '\t':
			out += "\\t";
			break;
		default:
			if (c < 0x20) {
				out += "\\u00";
				out.push_back(hex[c >> 4]);
				out.push_back(hex[c & 0xF]);
			} else {
				out.push_back(static_cast<char>(c));
			}
		}
	}
	out.push_back('"');
}

//! The walk's bounds: every value of a well-formed document fills a 4-byte slot of its own (the root, a
//! container's value slots), so a walk visits at most size / 4 + 1 values; one that visits more follows a
//! shared or cyclic node (two slots naming one container would render it twice, 2^128 times at depth 128).
struct WalkBudget {
	uint64_t values_left;
	bool check_keys;
	std::string unescaped;	// a checked key's, reused
};

bool AppendValue(const JsonDocument &doc, uint32_t value, uint32_t depth, WalkBudget &budget, std::string &out,
				 std::string &why) {
	if (budget.values_left == 0) {
		why = "a json document whose nodes are shared or cyclic";
		return false;
	}
	budget.values_left--;
	JsonDocument::Kind kind;
	if (!doc.KindOf(value, kind, why)) {
		return false;
	}
	switch (kind) {
	case JsonDocument::Kind::Null:
		out += "null";
		return true;
	case JsonDocument::Kind::False:
		out += "false";
		return true;
	case JsonDocument::Kind::True:
		out += "true";
		return true;
	case JsonDocument::Kind::Integer: {
		int64_t v = 0;
		if (!doc.IntegerOf(value, v, why)) {
			return false;
		}
		out += std::to_string(v);
		return true;
	}
	case JsonDocument::Kind::Decimal: {
		JsonDocument::Decimal d;
		if (!doc.DecimalOf(value, d, why)) {
			return false;
		}
		AppendDecimalText(d, out);
		return true;
	}
	case JsonDocument::Kind::String: {
		const uint8_t *bytes = nullptr;
		uint32_t length = 0;
		if (!doc.StringOf(value, bytes, length, why)) {
			return false;
		}
		AppendEscaped(bytes, length, out);
		return true;
	}
	case JsonDocument::Kind::Object:
	case JsonDocument::Kind::Array: {
		if (depth >= JSON_MAX_DEPTH) {
			why = "a json document nested past " + std::to_string(JSON_MAX_DEPTH);
			return false;
		}
		JsonDocument::Container c;
		if (!doc.ContainerOf(value, c, why)) {
			return false;
		}
		out.push_back(c.object ? '{' : '[');
		bool first = true;
		for (uint32_t i = 0; i < c.count; i++) {
			bool deleted = false;
			const uint8_t *key = nullptr;
			uint32_t key_length = 0;
			uint32_t member = 0;
			if (!doc.Member(c, i, deleted, key, key_length, member, why)) {
				return false;
			}
			if (deleted) {
				continue;
			}
			if (!first) {
				out.push_back(',');
			}
			first = false;
			if (c.object) {
				// A key as stored: its escapes as written (E9b).
				if (budget.check_keys && std::memchr(key, '\\', key_length) != nullptr &&
					!UnescapeJsonKey(key, key_length, budget.unescaped)) {
					why = "a json key whose escapes are not valid";
					return false;
				}
				out.push_back('"');
				out.append(reinterpret_cast<const char *>(key), key_length);
				out += "\":";
			}
			if (!AppendValue(doc, member, depth + 1, budget, out, why)) {
				return false;
			}
			if (out.size() > JSON_MAX_TEXT_BYTES) {
				why = "a json document whose text is longer than 2^31 - 1 bytes";
				return false;
			}
		}
		out.push_back(c.object ? '}' : ']');
		return true;
	}
	}
	return false;
}

int HexDigit(uint8_t c) {
	return c >= '0' && c <= '9'	  ? c - '0'
		   : c >= 'a' && c <= 'f' ? c - 'a' + 10
		   : c >= 'A' && c <= 'F' ? c - 'A' + 10
								  : -1;
}

void AppendUtf8(uint32_t cp, std::string &out) {
	if (cp < 0x80) {
		out.push_back(static_cast<char>(cp));
	} else if (cp < 0x800) {
		out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
		out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
	} else if (cp < 0x10000) {
		out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
		out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
		out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
	} else {
		out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
		out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
		out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
		out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
	}
}

}  // namespace

bool RenderJsonText(const uint8_t *data, size_t size, std::string &out, std::string &why, bool check_keys) {
	JsonDocument doc;
	if (!doc.Open(data, size, why)) {
		return false;
	}
	out.clear();
	WalkBudget budget{size / 4 + 1, check_keys, std::string()};
	if (!AppendValue(doc, doc.Root(), 0, budget, out, why)) {
		return false;
	}
	if (out.size() > JSON_MAX_TEXT_BYTES) {
		why = "a json document whose text is longer than 2^31 - 1 bytes";
		return false;
	}
	return true;
}

bool UnescapeJsonKey(const uint8_t *key, uint32_t length, std::string &out) {
	out.clear();
	for (uint32_t i = 0; i < length; i++) {
		const uint8_t c = key[i];
		if (c != '\\') {
			out.push_back(static_cast<char>(c));
			continue;
		}
		if (++i >= length) {
			return false;
		}
		switch (key[i]) {
		case '"':
		case '\\':
		case '/':
			out.push_back(static_cast<char>(key[i]));
			break;
		case 'b':
			out.push_back('\b');
			break;
		case 'f':
			out.push_back('\f');
			break;
		case 'n':
			out.push_back('\n');
			break;
		case 'r':
			out.push_back('\r');
			break;
		case 't':
			out.push_back('\t');
			break;
		case 'u': {
			auto read4 = [&](uint32_t at, uint32_t &cp) {
				if (at + 4 > length) {
					return false;
				}
				cp = 0;
				for (uint32_t k = 0; k < 4; k++) {
					const int d = HexDigit(key[at + k]);
					if (d < 0) {
						return false;
					}
					cp = (cp << 4) | static_cast<uint32_t>(d);
				}
				return true;
			};
			uint32_t cp = 0;
			if (!read4(i + 1, cp)) {
				return false;
			}
			i += 4;
			if (cp >= 0xD800 && cp <= 0xDBFF) {
				// A surrogate pair: the low half must follow as \uDC00-\uDFFF.
				uint32_t lo = 0;
				if (i + 2 >= length || key[i + 1] != '\\' || key[i + 2] != 'u' || !read4(i + 3, lo) || lo < 0xDC00 ||
					lo > 0xDFFF) {
					return false;
				}
				i += 6;
				cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
			} else if (cp >= 0xDC00 && cp <= 0xDFFF) {
				return false;
			}
			AppendUtf8(cp, out);
			break;
		}
		default:
			return false;
		}
	}
	return true;
}

}  // namespace json
}  // namespace codec
}  // namespace mssql
}  // namespace duckdb
