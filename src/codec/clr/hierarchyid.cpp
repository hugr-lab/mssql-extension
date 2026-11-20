// Spec 093 T4 (D5): a hierarchyid's MS-SSCLRT 2.2.2 bit string as its ToString() text, and back.
// DuckDB-free (D1).

#include "codec/clr/hierarchyid.hpp"

#include <cstring>

namespace duckdb {
namespace mssql {
namespace codec {
namespace hierarchyid {

namespace {

//! One label range: the prefix, then the rest of the label -- `x` a value bit (most significant first),
//! `0` / `1` a fixed bit, `T` the level bit -- and the values it holds. Thirteen ranges, prefix-free;
//! each one's value bits number exactly log2 of its size (2, 2, 3, 6, 10, 12, 32 and 48 above zero; 3,
//! 6, 12, 32 and 48 below it).
struct Pattern {
	const char *prefix;
	const char *rest;
	int64_t min;
	int64_t max;
};

const Pattern PATTERNS[] = {
	{"01", "xxT", 0, 3},
	{"100", "xxT", 4, 7},
	{"101", "xxxT", 8, 15},
	{"110", "xx0x1xxxT", 16, 79},
	{"1110", "xxx0xxx0x1xxxT", 80, 1103},
	{"11110", "xxxxx0xxx0x1xxxT", 1104, 5199},
	{"111110", "xxxxxxxxxxxxxxxxxxx0xxxxxx0xxx0x1xxxT", 5200, 4294972495LL},
	{"111111", "xxxxxxxxxxxxxx0xxxxxxxxxxxxxxxxxxxxx0xxxxxx0xxx0x1xxxT", 4294972496LL, 281479271683151LL},
	{"00111", "xxxT", -8, -1},
	{"0010", "xx0x1xxxT", -72, -9},
	{"000110", "xxxxx0xxx0x1xxxT", -4168, -73},
	{"000101", "xxxxxxxxxxxxxxxxxxx0xxxxxx0xxx0x1xxxT", -4294971464LL, -4169},
	{"000100", "xxxxxxxxxxxxxx0xxxxxxxxxxxxxxxxxxxxx0xxxxxx0xxx0x1xxxT", -281479271682120LL, -4294971465LL},
};

class BitReader {
public:
	BitReader(const uint8_t *data, size_t size) : data_(data), bits_(size * 8) {}
	size_t Remaining() const {
		return bits_ - pos_;
	}
	int Peek(size_t offset) const {
		const size_t at = pos_ + offset;
		return (data_[at >> 3] >> (7 - (at & 7))) & 1;
	}
	int Next() {
		const int bit = Peek(0);
		pos_++;
		return bit;
	}
	void Skip(size_t n) {
		pos_ += n;
	}
	//! Whether every bit left is zero (the padding after the last level).
	bool RestIsZero() const {
		for (size_t i = 0; i < Remaining(); i++) {
			if (Peek(i)) {
				return false;
			}
		}
		return true;
	}

private:
	const uint8_t *data_;
	size_t bits_;
	size_t pos_ = 0;
};

class BitWriter {
public:
	explicit BitWriter(std::vector<uint8_t> &out) : out_(out) {}
	void Put(int bit) {
		if ((count_ & 7) == 0) {
			out_.push_back(0);
		}
		if (bit) {
			out_.back() |= static_cast<uint8_t>(0x80 >> (count_ & 7));
		}
		count_++;
	}

private:
	std::vector<uint8_t> &out_;
	size_t count_ = 0;
};

//! The range whose prefix the bits at the reader start with, or null.
const Pattern *MatchPrefix(const BitReader &in) {
	for (const auto &pattern : PATTERNS) {
		const size_t length = std::strlen(pattern.prefix);
		if (length > in.Remaining()) {
			continue;
		}
		bool match = true;
		for (size_t i = 0; i < length && match; i++) {
			match = in.Peek(i) == pattern.prefix[i] - '0';
		}
		if (match) {
			return &pattern;
		}
	}
	return nullptr;
}

const Pattern *RangeOf(int64_t value) {
	for (const auto &pattern : PATTERNS) {
		if (value >= pattern.min && value <= pattern.max) {
			return &pattern;
		}
	}
	return nullptr;
}

//! An integer of `text[begin, end)`: an optional '-', then digits; false otherwise or past int64's range.
bool ParseInteger(const std::string &text, size_t begin, size_t end, int64_t &value) {
	bool negative = false;
	if (begin < end && text[begin] == '-') {
		negative = true;
		begin++;
	}
	if (begin == end || end - begin > 18) {
		return false;  // 18 digits hold every label (the largest has 15)
	}
	int64_t magnitude = 0;
	for (size_t i = begin; i < end; i++) {
		if (text[i] < '0' || text[i] > '9') {
			return false;
		}
		magnitude = magnitude * 10 + (text[i] - '0');
	}
	value = negative ? -magnitude : magnitude;
	return true;
}

}  // namespace

const char *DescribeStatus(HierarchyIdStatus status) {
	switch (status) {
	case HierarchyIdStatus::Ok:
		return "valid";
	case HierarchyIdStatus::TooLong:
		return "it is longer than a hierarchyid's 892 bytes";
	case HierarchyIdStatus::UnknownPrefix:
		return "a label starts with bits no label range has";
	case HierarchyIdStatus::BadFixedBit:
		return "a label's fixed bit is not its pattern's";
	case HierarchyIdStatus::Truncated:
		return "its bits end inside a label or a level";
	case HierarchyIdStatus::BadPadding:
		return "its padding is not zero bits shorter than a byte";
	case HierarchyIdStatus::TextSyntax:
		return "the text is not a path of /-separated levels of integers";
	case HierarchyIdStatus::OutOfRange:
		return "a label is past the range a hierarchyid holds";
	}
	return "invalid";
}

HierarchyIdStatus Decode(const uint8_t *data, size_t size, std::string &out) {
	if (size > MAX_BYTES) {
		return HierarchyIdStatus::TooLong;
	}
	out += '/';
	BitReader in(data, size);
	bool in_level = false;	// the last label read continued its level ('.')
	while (true) {
		// Every label has a 1 within its first four bits (its prefix), so only four zero bits -- or fewer
		// than four bits -- can be the end; the rest is checked then, once.
		const bool label_follows = in.Remaining() >= 4 && (in.Peek(0) | in.Peek(1) | in.Peek(2) | in.Peek(3)) != 0;
		if (!label_follows && in.RestIsZero()) {
			if (in_level) {
				return HierarchyIdStatus::Truncated;
			}
			// The padding: fewer than eight zero bits. Eight or more would be a byte the encoding never
			// writes; no label starts with four zero bits, so this is not a label.
			return in.Remaining() < 8 ? HierarchyIdStatus::Ok : HierarchyIdStatus::BadPadding;
		}
		const Pattern *pattern = MatchPrefix(in);
		if (!pattern) {
			return HierarchyIdStatus::UnknownPrefix;
		}
		in.Skip(std::strlen(pattern->prefix));
		const size_t rest = std::strlen(pattern->rest);
		if (rest > in.Remaining()) {
			return HierarchyIdStatus::Truncated;
		}
		uint64_t bits = 0;
		int level_end = 0;
		for (size_t i = 0; i < rest; i++) {
			const char c = pattern->rest[i];
			const int bit = in.Next();
			if (c == 'x') {
				bits = (bits << 1) | static_cast<uint64_t>(bit);
			} else if (c == 'T') {
				level_end = bit;
			} else if (bit != c - '0') {
				return HierarchyIdStatus::BadFixedBit;
			}
		}
		const int64_t stored = pattern->min + static_cast<int64_t>(bits);
		// A label that continues its level is stored as its value + 1.
		out += std::to_string(level_end ? stored : stored - 1);
		out += level_end ? '/' : '.';
		in_level = !level_end;
	}
}

HierarchyIdStatus Encode(const std::string &text, std::vector<uint8_t> &out) {
	out.clear();
	if (text.empty() || text.front() != '/' || text.back() != '/') {
		return HierarchyIdStatus::TextSyntax;
	}
	BitWriter bits(out);
	size_t at = 1;
	while (at < text.size()) {
		const size_t level_end = text.find('/', at);
		if (level_end == at) {
			return HierarchyIdStatus::TextSyntax;  // an empty level (`//`)
		}
		size_t label = at;
		while (label < level_end) {
			size_t label_end = text.find('.', label);
			const bool last = label_end == std::string::npos || label_end > level_end;
			if (last) {
				label_end = level_end;
			}
			int64_t value;
			if (!ParseInteger(text, label, label_end, value)) {
				return HierarchyIdStatus::TextSyntax;
			}
			const int64_t stored = last ? value : value + 1;
			const Pattern *pattern = RangeOf(stored);
			if (!pattern) {
				return HierarchyIdStatus::OutOfRange;
			}
			for (const char *p = pattern->prefix; *p; p++) {
				bits.Put(*p - '0');
			}
			const auto offset = static_cast<uint64_t>(stored - pattern->min);
			size_t value_bits = 0;
			for (const char *p = pattern->rest; *p; p++) {
				value_bits += *p == 'x';
			}
			size_t emitted = 0;
			for (const char *p = pattern->rest; *p; p++) {
				if (*p == 'x') {
					bits.Put(static_cast<int>((offset >> (value_bits - 1 - emitted)) & 1));
					emitted++;
				} else if (*p == 'T') {
					bits.Put(last ? 1 : 0);
				} else {
					bits.Put(*p - '0');
				}
			}
			if (last) {
				break;
			}
			label = label_end + 1;
			if (label >= level_end) {
				return HierarchyIdStatus::TextSyntax;  // a level ending in '.'
			}
		}
		at = level_end + 1;
	}
	return out.size() <= MAX_BYTES ? HierarchyIdStatus::Ok : HierarchyIdStatus::TooLong;
}

}  // namespace hierarchyid
}  // namespace codec
}  // namespace mssql
}  // namespace duckdb
