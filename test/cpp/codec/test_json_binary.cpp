// Spec 086 PR 6: SQL Server 2025's stored json value (codec/json/json_binary), DuckDB-free. Documents built by hand
// in the form E9 / E9b measured: the server's text of each, every prefix of one refused rather than
// misread, and each form the decoder refuses -- an unknown constant or tag, a header bit, a root that is no
// container, nesting past the server's 128, a cycle or a shared node, a decimal mantissa past 16 bytes -- refused by
// name.

#include "codec/json/json_binary.hpp"

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

using namespace duckdb::mssql::codec::json;

namespace {

int failures = 0;

void Check(bool ok, const std::string &what) {
	std::cout << (ok ? "  ok   " : "  FAIL ") << what << std::endl;
	if (!ok) {
		failures++;
	}
}

using Bytes = std::vector<uint8_t>;

void U16(Bytes &b, uint32_t v) {
	b.push_back(static_cast<uint8_t>(v));
	b.push_back(static_cast<uint8_t>(v >> 8));
}

void U32(Bytes &b, uint32_t v) {
	for (int i = 0; i < 4; i++) {
		b.push_back(static_cast<uint8_t>(v >> (8 * i)));
	}
}

constexpr uint32_t INT(int32_t v) {
	return 0x80000000u | (static_cast<uint32_t>(v) & 0x3FFFFFFFu);
}
constexpr uint32_t CONST_TRUE = 0xC0000001u, CONST_NULL = 0xC0000002u, CONST_EMPTY_STRING = 0xC0000006u;

//! A document: `body` after the 2-byte header (`wide` sets bit 0 of its second byte), then the trailer
//! naming `root` and `key_table` (absolute offsets; 0xFFFFFFFF none).
Bytes Doc(const Bytes &body, uint32_t root, uint32_t key_table = 0xFFFFFFFFu, bool wide = false) {
	Bytes b = {0x10, static_cast<uint8_t>(wide ? 1 : 0)};
	b.insert(b.end(), body.begin(), body.end());
	b.push_back(0x08);
	U32(b, 0);
	U32(b, root);
	U32(b, key_table);
	b.insert(b.end(), {0, 0, 0});
	return b;
}

std::string Text(const Bytes &doc, std::string *why_out = nullptr) {
	std::string out, why;
	if (!RenderJsonText(doc.data(), doc.size(), out, why)) {
		if (why_out) {
			*why_out = why;
		}
		return "<refused: " + why + ">";
	}
	return out;
}

bool Refused(const Bytes &doc, const std::string &part) {
	std::string why;
	Text(doc, &why);
	return !why.empty() && why.find(part) != std::string::npos;
}

// [1, true, null, "", "ab", -1]: an array node at offset 2 and a string item after it.
Bytes SmallArray() {
	Bytes body;
	// offset 2: array, count 6, flag 0, then six values.
	body.push_back(0x02);
	U16(body, 6);
	body.push_back(0);
	U32(body, INT(1));
	U32(body, CONST_TRUE);
	U32(body, CONST_NULL);
	U32(body, CONST_EMPTY_STRING);
	U32(body, 30);	// the string item below: 2 + 4 + 24
	U32(body, INT(-1));
	// offset 30: string "ab" (length 2: 0x05, one byte).
	body.insert(body.end(), {0x03, 0x05, 'a', 'b'});
	return Doc(body, 2);
}

// {"k":1,"x":"\"q\""}: two keys, an object and a key table.
Bytes SmallObject(bool deleted_second = false, uint8_t spare = 0) {
	Bytes body;
	// offset 2: key "k"; 5: key "x"; 8: string with a quote and a backslash.
	body.insert(body.end(), {0x07, 0x03, 'k', 0x07, 0x03, 'x', 0x03, 0x07, '"', 'q', '\\'});
	const uint32_t object_at = 2 + static_cast<uint32_t>(body.size());
	body.push_back(0x01);
	U16(body, 2);
	body.push_back(spare);
	U16(body, 0);
	U16(body, deleted_second ? 0x8001 : 1);
	for (uint8_t i = 0; i < spare; i++) {
		U16(body, 0xFFFF);
	}
	U32(body, INT(1));
	U32(body, 8);
	for (uint8_t i = 0; i < spare; i++) {
		U32(body, 0xDEADBEEF);
	}
	const uint32_t keys_at = 2 + static_cast<uint32_t>(body.size());
	body.push_back(0x06);
	U16(body, 2);
	body.push_back(0);
	U32(body, 0x11111111);	// the hashes: not read
	U32(body, 0x22222222);
	U32(body, 2);
	U32(body, 5);
	return Doc(body, object_at, keys_at);
}

void TestRender() {
	std::cout << "[1] documents rendered as the server writes them" << std::endl;
	Check(Text(SmallArray()) == "[1,true,null,\"\",\"ab\",-1]", "an array of each scalar: " + Text(SmallArray()));
	Check(Text(SmallObject()) == "{\"k\":1,\"x\":\"\\\"q\\\\\"}",
		  "an object, a string escaped: " + Text(SmallObject()));
	Check(Text(SmallObject(true)) == "{\"k\":1}", "a member deleted in place left out");
	Check(Text(SmallObject(false, 3)) == Text(SmallObject()), "a node's flag byte: its spare slots, skipped (E9b)");
	// Wide: the same object with u32 counts and key references.
	Bytes body = {0x07, 0x03, 'w'};
	const uint32_t object_at = 2 + static_cast<uint32_t>(body.size());
	body.push_back(0x01);
	U32(body, 1);
	body.push_back(0);
	U32(body, 0);  // key reference, u32
	U32(body, INT(7));
	const uint32_t keys_at = 2 + static_cast<uint32_t>(body.size());
	body.push_back(0x06);
	U32(body, 1);
	body.push_back(0);
	U32(body, 0);
	U32(body, 2);
	Check(Text(Doc(body, object_at, keys_at, true)) == "{\"w\":7}", "a wide document: u32 counts and references");
}

void TestNumbers() {
	std::cout << "[2] numbers" << std::endl;
	JsonDocument::Decimal d;
	auto text = [&](uint64_t low, uint64_t high, uint8_t scale, bool negative) {
		d.low = low;
		d.high = high;
		d.scale = scale;
		d.precision = 38;
		d.negative = negative;
		std::string s;
		AppendDecimalText(d, s);
		return s;
	};
	Check(text(250, 0, 2, false) == "2.50", "2.50 keeps its scale");
	Check(text(5, 0, 1, true) == "-0.5", "-0.5");
	Check(text(0, 0, 10, false) == "0.0000000000", "zero at scale 10");
	Check(text(1, 0, 10, false) == "0.0000000001", "a mantissa shorter than its scale");
	// 10^38 - 1: the largest 38-digit mantissa.
	const uint64_t high = 0x4B3B4CA85A86C47AULL, low = 0x098A223FFFFFFFFFULL;
	Check(text(low, high, 0, false) == std::string(38, '9'), "38 nines: " + text(low, high, 0, false));
	Check(text(low, high, 38, true) == "-0." + std::string(38, '9'), "38 nines at scale 38");
	// An int64 item and a decimal item in an array.
	Bytes body;
	body.push_back(0x02);
	U16(body, 2);
	body.push_back(0);
	U32(body, 14);
	U32(body, 23);
	body.push_back(0x04);  // offset 14: int64 -2^63
	U32(body, 0);
	U32(body, 0x80000000u);
	body.insert(body.end(), {0x05, 0x07, 0x05, 0x02, 0x01, 0x39, 0x30, 0x00, 0x00});  // offset 23: 123.45
	Check(Text(Doc(body, 2)) == "[-9223372036854775808,123.45]", "an int64 and a decimal item: " + Text(Doc(body, 2)));
	Bytes wide_mantissa = body;
	wide_mantissa[23 - 2 + 1] = 0x13 + 2;  // length 10: an 18-byte mantissa
	Check(Refused(Doc(wide_mantissa, 2), "mantissa"), "a decimal mantissa past 16 bytes refused");
}

void TestRefusals() {
	std::cout << "[3] what the decoder refuses, by name" << std::endl;
	const Bytes good = SmallArray();
	size_t prefixes_refused = 0;
	for (size_t cut = 0; cut < good.size(); cut++) {
		Bytes prefix(good.begin(), good.begin() + static_cast<long>(cut));
		std::string out, why;
		if (!RenderJsonText(prefix.data(), prefix.size(), out, why)) {
			prefixes_refused++;
		}
	}
	Check(prefixes_refused == good.size(), "every prefix of a document refused, none misread");
	Bytes header = good;
	header[1] = 0x02;
	Check(Refused(header, "header"), "a header bit beyond bit 0");
	Bytes trailer = good;
	trailer[trailer.size() - 16] = 0x09;
	Check(Refused(trailer, "trailer"), "a trailer of another form");
	Check(Refused(Doc({}, CONST_NULL), "root"), "a root that is no object or array");
	Bytes constant;
	constant.push_back(0x02);
	U16(constant, 1);
	constant.push_back(0);
	U32(constant, 0xC0000003u);
	Check(Refused(Doc(constant, 2), "constant"), "a constant E9 did not see");
	Bytes tag;
	tag.push_back(0x02);
	U16(tag, 1);
	tag.push_back(0);
	U32(tag, 10);
	tag.push_back(0x09);
	Check(Refused(Doc(tag, 2), "tag"), "an item tag E9 did not see");
	// A cycle: an array whose only element is itself.
	Bytes cycle;
	cycle.push_back(0x02);
	U16(cycle, 1);
	cycle.push_back(0);
	U32(cycle, 2);
	Check(Refused(Doc(cycle, 2), "shared or cyclic"), "a cycle refused by the walk's budget");
	// Two slots naming their own node: 2^128 values at depth 128 without the budget (review of PR 6).
	Bytes fork;
	fork.push_back(0x02);
	U16(fork, 2);
	fork.push_back(0);
	U32(fork, 2);
	U32(fork, 2);
	Check(Refused(Doc(fork, 2), "shared or cyclic"), "a node named by two slots of its own refused at once");
	// Nesting: 128 levels read, 129 refused.
	auto nested = [](uint32_t levels) {
		Bytes body;
		for (uint32_t i = 0; i < levels; i++) {
			body.push_back(0x02);
			U16(body, 1);
			body.push_back(0);
			U32(body, i + 1 < levels ? 2 + 8 * (i + 1) : INT(1));
		}
		return Doc(body, 2);
	};
	Check(Text(nested(128)) == std::string(128, '[') + "1" + std::string(128, ']'), "nesting 128 read");
	Check(Refused(nested(129), "nested past 128"), "nesting 129 refused");
	// An object in a document with no key table.
	Bytes no_keys;
	no_keys.push_back(0x01);
	U16(no_keys, 1);
	no_keys.push_back(0);
	U16(no_keys, 0);
	U32(no_keys, INT(1));
	Check(Refused(Doc(no_keys, 2), "no key table"), "an object's member with no key table");
}

void TestKeys() {
	std::cout << "[4] a stored key unescaped as a JSON parser reads it" << std::endl;
	auto unescape = [](const std::string &key, std::string &out) {
		return UnescapeJsonKey(reinterpret_cast<const uint8_t *>(key.data()), static_cast<uint32_t>(key.size()), out);
	};
	std::string out;
	Check(unescape("a\\\"b", out) && out == "a\"b", "\\\" -> \"");
	Check(unescape("c\\\\d\\/e\\n", out) && out == "c\\d/e\n", "\\\\, \\/, \\n");
	Check(unescape("\\u00e9\\u00E9", out) && out == "\xC3\xA9\xC3\xA9", "\\u00e9 either case -> é");
	Check(unescape("\\uD83D\\uDE00", out) && out == "\xF0\x9F\x98\x80", "a surrogate pair -> one code point");
	Check(!unescape("\\uD83D", out), "a lone high surrogate refused");
	Check(!unescape("\\uDE00", out), "a lone low surrogate refused");
	Check(!unescape("\\x", out), "an unknown escape refused");
	Check(!unescape("a\\", out), "a trailing backslash refused");
}

}  // namespace

int main() {
	TestRender();
	TestNumbers();
	TestRefusals();
	TestKeys();
	if (failures != 0) {
		std::cout << failures << " check(s) FAILED" << std::endl;
		return 1;
	}
	std::cout << "all checks passed" << std::endl;
	return 0;
}
