// Fuzz harness: a user table's records (spec 085 PR 2d, task 18b).
//
// The input is a rowset layout and a page. Byte 0 is the column count (at most 32); each column is 10
// bytes: its xtype, three bytes of sysrscols.ti above it (SetTypeInfo: a length, or a decimal's
// precision and scale, or a time type's scale), a signed u16 offset (> 0 fixed, -n the n-th variable
// column), a u16 null bit, a bit position, and a flag byte (bit 0 dropped, bit 1 the uniquifier, bit 2
// the default status, bit 3 a default given, whose length is the high nibble). The rest is the page,
// padded with zeros to 8 KB; its slot count is its own header's. Every slot goes through ReadSlot and,
// for a primary or forwarded record, RowVarCount, ForwardedFrom, LocateValue for every column (each
// byte a ValueRef names is read here, and it must lie in the page or in the layout's default) and
// DecodeRecord, with FixedLength and KnownFixedXtype on every column. Every malformed input must end in
// a FormatError; any sanitizer report, trap, signal, hang or other exception is a real bug.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "backup/backup_bytes.hpp"
#include "backup/backup_page.hpp"
#include "backup/backup_record.hpp"
#include "backup/backup_system_tables.hpp"

using namespace duckdb::mssql::backup;

static volatile size_t g_sink = 0;
static const size_t COLUMN_BYTES = 10;
static const size_t MAX_COLUMNS = 32;

static void Touch(const ValueRef &v, const std::vector<uint8_t> &page, const PhysicalColumn &column) {
	if (v.state == ValueState::Null) {
		return;
	}
	const uint8_t *begin = page.data(), *end = page.data() + page.size();
	const uint8_t *default_begin = reinterpret_cast<const uint8_t *>(column.default_bytes.data());
	const bool in_page = v.data >= begin && v.data + v.size <= end;
	const bool in_default =
		v.state == ValueState::Default && v.data == default_begin && v.size == column.default_bytes.size();
	if (!in_page && !in_default) {
		__builtin_trap();  // a value outside the record's page: the locator's bounds failed
	}
	for (uint32_t i = 0; i < v.size; i++) {
		g_sink += v.data[i];
	}
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
	if (size < 1) {
		return 0;
	}
	const size_t columns = std::min<size_t>(data[0], MAX_COLUMNS);
	size_t at = 1;
	if (size < at + columns * COLUMN_BYTES) {
		return 0;
	}
	RowsetLayout layout;
	layout.table = "fuzz";
	for (size_t i = 0; i < columns; i++, at += COLUMN_BYTES) {
		const uint8_t *d = data + at;
		PhysicalColumn c;
		const uint32_t ti = static_cast<uint32_t>(d[0]) | (static_cast<uint32_t>(d[1]) << 8) |
							(static_cast<uint32_t>(d[2]) << 16) | (static_cast<uint32_t>(d[3]) << 24);
		SetTypeInfo(c, ti);
		c.name = "c" + std::to_string(i);
		c.rscolid = static_cast<int32_t>(i + 1);
		c.offset = static_cast<int16_t>(d[4] | (d[5] << 8));
		c.null_bit = static_cast<uint16_t>(d[6] | (d[7] << 8));
		c.bit_position = d[8];
		const uint8_t flags = d[9];
		c.status = (flags & 1 ? 0x2u : 0u) | (flags & 2 ? 0x10u : 0u) | (flags & 4 ? 0x200u : 0u);
		if (flags & 8) {
			c.has_default = true;
			c.default_bytes.assign(flags >> 4, '\xA5');
		}
		layout.columns.push_back(std::move(c));
	}
	for (const auto &c : layout.columns) {
		g_sink += KnownFixedXtype(c.xtype);
		try {
			g_sink += c.FixedLength(layout.table);
		} catch (const FormatError &) {
		}
	}
	std::vector<uint8_t> page(PAGE_SIZE, 0);
	std::copy(data + at, data + std::min(size, at + PAGE_SIZE), page.begin());
	const uint16_t slots = static_cast<uint16_t>(page[22] | (page[23] << 8));
	for (uint16_t s = 0; s < slots && s < PAGE_SIZE / 2; s++) {
		RecordView r;
		try {
			r = ReadSlot(page.data(), PAGE_SIZE, slots, s, "fuzz");
		} catch (const FormatError &) {
			continue;
		}
		if (r.Type() != RECORD_PRIMARY && r.Type() != RECORD_FORWARDED) {
			g_sink += r.status_a;
			continue;
		}
		g_sink += r.HasVersionTag();
		size_t vars = 0;
		try {
			vars = RowVarCount(r, layout.table);
		} catch (const FormatError &) {
			continue;
		}
		if (r.Type() == RECORD_FORWARDED) {
			try {
				const RowLocation from = ForwardedFrom(r, layout.table);
				g_sink += from.page.page + from.slot;
			} catch (const FormatError &) {
			}
		}
		for (size_t k = 0; k < layout.columns.size(); k++) {
			try {
				Touch(LocateValue(r, vars, layout, k), page, layout.columns[k]);
			} catch (const FormatError &) {
			}
		}
		try {
			for (const auto &v : DecodeRecord(r, layout)) {
				g_sink += v.bytes.size() + v.is_null + v.off_row;
			}
		} catch (const FormatError &) {
		}
	}
	return 0;
}
