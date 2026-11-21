#include "backup/backup_cd_record.hpp"

#include <cstring>
#include <limits>

namespace duckdb {
namespace mssql {
namespace backup {

namespace {

std::string Where(const char *what, uint16_t offset) {
	return std::string("backup: the CD record of ") + what + " at offset " + std::to_string(offset);
}

}  // namespace

void CdRecord::BackPointer(uint32_t &page, uint16_t &file, uint16_t &slot) const {
	const uint8_t *p = data + back_pointer;
	page = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
		   (static_cast<uint32_t>(p[3]) << 24);
	file = static_cast<uint16_t>(p[4] | (p[5] << 8));
	slot = static_cast<uint16_t>(p[6] | (p[7] << 8));
}

void ReadCdRecord(const uint8_t *page, size_t page_size, size_t slot_array, uint16_t offset, const char *what,
				  CdRecord &out, bool padded) {
	if (offset < 96 || slot_array > page_size || offset >= slot_array) {
		throw FormatError(Where(what, offset) + " lies outside the page's records");
	}
	out.data = page + offset;
	out.offset = offset;
	out.limit = static_cast<uint16_t>(slot_array - offset);
	out.values.clear();
	out.column_count = 0;
	const uint8_t *d = out.data;
	const size_t limit = out.limit;
	auto need = [&](size_t bytes, const char *part) {
		if (bytes > limit) {
			throw FormatError(Where(what, offset) + ": its " + part + " runs past the page's slot array");
		}
	};
	need(1, "header");
	out.header = d[0];
	if (!(out.header & 0x01) || (out.header & 0xC0)) {
		throw FormatError(Where(what, offset) + " has header " + std::to_string(out.header) +
						  ", not a CD record the reader knows, in a compressed rowset");
	}
	const bool tagged = (out.header & CD_VERSION_TAG) != 0;
	const size_t tag = tagged ? CD_VERSION_TAG_BYTES : 0;
	const uint8_t type = (out.header >> 2) & 7;
	switch (type) {
	case CD_TYPE_STUB:
		// Not a CD record: the RID it forwards to (E22: header 0x09).
		need(9 + tag, "forwarding stub");
		out.kind = CdRecord::Kind::Stub;
		out.end = static_cast<uint16_t>(9 + tag);
		return;
	case CD_TYPE_GHOST_VERSION:
		// A heap row deleted under row versioning: the header and its version tag only (E22: 0x07).
		if (!tagged) {
			throw FormatError(Where(what, offset) + " is a version ghost with no version tag");
		}
		need(1 + tag, "version ghost");
		out.kind = CdRecord::Kind::Ghost;
		out.end = static_cast<uint16_t>(1 + tag < 9 ? 9 : 1 + tag);
		return;
	case CD_TYPE_GHOST:
		out.kind = CdRecord::Kind::Ghost;
		break;
	case CD_TYPE_PRIMARY:
		out.kind = CdRecord::Kind::Row;
		break;
	case CD_TYPE_FORWARDED:
	case CD_TYPE_GHOST_FORWARDED:
		out.kind = CdRecord::Kind::Forwarded;  // a ghost forwarded record is laid out as one: made a ghost below
		break;
	default:
		// Not seen (E22): it could be a row, so it is named rather than skipped.
		throw FormatError(Where(what, offset) + " is of record type " + std::to_string(type) +
						  ", which the reader does not know");
	}
	size_t pos = 1;
	need(pos + 1, "column count");
	size_t n = d[pos];
	if (n & 0x80) {
		need(pos + 2, "column count");
		n = ((n & 0x7F) << 8) | d[pos + 1];
		pos += 2;
	} else {
		pos += 1;
	}
	out.column_count = static_cast<uint16_t>(n);
	const size_t cd_bytes = (n + 1) / 2;
	need(pos + cd_bytes, "CD array");
	const uint8_t *cd = d + pos;
	pos += cd_bytes;
	const size_t clusters = (n + CD_CLUSTER_COLUMNS - 1) / CD_CLUSTER_COLUMNS;
	const size_t cluster_bytes = clusters > 1 ? clusters - 1 : 0;
	need(pos + cluster_bytes, "cluster array");
	const uint8_t *cluster_sizes = d + pos;
	pos += cluster_bytes;
	out.values.resize(n);
	size_t short_at = pos;
	size_t longs = 0;
	size_t cluster_start = short_at;
	for (size_t i = 0; i < n; i++) {
		CdValue &v = out.values[i];
		v.code = (i % 2 == 0) ? (cd[i / 2] & 0x0F) : (cd[i / 2] >> 4);
		if (v.code >= 2 && v.code <= 9) {
			v.offset = static_cast<uint16_t>(short_at);
			v.size = static_cast<uint16_t>(v.code - 1);
			short_at += v.size;
			need(short_at, "short region");
		} else if (v.code == CD_SYMBOL) {
			// A page-dictionary symbol (PAGE compression, E23): its one byte in the short region.
			v.offset = static_cast<uint16_t>(short_at);
			v.size = 1;
			short_at += 1;
			need(short_at, "short region");
		} else if (v.code == CD_LONG) {
			longs++;
		} else if (v.code > CD_SYMBOL) {
			throw FormatError(Where(what, offset) + ": column " + std::to_string(i + 1) + " has CD code " +
							  std::to_string(v.code) + ", which no value has");
		}
		// The cluster array says how many short bytes each full cluster holds: checked as a cluster ends.
		if ((i + 1) % CD_CLUSTER_COLUMNS == 0 && (i + 1) < n) {
			const size_t c = i / CD_CLUSTER_COLUMNS;
			if (short_at - cluster_start != cluster_sizes[c]) {
				throw FormatError(Where(what, offset) + ": its cluster " + std::to_string(c) + " holds " +
								  std::to_string(short_at - cluster_start) + " short bytes, its cluster array says " +
								  std::to_string(cluster_sizes[c]));
			}
			cluster_start = short_at;
		}
	}
	size_t end = short_at;
	if (out.header & 0x20) {
		need(end + 3, "long region");
		const size_t count = d[end + 1] | (d[end + 2] << 8);
		if (count != longs) {
			throw FormatError(Where(what, offset) + ": its long region lists " + std::to_string(count) +
							  " values, its CD array " + std::to_string(longs));
		}
		const size_t ends_at = end + 3;
		// With more than one cluster, a byte per cluster but the last follows the end offsets: how many
		// long values that cluster holds (E22).
		const size_t counts_at = ends_at + 2 * count;
		need(counts_at + cluster_bytes, "long region's offsets");
		std::vector<size_t> per_cluster(clusters, 0);
		for (size_t i = 0; i < n; i++) {
			if (out.values[i].code == CD_LONG) {
				per_cluster[i / CD_CLUSTER_COLUMNS]++;
			}
		}
		for (size_t c = 0; c < cluster_bytes; c++) {
			if (d[counts_at + c] != per_cluster[c]) {
				throw FormatError(Where(what, offset) + ": its cluster " + std::to_string(c) + " holds " +
								  std::to_string(per_cluster[c]) + " long values, its long region says " +
								  std::to_string(d[counts_at + c]));
			}
		}
		const size_t data_at = counts_at + cluster_bytes;
		size_t previous = 0;
		size_t k = 0;
		for (size_t i = 0; i < n; i++) {
			CdValue &v = out.values[i];
			if (v.code != CD_LONG) {
				continue;
			}
			const uint16_t raw = static_cast<uint16_t>(d[ends_at + 2 * k] | (d[ends_at + 2 * k + 1] << 8));
			const size_t stop = raw & 0x7FFF;
			if (stop < previous) {
				throw FormatError(Where(what, offset) + ": its long values' end offsets are not ascending");
			}
			need(data_at + stop, "long region");
			v.complex = (raw & 0x8000) != 0;
			v.offset = static_cast<uint16_t>(data_at + previous);
			v.size = static_cast<uint16_t>(stop - previous);
			previous = stop;
			k++;
		}
		end = data_at + previous;
	} else if (longs) {
		throw FormatError(Where(what, offset) + ": " + std::to_string(longs) + " long values and no long region");
	}
	if (out.kind == CdRecord::Kind::Forwarded) {
		out.back_pointer = static_cast<uint16_t>(end);	// after the data, before a version tag (E22)
		end += 8;
		need(end, "back pointer");
	}
	if (tagged) {
		end += tag;
		need(end, "version tag");
	}
	if (padded && end < 9) {
		end = 9;  // every row record is padded to 9 bytes (E22)
		need(end, "padding");
	}
	out.end = static_cast<uint16_t>(end);
	if (type == CD_TYPE_GHOST_FORWARDED) {
		out.kind = CdRecord::Kind::Ghost;  // no row: the row it was is deleted (E23)
	}
}

//===----------------------------------------------------------------------===//
// Values
//===----------------------------------------------------------------------===//

namespace {

//! A sign-flipped big-endian integer of `size` bytes (E22); 0 for none.
int64_t CdInteger(const uint8_t *p, size_t size) {
	if (size == 0) {
		return 0;
	}
	uint64_t v = 0;
	for (size_t i = 0; i < size; i++) {
		v = (v << 8) | p[i];
	}
	const unsigned bits = static_cast<unsigned>(8 * size);
	v ^= uint64_t(1) << (bits - 1);
	if (bits < 64 && (v >> (bits - 1)) & 1) {
		v |= ~uint64_t(0) << bits;	// sign-extend
	}
	return static_cast<int64_t>(v);
}

void PutLE(std::vector<uint8_t> &out, uint64_t v, size_t width) {
	for (size_t i = 0; i < width; i++) {
		out.push_back(static_cast<uint8_t>(v >> (8 * i)));
	}
}

//! An unsigned 128-bit integer, portable (MSVC has no __int128).
struct U128 {
	uint64_t hi = 0;
	uint64_t lo = 0;
	//! *this = *this * 10 + digit; false on overflow.
	bool MulAdd(unsigned digit) {
		const uint64_t lo_lo = (lo & 0xFFFFFFFFu) * 10u + digit;
		const uint64_t lo_hi = (lo >> 32) * 10u + (lo_lo >> 32);
		const uint64_t carry = lo_hi >> 32;
		if (hi > (std::numeric_limits<uint64_t>::max() - carry) / 10u) {
			return false;
		}
		hi = hi * 10u + carry;
		lo = (lo_hi << 32) | (lo_lo & 0xFFFFFFFFu);
		return true;
	}
	bool Less(const U128 &o) const {
		return hi < o.hi || (hi == o.hi && lo < o.lo);
	}
};

}  // namespace

namespace {

std::string Hex(const uint8_t *data, size_t size) {
	static const char *digits = "0123456789abcdef";
	std::string out = "0x";
	for (size_t i = 0; i < size; i++) {
		out += digits[data[i] >> 4];
		out += digits[data[i] & 15];
	}
	return out;
}

}  // namespace

void DecodeVardecimal(const uint8_t *data, size_t size, uint8_t precision, uint8_t scale, std::vector<uint8_t> &out,
					  const std::string &table, const std::string &column) {
	auto what = [&]() { return table + "." + column; };
	if (precision < 1 || precision > 38 || scale > precision) {
		throw FormatError("backup: " + what() + " is a decimal(" + std::to_string(precision) + ", " +
						  std::to_string(scale) + ")");
	}
	const size_t width = precision <= 9 ? 4 : precision <= 19 ? 8 : precision <= 28 ? 12 : 16;
	bool positive = true;
	std::string digits;
	int exponent = 0;
	if (size > 0) {
		positive = (data[0] & 0x80) != 0;
		exponent = static_cast<int>(data[0] & 0x7F) - 64;
		const size_t bits = 8 * (size - 1);
		for (size_t at = 0; at < bits; at += 10) {
			// Ten bits a group of three digits; the last group's trailing zero bits are not stored.
			unsigned group = 0;
			for (size_t b = 0; b < 10; b++) {
				const size_t bit = at + b;
				unsigned value = 0;
				if (bit < bits) {
					value = (data[1 + bit / 8] >> (7 - bit % 8)) & 1;
				}
				group = (group << 1) | value;
			}
			if (group > 999) {
				throw FormatError("backup: " + what() + " holds a vardecimal group of " + std::to_string(group));
			}
			digits += static_cast<char>('0' + group / 100);
			digits += static_cast<char>('0' + group / 10 % 10);
			digits += static_cast<char>('0' + group % 10);
		}
		while (!digits.empty() && digits.back() == '0') {
			digits.pop_back();	// trailing zeros change nothing; the value is digits x 10^(exponent - n + 1)
		}
	}
	// The unscaled value: digits x 10^shift, shift = exponent - n + 1 + scale.
	const int shift = digits.empty() ? 0 : exponent - static_cast<int>(digits.size()) + 1 + scale;
	if (shift < 0) {
		throw FormatError("backup: " + what() + " holds a vardecimal value (" + Hex(data, size) +
						  ") with more fraction digits than its scale " + std::to_string(scale));
	}
	U128 magnitude;
	U128 limit;
	for (int i = 0; i < precision; i++) {
		limit.MulAdd(i == 0 ? 1 : 0);
	}
	limit.MulAdd(0);  // 10^precision
	bool fits = true;
	for (char ch : digits) {
		fits = fits && magnitude.MulAdd(static_cast<unsigned>(ch - '0'));
	}
	for (int i = 0; i < shift && fits; i++) {
		fits = magnitude.MulAdd(0);
	}
	if (!fits || !magnitude.Less(limit)) {
		throw FormatError("backup: " + what() + " holds a vardecimal value (" + Hex(data, size) +
						  ") past its precision " + std::to_string(precision));
	}
	out.push_back(positive ? 1 : 0);
	PutLE(out, magnitude.lo, width < 8 ? width : 8);
	if (width > 8) {
		PutLE(out, magnitude.hi, width - 8);
	}
}

namespace {

const uint32_t STATIC_WINDOWS[8] = {0x0000, 0x0080, 0x0100, 0x0300, 0x2000, 0x2080, 0x2100, 0x3000};
const uint32_t DYNAMIC_WINDOWS[8] = {0x0080, 0x00C0, 0x0400, 0x0600, 0x0900, 0x3040, 0x30A0, 0xFF00};

template <class Name>
uint32_t WindowOffset(uint8_t x, const Name &what) {
	if (x >= 0x01 && x <= 0x67) {
		return static_cast<uint32_t>(x) * 0x80;
	}
	if (x >= 0x68 && x <= 0xA7) {
		return static_cast<uint32_t>(x) * 0x80 + 0xAC00;
	}
	switch (x) {
	case 0xF9:
		return 0x00C0;
	case 0xFA:
		return 0x0250;
	case 0xFB:
		return 0x0370;
	case 0xFC:
		return 0x0530;
	case 0xFD:
		return 0x3040;
	case 0xFE:
		return 0x30A0;
	case 0xFF:
		return 0xFF60;
	default:
		throw FormatError("backup: " + what() + " defines an SCSU window by reserved byte " + std::to_string(x));
	}
}

void PutUnit(std::vector<uint8_t> &out, uint32_t unit) {
	out.push_back(static_cast<uint8_t>(unit));
	out.push_back(static_cast<uint8_t>(unit >> 8));
}

//! A code point (a window can hold one above U+FFFF: as a surrogate pair).
void PutCodePoint(std::vector<uint8_t> &out, uint32_t cp) {
	if (cp > 0xFFFF) {
		cp -= 0x10000;
		PutUnit(out, 0xD800 | (cp >> 10));
		PutUnit(out, 0xDC00 | (cp & 0x3FF));
	} else {
		PutUnit(out, cp);
	}
}

}  // namespace

void DecodeCompressedUnicode(const uint8_t *data, size_t size, std::vector<uint8_t> &out, const std::string &table,
							 const std::string &column) {
	auto what = [&]() { return table + "." + column; };
	if (size % 2 == 0) {
		out.insert(out.end(), data, data + size);  // not compressed: UTF-16LE as it is (E22)
		return;
	}
	uint32_t windows[8];
	std::memcpy(windows, DYNAMIC_WINDOWS, sizeof(windows));
	size_t active = 0;
	bool unicode = false;
	size_t i = 0;
	auto byte = [&]() -> uint8_t {
		if (i >= size) {
			throw FormatError("backup: " + what() + " ends inside an SCSU sequence");
		}
		return data[i++];
	};
	while (i < size) {
		const uint8_t x = data[i++];
		if (!unicode) {
			if (x >= 0x80) {
				PutCodePoint(out, windows[active] + (x - 0x80));
			} else if (x == 0x00 || x == 0x09 || x == 0x0A || x == 0x0D || x >= 0x20) {
				PutUnit(out, x);
			} else if (x >= 0x01 && x <= 0x08) {  // SQn: one character quoted from window n
				const uint8_t y = byte();
				PutCodePoint(out, y < 0x80 ? STATIC_WINDOWS[x - 1] + y : windows[x - 1] + (y - 0x80));
			} else if (x == 0x0B) {	 // SDX: an extended window
				const uint8_t hi = byte(), lo = byte();
				active = hi >> 5;
				windows[active] = 0x10000 + ((((hi & 0x1Fu) << 8) | lo) << 7);
			} else if (x == 0x0E) {	 // SQU: one UTF-16 unit quoted
				const uint8_t hi = byte(), lo = byte();
				PutUnit(out, (static_cast<uint32_t>(hi) << 8) | lo);
			} else if (x == 0x0F) {	 // SCU: Unicode mode
				unicode = true;
			} else if (x >= 0x10 && x <= 0x17) {  // SCn
				active = x - 0x10;
			} else if (x >= 0x18 && x <= 0x1F) {  // SDn
				active = x - 0x18;
				windows[active] = WindowOffset(byte(), what);
			} else {
				throw FormatError("backup: " + what() + " holds the reserved SCSU tag " + std::to_string(x));
			}
		} else {
			if (x >= 0xE0 && x <= 0xE7) {  // UCn
				active = x - 0xE0;
				unicode = false;
			} else if (x >= 0xE8 && x <= 0xEF) {  // UDn
				active = x - 0xE8;
				windows[active] = WindowOffset(byte(), what);
				unicode = false;
			} else if (x == 0xF0) {	 // UQU
				const uint8_t hi = byte(), lo = byte();
				PutUnit(out, (static_cast<uint32_t>(hi) << 8) | lo);
			} else if (x == 0xF1) {	 // UDX
				const uint8_t hi = byte(), lo = byte();
				active = hi >> 5;
				windows[active] = 0x10000 + ((((hi & 0x1Fu) << 8) | lo) << 7);
				unicode = false;
			} else if (x == 0xF2) {
				throw FormatError("backup: " + what() + " holds the reserved SCSU tag 242");
			} else {
				const uint8_t lo = byte();
				PutUnit(out, (static_cast<uint32_t>(x) << 8) | lo);
			}
		}
	}
}

void ReadPageCompression(const uint8_t *page, size_t page_size, size_t slot_array, const char *what,
						 PageCompression &out) {
	constexpr size_t AT = 96;  // the page header's length: the record follows it
	out.present = true;
	out.has_anchor = false;
	out.dictionary.clear();
	out.page = page;
	auto fail = [&](const std::string &why) {
		throw FormatError(std::string("backup: ") + what + ": its compression-information record " + why);
	};
	if (slot_array > page_size || slot_array < AT + 5) {
		fail("has no room before the slot array");
	}
	const uint8_t *ci = page + AT;
	const size_t room = slot_array - AT;
	const uint8_t flags = ci[0];
	if (flags & ~0x06) {
		fail("has flags " + std::to_string(flags) + ", which the reader does not know");
	}
	const bool has_dictionary = (flags & 0x04) != 0;
	const size_t fields = has_dictionary ? 7 : 5;
	if (fields > room) {
		fail("runs past the page's slot array");
	}
	const size_t dictionary_at = has_dictionary ? static_cast<size_t>(ci[3] | (ci[4] << 8)) : 0;
	const size_t size =
		has_dictionary ? static_cast<size_t>(ci[5] | (ci[6] << 8)) : static_cast<size_t>(ci[3] | (ci[4] << 8));
	if (size < fields || size > room) {
		fail("of " + std::to_string(size) + " bytes runs past the page's records (" + std::to_string(room) + ")");
	}
	out.end = static_cast<uint16_t>(AT + size);
	const size_t anchor_end = has_dictionary ? dictionary_at : size;
	if (has_dictionary && (dictionary_at < fields || dictionary_at > size)) {
		fail("places its dictionary at " + std::to_string(dictionary_at) + ", outside it");
	}
	if (flags & 0x02) {
		ReadCdRecord(page, page_size, AT + anchor_end, static_cast<uint16_t>(AT + fields), what, out.anchor, false);
		if (out.anchor.kind != CdRecord::Kind::Row) {
			fail("holds an anchor record that is not a row");
		}
		out.has_anchor = true;
	}
	if (has_dictionary) {
		const uint8_t *d = ci + dictionary_at;
		const size_t length = size - dictionary_at;
		if (length < 2) {
			fail("has a dictionary with no entry count");
		}
		const size_t count = d[0] | (d[1] << 8);
		const size_t data_at = 2 + 2 * count;
		if (data_at > length) {
			fail("lists " + std::to_string(count) + " dictionary entries past its end");
		}
		size_t previous = data_at;
		for (size_t i = 0; i < count; i++) {
			const size_t stop = d[2 + 2 * i] | (d[3 + 2 * i] << 8);
			if (stop < previous || stop > length) {
				fail("has dictionary entry " + std::to_string(i) + " ending at " + std::to_string(stop) +
					 ", outside its dictionary");
			}
			out.dictionary.emplace_back(static_cast<uint16_t>(AT + dictionary_at + previous),
										static_cast<uint16_t>(stop - previous));
			previous = stop;
		}
	}
}

void CdRow::Store(const Staging &s, size_t bytes) {
	at_[s.k] = s.start;
	values_[s.k].size = static_cast<uint32_t>(bytes);
}

//! A PAGE value put together in rebuilt_, which the next column reuses, is kept in the row's buffer
//! instead; anything else is pointed at where it already lies.
void CdRow::StoreAsIs(const Staging &s) {
	if (s.rebuilt) {
		buffer_.insert(buffer_.end(), s.p, s.p + s.size);
		Store(s, s.size);
	} else {
		values_[s.k].data = s.p;
		values_[s.k].size = static_cast<uint32_t>(s.size);
	}
}

void CdRow::StoreInteger(const Staging &s, size_t width) {
	if (s.size > width) {
		throw FormatError("backup: " + s.What() + " holds " + std::to_string(s.size) + " bytes for a " +
						  std::to_string(width) + "-byte integer");
	}
	PutLE(buffer_, static_cast<uint64_t>(CdInteger(s.p, s.size)), width);
	Store(s, width);
}

void CdRow::StorePadded(const Staging &s, uint8_t pad, size_t width) {
	if (s.size > width) {
		throw FormatError("backup: " + s.What() + " holds " + std::to_string(s.size) + " bytes, past its " +
						  std::to_string(width));
	}
	buffer_.insert(buffer_.end(), s.p, s.p + s.size);
	buffer_.insert(buffer_.end(), width - s.size, pad);
	Store(s, width);
}

//! The integer, bit, vardecimal and IEEE families, each put in the uncompressed record's form.
bool CdRow::StageNumeric(const Staging &s) {
	switch (s.c.xtype) {
	case XTYPE_TINYINT:
		if (s.size > 1) {
			throw FormatError("backup: " + s.What() + " holds " + std::to_string(s.size) + " bytes for a tinyint");
		}
		buffer_.push_back(s.size ? s.p[0] : 0);	 // as it is, unflipped (E22)
		Store(s, 1);
		return true;
	case XTYPE_SMALLINT:
		StoreInteger(s, 2);
		return true;
	case XTYPE_INT:
	case XTYPE_SMALLMONEY:
		StoreInteger(s, 4);
		return true;
	case XTYPE_BIGINT:
	case XTYPE_MONEY:
		StoreInteger(s, 8);
		return true;
	case XTYPE_BIT:
		if (s.cd.code != CD_EMPTY && s.cd.code != CD_BIT_ONE) {
			throw FormatError("backup: " + s.What() + ", a bit, holds CD code " + std::to_string(s.cd.code));
		}
		if (s.c.bit_position > 7) {
			throw FormatError("backup: " + s.What() + " has bit position " + std::to_string(s.c.bit_position));
		}
		buffer_.push_back(s.cd.code == CD_BIT_ONE ? static_cast<uint8_t>(1u << s.c.bit_position) : 0);
		Store(s, 1);
		return true;
	case XTYPE_DECIMAL:
	case XTYPE_NUMERIC:
		DecodeVardecimal(s.p, s.size, s.c.precision, s.c.scale, buffer_, s.layout.table, s.c.name);
		Store(s, buffer_.size() - s.start);
		return true;
	case XTYPE_REAL:
	case XTYPE_FLOAT: {
		// The IEEE value's little-endian bytes without their low-order zero bytes (E22).
		const size_t width = s.c.xtype == XTYPE_REAL ? 4 : 8;
		if (s.size > width) {
			throw FormatError("backup: " + s.What() + " holds " + std::to_string(s.size) + " bytes for a " +
							  std::to_string(width) + "-byte float");
		}
		buffer_.insert(buffer_.end(), width - s.size, 0);
		buffer_.insert(buffer_.end(), s.p, s.p + s.size);
		Store(s, width);
		return true;
	}
	default:
		return false;  // another family's
	}
}

//! datetime, smalldatetime, and the fixed-width date / time / guid types.
bool CdRow::StageTemporal(const Staging &s) {
	switch (s.c.xtype) {
	case XTYPE_DATETIME: {
		if (s.size > 8) {
			throw FormatError("backup: " + s.What() + " holds " + std::to_string(s.size) + " bytes for a datetime");
		}
		const uint64_t whole = static_cast<uint64_t>(CdInteger(s.p, s.size));  // days << 32 | ticks
		PutLE(buffer_, whole & 0xFFFFFFFFu, 4);	 // the uncompressed form: ticks, then days
		PutLE(buffer_, whole >> 32, 4);
		Store(s, 8);
		return true;
	}
	case XTYPE_SMALLDATETIME:
		if (s.size == 2) {
			buffer_.push_back(0);  // minutes 0: the day count alone (E22)
			buffer_.push_back(0);
			buffer_.insert(buffer_.end(), s.p, s.p + 2);
			Store(s, 4);
		} else if (s.size == 0 || s.size == 4) {
			StorePadded(s, 0, 4);
		} else {
			throw FormatError("backup: " + s.What() + " holds " + std::to_string(s.size) +
							  " bytes for a smalldatetime");
		}
		return true;
	case XTYPE_DATE:
	case XTYPE_TIME:
	case XTYPE_DATETIME2:
	case XTYPE_DATETIMEOFFSET:
	case XTYPE_UNIQUEIDENTIFIER: {
		const size_t width = s.c.FixedLength(s.layout.table);
		if (s.size == width) {
			StoreAsIs(s);
		} else if (s.size == 0) {
			StorePadded(s, 0, width);
		} else {
			throw FormatError("backup: " + s.What() + " holds " + std::to_string(s.size) + " bytes, its type " +
							  std::to_string(width));
		}
		return true;
	}
	default:
		return false;  // another family's
	}
}

//! char / binary (padded or as stored), rowversion, nchar / nvarchar (SCSU), and everything
//! else -- varchar, varbinary, the MAX and LOB types -- as stored.
void CdRow::StageBytes(const Staging &s) {
	switch (s.c.xtype) {
	case XTYPE_CHAR:
	case XTYPE_BINARY:
		if (s.c.unpadded) {
			if (s.size > s.c.length) {
				throw FormatError("backup: " + s.What() + " holds " + std::to_string(s.size) + " bytes, past its " +
								  std::to_string(s.c.length));
			}
			StoreAsIs(s);  // nullable, under ANSI_PADDING OFF: stored and read as given (E22)
		} else {
			StorePadded(s, s.c.xtype == XTYPE_CHAR ? 0x20 : 0, s.c.length);
		}
		return;
	case XTYPE_TIMESTAMP:
		// Its 8 bytes big-endian, the leading zero bytes left out (E22: 0x7D1 -> 07 d1).
		if (s.size > 8) {
			throw FormatError("backup: " + s.What() + " holds " + std::to_string(s.size) + " bytes for a rowversion");
		}
		buffer_.insert(buffer_.end(), 8 - s.size, 0);
		buffer_.insert(buffer_.end(), s.p, s.p + s.size);
		Store(s, 8);
		return;
	case XTYPE_NCHAR:
	case XTYPE_NVARCHAR: {
		DecodeCompressedUnicode(s.p, s.size, buffer_, s.layout.table, s.c.name);
		const size_t decoded = buffer_.size() - s.start;
		if (s.c.xtype == XTYPE_NCHAR) {
			if (decoded > s.c.length) {
				throw FormatError("backup: " + s.What() + " holds " + std::to_string(decoded / 2) +
								  " characters, past its " + std::to_string(s.c.length / 2));
			}
			for (size_t b = decoded; b < s.c.length; b += 2) {
				buffer_.push_back(0x20);  // nchar's trailing spaces were left out
				buffer_.push_back(0x00);
			}
		}
		Store(s, buffer_.size() - s.start);
		return;
	}
	default:
		// varchar, varbinary, the MAX and LOB types, and the types read as their stored bytes: as stored.
		StoreAsIs(s);
		return;
	}
}

//! `s`'s value staged into `buffer_` for column `s.k`: located only when the scan does not read
//! it, else put in the form the uncompressed record holds it (E22).
void CdRow::StageValue(const Staging &s, const std::vector<bool> *needed) {
	if (needed && (s.k >= needed->size() || !(*needed)[s.k])) {
		StoreAsIs(s);  // located only: not read by this scan
		return;
	}
	if (s.cd.code == CD_BIT_ONE && s.c.xtype != XTYPE_BIT) {
		throw FormatError("backup: " + s.What() + " holds CD code 11 (bit 1) and is not a bit");
	}
	if (s.c.Uniquifier()) {
		if (s.size == 0) {
			values_[s.k].size = 0;	// absent: 0, as the uncompressed record's empty uniquifier
			values_[s.k].data = s.p;
		} else {
			StoreInteger(s, 4);
		}
		return;
	}
	if (StageNumeric(s)) {
		return;
	}
	if (StageTemporal(s)) {
		return;
	}
	StageBytes(s);
}

//! A value's bytes under PAGE compression (E23): a symbol is its dictionary entry; an anchored
//! column's value (an entry too) is its prefix of the anchor's value and its suffix; EMPTY there,
//! the anchor's value whole. `p` / `size` / `rebuilt` are moved on to what the value actually is.
void CdRow::RebuildFromPage(const PageCompression &page, const CdValue &cd, size_t index, const RowsetLayout &layout,
							const PhysicalColumn &c, const uint8_t *&p, size_t &size, bool &rebuilt) {
	auto what = [&]() { return layout.table + "." + c.name; };
	const CdValue *anchor =
		page.has_anchor && index < page.anchor.column_count && page.anchor.values[index].code != CD_NULL
			? &page.anchor.values[index]
			: nullptr;
	if (anchor && (anchor->complex || anchor->code == CD_SYMBOL)) {
		throw FormatError("backup: " + what() + " has an anchor value the reader does not know (code " +
						  std::to_string(anchor->code) + ")");
	}
	const uint8_t *anchor_data = anchor ? page.anchor.data + anchor->offset : nullptr;
	const size_t anchor_size = anchor ? (anchor->code == CD_EMPTY ? 0 : anchor->size) : 0;
	bool prefixed = anchor && cd.code != CD_EMPTY && cd.code != CD_BIT_ONE;
	if (cd.code == CD_SYMBOL) {
		const size_t symbol = p[0];
		if (symbol >= page.dictionary.size()) {
			throw FormatError("backup: " + what() + " holds dictionary symbol " + std::to_string(symbol) +
							  " of a page dictionary of " + std::to_string(page.dictionary.size()));
		}
		p = page.page + page.dictionary[symbol].first;
		size = page.dictionary[symbol].second;
	} else if (cd.code == CD_EMPTY && anchor) {
		p = anchor_data;
		size = anchor_size;
		rebuilt = true;	 // the anchor's value: stored, not empty
	}
	if (prefixed) {
		if (size == 0) {
			throw FormatError("backup: " + what() + " holds no prefix length beside its anchor");
		}
		size_t prefix = p[0];
		size_t skip = 1;
		if (prefix & 0x80) {
			if (size < 2) {
				throw FormatError("backup: " + what() + " holds a two-byte prefix length of one byte");
			}
			prefix = ((prefix & 0x7F) << 8) | p[1];
			skip = 2;
		}
		if (prefix > anchor_size) {
			throw FormatError("backup: " + what() + " holds a prefix of " + std::to_string(prefix) +
							  " bytes of an anchor value of " + std::to_string(anchor_size));
		}
		rebuilt_.assign(anchor_data, anchor_data + prefix);
		rebuilt_.insert(rebuilt_.end(), p + skip, p + size);
		p = rebuilt_.data();
		size = rebuilt_.size();
		rebuilt = true;
	}
}

void CdRow::Read(const CdRecord &record, const RowsetLayout &layout, const std::vector<bool> *needed,
				 const PageCompression *page) {
	const size_t n = layout.columns.size();
	values_.assign(n, ValueRef());
	at_.assign(n, SIZE_MAX);
	buffer_.clear();
	for (size_t k = 0; k < n; k++) {
		const PhysicalColumn &c = layout.columns[k];
		ValueRef &v = values_[k];
		if (c.Dropped()) {
			continue;
		}
		// The column's name for an error only: built when one is thrown, not for every value.
		auto what = [&]() { return layout.table + "." + c.name; };
		if (c.offset >= 0) {
			throw FormatError("backup: " + what() + " is described at fixed offset " + std::to_string(c.offset) +
							  " in a compressed rowset, whose columns are CD indexes");
		}
		const size_t index = static_cast<size_t>(-static_cast<int32_t>(c.offset)) - 1;
		if (index >= record.column_count) {
			// Added after the record was written (a metadata-only ADD COLUMN): its default, else NULL.
			if (c.has_default) {
				v.state = ValueState::Default;
				v.data = reinterpret_cast<const uint8_t *>(c.default_bytes.data());
				v.size = c.Unpadded(v.data, static_cast<uint32_t>(c.default_bytes.size()));
			} else if (c.HasDefaultStatus()) {
				throw FormatError("backup: " + what() +
								  " was added after a record was written, with a default the reader was not given");
			}
			continue;
		}
		const CdValue &cd = record.values[index];
		if (cd.code == CD_NULL) {
			continue;
		}
		const uint8_t *p = record.data + cd.offset;
		size_t size = (cd.code == CD_EMPTY || cd.code == CD_BIT_ONE) ? 0 : cd.size;
		bool rebuilt = false;
		if (page && page->present && !cd.complex) {
			RebuildFromPage(*page, cd, index, layout, c, p, size, rebuilt);
		} else if (cd.code == CD_SYMBOL) {
			throw FormatError("backup: " + what() + " holds a page-dictionary symbol outside PAGE compression");
		}
		if (cd.complex) {
			v.state = ValueState::OffRow;
			v.data = p;
			v.size = cd.size;
			continue;
		}
		v.state = ValueState::Inline;
		const Staging s = {layout, c, cd, k, buffer_.size(), p, size, rebuilt};
		StageValue(s, needed);
	}
	for (size_t k = 0; k < n; k++) {
		if (at_[k] != SIZE_MAX) {
			values_[k].data = buffer_.data() + at_[k];
		}
	}
}

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb
