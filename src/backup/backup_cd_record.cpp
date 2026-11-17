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
				  CdRecord &out) {
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
						  ", not a CD record the reader knows, in a ROW-compressed rowset");
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
		out.kind = CdRecord::Kind::Forwarded;
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
	if (end < 9) {
		end = 9;  // every record is padded to 9 bytes (E22)
		need(end, "padding");
	}
	out.end = static_cast<uint16_t>(end);
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

void CdRow::Read(const CdRecord &record, const RowsetLayout &layout, const std::vector<bool> *needed) {
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
							  " in a ROW-compressed rowset, whose columns are CD indexes");
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
		if (cd.code == CD_SYMBOL) {
			throw FormatError("backup: " + what() + " holds a page-dictionary symbol outside PAGE compression");
		}
		const uint8_t *p = record.data + cd.offset;
		const size_t size = (cd.code == CD_EMPTY || cd.code == CD_BIT_ONE) ? 0 : cd.size;
		if (cd.complex) {
			v.state = ValueState::OffRow;
			v.data = p;
			v.size = cd.size;
			continue;
		}
		v.state = ValueState::Inline;
		if (needed && (k >= needed->size() || !(*needed)[k])) {
			v.data = p;	 // located only: not read by this scan
			v.size = static_cast<uint32_t>(size);
			continue;
		}
		if (cd.code == CD_BIT_ONE && c.xtype != XTYPE_BIT) {
			throw FormatError("backup: " + what() + " holds CD code 11 (bit 1) and is not a bit");
		}
		const size_t start = buffer_.size();
		auto as_is = [&]() {
			v.data = p;
			v.size = static_cast<uint32_t>(size);
		};
		auto stored = [&](size_t bytes) {
			at_[k] = start;
			v.size = static_cast<uint32_t>(bytes);
		};
		auto integer = [&](size_t width) {
			if (size > width) {
				throw FormatError("backup: " + what() + " holds " + std::to_string(size) + " bytes for a " +
								  std::to_string(width) + "-byte integer");
			}
			PutLE(buffer_, static_cast<uint64_t>(CdInteger(p, size)), width);
			stored(width);
		};
		auto padded = [&](uint8_t pad, size_t width) {
			if (size > width) {
				throw FormatError("backup: " + what() + " holds " + std::to_string(size) + " bytes, past its " +
								  std::to_string(width));
			}
			buffer_.insert(buffer_.end(), p, p + size);
			buffer_.insert(buffer_.end(), width - size, pad);
			stored(width);
		};
		if (c.Uniquifier()) {
			if (size == 0) {
				v.size = 0;	 // absent: 0, as the uncompressed record's empty uniquifier
				v.data = p;
			} else {
				integer(4);
			}
			continue;
		}
		switch (c.xtype) {
		case XTYPE_TINYINT:
			if (size > 1) {
				throw FormatError("backup: " + what() + " holds " + std::to_string(size) + " bytes for a tinyint");
			}
			buffer_.push_back(size ? p[0] : 0);	 // as it is, unflipped (E22)
			stored(1);
			break;
		case XTYPE_SMALLINT:
			integer(2);
			break;
		case XTYPE_INT:
		case XTYPE_SMALLMONEY:
			integer(4);
			break;
		case XTYPE_BIGINT:
		case XTYPE_MONEY:
			integer(8);
			break;
		case XTYPE_BIT:
			if (cd.code != CD_EMPTY && cd.code != CD_BIT_ONE) {
				throw FormatError("backup: " + what() + ", a bit, holds CD code " + std::to_string(cd.code));
			}
			if (c.bit_position > 7) {
				throw FormatError("backup: " + what() + " has bit position " + std::to_string(c.bit_position));
			}
			buffer_.push_back(cd.code == CD_BIT_ONE ? static_cast<uint8_t>(1u << c.bit_position) : 0);
			stored(1);
			break;
		case XTYPE_DECIMAL:
		case XTYPE_NUMERIC:
			DecodeVardecimal(p, size, c.precision, c.scale, buffer_, layout.table, c.name);
			stored(buffer_.size() - start);
			break;
		case XTYPE_REAL:
		case XTYPE_FLOAT: {
			// The IEEE value's little-endian bytes without their low-order zero bytes (E22).
			const size_t width = c.xtype == XTYPE_REAL ? 4 : 8;
			if (size > width) {
				throw FormatError("backup: " + what() + " holds " + std::to_string(size) + " bytes for a " +
								  std::to_string(width) + "-byte float");
			}
			buffer_.insert(buffer_.end(), width - size, 0);
			buffer_.insert(buffer_.end(), p, p + size);
			stored(width);
			break;
		}
		case XTYPE_DATETIME: {
			if (size > 8) {
				throw FormatError("backup: " + what() + " holds " + std::to_string(size) + " bytes for a datetime");
			}
			const uint64_t whole = static_cast<uint64_t>(CdInteger(p, size));  // days << 32 | ticks
			PutLE(buffer_, whole & 0xFFFFFFFFu, 4);	 // the uncompressed form: ticks, then days
			PutLE(buffer_, whole >> 32, 4);
			stored(8);
			break;
		}
		case XTYPE_SMALLDATETIME:
			if (size == 2) {
				buffer_.push_back(0);  // minutes 0: the day count alone (E22)
				buffer_.push_back(0);
				buffer_.insert(buffer_.end(), p, p + 2);
				stored(4);
			} else if (size == 0 || size == 4) {
				padded(0, 4);
			} else {
				throw FormatError("backup: " + what() + " holds " + std::to_string(size) +
								  " bytes for a smalldatetime");
			}
			break;
		case XTYPE_DATE:
		case XTYPE_TIME:
		case XTYPE_DATETIME2:
		case XTYPE_DATETIMEOFFSET:
		case XTYPE_UNIQUEIDENTIFIER: {
			const size_t width = c.FixedLength(layout.table);
			if (size == width) {
				as_is();
			} else if (size == 0) {
				padded(0, width);
			} else {
				throw FormatError("backup: " + what() + " holds " + std::to_string(size) + " bytes, its type " +
								  std::to_string(width));
			}
			break;
		}
		case XTYPE_CHAR:
		case XTYPE_BINARY:
			if (c.unpadded) {
				if (size > c.length) {
					throw FormatError("backup: " + what() + " holds " + std::to_string(size) + " bytes, past its " +
									  std::to_string(c.length));
				}
				as_is();  // nullable, under ANSI_PADDING OFF: stored and read as given (E22)
			} else {
				padded(c.xtype == XTYPE_CHAR ? 0x20 : 0, c.length);
			}
			break;
		case XTYPE_TIMESTAMP:
			// Its 8 bytes big-endian, the leading zero bytes left out (E22: 0x7D1 -> 07 d1).
			if (size > 8) {
				throw FormatError("backup: " + what() + " holds " + std::to_string(size) + " bytes for a rowversion");
			}
			buffer_.insert(buffer_.end(), 8 - size, 0);
			buffer_.insert(buffer_.end(), p, p + size);
			stored(8);
			break;
		case XTYPE_NCHAR:
		case XTYPE_NVARCHAR: {
			DecodeCompressedUnicode(p, size, buffer_, layout.table, c.name);
			const size_t decoded = buffer_.size() - start;
			if (c.xtype == XTYPE_NCHAR) {
				if (decoded > c.length) {
					throw FormatError("backup: " + what() + " holds " + std::to_string(decoded / 2) +
									  " characters, past its " + std::to_string(c.length / 2));
				}
				for (size_t b = decoded; b < c.length; b += 2) {
					buffer_.push_back(0x20);  // nchar's trailing spaces were left out
					buffer_.push_back(0x00);
				}
			}
			stored(buffer_.size() - start);
			break;
		}
		default:
			// varchar, varbinary, the MAX and LOB types, and the types read as their stored bytes: as stored.
			as_is();
			break;
		}
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
