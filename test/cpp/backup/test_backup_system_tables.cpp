// Spec 085 PR 2b step 5 (task 11): the system base tables of the committed samples (synthetic, SQL
// Server 2025). The five bootstrap tables read through their literal layouts, which agree with the
// tables' own description of themselves; any of the five read again through that description gives
// the same values; the samples' two user tables, their columns and their rowsets found; a system
// table read through its own description; and the decoder's rules for a column added after a record
// was written, and a literal the database describes otherwise, refused by name.

#include <functional>
#include <iostream>
#include <set>
#include <string>
#include <vector>

#include "backup/backup_opened.hpp"
#include "backup/backup_system_tables.hpp"
#include "duckdb.hpp"

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

const std::string DIR = "test/data/backup/";

BootstrapTables Bootstrap(const OpenedPages &pages) {
	Page boot;
	pages.store->ReadLive(1, 9, boot, "the test");
	return ReadBootstrapTables(*pages.store, ReadBootPage(boot));
}

const SysObject *UserTable(const BootstrapTables &t, const std::string &name) {
	for (const auto &o : t.objects) {
		if (o.type == "U " && o.name == name) {
			return &o;
		}
	}
	return nullptr;
}

// Days from 1900-01-01 to y-m-d (proleptic Gregorian; Hinnant's days_from_civil).
int64_t DaysFrom1900(int64_t y, unsigned m, unsigned d) {
	auto civil = [](int64_t yy, unsigned mm, unsigned dd) {
		yy -= mm <= 2;
		const int64_t era = (yy >= 0 ? yy : yy - 399) / 400;
		const unsigned yoe = static_cast<unsigned>(yy - era * 400);
		const unsigned doy = (153 * (mm + (mm > 2 ? -3 : 9)) + 2) / 5 + dd - 1;
		const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
		return era * 146097 + static_cast<int64_t>(doe);
	};
	return civil(y, m, d) - civil(1900, 1, 1);
}

// A page of 8 KB holding one record at offset 96 (slot 0), for the decoder's synthetic cases.
std::vector<uint8_t> PageWith(const std::vector<uint8_t> &record) {
	std::vector<uint8_t> page(PAGE_SIZE, 0);
	std::copy(record.begin(), record.end(), page.begin() + 96);
	page[PAGE_SIZE - 2] = 96;
	return page;
}

void PutU16(std::vector<uint8_t> &b, uint16_t v) {
	b.push_back(static_cast<uint8_t>(v));
	b.push_back(static_cast<uint8_t>(v >> 8));
}

// A forwarded record (type 1): int `id`, then the variable columns `vars` and a back pointer
// (`tag`, 10 bytes, complex) last; column 2 NULL in the bitmap when `v_null`.
std::vector<uint8_t> Forwarded(int32_t id, const std::vector<std::string> &vars, bool v_null, uint16_t tag) {
	std::vector<uint8_t> r = {0x32, 0x00};	// type 1, null bitmap, variable columns
	PutU16(r, 8);							// the fixed part ends at 8
	for (int i = 0; i < 4; i++) {
		r.push_back(static_cast<uint8_t>(id >> (8 * i)));
	}
	PutU16(r, 2);  // two columns
	r.push_back(v_null ? 0x02 : 0x00);
	const uint16_t count = static_cast<uint16_t>(vars.size() + 1);
	PutU16(r, count);
	uint16_t end = static_cast<uint16_t>(r.size() + 2 * count);
	for (const auto &v : vars) {
		end = static_cast<uint16_t>(end + v.size());
		PutU16(r, end);
	}
	PutU16(r, static_cast<uint16_t>((end + 10) | 0x8000));
	for (const auto &v : vars) {
		r.insert(r.end(), v.begin(), v.end());
	}
	PutU16(r, tag);
	for (int i = 0; i < 8; i++) {
		r.push_back(static_cast<uint8_t>(i + 1));  // the original RID
	}
	return r;
}

std::vector<std::string> ColumnNames(const RowsetLayout &layout) {
	std::vector<std::string> names;
	for (const auto &c : layout.columns) {
		names.push_back(c.name);
	}
	return names;
}

}  // namespace

