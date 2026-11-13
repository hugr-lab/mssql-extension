// Spec 085 PR 2d, task 15: RecordStager -- a user table's records into DuckDB vectors through the
// codec's staging and the live read path's family kernels. Every row of the committed sample's two
// tables (SQL Server 2025) against the expressions samples.sql generated them with, over several
// chunks; then synthetic records for the types the sample lacks (money, smallmoney, smalldatetime,
// datetimeoffset, time, date, uniqueidentifier, char, nchar, binary, timestamp, tinyint, smallint,
// real, float, bits), added columns' defaults, a value stored off row, and the refusals.
#include <cmath>
#include <cstring>
#include <functional>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "backup/backup_function_common.hpp"
#include "backup/backup_lob_map.hpp"
#include "backup/backup_opened.hpp"
#include "backup/backup_record_stager.hpp"
#include "codec/code_page/code_page.hpp"
#include "codec/string_codec.hpp"
#include "duckdb.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/common/types/timestamp.hpp"

using namespace duckdb;
using namespace duckdb::mssql::backup;

namespace {

int failures = 0;

void Check(bool ok, const std::string &what) {
	std::cout << (ok ? "  ok   " : "  FAIL ") << what << std::endl;
	if (!ok) {
		failures++;
	}
}

std::string ErrorOf(const std::function<void()> &f) {
	try {
		f();
	} catch (std::exception &e) {
		return e.what();
	}
	return std::string();
}

// Every row of `table`'s first partition through a stager of `ids`, chunk by chunk, each handed to
// `row` with its chunk and index. Returns the chunk count.
size_t ScanTable(const PageStore &store, const CatalogTable &table, const std::vector<int32_t> &ids,
				 const std::function<void(DataChunk &, idx_t)> &row) {
	RecordStager stager(table, ids);
	const CatalogPartition &p = table.partitions[0];
	stager.UseLayout(table.Layout(p));
	const SysAllocUnit *in_row = nullptr;
	for (const auto &u : table.units) {
		if (u.unit.owner_id == p.rowset_id && u.unit.type == 1) {
			in_row = &u.unit;
		}
	}
	DataChunk chunk;
	chunk.Initialize(Allocator::DefaultAllocator(), stager.Types());
	size_t chunks = 0;
	auto flush = [&]() {
		stager.FinalizeChunk(chunk);
		for (idx_t r = 0; r < chunk.size(); r++) {
			row(chunk, r);
		}
		chunks++;
		chunk.Reset();
		stager.BeginChunk(chunk);
	};
	stager.BeginChunk(chunk);
	Page page;
	for (const auto &at : UnitPages(store, *in_row)) {
		if (!ReadUnitDataPage(store, *in_row, at, table.name, page)) {
			continue;
		}
		for (uint16_t s = 0; s < page.header.slot_count; s++) {
			const uint16_t offset =
				static_cast<uint16_t>(page.bytes[PAGE_SIZE - 2 - 2 * s] | (page.bytes[PAGE_SIZE - 1 - 2 * s] << 8));
			if (offset < 96) {
				continue;
			}
			const RecordView r = ReadSlot(page.bytes.data(), PAGE_SIZE, page.header.slot_count, s, "the test");
			if (r.Type() != RECORD_PRIMARY && r.Type() != RECORD_FORWARDED) {
				continue;
			}
			stager.Append(r);
			if (stager.Full()) {
				flush();
			}
		}
	}
	if (stager.Count()) {
		flush();
	}
	return chunks;
}

std::vector<int32_t> AllColumns(const CatalogTable &t) {
	std::vector<int32_t> ids;
	for (const auto &c : t.columns) {
		ids.push_back(c.column_id);
	}
	return ids;
}

timestamp_t Ts(int32_t y, int32_t m, int32_t d, int32_t h = 0, int32_t mi = 0, int32_t s = 0, int32_t us = 0) {
	return Timestamp::FromDatetime(Date::FromDate(y, m, d), dtime_t(Time::FromTime(h, mi, s, us)));
}

// --- synthetic records -------------------------------------------------------------------------

void Put(std::vector<uint8_t> &b, uint64_t v, size_t bytes) {
	for (size_t i = 0; i < bytes; i++) {
		b.push_back(i < 8 ? static_cast<uint8_t>(v >> (8 * i)) : 0);  // little-endian, zero past 8 bytes
	}
}

// Days from 0001-01-01 (SQL Server's date) and from 1900-01-01 (datetime / smalldatetime).
int64_t Days0001(int32_t y, int32_t m, int32_t d) {
	return Date::EpochDays(Date::FromDate(y, m, d)) - Date::EpochDays(Date::FromDate(1, 1, 1));
}
int64_t Days1900(int32_t y, int32_t m, int32_t d) {
	return Date::EpochDays(Date::FromDate(y, m, d)) - Date::EpochDays(Date::FromDate(1900, 1, 1));
}

// A FixedVar record: `fixed` after the 4-byte header, `columns` null-bitmap bits (bit i set = column
// i + 1 NULL), the variable columns `vars` (complex ones marked in `complex`).
std::vector<uint8_t> Record(const std::vector<uint8_t> &fixed, uint16_t columns, uint64_t nulls,
							const std::vector<std::vector<uint8_t>> &vars, uint32_t complex = 0) {
	std::vector<uint8_t> r = {static_cast<uint8_t>(0x10 | (vars.empty() ? 0 : 0x20)), 0};
	Put(r, 4 + fixed.size(), 2);
	r.insert(r.end(), fixed.begin(), fixed.end());
	Put(r, columns, 2);
	Put(r, nulls, (columns + 7) / 8);
	if (!vars.empty()) {
		Put(r, vars.size(), 2);
		size_t end = r.size() + 2 * vars.size();
		for (size_t i = 0; i < vars.size(); i++) {
			end += vars[i].size();
			Put(r, end | ((complex >> i) & 1 ? 0x8000 : 0), 2);
		}
		for (const auto &v : vars) {
			r.insert(r.end(), v.begin(), v.end());
		}
	}
	return r;
}

Page PageOf(const std::vector<std::vector<uint8_t>> &records) {
	Page page;
	page.bytes.assign(PAGE_SIZE, 0);
	size_t at = 96;
	for (size_t i = 0; i < records.size(); i++) {
		std::copy(records[i].begin(), records[i].end(), page.bytes.begin() + at);
		page.bytes[PAGE_SIZE - 2 - 2 * i] = static_cast<uint8_t>(at);
		page.bytes[PAGE_SIZE - 1 - 2 * i] = static_cast<uint8_t>(at >> 8);
		at += records[i].size();
	}
	page.header.slot_count = static_cast<uint16_t>(records.size());
	return page;
}

// A text page of LOB unit `object << 16` holding `records` (fragment records), from offset 96.
Page TextPageOf(uint32_t object, const std::vector<std::vector<uint8_t>> &records) {
	Page page;
	page.bytes.assign(PAGE_SIZE, 0);
	page.bytes[0] = 1;
	page.bytes[1] = 3;	// TEXT_MIX
	for (int i = 0; i < 4; i++) {
		page.bytes[24 + i] = static_cast<uint8_t>(object >> (8 * i));  // m_objId: the unit's id is it << 16
	}
	size_t at = 96;
	for (size_t i = 0; i < records.size(); i++) {
		std::copy(records[i].begin(), records[i].end(), page.bytes.begin() + at);
		page.bytes[PAGE_SIZE - 2 - 2 * i] = static_cast<uint8_t>(at);
		page.bytes[PAGE_SIZE - 1 - 2 * i] = static_cast<uint8_t>(at >> 8);
		at += records[i].size();
	}
	page.bytes[22] = static_cast<uint8_t>(records.size());
	page.header = ReadPageHeader(page.bytes.data());
	return page;
}

// A fragment record of `type` with `body` after its 14-byte header.
std::vector<uint8_t> FragmentOf(uint16_t type, const std::vector<uint8_t> &body) {
	std::vector<uint8_t> r = {0x08, 0x00};
	Put(r, 14 + body.size(), 2);
	Put(r, 0x10000, 8);
	Put(r, type, 2);
	r.insert(r.end(), body.begin(), body.end());
	return r;
}

CatalogColumn Col(int32_t id, const char *name, uint8_t xtype, const char *type, int16_t max_length,
				  uint8_t precision = 0, uint8_t scale = 0, uint32_t collation = 0) {
	CatalogColumn c;
	c.column_id = id;
	c.name = name;
	c.system_type_id = xtype;
	c.type_name = type;
	c.system_type_name = type;
	c.max_length = max_length;
	c.precision = precision;
	c.scale = scale;
	c.collation_id = collation;
	c.is_nullable = true;
	return c;
}

PhysicalColumn Phys(int32_t id, uint8_t xtype, int16_t offset, uint16_t null_bit, uint16_t length = 0,
					uint8_t precision = 0, uint8_t scale = 0) {
	PhysicalColumn p;
	p.name = "c" + std::to_string(id);
	p.rscolid = id;
	p.xtype = xtype;
	p.offset = offset;
	p.null_bit = null_bit;
	p.length = length;
	p.precision = precision;
	p.scale = scale;
	return p;
}

std::vector<uint8_t> Utf16(const std::string &ascii) {
	std::vector<uint8_t> out;
	for (char ch : ascii) {
		out.push_back(static_cast<uint8_t>(ch));
		out.push_back(0);
	}
	return out;
}

}  // namespace

int main() {
	std::cout << "test_backup_record_stager (spec 085 PR 2d, task 15)" << std::endl;
	DuckDB db(nullptr);
	Connection con(db);

	// The committed sample: every row of both tables against samples.sql's expressions.
	{
		auto opened = OpenBackupCatalog(*con.context, {"test/data/backup/full.bak"});
		const PageStore &store = *opened->pages->store;
		const CatalogTable *items = opened->model.FindTable("dbo", "items");
		const CatalogTable *events = opened->model.FindTable("dbo", "events");
		Check(items && events && items->decodable && events->decodable, "the sample's two tables are decodable");

		// items (id int PRIMARY KEY, name nvarchar(40), code varchar(12) NULL, price decimal(12, 2),
		// created datetime2(3), flag bit, blob varbinary(64) NULL), 2000 rows.
		size_t rows = 0, bad = 0;
		std::string first_bad;
		const size_t chunks = ScanTable(store, *items, AllColumns(*items), [&](DataChunk &c, idx_t r) {
			rows++;
			const int32_t i = c.GetValue(0, r).GetValue<int32_t>();
			const char32_t cp = static_cast<char32_t>(0x00E9 + i % 7);
			std::string name = "item " + std::to_string(i) + " ";
			name += static_cast<char>(0xC0 | (cp >> 6));
			name += static_cast<char>(0x80 | (cp & 0x3F));
			const Value code = c.GetValue(2, r), blob = c.GetValue(6, r);
			const bool ok =
				c.GetValue(1, r).ToString() == name &&
				(i % 5 == 0 ? code.IsNull() : code.ToString() == "C" + std::to_string(i % 997)) &&
				c.GetValue(3, r) == Value::DECIMAL(static_cast<int64_t>(i) * 125, 12, 2) &&
				c.GetValue(4, r) == Value::TIMESTAMPMS(timestamp_ms_t(Timestamp::GetEpochMs(Ts(2026, 1, 1)) +
																	  static_cast<int64_t>(i) * 37000)) &&
				c.GetValue(5, r) == Value::BOOLEAN(i % 2 == 1) &&
				(i % 3 == 0 ? !blob.IsNull() && StringValue::Get(blob).size() == 32 : blob.IsNull());
			if (!ok && bad++ == 0) {
				first_bad = std::to_string(i) + ": " + c.GetValue(1, r).ToString() + " / " + code.ToString() + " / " +
							c.GetValue(3, r).ToString() + " / " + c.GetValue(4, r).ToString();
			}
		});
		Check(rows == 2000 && bad == 0, "items: 2000 rows, every value its expression's (" + std::to_string(bad) +
											" bad" + (first_bad.empty() ? "" : ", first " + first_bad) + ")");
		Check(chunks >= 1, "items: " + std::to_string(chunks) + " chunk(s)");
		Check(items->columns.size() == 7 &&
				  RecordStager(*items, AllColumns(*items)).Types()[4].id() == LogicalTypeId::TIMESTAMP_MS,
			  "D6's types: datetime2(3) as TIMESTAMP_MS");

		// events (event_id bigint, item_id int, note varchar(100) NULL, at datetime), a heap, 3000 rows:
		// more than one chunk.
		rows = 0;
		bad = 0;
		const size_t event_chunks = ScanTable(store, *events, AllColumns(*events), [&](DataChunk &c, idx_t r) {
			rows++;
			const int64_t i = c.GetValue(0, r).GetValue<int64_t>();
			const Value note = c.GetValue(2, r);
			const bool ok = c.GetValue(1, r) == Value::INTEGER(static_cast<int32_t>(1 + i % 2000)) &&
							(i % 4 == 0 ? note.IsNull() : note.ToString() == "event " + std::to_string(i)) &&
							c.GetValue(3, r) == Value::TIMESTAMP(Timestamp::FromEpochMicroSeconds(
													Timestamp::GetEpochMicroSeconds(Ts(2026, 2, 1)) + i * 60000000));
			bad += ok ? 0 : 1;
		});
		Check(rows == 3000 && bad == 0 && event_chunks == 2, "events: 3000 rows over " + std::to_string(event_chunks) +
																 " chunks, datetime's halves swapped (" +
																 std::to_string(bad) + " bad)");

		// A projection: two columns, the second before the first.
		rows = 0;
		bad = 0;
		ScanTable(store, *events, {4, 1}, [&](DataChunk &c, idx_t r) {
			rows++;
			const int64_t i = c.GetValue(1, r).GetValue<int64_t>();
			bad += c.ColumnCount() == 2 &&
						   c.GetValue(0, r) == Value::TIMESTAMP(Timestamp::FromEpochMicroSeconds(
												   Timestamp::GetEpochMicroSeconds(Ts(2026, 2, 1)) + i * 60000000))
					   ? 0
					   : 1;
		});
		Check(rows == 3000 && bad == 0, "a projection of (at, event_id)");
	}

	// Synthetic records: the types the sample lacks.
	{
		const auto *latin = mssql::codec::code_page::FindCollationByName("Latin1_General_CI_AS", 20);
		CatalogTable t;
		t.schema_name = "dbo";
		t.name = "synthetic";
		t.columns = {Col(1, "fee", XTYPE_MONEY, "money", 8, 19, 4),
					 Col(2, "small_fee", XTYPE_SMALLMONEY, "smallmoney", 4, 10, 4),
					 Col(3, "sdt", XTYPE_SMALLDATETIME, "smalldatetime", 4),
					 Col(4, "dt", XTYPE_DATETIME, "datetime", 8),
					 Col(5, "day", XTYPE_DATE, "date", 3),
					 Col(6, "at_time", XTYPE_TIME, "time", 4, 12, 3),
					 Col(7, "placed_at", XTYPE_DATETIMEOFFSET, "datetimeoffset", 9, 30, 3),
					 Col(8, "ref", XTYPE_UNIQUEIDENTIFIER, "uniqueidentifier", 16),
					 Col(9, "tiny", XTYPE_TINYINT, "tinyint", 1),
					 Col(10, "small", XTYPE_SMALLINT, "smallint", 2),
					 Col(11, "ratio", XTYPE_REAL, "real", 4),
					 Col(12, "score", XTYPE_FLOAT, "float", 8),
					 Col(13, "padded", XTYPE_CHAR, "char", 10, 0, 0, static_cast<uint32_t>(latin->collation_id)),
					 Col(14, "npadded", XTYPE_NCHAR, "nchar", 10),
					 Col(15, "fixed_bin", XTYPE_BINARY, "binary", 8),
					 Col(16, "version", XTYPE_TIMESTAMP, "timestamp", 8),
					 Col(17, "b1", XTYPE_BIT, "bit", 1),
					 Col(18, "b2", XTYPE_BIT, "bit", 1),
					 Col(19, "added", XTYPE_INT, "int", 4),
					 Col(20, "added_bit", XTYPE_BIT, "bit", 1),
					 Col(21, "note", XTYPE_VARCHAR, "varchar", 20, 0, 0, static_cast<uint32_t>(latin->collation_id))};
		RowsetLayout layout;
		layout.table = "synthetic";
		layout.rowset_id = 1;
		// The fixed part: fee @4, small_fee @12, sdt @16, dt @20, day @28, at_time @31, placed_at @35,
		// ref @44, tiny @60, small @61, ratio @63, score @67, padded @75, npadded @85, fixed_bin @95,
		// version @103, the bits' byte @111; 112 bytes in all. note is the one variable column.
		layout.columns = {Phys(1, XTYPE_MONEY, 4, 1),
						  Phys(2, XTYPE_SMALLMONEY, 12, 2),
						  Phys(3, XTYPE_SMALLDATETIME, 16, 3),
						  Phys(4, XTYPE_DATETIME, 20, 4),
						  Phys(5, XTYPE_DATE, 28, 5),
						  Phys(6, XTYPE_TIME, 31, 6, 0, 0, 3),
						  Phys(7, XTYPE_DATETIMEOFFSET, 35, 7, 0, 0, 3),
						  Phys(8, XTYPE_UNIQUEIDENTIFIER, 44, 8),
						  Phys(9, XTYPE_TINYINT, 60, 9),
						  Phys(10, XTYPE_SMALLINT, 61, 10),
						  Phys(11, XTYPE_REAL, 63, 11),
						  Phys(12, XTYPE_FLOAT, 67, 12),
						  Phys(13, XTYPE_CHAR, 75, 13, 10),
						  Phys(14, XTYPE_NCHAR, 85, 14, 10),
						  Phys(15, XTYPE_BINARY, 95, 15, 8),
						  Phys(16, XTYPE_TIMESTAMP, 103, 16),
						  Phys(17, XTYPE_BIT, 111, 17),
						  Phys(18, XTYPE_BIT, 111, 18),
						  Phys(19, XTYPE_INT, 112, 20),	 // added after the records: their column count is 19
						  Phys(20, XTYPE_BIT, 116, 21),
						  Phys(21, XTYPE_VARCHAR, -1, 19, 20)};
		layout.columns[17].bit_position = 1;
		layout.columns[18].status = 0x200;
		layout.columns[18].has_default = true;
		layout.columns[18].default_bytes = std::string("\x2a\x00\x00\x00", 4);
		layout.columns[19].status = 0x200;
		layout.columns[19].has_default = true;
		layout.columns[19].default_bytes = std::string("\x01", 1);
		layout.columns[19].bit_position = 3;  // a record's bit 3 of a byte is not what a default holds

		std::vector<uint8_t> fixed;
		Put(fixed, 12345678, 8);										// money 1234.5678
		Put(fixed, static_cast<uint32_t>(-15000), 4);					// smallmoney -1.5
		Put(fixed, 60, 2);												// smalldatetime: 01:00
		Put(fixed, static_cast<uint64_t>(Days1900(2025, 1, 1)), 2);		// ... 2025-01-01
		Put(fixed, 1, 4);												// datetime: 1 tick
		Put(fixed, static_cast<uint64_t>(Days1900(2025, 1, 1)), 4);		// ... 2025-01-01
		Put(fixed, static_cast<uint64_t>(Days0001(2026, 1, 2)), 3);		// date
		Put(fixed, 36000001, 4);										// time(3) 10:00:00.001
		Put(fixed, static_cast<uint64_t>((21 * 3600 + 60) * 1000), 4);	// dto(3): 21:01 UTC
		Put(fixed, static_cast<uint64_t>(Days0001(2025, 12, 31)), 3);	// ... 2025-12-31
		Put(fixed, 180, 2);												// ... +03:00
		for (int i = 0; i < 16; i++) {
			fixed.push_back(static_cast<uint8_t>(i));  // uniqueidentifier
		}
		fixed.push_back(200);					   // tinyint
		Put(fixed, static_cast<uint16_t>(-2), 2);  // smallint
		float half = 0.5f;
		uint32_t half_bits;
		std::memcpy(&half_bits, &half, 4);
		Put(fixed, half_bits, 4);  // real
		double quarter = 0.25;
		uint64_t quarter_bits;
		std::memcpy(&quarter_bits, &quarter, 8);
		Put(fixed, quarter_bits, 8);  // float
		for (char ch : std::string("p1        ")) {
			fixed.push_back(static_cast<uint8_t>(ch));	// char(10)
		}
		const std::vector<uint8_t> n1 = Utf16("n1   ");
		fixed.insert(fixed.end(), n1.begin(), n1.end());  // nchar(5)
		for (int i = 0; i < 8; i++) {
			fixed.push_back(static_cast<uint8_t>(i == 7 ? 1 : 0));	// binary(8)
		}
		Put(fixed, 0x07D1000000000000ull, 8);			// timestamp
		fixed.push_back(0x02);							// b1 = 0, b2 = 1
		const std::vector<uint8_t> note = {'h', 0xE9};	// cp1252's e acute
		// Row 0 every value present; row 1 all NULL but the bits' byte (a NULL bit is the bitmap's).
		const Page page = PageOf({Record(fixed, 19, 0, {note}), Record(fixed, 19, 0x7FFFF, {})});

		std::vector<int32_t> ids;
		for (int32_t i = 1; i <= 21; i++) {
			ids.push_back(i);
		}
		RecordStager stager(t, ids);
		stager.UseLayout(layout);
		DataChunk chunk;
		chunk.Initialize(Allocator::DefaultAllocator(), stager.Types());
		stager.BeginChunk(chunk);
		for (uint16_t s = 0; s < 2; s++) {
			stager.Append(ReadSlot(page.bytes.data(), PAGE_SIZE, 2, s, "the test"));
		}
		stager.FinalizeChunk(chunk);
		auto v = [&](size_t column, idx_t row) { return chunk.GetValue(column, row); };
		Check(chunk.size() == 2, "two rows");
		Check(v(0, 0) == Value::DECIMAL(static_cast<int64_t>(12345678), 19, 4),
			  "money, a plain i64 in the record: " + v(0, 0).ToString());
		Check(v(1, 0) == Value::DECIMAL(static_cast<int64_t>(-15000), 10, 4), "smallmoney: " + v(1, 0).ToString());
		Check(v(2, 0) == Value::TIMESTAMP(Ts(2025, 1, 1, 1)),
			  "smalldatetime, its halves swapped: " + v(2, 0).ToString());
		Check(v(3, 0).ToString().rfind("2025-01-01 00:00:00.003", 0) == 0, "datetime, one tick: " + v(3, 0).ToString());
		Check(v(4, 0) == Value::DATE(Date::FromDate(2026, 1, 2)), "date: " + v(4, 0).ToString());
		Check(v(5, 0) == Value::TIME(dtime_t(Time::FromTime(10, 0, 0, 1000))), "time(3): " + v(5, 0).ToString());
		Check(v(6, 0) == Value::TIMESTAMPTZ(timestamp_tz_t(Ts(2025, 12, 31, 21, 1))),
			  "datetimeoffset(3), at its UTC instant: " + v(6, 0).ToString());
		Check(v(7, 0).ToString() == "03020100-0504-0706-0809-0a0b0c0d0e0f", "uniqueidentifier: " + v(7, 0).ToString());
		Check(v(8, 0) == Value::UTINYINT(200) && v(9, 0) == Value::SMALLINT(-2) && v(10, 0) == Value::FLOAT(0.5f) &&
				  v(11, 0) == Value::DOUBLE(0.25),
			  "tinyint, smallint, real, float straight into the vector");
		Check(v(12, 0).ToString() == "p1" && v(13, 0).ToString() == "n1", "char(10) and nchar(5), trimmed");
		Check(
			v(14, 0).ToString() == "\\x00\\x00\\x00\\x00\\x00\\x00\\x00\\x01" && StringValue::Get(v(15, 0)).size() == 8,
			"binary(8) as stored, timestamp's 8 bytes");
		Check(v(16, 0) == Value::BOOLEAN(false) && v(17, 0) == Value::BOOLEAN(true), "two bits of one byte");
		Check(v(18, 0) == Value::INTEGER(42) && v(19, 0) == Value::BOOLEAN(true),
			  "added columns: their defaults (a bit's default is its own byte)");
		Check(v(20, 0).ToString() == "h\xC3\xA9", "varchar through its code page (0xE9 is U+00E9)");
		bool all_null = true;
		for (size_t c = 0; c < 18; c++) {
			all_null = all_null && v(c, 1).IsNull();
		}
		all_null = all_null && v(20, 1).IsNull();
		Check(all_null && v(18, 1) == Value::INTEGER(42) && v(20, 1).IsNull(),
			  "a row of NULLs, the bits included; the added column still its default");

		// A value stored off row (row-overflow here): with no pages to read it from, refused by name...
		std::vector<uint8_t> overflow = {0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
		Put(overflow, 5, 4);   // the value's length
		Put(overflow, 77, 4);  // its DATA fragment: page 77, file 1, slot 0
		Put(overflow, 1, 2);
		Put(overflow, 0, 2);
		const Page off = PageOf({Record(fixed, 19, 0, {overflow}, 1)});
		stager.BeginChunk(chunk);
		Check(ErrorOf([&]() {
				  stager.Append(ReadSlot(off.bytes.data(), PAGE_SIZE, 1, 0, "the test"));
			  }).find("dbo.synthetic.note is stored off row, and the stager was given no pages") != std::string::npos,
			  "an off-row value with no pages to read it from is refused naming the column");
		// ... and with them, read from its fragment on the partition's row-overflow unit (PR 3).
		{
			const Page fragment_page = TextPageOf(9, {FragmentOf(3, {'h', 'e', 'l', 'l', 'o'})});
			LobPageFetch fetch = [&](const PagePointer &at, Page &out) {
				if (at.page != 77) {
					throw FormatError("no such page");
				}
				out = fragment_page;
			};
			LobUnits units;
			units.row_overflow = static_cast<uint64_t>(9) << 16;
			RecordStager reads(t, {21});
			reads.UseLayout(layout);
			reads.UseOffRow(&fetch, units);
			reads.MapOffRow(true);
			DataChunk one;
			one.Initialize(Allocator::DefaultAllocator(), reads.Types());
			reads.BeginChunk(one);
			reads.Append(ReadSlot(off.bytes.data(), PAGE_SIZE, 1, 0, "the test"));
			const std::string map = reads.LastRowLobMap();
			reads.FinalizeChunk(one);
			Check(one.GetValue(0, 0).ToString() == "hello", "a row-overflow value read from its fragment");
			const auto entries = ParseLobMap(reinterpret_cast<const uint8_t *>(map.data()), map.size(), "the test row");
			Check(entries.size() == 1 && entries[0].column_id == 21 && entries[0].form == OffRowForm::RowOverflow &&
					  entries[0].length == 5 && entries[0].pointer == std::string(overflow.begin(), overflow.end()) &&
					  entries[0].nodes.size() == 1 && entries[0].nodes[0].page.page == 77 &&
					  entries[0].nodes[0].slot == 0 && entries[0].nodes[0].length == 5,
				  "... and the row's map: column 21, row-overflow, its pointer and its one fragment");
		}
		// ... which a projection without the column reads past.
		RecordStager narrow(t, {1, 9});
		narrow.UseLayout(layout);
		DataChunk two;
		two.Initialize(Allocator::DefaultAllocator(), narrow.Types());
		narrow.BeginChunk(two);
		narrow.Append(ReadSlot(off.bytes.data(), PAGE_SIZE, 1, 0, "the test"));
		narrow.FinalizeChunk(two);
		Check(two.size() == 1 && two.GetValue(1, 0) == Value::UTINYINT(200), "a projection without it reads the row");

		// No refusal by type: a type still to be converted is its stored bytes; only what has no stored
		// value (or keeps it in the sparse vector) is refused, by name.
		CatalogTable refused = t;
		refused.columns.push_back(Col(22, "computed", XTYPE_INT, "int", 4));
		refused.columns.back().is_computed = true;
		refused.columns.push_back(Col(23, "sp", XTYPE_INT, "int", 4));
		refused.columns.back().is_sparse = true;
		Check(ErrorOf([&]() { RecordStager(refused, {22}); }).find("dbo.synthetic.computed: a computed column") !=
					  std::string::npos &&
				  ErrorOf([&]() { RecordStager(refused, {23}); }).find("sparse vector") != std::string::npos,
			  "a computed column that is not persisted and a sparse column are not readable, by name");
		const std::vector<std::pair<CatalogColumn, std::string>> raws = {
			{Col(30, "variant", XTYPE_SQL_VARIANT, "sql_variant", 8016), "sql_variant"},
			{Col(31, "doc", XTYPE_XML, "xml", -1), "binary XML"},
			{Col(32, "shape", XTYPE_CLR, "geometry", -1), "geometry"},
			{Col(33, "node", XTYPE_CLR, "hierarchyid", 892), "hierarchyid"},
			{Col(34, "j", 244, "json", -1), "json"},
			{Col(35, "mystery", 250, "", 16), "does not know"}};
		bool all_raw = true;
		for (const auto &rc : raws) {
			const ColumnReading reading = ReadingOf(rc.first);
			all_raw = all_raw && reading.readable && reading.raw && reading.type.id() == LogicalTypeId::BLOB &&
					  reading.note.find(rc.second) != std::string::npos;
		}
		Check(all_raw, "sql_variant, xml, geometry, hierarchyid, json and an unknown type: their stored bytes, a BLOB");
		CatalogColumn odd_collation = Col(36, "t", XTYPE_VARCHAR, "varchar", 10, 0, 0, 0x7fffffffu);
		Check(ReadingOf(odd_collation).raw &&
				  ReadingOf(odd_collation).note.find("newer than the code-page tables") != std::string::npos,
			  "a varchar of a collation newer than the tables: its stored bytes");
		// A raw column through the stager: a sql_variant's bytes as stored, a BLOB.
		CatalogTable with_variant = t;
		with_variant.columns.push_back(Col(37, "variant", XTYPE_SQL_VARIANT, "sql_variant", 8016));
		RowsetLayout variant_layout = layout;
		variant_layout.columns.push_back(Phys(37, XTYPE_SQL_VARIANT, -2, 19));
		const std::vector<uint8_t> variant = {0x38, 0x01, 0x2a, 0x00, 0x00, 0x00};	// an int 42 in a sql_variant
		const Page variant_page = PageOf({Record(fixed, 19, 0, {note, variant})});
		RecordStager variant_stager(with_variant, {37});
		variant_stager.UseLayout(variant_layout);
		DataChunk variant_chunk;
		variant_chunk.Initialize(Allocator::DefaultAllocator(), variant_stager.Types());
		variant_stager.BeginChunk(variant_chunk);
		variant_stager.Append(ReadSlot(variant_page.bytes.data(), PAGE_SIZE, 1, 0, "the test"));
		variant_stager.FinalizeChunk(variant_chunk);
		Check(variant_stager.Types()[0].id() == LogicalTypeId::BLOB &&
				  variant_chunk.GetValue(0, 0).ToString() == "8\\x01*\\x00\\x00\\x00",
			  "a sql_variant column reads as its stored bytes: " + variant_chunk.GetValue(0, 0).ToString());
		Check(ErrorOf([&]() { RecordStager(t, {99}); }).find("has no column 99") != std::string::npos,
			  "a column the table does not have");
		RowsetLayout wrong = layout;
		wrong.columns[0].xtype = XTYPE_SMALLMONEY;	// 4 bytes where money takes 8
		Check(ErrorOf([&]() {
				  RecordStager s(t, {1});
				  s.UseLayout(wrong);
			  }).find("where its type takes 8 fixed bytes") != std::string::npos,
			  "a physical column of another width than its type is refused");
		// After an Append that threw, the chunk is spoiled until BeginChunk.
		Check(ErrorOf([&]() {
				  stager.Append(ReadSlot(page.bytes.data(), PAGE_SIZE, 2, 0, "the test"));
			  }).find("spoiled") != std::string::npos,
			  "an Append after one that threw is refused, not a row shifted");
		stager.BeginChunk(chunk);
		stager.Append(ReadSlot(page.bytes.data(), PAGE_SIZE, 2, 0, "the test"));
		stager.FinalizeChunk(chunk);
		Check(chunk.size() == 1 && chunk.GetValue(8, 0) == Value::UTINYINT(200), "and BeginChunk starts a clean one");
		// A refused layout leaves the stager on the one it had.
		RecordStager kept(t, {9, 1});
		kept.UseLayout(layout);
		RowsetLayout refused_layout = layout;
		refused_layout.columns[0].xtype = XTYPE_SMALLMONEY;
		Check(!ErrorOf([&]() { kept.UseLayout(refused_layout); }).empty(), "a layout of the wrong width is refused");
		DataChunk kept_chunk;
		kept_chunk.Initialize(Allocator::DefaultAllocator(), kept.Types());
		kept.BeginChunk(kept_chunk);
		kept.Append(ReadSlot(page.bytes.data(), PAGE_SIZE, 2, 0, "the test"));
		kept.FinalizeChunk(kept_chunk);
		Check(kept_chunk.GetValue(0, 0) == Value::UTINYINT(200) &&
				  kept_chunk.GetValue(1, 0) == Value::DECIMAL(static_cast<int64_t>(12345678), 19, 4),
			  "and the stager still reads by the layout it had");
	}

	// More types: in-row MAX values, UTF-8 varchar and char(n), datetime2(7) as TIMESTAMP_NS, decimal at
	// 9, 13 and 17 bytes, defaults that need their halves swapped (datetime, money), and an odd UTF-16
	// length.
	{
		const auto *latin = mssql::codec::code_page::FindCollationByName("Latin1_General_CI_AS", 20);
		const auto *utf8 = mssql::codec::code_page::FindCollationByName("Latin1_General_100_CI_AS_SC_UTF8", 32);
		Check(latin && utf8 && utf8->code_page == 65001, "the two collations are in the tables");
		CatalogTable t;
		t.schema_name = "dbo";
		t.name = "more";
		t.columns = {Col(1, "u8c", XTYPE_CHAR, "char", 6, 0, 0, static_cast<uint32_t>(utf8->collation_id)),
					 Col(2, "dt2", XTYPE_DATETIME2, "datetime2", 8, 27, 7),
					 Col(3, "d18", XTYPE_DECIMAL, "decimal", 9, 18, 2),
					 Col(4, "d28", XTYPE_DECIMAL, "decimal", 13, 28, 4),
					 Col(5, "d38", XTYPE_DECIMAL, "decimal", 17, 38, 0),
					 Col(6, "m", XTYPE_VARCHAR, "varchar", -1, 0, 0, static_cast<uint32_t>(latin->collation_id)),
					 Col(7, "nm", XTYPE_NVARCHAR, "nvarchar", -1),
					 Col(8, "u8", XTYPE_VARCHAR, "varchar", 20, 0, 0, static_cast<uint32_t>(utf8->collation_id)),
					 Col(9, "n_odd", XTYPE_NVARCHAR, "nvarchar", 20),
					 Col(10, "added_dt", XTYPE_DATETIME, "datetime", 8),
					 Col(11, "added_money", XTYPE_MONEY, "money", 8, 19, 4)};
		RowsetLayout layout;
		layout.table = "more";
		layout.rowset_id = 2;
		layout.columns = {Phys(1, XTYPE_CHAR, 4, 1, 6),
						  Phys(2, XTYPE_DATETIME2, 10, 2, 0, 0, 7),
						  Phys(3, XTYPE_DECIMAL, 18, 3, 0, 18, 2),
						  Phys(4, XTYPE_DECIMAL, 27, 4, 0, 28, 4),
						  Phys(5, XTYPE_DECIMAL, 40, 5, 0, 38, 0),
						  Phys(6, XTYPE_VARCHAR, -1, 6),
						  Phys(7, XTYPE_NVARCHAR, -2, 7),
						  Phys(8, XTYPE_VARCHAR, -3, 8, 20),
						  Phys(9, XTYPE_NVARCHAR, -4, 9, 20),
						  Phys(10, XTYPE_DATETIME, 57, 10),	 // added after the records: their count is 9
						  Phys(11, XTYPE_MONEY, 65, 11)};
		std::vector<uint8_t> dt_default;
		Put(dt_default, 300, 4);  // 1 s in ticks
		Put(dt_default, static_cast<uint64_t>(Days1900(2025, 1, 1)), 4);
		layout.columns[9].status = layout.columns[10].status = 0x200;
		layout.columns[9].has_default = layout.columns[10].has_default = true;
		layout.columns[9].default_bytes = std::string(dt_default.begin(), dt_default.end());
		layout.columns[10].default_bytes = std::string("\x10\x27\x00\x00\x00\x00\x00\x00", 8);	// 1.0000

		std::vector<uint8_t> fixed;
		for (char ch : std::string("ab    ")) {
			fixed.push_back(static_cast<uint8_t>(ch));	// char(6), UTF-8
		}
		Put(fixed, 1234567, 5);	 // datetime2(7): 0.1234567 s
		Put(fixed, static_cast<uint64_t>(Days0001(2026, 1, 1)), 3);
		fixed.push_back(1);	 // decimal(18, 2) 1234567890123.45: positive, then the mantissa
		Put(fixed, 123456789012345ull, 8);
		fixed.push_back(1);	 // decimal(28, 4) 1.5000
		Put(fixed, 15000, 12);
		fixed.push_back(0);	 // decimal(38, 0) -7: the sign byte 0 is negative
		Put(fixed, 7, 16);
		const std::string text = "\xD1\x82\xD0\xB5\xD0\xBA\xD1\x81\xD1\x82";  // UTF-8 for a Russian word
		const std::vector<uint8_t> m = {'m', 0xE9, 'x'};
		const std::vector<uint8_t> nm = {0x36, 0x04, 0x36, 0x04};  // U+0436 twice
		const std::vector<uint8_t> u8(text.begin(), text.end());
		const std::vector<uint8_t> n_odd = {'a', 0, 'b'};
		const Page page =
			PageOf({Record(fixed, 9, 0, {m, nm, u8, Utf16("ok")}), Record(fixed, 9, 0, {m, nm, u8, n_odd})});
		RecordStager stager(t, {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11});
		stager.UseLayout(layout);
		DataChunk chunk;
		chunk.Initialize(Allocator::DefaultAllocator(), stager.Types());
		stager.BeginChunk(chunk);
		stager.Append(ReadSlot(page.bytes.data(), PAGE_SIZE, 2, 0, "the test"));
		stager.FinalizeChunk(chunk);
		auto v = [&](size_t column) { return chunk.GetValue(column, 0); };
		Check(v(0).ToString() == "ab", "char(6) under a UTF-8 collation, trimmed: '" + v(0).ToString() + "'");
		Check(stager.Types()[1].id() == LogicalTypeId::TIMESTAMP_NS && v(1).ToString() == "2026-01-01 00:00:00.1234567",
			  "datetime2(7) as TIMESTAMP_NS, every digit: " + v(1).ToString());
		Check(v(2).ToString() == "1234567890123.45" && v(3).ToString() == "1.5000" && v(4).ToString() == "-7",
			  "decimal at 9, 13 and 17 bytes: " + v(2).ToString() + ", " + v(3).ToString() + ", " + v(4).ToString());
		Check(v(5).ToString() == "m\xC3\xA9x" && v(6).ToString() == "\xD0\xB6\xD0\xB6",
			  "in-row varchar(max) through its code page, nvarchar(max) as UTF-16");
		Check(v(7).ToString() == text && v(8).ToString() == "ok", "varchar under a UTF-8 collation, as it is");
		Check(v(9).ToString() == "2025-01-01 00:00:01" && v(10).ToString() == "1.0000",
			  "defaults of datetime and money, their halves swapped like a record's: " + v(9).ToString() + ", " +
				  v(10).ToString());
		stager.BeginChunk(chunk);
		Check(ErrorOf([&]() {
				  stager.Append(ReadSlot(page.bytes.data(), PAGE_SIZE, 2, 1, "the test"));
			  }).find("dbo.more.n_odd holds 3 bytes, not a whole number of UTF-16 units") != std::string::npos,
			  "an odd UTF-16 length is refused naming the column");
	}

	// The double-byte pages, decoded on the client like any other: a backup has no server to convert
	// them (where the TDS path keeps the server's cast for them, only because it is faster there).
	{
		struct Page_ {
			const char *collation;
			std::vector<uint8_t> bytes;
			const char *utf8;
		};
		const std::vector<Page_> pages = {
			{"Japanese_CI_AS",
			 {0x93, 0xfa, 0x96, 0x7b, 0xb1, 0xb2},
			 "\xE6\x97\xA5\xE6\x9C\xAC\xEF\xBD\xB1\xEF\xBD\xB2"},
			{"Chinese_PRC_CI_AS", {0xd6, 0xd0, 0xce, 0xc4}, "\xE4\xB8\xAD\xE6\x96\x87"},
			{"Korean_Wansung_CI_AS", {0xc7, 0xd1, 0xb1, 0xb9}, "\xED\x95\x9C\xEA\xB5\xAD"},
			{"Chinese_Taiwan_Stroke_CI_AS", {0xa4, 0xa4, 0xa4, 0xe5}, "\xE4\xB8\xAD\xE6\x96\x87"}};
		for (const auto &pg : pages) {
			const auto *collation =
				mssql::codec::code_page::FindCollationByName(pg.collation, std::strlen(pg.collation));
			if (!collation) {
				Check(false, std::string("the collation ") + pg.collation + " is in the tables");
				continue;
			}
			const uint32_t id = static_cast<uint32_t>(collation->collation_id);
			CatalogTable t;
			t.schema_name = "dbo";
			t.name = "dbcs";
			t.columns = {Col(1, "c", XTYPE_CHAR, "char", 8, 0, 0, id),
						 Col(2, "v", XTYPE_VARCHAR, "varchar", 20, 0, 0, id)};
			Check(!ReadingOf(t.columns[0]).raw && ReadingOf(t.columns[0]).note.empty() &&
					  !ReadingOf(t.columns[1]).raw && ReadingOf(t.columns[1]).note.empty(),
				  std::string(pg.collation) + ": converted, not raw");
			RowsetLayout layout;
			layout.table = "dbcs";
			layout.columns = {Phys(1, XTYPE_CHAR, 4, 1, 8), Phys(2, XTYPE_VARCHAR, -1, 2, 20)};
			std::vector<uint8_t> fixed(pg.bytes);  // char(8): the text, padded with spaces
			fixed.resize(8, 0x20);
			const Page page = PageOf({Record(fixed, 2, 0, {pg.bytes})});
			RecordStager stager(t, {1, 2});
			stager.UseLayout(layout);
			DataChunk chunk;
			chunk.Initialize(Allocator::DefaultAllocator(), stager.Types());
			stager.BeginChunk(chunk);
			stager.Append(ReadSlot(page.bytes.data(), PAGE_SIZE, 1, 0, "the test"));
			stager.FinalizeChunk(chunk);
			Check(chunk.GetValue(0, 0).ToString() == pg.utf8 && chunk.GetValue(1, 0).ToString() == pg.utf8,
				  std::string(pg.collation) + ": char(8) trimmed and varchar decode to " +
					  chunk.GetValue(1, 0).ToString());
		}
	}

	// 932's edges at the stager: a trail byte in the ASCII range (0x95 0x5C is a kanji, not a
	// backslash), a value past the kernel's 16-byte block, and a lead byte left alone at the end
	// (it reads as nothing, as the server drops it).
	{
		const auto *japanese = mssql::codec::code_page::FindCollationByName("Japanese_CI_AS", 14);
		const uint32_t id = static_cast<uint32_t>(japanese->collation_id);
		CatalogTable t;
		t.schema_name = "dbo";
		t.name = "edges";
		t.columns = {Col(1, "v", XTYPE_VARCHAR, "varchar", 100, 0, 0, id)};
		RowsetLayout layout;
		layout.table = "edges";
		layout.columns = {Phys(1, XTYPE_VARCHAR, -1, 1, 100)};
		const std::vector<uint8_t> unit = {0x93, 0xfa, 0x96, 0x7b, 0x95, 0x5c, 'A'};  // 日本表A
		std::vector<uint8_t> longer;
		for (int i = 0; i < 3; i++) {
			longer.insert(longer.end(), unit.begin(), unit.end());	// 21 bytes
		}
		const std::vector<uint8_t> lone = {'x', 0x93};
		const Page page =
			PageOf({Record({}, 1, 0, {{0x95, 0x5c}}), Record({}, 1, 0, {longer}), Record({}, 1, 0, {lone})});
		RecordStager stager(t, {1});
		stager.UseLayout(layout);
		DataChunk chunk;
		chunk.Initialize(Allocator::DefaultAllocator(), stager.Types());
		stager.BeginChunk(chunk);
		for (uint16_t s = 0; s < 3; s++) {
			stager.Append(ReadSlot(page.bytes.data(), PAGE_SIZE, 3, s, "the test"));
		}
		stager.FinalizeChunk(chunk);
		const std::string kanji = "\xE8\xA1\xA8", word =
													  "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xA1\xA8"
													  "A";
		Check(chunk.GetValue(0, 0).ToString() == kanji, "932: 0x95 0x5C is one kanji, its trail byte not a backslash");
		Check(chunk.GetValue(0, 1).ToString() == word + word + word, "932: a 21-byte value, past the 16-byte block");
		Check(chunk.GetValue(0, 2).ToString() == "x", "932: a lead byte alone at the end reads as nothing");
	}

	// text / ntext / image are read since PR 3: in row with text in row on, behind a 16-byte pointer to a
	// root record otherwise, a NULL root as NULL (E21). Types still to be converted are read as their
	// bytes: an unknown fixed-width type at its offset, an xml.
	{
		const auto *latin = mssql::codec::code_page::FindCollationByName("Latin1_General_CI_AS", 20);
		CatalogTable t;
		t.schema_name = "dbo";
		t.name = "legacy";
		t.columns = {Col(1, "odd", 250, "", 6),
					 Col(2, "notes", XTYPE_TEXT, "text", 16, 0, 0, static_cast<uint32_t>(latin->collation_id)),
					 Col(3, "nnotes", XTYPE_NTEXT, "ntext", 16), Col(4, "doc", XTYPE_XML, "xml", -1)};
		RowsetLayout layout;
		layout.table = "legacy";
		layout.columns = {Phys(1, 250, 4, 1, 6), Phys(2, XTYPE_TEXT, -1, 2), Phys(3, XTYPE_NTEXT, -2, 3),
						  Phys(4, XTYPE_XML, -3, 4)};
		const std::vector<uint8_t> odd = {1, 2, 3, 4, 5, 6};
		const std::vector<uint8_t> pointer(16, 0x11), doc = {0xdf, 0xff, 0x01, 0xb0, 0x04};
		const Page page = PageOf({Record(odd, 4, 0x04, {pointer, {}, doc})});
		const ColumnReading notes = ReadingOf(t.columns[1]), nnotes = ReadingOf(t.columns[2]);
		Check(notes.readable && nnotes.readable && notes.type.id() == LogicalTypeId::VARCHAR &&
				  nnotes.type.id() == LogicalTypeId::VARCHAR,
			  "text and ntext are readable, as VARCHAR");
		{
			// text in row: 16 bytes of text in the row, no complex bit.
			RecordStager inline_text(t, {2});
			inline_text.UseLayout(layout);
			DataChunk c;
			c.Initialize(Allocator::DefaultAllocator(), inline_text.Types());
			inline_text.BeginChunk(c);
			inline_text.Append(ReadSlot(page.bytes.data(), PAGE_SIZE, 1, 0, "the test"));
			inline_text.FinalizeChunk(c);
			Check(c.GetValue(0, 0).ToString() == std::string(16, '\x11'), "a text value in row (text in row on)");
		}
		{
			// Behind 16-byte pointers to root records on page 300: a SMALL_ROOT "abc", a NULL root, an
			// empty SMALL_ROOT, and a LARGE_ROOT of level 0 over two DATA fragments of 3,000 bytes of
			// UTF-16 on pages 301 and 302 (an ntext of 3,000 characters, past the staging's first buffer).
			std::vector<uint8_t> utf16;
			for (int i = 0; i < 750; i++) {
				utf16.insert(utf16.end(), {'a', 0, 'b', 0});
			}
			std::vector<uint8_t> large = {5, 0, 2, 0, 0, 0, 0, 0, 0, 0};
			Put(large, 3000, 4);
			Put(large, 301, 4);
			Put(large, 1, 2);
			Put(large, 0, 2);
			Put(large, 6000, 4);
			Put(large, 302, 4);
			Put(large, 1, 2);
			Put(large, 0, 2);
			std::map<uint32_t, Page> lob_pages;
			lob_pages[300] = TextPageOf(
				11, {FragmentOf(0, {3, 0, 0, 0, 0, 0, 'a', 'b', 'c'}), FragmentOf(8, std::vector<uint8_t>(10, 0)),
					 FragmentOf(0, {0, 0, 0, 0, 0, 0}), FragmentOf(5, large)});
			lob_pages[301] = TextPageOf(11, {FragmentOf(3, utf16)});
			lob_pages[302] = TextPageOf(11, {FragmentOf(3, utf16)});
			LobPageFetch fetch = [&](const PagePointer &at, Page &out) {
				auto it = lob_pages.find(at.page);
				if (it == lob_pages.end() || at.file != 1) {
					throw FormatError("backup: no page " + at.ToString() + " in the test");
				}
				out = it->second;
			};
			LobUnits units;
			units.lob = static_cast<uint64_t>(11) << 16;
			auto root_pointer = [](uint32_t page, uint16_t slot) {
				std::vector<uint8_t> p;
				Put(p, 0x10000, 8);
				Put(p, page, 4);
				Put(p, 1, 2);
				Put(p, slot, 2);
				return p;
			};
			const Page rows = PageOf({Record(odd, 4, 0, {root_pointer(300, 0), root_pointer(300, 3), doc}, 0x3),
									  Record(odd, 4, 0, {root_pointer(300, 2), root_pointer(300, 1), doc}, 0x3),
									  Record(odd, 4, 0, {root_pointer(999, 0), root_pointer(300, 1), doc}, 0x3)});
			RecordStager pointed(t, {2, 3});
			pointed.UseLayout(layout);
			pointed.UseOffRow(&fetch, units);
			DataChunk c;
			c.Initialize(Allocator::DefaultAllocator(), pointed.Types());
			pointed.BeginChunk(c);
			pointed.Append(ReadSlot(rows.bytes.data(), PAGE_SIZE, 3, 0, "the test"));
			pointed.Append(ReadSlot(rows.bytes.data(), PAGE_SIZE, 3, 1, "the test"));
			pointed.FinalizeChunk(c);
			std::string abab;
			for (int i = 0; i < 1500; i++) {
				abab += "ab";
			}
			Check(c.GetValue(0, 0).ToString() == "abc" && c.GetValue(1, 0).ToString() == abab,
				  "behind root pointers: a SMALL_ROOT's text, an ntext of two DATA fragments (6,000 bytes)");
			Check(!c.GetValue(0, 1).IsNull() && c.GetValue(0, 1).ToString().empty() && c.GetValue(1, 1).IsNull(),
				  "an empty SMALL_ROOT is an empty value; a NULL root is NULL, its null bit clear");
			// A pointer to a page that is not there: refused by name, the chunk spoiled; a new one reads on.
			c.Reset();
			pointed.BeginChunk(c);
			Check(ErrorOf([&]() {
					  pointed.Append(ReadSlot(rows.bytes.data(), PAGE_SIZE, 3, 2, "the test"));
				  }).find("no page") != std::string::npos,
				  "a value whose page is missing is refused");
			c.Reset();
			pointed.BeginChunk(c);
			pointed.Append(ReadSlot(rows.bytes.data(), PAGE_SIZE, 3, 0, "the test"));
			pointed.FinalizeChunk(c);
			Check(c.size() == 1 && c.GetValue(0, 0).ToString() == "abc", "... and the next chunk reads on");
			// The map lists every off-row value of the row, the columns not projected included.
			RecordStager mapped(t, {2});
			mapped.UseLayout(layout);
			mapped.UseOffRow(&fetch, units);
			mapped.MapOffRow(true);
			DataChunk m;
			m.Initialize(Allocator::DefaultAllocator(), mapped.Types());
			mapped.BeginChunk(m);
			mapped.Append(ReadSlot(rows.bytes.data(), PAGE_SIZE, 3, 0, "the test"));
			const std::string map = mapped.LastRowLobMap();
			mapped.FinalizeChunk(m);
			const auto entries = ParseLobMap(reinterpret_cast<const uint8_t *>(map.data()), map.size(), "the test row");
			Check(entries.size() == 2 && entries[0].column_id == 2 && entries[1].column_id == 3 &&
					  entries[0].form == OffRowForm::RootPointer && entries[0].length == 3 &&
					  entries[1].length == 6000 && entries[1].nodes.size() == 3 &&
					  entries[1].nodes[0].fragment_type == 5 && entries[1].nodes[2].page.page == 302 &&
					  entries[1].nodes[2].offset == 3000,
				  "the map: notes and nnotes (not projected), nnotes's root and its two leaves");
		}
		RecordStager stager(t, {1, 4});
		stager.UseLayout(layout);
		Check(stager.Types()[0].id() == LogicalTypeId::BLOB && stager.Types()[1].id() == LogicalTypeId::BLOB,
			  "an unknown type and xml as BLOB");
		DataChunk chunk;
		chunk.Initialize(Allocator::DefaultAllocator(), stager.Types());
		stager.BeginChunk(chunk);
		stager.Append(ReadSlot(page.bytes.data(), PAGE_SIZE, 1, 0, "the test"));
		stager.FinalizeChunk(chunk);
		Check(chunk.GetValue(0, 0).ToString() == "\\x01\\x02\\x03\\x04\\x05\\x06" &&
				  StringValue::Get(chunk.GetValue(1, 0)).size() == 5,
			  "an unknown fixed type's 6 bytes at its offset, xml's stored bytes");
		Check(ReadingOf(t.columns[0]).note.find("does not know") != std::string::npos &&
				  SqlTypeText("", "int", XTYPE_INT, 4, 10, 0) == "int",
			  "an unknown type says so; a type with no row of its own is named by its system type");
		// The stager's own order: an Append before a BeginChunk, and vectors of another type, are refused.
		RecordStager unbegun(t, {1});
		unbegun.UseLayout(layout);
		DataChunk wrong;
		wrong.Initialize(Allocator::DefaultAllocator(), {LogicalType::INTEGER});
		Check(ErrorOf([&]() {
				  unbegun.Append(ReadSlot(page.bytes.data(), PAGE_SIZE, 1, 0, "the test"));
			  }).find("not begun") != std::string::npos &&
				  ErrorOf([&]() { unbegun.BeginChunk(wrong); }).find("vector is INTEGER") != std::string::npos,
			  "an Append with no BeginChunk, and a vector of another type, are refused");
	}

	// PR 3 S4: a value of a column the binary kernel publishes, past DIRECT_VALUE_BYTES, is written straight
	// into its output vector (PrepareRow says so: direct bytes, nothing staged), beside a staged one.
	{
		CatalogTable t;
		t.schema_name = "dbo";
		t.name = "blobs";
		t.columns = {Col(1, "id", XTYPE_INT, "int", 4), Col(2, "b", XTYPE_VARBINARY, "varbinary", -1)};
		RowsetLayout layout;
		layout.table = "blobs";
		layout.columns = {Phys(1, XTYPE_INT, 4, 1, 4), Phys(2, XTYPE_VARBINARY, -1, 2)};
		const uint32_t leaves = 150, leaf = 8000;
		std::map<uint32_t, Page> pages;
		std::vector<uint8_t> internal;
		Put(internal, leaves, 2);
		Put(internal, leaves, 2);
		Put(internal, 0, 2);  // level 0: the links name DATA fragments
		for (uint32_t k = 0; k < leaves; k++) {
			Put(internal, static_cast<uint64_t>(leaf) * (k + 1), 8);
			Put(internal, 1000 + k, 4);
			Put(internal, 1, 2);
			Put(internal, 0, 2);
			pages[1000 + k] = TextPageOf(13, {FragmentOf(3, std::vector<uint8_t>(leaf, static_cast<uint8_t>(k)))});
		}
		pages[999] = TextPageOf(13, {FragmentOf(2, internal)});
		pages[999].bytes[1] = 4;  // a text-tree page
		pages[999].header = ReadPageHeader(pages[999].bytes.data());
		LobPageFetch fetch = [&](const PagePointer &at, Page &out) { out = pages.at(at.page); };
		LobUnits units;
		units.lob = static_cast<uint64_t>(13) << 16;
		std::vector<uint8_t> root = {4, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0};  // an in-row root of level 1
		Put(root, static_cast<uint64_t>(leaf) * leaves, 4);
		Put(root, 999, 4);
		Put(root, 1, 2);
		Put(root, 0, 2);
		const std::vector<uint8_t> id1 = {1, 0, 0, 0}, id2 = {2, 0, 0, 0};
		const Page rows = PageOf({Record(id1, 2, 0, {root}, 0x1), Record(id2, 2, 0, {{'s', 'm', 'a', 'l', 'l'}})});
		RecordStager stager(t, {1, 2});
		stager.UseLayout(layout);
		stager.UseOffRow(&fetch, units);
		DataChunk c;
		c.Initialize(Allocator::DefaultAllocator(), stager.Types());
		stager.BeginChunk(c);
		const RecordView first = ReadSlot(rows.bytes.data(), PAGE_SIZE, 2, 0, "the test");
		const OffRowNeed need = stager.PrepareRow(first);
		Check(need.direct == uint64_t(leaf) * leaves && need.staged == 0 && need.map == 0,
			  "a 1.2 MB varbinary(max) value: written direct (" + std::to_string(need.direct) +
				  " bytes), nothing staged");
		stager.Append(first);
		stager.Append(ReadSlot(rows.bytes.data(), PAGE_SIZE, 2, 1, "the test"));
		stager.FinalizeChunk(c);
		const std::string value = StringValue::Get(c.GetValue(1, 0));
		bool same = value.size() == size_t(leaf) * leaves;
		for (uint32_t k = 0; same && k < leaves; k++) {
			same = value[size_t(k) * leaf] == static_cast<char>(k) &&
				   value[size_t(k) * leaf + leaf - 1] == static_cast<char>(k);
		}
		Check(same && StringValue::Get(c.GetValue(1, 1)) == "small" && c.GetValue(0, 0) == Value::INTEGER(1),
			  "... its bytes in the vector, in order, beside a staged value of the next row");
	}

	// PR 3 S4: a long UTF-16 or code-page value is decoded block by block straight into its vector; the
	// text must be what the batch kernel makes of the whole value, characters cut by a block's or a
	// fragment's end included (a surrogate pair, a double-byte pair, a lone lead byte at the end).
	{
		auto lob_of = [&](const std::vector<uint8_t> &content, uint32_t first_page, std::map<uint32_t, Page> &pages) {
			const uint32_t leaf = 8000;
			const uint32_t leaves = static_cast<uint32_t>((content.size() + leaf - 1) / leaf);
			std::vector<uint8_t> internal;
			Put(internal, leaves, 2);
			Put(internal, leaves, 2);
			Put(internal, 0, 2);
			for (uint32_t k = 0; k < leaves; k++) {
				const size_t from = size_t(k) * leaf, to = std::min(content.size(), from + leaf);
				Put(internal, to, 8);
				Put(internal, first_page + 1 + k, 4);
				Put(internal, 1, 2);
				Put(internal, 0, 2);
				pages[first_page + 1 + k] = TextPageOf(
					17,
					{FragmentOf(3, std::vector<uint8_t>(content.begin() + long(from), content.begin() + long(to)))});
			}
			pages[first_page] = TextPageOf(17, {FragmentOf(2, internal)});
			pages[first_page].bytes[1] = 4;
			pages[first_page].header = ReadPageHeader(pages[first_page].bytes.data());
			std::vector<uint8_t> root = {4, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0};
			Put(root, content.size(), 4);
			Put(root, first_page, 4);
			Put(root, 1, 2);
			Put(root, 0, 2);
			return root;
		};
		// UTF-16: 'a' then U+1F600 (a surrogate pair), so pairs straddle every block's and leaf's end;
		// every 5,000th unit a lone high surrogate before a 'b'.
		std::vector<uint8_t> utf16;
		for (size_t i = 0; utf16.size() < 1100000; i++) {
			if (i % 5000 == 4999) {
				utf16.insert(utf16.end(), {0x00, 0xD8, 'b', 0});
			}
			utf16.insert(utf16.end(), {'a', 0, 0x3D, 0xD8, 0x00, 0xDE});
		}
		// cp932: 'a' then a double-byte pair (0x82 0xA0), the pairs straddling ends; a lone lead at the end.
		std::vector<uint8_t> cp932;
		while (cp932.size() < 1100000) {
			cp932.insert(cp932.end(), {'a', 0x82, 0xA0});
		}
		cp932.push_back(0x82);
		// cp1252: every byte value.
		std::vector<uint8_t> cp1252;
		for (size_t i = 0; cp1252.size() < 1100000; i++) {
			cp1252.push_back(static_cast<uint8_t>(i * 7 + 1));
		}
		const auto *latin = mssql::codec::code_page::FindCollationByName("Latin1_General_CI_AS", 20);
		const auto *japanese = mssql::codec::code_page::FindCollationByName("Japanese_CI_AS", 14);
		CatalogTable t;
		t.schema_name = "dbo";
		t.name = "texts";
		t.columns = {Col(1, "id", XTYPE_INT, "int", 4), Col(2, "n", XTYPE_NVARCHAR, "nvarchar", -1),
					 Col(3, "j", XTYPE_VARCHAR, "varchar", -1, 0, 0, static_cast<uint32_t>(japanese->collation_id)),
					 Col(4, "l", XTYPE_VARCHAR, "varchar", -1, 0, 0, static_cast<uint32_t>(latin->collation_id))};
		RowsetLayout layout;
		layout.table = "texts";
		layout.columns = {Phys(1, XTYPE_INT, 4, 1, 4), Phys(2, XTYPE_NVARCHAR, -1, 2), Phys(3, XTYPE_VARCHAR, -2, 3),
						  Phys(4, XTYPE_VARCHAR, -3, 4)};
		std::map<uint32_t, Page> pages;
		const std::vector<uint8_t> rn = lob_of(utf16, 2000, pages), rj = lob_of(cp932, 3000, pages),
								   rl = lob_of(cp1252, 4000, pages);
		LobPageFetch fetch = [&](const PagePointer &at, Page &out) { out = pages.at(at.page); };
		LobUnits units;
		units.lob = static_cast<uint64_t>(17) << 16;
		const std::vector<uint8_t> id1 = {1, 0, 0, 0};
		// Short values after the long ones, in the same chunk: each column's kernel splits its staged values
		// around the long value's placeholder (review of PR 3 S4: a UTF-16 placeholder without its delimiter
		// cost the next value a byte).
		const std::vector<uint8_t> id2 = {2, 0, 0, 0}, id3 = {3, 0, 0, 0};
		const Page rows = PageOf({Record(id1, 4, 0, {rn, rj, rl}, 0x7),
								  Record(id2, 4, 0, {{0xE9, 0x00, 0xFC, 0x00}, {0x82, 0xA0}, {0xE9}}),
								  Record(id3, 4, 0, {{'x', 0, 0xE9, 0}, {'y', 0x82, 0xA0}, {'z', 0xFC}})});
		RecordStager stager(t, {2, 3, 4});
		stager.UseLayout(layout);
		stager.UseOffRow(&fetch, units);
		DataChunk c;
		c.Initialize(Allocator::DefaultAllocator(), stager.Types());
		stager.BeginChunk(c);
		const RecordView record = ReadSlot(rows.bytes.data(), PAGE_SIZE, 3, 0, "the test");
		const OffRowNeed need = stager.PrepareRow(record);
		Check(need.staged == 0 && need.direct == 3 * (utf16.size() / 2) +
													 mssql::codec::code_page::Utf8Bound(cp932.size()) +
													 mssql::codec::code_page::Utf8Bound(cp1252.size()),
			  "three long text values: streamed, room for their longest UTF-8 held, nothing staged");
		stager.Append(record);
		stager.Append(ReadSlot(rows.bytes.data(), PAGE_SIZE, 3, 1, "the test"));
		stager.Append(ReadSlot(rows.bytes.data(), PAGE_SIZE, 3, 2, "the test"));
		stager.FinalizeChunk(c);
		Check(c.GetValue(0, 1).ToString() == "\xC3\xA9\xC3\xBC" && c.GetValue(0, 2).ToString() == "x\xC3\xA9" &&
				  c.GetValue(1, 1).ToString() == "\xE3\x81\x82" && c.GetValue(1, 2).ToString() == "y\xE3\x81\x82" &&
				  c.GetValue(2, 1).ToString() == "\xC3\xA9" && c.GetValue(2, 2).ToString() == "z\xC3\xBC",
			  "short non-ASCII values after the streamed ones, in the same chunk, read whole");
		// The batch kernels' own functions over each whole value.
		std::string want_n(utf16.size() * 3 / 2 + 16, '\0');
		idx_t replaced = 0;
		want_n.resize(mssql::codec::string::DecodeUtf16Block(utf16.data(), utf16.size() / 2, &want_n[0], replaced));
		const auto *page932 = mssql::codec::code_page::FindCodePage(932);
		const auto *page1252 = mssql::codec::code_page::FindCodePage(1252);
		std::string want_j(mssql::codec::code_page::Utf8Bound(cp932.size()), '\0');
		want_j.resize(mssql::codec::code_page::DecodeToUtf8(*page932, cp932.data(), cp932.size(), &want_j[0]));
		std::string want_l(mssql::codec::code_page::Utf8Bound(cp1252.size()), '\0');
		want_l.resize(mssql::codec::code_page::DecodeToUtf8(*page1252, cp1252.data(), cp1252.size(), &want_l[0]));
		Check(replaced > 0 && StringValue::Get(c.GetValue(0, 0)) == want_n,
			  "UTF-16 streamed: the whole value's text (pairs cut by block ends, lone surrogates replaced)");
		Check(StringValue::Get(c.GetValue(1, 0)) == want_j,
			  "cp932 streamed: the whole value's text (pairs cut by block ends, the lone last lead read as nothing)");
		Check(StringValue::Get(c.GetValue(2, 0)) == want_l, "cp1252 streamed: the whole value's text");
	}

	std::cout << (failures ? "FAILED" : "PASSED") << " (" << failures << " failure(s))" << std::endl;
	return failures ? 1 : 0;
}
