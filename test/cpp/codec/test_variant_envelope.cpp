// Spec 093 T1: the sql_variant envelope as SQL Server sends it (experiment S1,
// specs/093-type-kernels/measured/S1.md), split and written as DuckDB VARIANT payloads. Every S1 row's
// tag, the payload bytes of the cases that carry a rule (D3's table: decimal widths and signs, money,
// time(7) in nanoseconds, datetime2(7) past 2262 in microseconds, a datetimeoffset's UTC instant, the
// GUID's byte order, char / nchar trimmed, code pages 1251 and 932, UTF-8, a surrogate pair), and the
// malformed envelopes refused by name. DuckDB-free, as the kernel is.

#include "codec/variant/variant_envelope.hpp"

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

using namespace duckdb::mssql::codec::variant;

namespace {

int failures = 0;

void Check(bool ok, const std::string &what) {
	if (!ok) {
		std::cout << "  FAIL " << what << std::endl;
		failures++;
	}
}

std::vector<uint8_t> Hex(const std::string &hex) {
	std::vector<uint8_t> out;
	for (size_t i = 0; i + 1 < hex.size(); i += 2) {
		out.push_back(static_cast<uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
	}
	return out;
}

std::string ToHex(const std::string &bytes) {
	static const char *digits = "0123456789abcdef";
	std::string out;
	for (unsigned char c : bytes) {
		out.push_back(digits[c >> 4]);
		out.push_back(digits[c & 15]);
	}
	return out;
}

std::string LeHex(uint64_t v, size_t bytes) {
	std::string raw;
	for (size_t i = 0; i < bytes; i++) {
		raw.push_back(static_cast<char>(v >> (8 * i)));
	}
	return ToHex(raw);
}

struct Row {
	int id;
	uint8_t base_type;
	const char *properties;
	const char *value;
	VariantTag tag;
};

// S1's rows: id, base type, properties, value (as measured), the tag D3 gives.
const Row ROWS[] = {
	{1, 0x32, "", "01", VariantTag::BOOL_TRUE},
	{2, 0x32, "", "00", VariantTag::BOOL_FALSE},
	{3, 0x30, "", "ff", VariantTag::UINT8},
	{4, 0x34, "", "0080", VariantTag::INT16},
	{5, 0x38, "", "00000080", VariantTag::INT32},
	{6, 0x7F, "", "0000000000000080", VariantTag::INT64},
	{7, 0x3B, "", "0000c03f", VariantTag::FLOAT},
	{8, 0x3E, "", "039300aa4bdd3dfe", VariantTag::DOUBLE},
	{9, 0x6A, "0101", "0105000000000000000000000000000000", VariantTag::DECIMAL},
	{10, 0x6A, "0902", "0015cd5b07000000000000000000000000", VariantTag::DECIMAL},
	{11, 0x6A, "1200", "014ef330a64b9bb6010000000000000000", VariantTag::DECIMAL},
	{12, 0x6A, "1309", "011581e97df41022110000000000000000", VariantTag::DECIMAL},
	{13, 0x6A, "1c08", "004ef338be917a796deb35fd0300000000", VariantTag::DECIMAL},
	{14, 0x6A, "2600", "01ffffffff3f228a097ac4865aa84c3b4b", VariantTag::DECIMAL},
	{15, 0x6A, "2626", "0001000000000000000000000000000000", VariantTag::DECIMAL},
	{16, 0x6C, "0a03", "01d4300000000000000000000000000000", VariantTag::DECIMAL},
	{17, 0x3C, "", "0000008000000000", VariantTag::DECIMAL},
	{18, 0x7A, "", "ffffff7f", VariantTag::DECIMAL},
	{19, 0x28, "", "000000", VariantTag::DATE},
	{20, 0x28, "", "dab937", VariantTag::DATE},
	{21, 0x29, "00", "7f5101", VariantTag::TIME_MICROS},
	{22, 0x29, "03", "7b512502", VariantTag::TIME_MICROS},
	{23, 0x29, "06", "404ac66108", VariantTag::TIME_MICROS},
	{24, 0x29, "07", "87e6bed153", VariantTag::TIME_NANOS},
	{25, 0x2A, "00", "a08c0020490b", VariantTag::TIMESTAMP_SEC},
	{26, 0x2A, "03", "7b51250220490b", VariantTag::TIMESTAMP_MILIS},
	{27, 0x2A, "06", "404ac6610820490b", VariantTag::TIMESTAMP_MICROS},
	{28, 0x2A, "07", "87e6bed15320490b", VariantTag::TIMESTAMP_NANOS},
	{29, 0x2A, "07", "ffbf692ac9dab937", VariantTag::TIMESTAMP_MICROS},
	{30, 0x2A, "07", "0000000000000000", VariantTag::TIMESTAMP_MICROS},
	{31, 0x3D, "", "01b5000071987500", VariantTag::TIMESTAMP_MICROS},
	{32, 0x3A, "", "01b5ac01", VariantTag::TIMESTAMP_MICROS},
	{33, 0x2B, "00", "4019011f490b4803", VariantTag::TIMESTAMP_MICROS_TZ},
	{34, 0x2B, "06", "40e201000021490bb8fc", VariantTag::TIMESTAMP_MICROS_TZ},
	{35, 0x2B, "07", "87aa06b82520490b4a01", VariantTag::TIMESTAMP_NANOS_TZ},
	{36, 0x2B, "07", "ffbf692ac9dab9370000", VariantTag::TIMESTAMP_MICROS_TZ},
	{37, 0x24, "", "ff19966f868b11d0b42d00c04fc964ff", VariantTag::UUID},
	{38, 0xAD, "0400", "01020000", VariantTag::BLOB},
	{39, 0xA5, "0a00", "010203", VariantTag::BLOB},
	{40, 0xAF, "0904d000340500", "6162202020", VariantTag::VARCHAR},
	{41, 0xA7, "0904d000340a00", "636166e9", VariantTag::VARCHAR},
	{42, 0xA7, "1904d000000a00", "e4eeec", VariantTag::VARCHAR},
	{43, 0xA7, "1104d000000a00", "8abf8e9a", VariantTag::VARCHAR},
	{44, 0xA7, "0904d024001400", "f09f9880c3a9", VariantTag::VARCHAR},
	{45, 0xEF, "0904d000340a00", "61006200200020002000", VariantTag::VARCHAR},
	{46, 0xE7, "0904d020001400", "3dd800de7800", VariantTag::VARCHAR},
	{47, 0xAF, "1904d000000600", "e4eeec202020", VariantTag::VARCHAR},
	{49, 0xA7, "0904d000340500", "", VariantTag::VARCHAR},
	{50, 0xA5, "0500", "", VariantTag::BLOB},
};

std::vector<uint8_t> Envelope(uint8_t base_type, const std::string &properties, const std::string &value) {
	std::vector<uint8_t> out = {base_type, static_cast<uint8_t>(properties.size() / 2)};
	for (auto b : Hex(properties)) {
		out.push_back(b);
	}
	for (auto b : Hex(value)) {
		out.push_back(b);
	}
	return out;
}

bool Encode(const std::vector<uint8_t> &envelope, std::string &blob, VariantTag &tag, EncodeNote &note,
			std::string &why) {
	WireVariant v;
	return SplitTdsVariant(envelope.data(), envelope.size(), v, why) && EncodeVariant(v, blob, tag, note, why);
}

std::string PayloadOf(int id) {
	for (const auto &r : ROWS) {
		if (r.id == id) {
			std::string blob, why;
			VariantTag tag;
			EncodeNote note;
			Encode(Envelope(r.base_type, r.properties, r.value), blob, tag, note, why);
			return ToHex(blob);
		}
	}
	return "missing";
}

std::string Varchar(const std::string &utf8) {
	return LeHex(utf8.size(), 1) + ToHex(utf8);
}

}  // namespace

int main() {
	std::cout << "sql_variant envelope -> VARIANT (spec 093 T1, experiment S1)" << std::endl;
	for (const auto &r : ROWS) {
		std::string blob, why;
		VariantTag tag = VariantTag::VARIANT_NULL;
		EncodeNote note;
		const bool ok = Encode(Envelope(r.base_type, r.properties, r.value), blob, tag, note, why);
		Check(ok && tag == r.tag && !note.unknown_collation,
			  "row " + std::to_string(r.id) + ": tag " + std::to_string(static_cast<int>(tag)) + " " + why);
	}

	// Integers and floats: the value's own bytes.
	Check(PayloadOf(5) == "00000080", "int -2147483648");
	Check(PayloadOf(1) == "", "bit: no payload");
	// decimal(p,s): varint width, varint scale, then int16 / int32 / int64 / hugeint by width.
	Check(PayloadOf(9) == "0101" + LeHex(5, 2), "decimal(1,1) 0.5: int16 5");
	Check(PayloadOf(10) == "0902" + LeHex(static_cast<uint32_t>(-123456789), 4), "decimal(9,2) negative: int32");
	Check(PayloadOf(11) == "1200" + LeHex(123456789012345678ULL, 8), "decimal(18,0): int64");
	Check(PayloadOf(14) == "2600" + std::string("ffffffff3f228a097ac4865aa84c3b4b"),
		  "decimal(38,0) 10^38-1: hugeint, lower then upper");
	{
		// decimal(28,8) negative: the hugeint is the two's complement of the mantissa
		const std::string p = PayloadOf(13);
		const auto bytes = Hex(p.substr(4));
		uint64_t lo = 0, hi = 0;
		for (int i = 7; i >= 0; i--) {
			lo = (lo << 8) | bytes[i];
			hi = (hi << 8) | bytes[8 + i];
		}
		const uint64_t neg_lo = ~lo + 1, neg_hi = ~hi + (neg_lo == 0 ? 1 : 0);
		Check(p.substr(0, 4) == "1c08" && neg_lo == 0x6d797a91be38f34eULL && neg_hi == 0x03fd35ebULL && (hi >> 63),
			  "decimal(28,8) negative: two's complement hugeint");
	}
	// money: DECIMAL(19,4) as a hugeint; smallmoney DECIMAL(10,4) as an int64
	Check(PayloadOf(17) == "1304" + LeHex(0x8000000000000000ULL, 8) + LeHex(~0ULL, 8), "money minimum");
	Check(PayloadOf(18) == "0a04" + LeHex(2147483647, 8), "smallmoney maximum");
	// date: days since 1970
	Check(PayloadOf(19) == LeHex(static_cast<uint32_t>(-719162), 4), "date 0001-01-01");
	// time(7) in nanoseconds, time(3) in microseconds
	Check(PayloadOf(24) == LeHex(36000123456700ULL, 8), "time(7) 10:00:00.1234567: nanoseconds");
	Check(PayloadOf(22) == LeHex(36000123000ULL, 8), "time(3): microseconds");
	// datetime2: seconds (0), milliseconds (3), microseconds (6), nanoseconds (7) or past 2262 microseconds
	Check(PayloadOf(25) == LeHex(1767261600ULL, 8), "datetime2(0): seconds");
	Check(PayloadOf(26) == LeHex(1767261600123ULL, 8), "datetime2(3): milliseconds");
	Check(PayloadOf(28) == LeHex(1767261600123456700ULL, 8), "datetime2(7) 2026: nanoseconds");
	Check(PayloadOf(29) == LeHex(253402300799999999ULL, 8), "datetime2(7) 9999-12-31: microseconds, truncated");
	// datetime: 1/300 s ticks read as the column reads them
	Check(PayloadOf(31) == LeHex(1794553689123333ULL, 8), "datetime 2026-11-13 07:08:09.123");
	// datetimeoffset: the UTC instant
	Check(PayloadOf(33) == LeHex(1767211200000000ULL, 8), "datetimeoffset(0) +14:00: UTC microseconds");
	Check(PayloadOf(35) == LeHex(1767241800123456700ULL, 8), "datetimeoffset(7) +05:30: UTC nanoseconds");
	Check(PayloadOf(36) == LeHex(253402300799999999ULL, 8), "datetimeoffset(7) 9999-12-31: microseconds");
	// uniqueidentifier: canonical big-endian, the top bit flipped (DuckDB's UUID)
	Check(PayloadOf(37) == LeHex(0xB42D00C04FC964FFULL, 8) + LeHex(0xEF9619FF8B86D011ULL, 8), "uniqueidentifier");
	// binary as stored (padded); strings by their collation, char / nchar trimmed
	Check(PayloadOf(38) == "0401020000", "binary(4): padded");
	Check(PayloadOf(40) == Varchar("ab"), "char(5): trimmed");
	Check(PayloadOf(41) == Varchar("caf\xc3\xa9"), "varchar 1252");
	Check(PayloadOf(42) == Varchar("\xd0\xb4\xd0\xbe\xd0\xbc"), "varchar 1251");
	Check(PayloadOf(43) == Varchar("\xe6\xbc\xa2\xe5\xad\x97"), "varchar 932");
	Check(PayloadOf(44) == Varchar("\xf0\x9f\x98\x80\xc3\xa9"), "varchar UTF-8: as is");
	Check(PayloadOf(45) == Varchar("ab"), "nchar(5): trimmed");
	Check(PayloadOf(46) == Varchar("\xf0\x9f\x98\x80x"), "nvarchar: a surrogate pair");
	Check(PayloadOf(47) == Varchar("\xd0\xb4\xd0\xbe\xd0\xbc"), "char(6) 1251: trimmed");
	Check(PayloadOf(49) == "00" && PayloadOf(50) == "00", "empty varchar / varbinary: a value of 0 bytes");

	// An empty nvarchar; a decimal in the column form (a 4-byte mantissa), as another server may send it.
	{
		std::string blob, why;
		VariantTag tag;
		EncodeNote note;
		Check(Encode(Envelope(0xE7, "0904d020001400", ""), blob, tag, note, why) && tag == VariantTag::VARCHAR &&
				  ToHex(blob) == "00",
			  "empty nvarchar");
		blob.clear();
		Check(Encode(Envelope(0x6A, "0902", "0015cd5b07"), blob, tag, note, why) &&
				  ToHex(blob) == "0902" + LeHex(static_cast<uint32_t>(-123456789), 4),
			  "decimal(9,2) with a 4-byte mantissa");
	}
	// An unpaired surrogate reads as U+FFFD, as the column kernel reads it.
	{
		std::string blob, why;
		VariantTag tag;
		EncodeNote note;
		Check(Encode(Envelope(0xE7, "0904d020001400", "3dd87800"), blob, tag, note, why) &&
				  ToHex(blob) == Varchar("\xef\xbf\xbdx"),
			  "nvarchar: an unpaired surrogate as U+FFFD");
	}
	// A collation this build has no table for: the bytes, a BLOB, and a note for the warning.
	{
		std::string blob, why;
		VariantTag tag;
		EncodeNote note;
		Check(Encode(Envelope(0xA7, "ffff0f00000a00", "6162"), blob, tag, note, why) && tag == VariantTag::BLOB &&
				  note.unknown_collation && ToHex(blob) == "026162",
			  "an unknown collation: the bytes as a BLOB, noted");
	}
	// UTF-8 collation, bytes that are not UTF-8: the bytes as a BLOB, noted.
	{
		std::string blob, why;
		VariantTag tag;
		EncodeNote note;
		Check(Encode(Envelope(0xA7, "0904d024001400", "61ff"), blob, tag, note, why) && tag == VariantTag::BLOB &&
				  note.invalid_utf8 && ToHex(blob) == "0261ff",
			  "UTF-8 collation, invalid bytes: a BLOB, noted");
	}
	// Malformed envelopes, refused by name.
	{
		const std::vector<std::pair<std::vector<uint8_t>, std::string>> bad = {
			{{0x38}, "shorter than its header"},
			{Envelope(0x99, "", "00"), "cannot hold"},
			{Envelope(0x38, "00", "00000000"), "property bytes"},
			{Envelope(0x38, "", "000000"), "bytes where its base type has 4"},
			{Envelope(0x29, "08", "0000000000"), "scale 8"},
			{Envelope(0x2A, "07", "000000000000"), "bytes where its base type has 8"},
			{Envelope(0x6A, "0100", "010a000000000000000000000000000000"), "past its precision"},
			{Envelope(0x6A, "0900", "01000000"), "not a sign and 4, 8, 12 or 16"},
			{Envelope(0x6A, "0900", "0100000000000000"), "not a sign and 4, 8, 12 or 16"},
			{Envelope(0x6A, "2700", "0100000000000000000000000000000000"), "decimal(39,0)"},
			{Envelope(0x6A, "0203", "0100000000000000000000000000000000"), "decimal(2,3)"},
			{Envelope(0xE7, "0904d020001400", "616263"), "odd"},
			{Envelope(0x29, "07", "00c0692ac9"), "past 24:00"},
			{Envelope(0x3D, "", "22ccffad00000000"), "outside 1753-9999"},
			{Envelope(0x3D, "", "0000000000ff8b01"), "outside 1753-9999"},
		};
		for (const auto &b : bad) {
			std::string blob, why;
			VariantTag tag;
			EncodeNote note;
			const bool ok = Encode(b.first, blob, tag, note, why);
			Check(!ok && why.find(b.second) != std::string::npos, "refused: " + b.second + " (got: " + why + ")");
		}
	}
	std::cout << (failures ? "FAILED: " + std::to_string(failures) + " check(s)" : "all checks passed") << std::endl;
	return failures ? 1 : 0;
}