int main() {
	std::cout << "System base tables (spec 085 PR 2b step 5)" << std::endl;
	DuckDB db(nullptr);
	Connection con(db);
	auto &context = *con.context;

	const std::vector<std::vector<std::string>> forms = {{"full.bak"},
														 {"full_xpress.bak"},
														 {"full_zstd.bak"},
														 {"stripe_1.bak", "stripe_2.bak"},
														 {"stripe_zstd_1.bak", "stripe_zstd_2.bak"}};
	for (const auto &form : forms) {
		std::vector<std::string> paths;
		for (const auto &f : form) {
			paths.push_back(DIR + f);
		}
		auto headers = OpenBackupHeaders(context, paths);
		auto pages = OpenBackupPages(context, *headers);
		BootstrapTables t;
		const std::string error = ErrorOf([&]() { t = Bootstrap(*pages); });
		const SysObject *items = UserTable(t, "items");
		const SysObject *events = UserTable(t, "events");
		Check(error.empty() && t.allocation_units.size() > 100 && t.objects.size() > 50 && items && events &&
				  items->schema_id == 1 && events->schema_id == 1,
			  form.front() + ": the bootstrap tables read and agree with their own description (" +
				  std::to_string(t.allocation_units.size()) + " units, " + std::to_string(t.objects.size()) +
				  " objects); dbo.items and dbo.events found" + (error.empty() ? "" : " -- " + error));
		if (!error.empty() || !items || !events) {
			continue;
		}
		const RowsetLayout li = t.BaseLayout(items->id);
		const RowsetLayout le = t.BaseLayout(events->id);
		const std::vector<std::string> want_items = {"id", "name", "code", "price", "created", "flag", "blob"};
		const std::vector<std::string> want_events = {"event_id", "item_id", "note", "at"};
		Check(ColumnNames(li) == want_items && ColumnNames(le) == want_events,
			  form.front() + ": the user tables' physical columns, named by syscolpars, in rscolid order");
		int32_t items_index = -1, events_index = -1;
		int64_t items_rows = -1, events_rows = -1;
		for (const auto &r : t.rowsets) {
			if (r.rowset_id == li.rowset_id) {
				items_index = r.index_id;
				items_rows = r.rows;
			}
			if (r.rowset_id == le.rowset_id) {
				events_index = r.index_id;
				events_rows = r.rows;
			}
		}
		Check(items_index == 1 && events_index == 0 && items_rows == 2000 && events_rows == 3000,
			  form.front() + ": items' base rowset is its clustered index (2000 rows), events' a heap (3000 rows)");
		const PhysicalColumn *flag = li.Find("flag");
		const PhysicalColumn *note = le.Find("note");
		Check(flag && flag->xtype == XTYPE_BIT && flag->offset > 0 && note && note->xtype == XTYPE_VARCHAR &&
				  note->offset < 0 && note->length == 100,
			  form.front() + ": a bit column has a fixed offset, varchar(100) a variable index and 100 bytes");
	}

	auto headers = OpenBackupHeaders(context, {DIR + "full.bak"});
	auto pages = OpenBackupPages(context, *headers);
	const BootstrapTables t = Bootstrap(*pages);

	// Each bootstrap table read again through its own description gives the literals' values.
	{
		const SystemTable units = ReadSystemTable(*pages->store, t, "sysallocunits");
		std::set<std::pair<uint64_t, std::string>> generic, literal;
		const size_t auid = units.Column("auid"), iam = units.Column("pgfirstiam");
		for (size_t r = 0; r < units.rows.size(); r++) {
			generic.insert(std::make_pair(static_cast<uint64_t>(units.Int(r, auid)), units.Pointer(r, iam).ToString()));
		}
		for (const auto &a : t.allocation_units) {
			literal.insert(std::make_pair(a.auid, a.first_iam.ToString()));
		}
		Check(generic == literal && units.layout.Find("pcreserved") != nullptr,
			  "sysallocunits through its own description: the same units and first IAM pages, and the columns the "
			  "literals skip (pcreserved)");
		const SystemTable rscols = ReadSystemTable(*pages->store, t, "sysrscols");
		Check(rscols.rows.size() == t.rscols.size() && rscols.layout.Find("ordlock") != nullptr,
			  "sysrscols through its own description: every row, and 2022's ordlock");
		const SystemTable objects = ReadSystemTable(*pages->store, t, "sysschobjs");
		std::set<std::string> a, b;
		const size_t name = objects.Column("name");
		for (size_t r = 0; r < objects.rows.size(); r++) {
			a.insert(objects.Text(r, name));
		}
		for (const auto &o : t.objects) {
			b.insert(o.name);
		}
		Check(a == b, "sysschobjs through its own description: the same names");
	}

	// A system table the bootstrap does not read, through its own description: schema 1 is dbo.
	{
		const SystemTable classes = ReadSystemTable(*pages->store, t, "sysclsobjs");
		const size_t cls = classes.Column("class"), id = classes.Column("id"), name = classes.Column("name");
		bool dbo = false, sys = false;
		for (size_t r = 0; r < classes.rows.size(); r++) {
			if (classes.Int(r, cls) == 50 && classes.Int(r, id) == 1 && classes.Text(r, name) == "dbo") {
				dbo = true;
			}
			if (classes.Int(r, cls) == 50 && classes.Int(r, id) == 4 && classes.Text(r, name) == "sys") {
				sys = true;
			}
		}
		Check(dbo && sys, "sysclsobjs: schema 1 is dbo, schema 4 sys (class 50)");
		Check(ErrorOf([&]() { ReadSystemTable(*pages->store, t, "sysnosuchtable"); }).find("no system table") !=
				  std::string::npos,
			  "a system table the database lacks is refused by name");
	}

	// A literal the database describes otherwise is refused, naming the field.
	{
		BootstrapTables moved = t;
		const RowsetLayout objects = t.BaseLayout(t.SystemObject("sysschobjs")->id);
		for (auto &rc : moved.rscols) {
			if (rc.rowset_id == objects.rowset_id && rc.rscolid == objects.Find("type")->rscolid) {
				rc.offset = 18;
			}
		}
		const std::string e = ErrorOf([&]() { CheckBootstrapLayouts(moved); });
		Check(e.find("sysschobjs.type") != std::string::npos && e.find("offset 18") != std::string::npos,
			  "a sysschobjs.type the database puts at another offset is refused by name: " + e);
		Check(ErrorOf([&]() { CheckBootstrapLayouts(t); }).empty(), "the samples' own description passes");
	}

	// The heap dbo.events, decoded through its own layout: bigint, int, varchar (NULL every fourth row)
	// and datetime, as samples.sql wrote them (i, 1 + i % 2000, 'event i', 2026-02-01 + i minutes).
	{
		const SysObject *events = UserTable(t, "events");
		const RowsetLayout layout = t.BaseLayout(events->id);
		const std::vector<SystemRow> rows =
			ReadRowsetRows(*pages->store, t.InRowUnit(layout.rowset_id, "events"), layout);
		SystemTable table;
		table.layout = layout;
		table.rows = rows;
		const size_t event_id = table.Column("event_id"), item_id = table.Column("item_id"),
					 note = table.Column("note"), at = table.Column("at");
		std::set<int64_t> ids;
		size_t wrong = 0;
		const int64_t base_day = DaysFrom1900(2026, 2, 1);
		for (size_t r = 0; r < rows.size(); r++) {
			const int64_t i = table.Int(r, event_id);
			ids.insert(i);
			const SystemValue &when = rows[r].values[at];
			ByteReader w(reinterpret_cast<const uint8_t *>(when.bytes.data()), when.bytes.size(), "at");
			const int64_t minutes = i % 1440, days = base_day + i / 1440;
			const bool ok =
				table.Int(r, item_id) == 1 + i % 2000 &&
				(i % 4 == 0 ? table.IsNull(r, note) : table.Text(r, note) == "event " + std::to_string(i)) &&
				when.bytes.size() == 8 && w.I32(4) == days && w.U32(0) == minutes * 60 * 300;
			wrong += ok ? 0 : 1;
		}
		Check(rows.size() == 3000 && ids.size() == 3000 && *ids.begin() == 1 && *ids.rbegin() == 3000 && wrong == 0,
			  "dbo.events through its own layout: 3000 rows, every bigint, int, varchar (NULL by the bitmap) and "
			  "datetime as written (" +
				  std::to_string(wrong) + " wrong)");
	}

	// A forwarded record: its last variable column, the back pointer, is not a column; a trailing
	// variable column left out (NULL) before it; a record whose last column is not one is refused.
	{
		RowsetLayout layout;
		layout.table = "t";
		PhysicalColumn id;
		id.name = "id";
		id.xtype = XTYPE_INT;
		id.offset = 4;
		id.null_bit = 1;
		PhysicalColumn v;
		v.name = "v";
		v.xtype = XTYPE_VARCHAR;
		v.offset = -1;
		v.null_bit = 2;
		layout.columns = {id, v};
		auto decode = [&](const std::vector<uint8_t> &record) {
			const std::vector<uint8_t> page = PageWith(record);
			return DecodeRecord(ReadSlot(page.data(), PAGE_SIZE, 1, 0, "t"), layout);
		};
		const auto with = decode(Forwarded(7, {"ab"}, false, 0x0400));
		Check(ValueInt(layout, 0, with[0]) == 7 && ValueText(layout, 1, with[1]) == "ab" && !with[1].off_row,
			  "a forwarded record: its columns read, the back pointer left out");
		const auto trailing = decode(Forwarded(8, {}, true, 0x0400));
		Check(ValueInt(layout, 0, trailing[0]) == 8 && trailing[1].is_null,
			  "a forwarded record whose trailing variable column is NULL: only the back pointer follows");
		Check(ErrorOf([&]() { decode(Forwarded(9, {"ab"}, false, 0x0401)); }).find("no back pointer") !=
				  std::string::npos,
			  "a forwarded record whose last column is not tagged as a back pointer is refused");
		std::vector<uint8_t> plain = Forwarded(10, {"ab"}, false, 0x0400);
		// The back pointer's end offset is the u16 just before the data ("ab" and 10 bytes): its high
		// byte, 13 bytes from the end, loses the complex bit.
		plain[plain.size() - 13] &= 0x7F;
		const std::vector<uint8_t> page = PageWith(plain);
		std::string e;
		try {
			DecodeRecord(ReadSlot(page.data(), PAGE_SIZE, 1, 0, "t"), layout);
		} catch (std::exception &x) {
			e = x.what();
		}
		Check(e.find("no back pointer") != std::string::npos,
			  "a forwarded record whose last column is not complex is refused");
	}

	// A column added after the record was written: its default, NULL, or refused.
	{
		const SysAllocUnit &unit = t.InRowUnit(t.BaseLayout(t.SystemObject("sysrowsets")->id).rowset_id, "sysrowsets");
		RowsetLayout layout = BootstrapLayout("sysrowsets");
		PhysicalColumn added;
		added.name = "added";
		added.xtype = XTYPE_INT;
		added.offset = 4;
		added.null_bit = 200;  // past every record's column count
		layout.columns.push_back(added);
		const std::vector<SystemRow> plain = ReadRowsetRows(*pages->store, unit, layout);
		Check(!plain.empty() && plain[0].values.back().is_null, "an added column with no default reads NULL");
		layout.columns.back().status = 0x200;
		Check(ErrorOf([&]() {
				  ReadRowsetRows(*pages->store, unit, layout);
			  }).find("default the reader was not given") != std::string::npos,
			  "an added column with the default status and no default given is refused");
		layout.columns.back().has_default = true;
		layout.columns.back().default_bytes = std::string("\x07\x00\x00\x00", 4);
		const std::vector<SystemRow> with = ReadRowsetRows(*pages->store, unit, layout);
		Check(!with.empty() && !with[0].values.back().is_null &&
				  with[0].values.back().bytes == layout.columns.back().default_bytes,
			  "an added column with a default reads it");
		layout.columns.back() = added;
		layout.columns.back().null_bit = 1;
		layout.columns.back().offset = 8000;
		Check(ErrorOf([&]() { ReadRowsetRows(*pages->store, unit, layout); }).find("lies past the fixed part") !=
				  std::string::npos,
			  "a fixed column past the record's fixed part is refused");
	}

	std::cout << (failures ? "FAILED" : "PASSED") << " (" << failures << " failure(s))" << std::endl;
	return failures ? 1 : 0;
}
