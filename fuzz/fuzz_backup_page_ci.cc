// Fuzz harness: PAGE compression (spec 086 PR 2): a page's compression-information record (its anchor
// record and its dictionary) and the CD records whose values it rebuilds (dictionary symbols, prefixes of
// the anchor). Built on fuzz_backup_cd's input, read as a PAGE rowset's.
//
// The input is fuzz_backup_record's: a rowset layout, then a page. Byte 0 is the column count (at most
// 32); each column is 10 bytes: its xtype, three bytes of sysrscols.ti above it (SetTypeInfo), a signed
// u16 offset (a CD index is -offset - 1), a u16 null bit, a bit position, and a flag byte (bit 0
// dropped, bit 1 the uniquifier, bit 2 the default status, bit 3 a default given, whose length is the
// high nibble). The rest is the page, padded with zeros to 8 KB; its slot count is its own header's.
// Every slot goes through ReadCdRecord and, for a row, CdRow::Read over the layout (every value
// normalized: the integers, vardecimal, the floats, datetime, the padded types, SCSU), each value's
// bytes read; and DecodeCompressedUnicode / DecodeVardecimal straight over the slot's bytes. Every
// malformed input must end in a FormatError; any sanitizer report, trap, signal, hang or other
// exception is a real bug.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "backup/backup_bytes.hpp"
#include "backup/backup_cd_record.hpp"
#include "backup/backup_page.hpp"
#include "backup/backup_system_tables.hpp"

using namespace duckdb::mssql::backup;

static volatile size_t g_sink = 0;
static const size_t COLUMN_BYTES = 10;
static const size_t MAX_COLUMNS = 32;

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
	layout.compression = 2;
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
		c.bit_position = d[8] & 7;
		const uint8_t flags = d[9];
		c.status = (flags & 1 ? 0x2u : 0u) | (flags & 2 ? 0x10u : 0u) | (flags & 4 ? 0x200u : 0u);
		if (flags & 8) {
			c.has_default = true;
			c.default_bytes.assign(flags >> 4, '\xA5');
		}
		layout.columns.push_back(std::move(c));
	}
	std::vector<uint8_t> page(PAGE_SIZE, 0);
	std::copy(data + at, data + std::min(size, at + PAGE_SIZE), page.begin());
	const uint16_t slots = static_cast<uint16_t>(page[22] | (page[23] << 8));
	CdRecord record;
	CdRow row;
	std::vector<uint8_t> scratch;
	PageCompression ci;
	bool has_ci = false;
	{
		const size_t slot_array = PAGE_SIZE - 2u * std::min<size_t>(slots, PAGE_SIZE / 2 - 48);
		try {
			ReadPageCompression(page.data(), PAGE_SIZE, slot_array, "fuzz", ci);
			has_ci = true;
			for (const auto &e : ci.dictionary) {
				for (uint16_t i = 0; i < e.second; i++) {
					g_sink += page[e.first + i];  // within the record: ReadPageCompression's bounds
				}
			}
			for (const auto &v : ci.anchor.values) {
				for (uint16_t i = 0; i < v.size; i++) {
					g_sink += ci.anchor.data[v.offset + i];
				}
			}
		} catch (const FormatError &) {
		}
	}
	for (uint16_t s = 0; s < slots && s < PAGE_SIZE / 2; s++) {
		const size_t slot_array = PAGE_SIZE - 2u * slots;
		const size_t entry = PAGE_SIZE - 2u - 2u * s;
		if (slot_array > PAGE_SIZE || entry + 1 >= PAGE_SIZE) {
			break;
		}
		const uint16_t offset = static_cast<uint16_t>(page[entry] | (page[entry + 1] << 8));
		try {
			ReadCdRecord(page.data(), PAGE_SIZE, slot_array, offset, "fuzz", record);
		} catch (const FormatError &) {
			continue;
		}
		g_sink += record.end + static_cast<size_t>(record.kind);
		if (record.kind == CdRecord::Kind::Forwarded) {
			uint32_t p = 0;
			uint16_t f = 0, sl = 0;
			record.BackPointer(p, f, sl);
			g_sink += p + f + sl;
		}
		for (const auto &v : record.values) {
			// Each stored value lies in the record (ReadCdRecord's bounds): read it, and decode it as the
			// compressed Unicode and vardecimal forms whatever its column.
			for (uint16_t i = 0; i < v.size; i++) {
				g_sink += record.data[v.offset + i];
			}
			scratch.clear();
			try {
				DecodeCompressedUnicode(record.data + v.offset, v.size, scratch, layout.table, "v");
			} catch (const FormatError &) {
			}
			scratch.clear();
			try {
				DecodeVardecimal(record.data + v.offset, v.size, 38, static_cast<uint8_t>(v.code % 39), scratch,
								 layout.table, "v");
			} catch (const FormatError &) {
			}
			g_sink += scratch.size();
		}
		if (record.kind != CdRecord::Kind::Row && record.kind != CdRecord::Kind::Forwarded) {
			continue;
		}
		for (int pass = 0; pass < 2; pass++) {
			try {
				row.Read(record, layout, nullptr, pass == 0 && has_ci ? &ci : nullptr);
			} catch (const FormatError &) {
				continue;
			}
			for (size_t k = 0; k < row.Size(); k++) {
				const ValueRef &v = row.Value(k);
				if (v.state == ValueState::Null) {
					continue;
				}
				for (uint32_t i = 0; i < v.size; i++) {
					g_sink += v.data[i];  // in the page, the layout's default or the row's own buffer
				}
			}
		}
	}
	return 0;
}
