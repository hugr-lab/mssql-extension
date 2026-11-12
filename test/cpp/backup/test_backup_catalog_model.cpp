// Spec 085 PR 2b step 6 (task 12): the catalog model of the committed samples (synthetic, SQL Server
// 2025), against what samples.sql created: schemas, the two user tables (shape, rows, columns with
// their types, nullability, collations), no views; and the model's decoders on the bytes measured on
// the catalog fixture (2022): sql_variant values of each form and a module's text in its two forms.
// The catalog fixture itself (identity, computed, sparse, alias types, encrypted and long views, the
// defaults of added columns) is not committed: a local script compared the model with the server's own
// catalog of it (spec 085 § Rules, catalog); the oracle lane compares it through the file functions (task
// 17's compare_catalog.py).

#include <functional>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

#include "backup/backup_catalog_model.hpp"
#include "backup/backup_collation.hpp"
#include "backup/backup_function_common.hpp"
#include "backup/backup_opened.hpp"
#include "backup/backup_sql_variant.hpp"
#include "codec/code_page/code_page.hpp"
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

std::string Bytes(const std::vector<int> &v) {
	std::string s;
	for (int b : v) {
		s.push_back(static_cast<char>(b));
	}
	return s;
}

const std::string DIR = "test/data/backup/";

struct Want {
	const char *name;
	const char *type;
	int16_t max_length;
	bool nullable;
	const char *collation;	// "" for none
};

}  // namespace

int main() {
	std::cout << "The catalog model (spec 085 PR 2b step 6)" << std::endl;
	DuckDB db(nullptr);
	Connection con(db);
	auto &context = *con.context;

	const std::vector<std::vector<std::string>> forms = {
		{"full.bak"}, {"full_xpress.bak"}, {"stripe_1.bak", "stripe_2.bak"}};
	for (const auto &form : forms) {
		std::vector<std::string> paths;
		for (const auto &f : form) {
			paths.push_back(DIR + f);
		}
		auto headers = OpenBackupHeaders(context, paths);
		auto pages = OpenBackupPages(context, *headers);
		Page boot;
		pages->store->ReadLive(1, 9, boot, "the test");
		CatalogModel m;
		const std::string error = ErrorOf(
			[&]() { m = BuildCatalogModel(*pages->store, ReadBootstrapTables(*pages->store, ReadBootPage(boot))); });
		const CatalogTable *items = m.FindTable("dbo", "items");
		const CatalogTable *events = m.FindTable("dbo", "events");
		Check(error.empty() && m.tables.size() == 2 && items && events && m.views.empty() && m.SchemaName(1) == "dbo" &&
				  m.SchemaName(4) == "sys",
			  form.front() +
				  ": dbo.items and dbo.events, no other user table (the MS-shipped trace_xe_* left out), "
				  "no view; schemas dbo and sys" +
				  (error.empty() ? "" : " -- " + error));
		if (!items || !events || items->columns.size() != 7 || items->partitions.size() != 1) {
			continue;
		}
		Check(items->shape == TableShape::Clustered && items->row_count == 2000 && items->partitions.size() == 1 &&
				  events->shape == TableShape::Heap && events->row_count == 3000 && items->decodable &&
				  events->decodable && items->reason.empty(),
			  form.front() +
				  ": items clustered, 2000 rows; events a heap, 3000 rows; one partition each, both decodable");
		const std::vector<Want> want_items = {{"id", "int", 4, false, ""},
											  {"name", "nvarchar", 80, false, "SQL_Latin1_General_CP1_CI_AS"},
											  {"code", "varchar", 12, true, "SQL_Latin1_General_CP1_CI_AS"},
											  {"price", "decimal", 9, false, ""},
											  {"created", "datetime2", 7, false, ""},
											  {"flag", "bit", 1, false, ""},
											  {"blob", "varbinary", 64, true, ""}};
		bool columns_ok = items->columns.size() == want_items.size();
		for (size_t i = 0; columns_ok && i < want_items.size(); i++) {
			const CatalogColumn &c = items->columns[i];
			const Want &w = want_items[i];
			const CollationRow *row = c.collation_id ? CollationById(static_cast<int32_t>(c.collation_id)) : nullptr;
			const std::string collation = row ? row->name : "";
			columns_ok = c.column_id == static_cast<int32_t>(i + 1) && c.name == w.name && c.type_name == w.type &&
						 c.max_length == w.max_length && c.is_nullable == w.nullable && collation == w.collation &&
						 !c.is_identity && !c.is_computed && !c.is_sparse;
		}
		const CatalogColumn &price = items->columns[3];
		Check(columns_ok && price.precision == 12 && price.scale == 2 && items->columns[4].scale == 3,
			  form.front() +
				  ": items' columns as samples.sql declared them (names, sys.types names, lengths, "
				  "nullability, collations, decimal(12, 2), datetime2(3))");
		size_t in_row = 0;
		for (const auto &u : items->units) {
			in_row += u.unit.type == 1 && u.index_id == 1 ? 1 : 0;
		}
		Check(in_row == 1 && items->layouts.size() == 1 && items->Layout(items->partitions[0]).columns.size() == 7 &&
				  items->Layout(items->partitions[0]).rowset_id == 0 && items->partitions[0].rowset_id != 0,
			  form.front() +
				  ": items' clustered index has one in-row unit, and its layout seven physical columns (the shared "
				  "layout's rowset id 0, "
				  "the partition's its own)");
	}

	// sql_variant: the bytes of the catalog fixture's defaults (2022).
	{
		const SqlVariant i = ParseSqlVariant(Bytes({0x38, 0x01, 0x2a, 0, 0, 0}), "int");
		const SqlVariant s =
			ParseSqlVariant(Bytes({0xa7, 0x01, 0x0a, 0x00, 0x08, 0xd0, 0x00, 0x34, 'a', 'b', 'c'}), "varchar");
		const SqlVariant d = ParseSqlVariant(Bytes({0x6a, 0x01, 0x09, 0x02, 0x01, 0x7d, 0, 0, 0}), "decimal");
		const SqlVariant t =
			ParseSqlVariant(Bytes({0x2a, 0x01, 0x03, 0x2e, 0x8b, 0xa8, 0x00, 0x21, 0x49, 0x0b}), "datetime2");
		const SqlVariant b = ParseSqlVariant(Bytes({0xa5, 0x01, 0x08, 0x00, 0x01, 0x02}), "varbinary");
		Check(
			i.xtype == 56 && SqlVariantInt(i, "int") == 42 && s.xtype == 167 && s.max_length == 10 &&
				s.collation == 0x3400d008 && s.value == "abc" && d.precision == 9 && d.scale == 2 &&
				d.value == Bytes({0x01, 0x7d, 0, 0, 0}) && t.scale == 3 && t.value.size() == 7 && b.max_length == 8 &&
				b.value == Bytes({0x01, 0x02}),
			"sql_variant: int, varchar (length, collation), decimal (precision, scale), datetime2 (scale), varbinary");
		Check(
			ErrorOf([&]() {
				ParseSqlVariant(Bytes({0x38, 0x02, 0, 0, 0, 0}), "v");
			}).find("version 2") != std::string::npos &&
				ErrorOf([&]() {
					ParseSqlVariant(Bytes({0xf0, 0x01}), "v");
				}).find("xtype 240") != std::string::npos &&
				!ErrorOf([&]() {
					 ParseSqlVariant(Bytes({0xa7, 0x01, 0x0a}), "v");
				 }).empty() &&
				ErrorOf([&]() { SqlVariantInt(s, "v"); }).find("not an integer") != std::string::npos,
			"sql_variant: another version, an unknown base type, short properties and a non-integer read are refused");
	}

	// A module's text: Latin-1 bytes under flag 0x2, else UTF-16LE.
	{
		Check(ModuleText(Bytes({'c', 'a', 'f', 0xe9}), 2, "t") == "caf\xc3\xa9" &&
				  ModuleText(Bytes({0x1a, 0x04, 0xac, 0x20, 0, 0}), 0, "t") == std::string("\xd0\x9a\xe2\x82\xac\0", 6),
			  "a module's text: Latin-1 under flag 0x2 (é), UTF-16LE otherwise (К, the euro sign, a NUL kept)");
		Check(ErrorOf([&]() {
				  ModuleText(Bytes({'a', 'b', 'c'}), 0, "t");
			  }).find("odd number") != std::string::npos,
			  "a module's text stored as UTF-16 with an odd number of bytes is refused");
	}

	// The sysobjvalues rows that may leave (DQ-11), over a synthetic catalog: a view, an encrypted view,
	// a default, a procedure, a trigger, a multi-statement function whose return table has a computed
	// column, a user table with one, an MS-shipped view, and partition function 65536.
	{
		BootstrapTables t;
		auto object = [&](int32_t id, const char *type, int32_t status) {
			SysObject o;
			o.id = id;
			o.type = type;
			o.status = status;
			t.objects.push_back(o);
		};
		object(100, "V ", 0);
		object(101, "V ", VIEW_ENCRYPTED);
		object(102, "D ", 0);
		object(103, "P ", 0);
		object(104, "TR", 0);
		object(105, "TF", VIEW_ENCRYPTED);
		object(106, "U ", 0);
		object(107, "V ", OBJECT_MS_SHIPPED);
		auto column = [&](int32_t object_id, int32_t id, int32_t status) {
			SysColumn c;
			c.object_id = object_id;
			c.column_id = id;
			c.status = status;
			t.columns.push_back(c);
		};
		column(105, 2, COLUMN_COMPUTED);
		column(106, 2, COLUMN_COMPUTED);
		column(106, 3, 0);
		const ObjectValueFilter f(t, {65536});
		Check(f.Keep(1, 100, 0, 0) && f.Keep(1, 102, 0, 0) && f.Keep(2, 106, 2, 0) && f.Keep(30, 65536, 1, 1) &&
				  f.Keep(30, 65536, 3, 1),
			  "DQ-11 serves: a view's and a default's definition, a user table's computed column, the boundaries "
			  "of a partition function");
		Check(!f.Keep(1, 101, 0, 0) && !f.Keep(1, 103, 0, 0) && !f.Keep(1, 104, 0, 0) && !f.Keep(1, 105, 0, 0) &&
				  !f.Keep(1, 107, 0, 0) && !f.Keep(2, 105, 2, 0) && !f.Keep(2, 106, 3, 0) && !f.Keep(30, 999, 1, 1) &&
				  !f.Keep(60, 106, 1, 0) && !f.Keep(1, 100, 1, 0) && !f.Keep(1, 100, 0, 1) && !f.Keep(2, 106, 2, 1),
			  "DQ-11 keeps back: an encrypted view, a procedure, a trigger, a function and its return table's "
			  "computed column, an MS-shipped view, a plain column, another object's boundaries, another class, "
			  "another subobjid or valnum");
	}

	// A sql_variant's value as TYPE mssql reads its base type (D6): the partition boundaries of the
	// partitioning fixture (2022), byte for byte.
	{
		auto text = [](const std::vector<int> &bytes) {
			return SqlVariantValue(ParseSqlVariant(Bytes(bytes), "v"), "v");
		};
		auto show = [](const Value &v) { return v.type().ToString() + " " + v.ToString(); };
		const Value dt2 = text({0x2a, 0x01, 0x07, 0xff, 0xbf, 0x69, 0x2a, 0xc9, 0x3e, 0x49, 0x0b});
		const Value dto = text({0x2b, 0x01, 0x00, 0x60, 0x35, 0x01, 0x3e, 0x49, 0x0b, 0x78, 0x00});
		const Value dt = text({0x3d, 0x01, 0xff, 0x81, 0x8b, 0x01, 0xe3, 0xb3, 0x00, 0x00});
		const Value sdt = text({0x3a, 0x01, 0x9f, 0x05, 0xe3, 0xb3});
		const Value tm = text({0x29, 0x01, 0x03, 0xf4, 0x2f, 0x93, 0x02});
		const Value dec = text({0x6a, 0x01, 0x0a, 0x02, 0x00, 0x0d, 0x02, 0, 0, 0, 0, 0, 0});
		const Value guid = text({0x24, 0x01, 0xff, 0x19, 0x96, 0x6f, 0x86, 0x8b, 0x11, 0xd0, 0xb4, 0x2d, 0x00, 0xc0,
								 0x4f, 0xc9, 0x64, 0xff});
		const Value bin = text({0xa5, 0x01, 0x08, 0x00, 0xff, 0x00, 0xff});
		const Value vc = text({0xa7, 0x01, 0x0a, 0x00, 0x08, 0xd0, 0x00, 0x34, 0x47});
		const Value nvc = text({0xe7, 0x01, 0x28, 0x00, 0x08, 0xd0, 0x00, 0x34, 0x16, 0x04, 0x43, 0x04, 0x3a, 0x04});
		const Value tiny = text({0x30, 0x01, 0xff});
		Check(show(dt2) == "TIMESTAMP_NS 2026-01-31 23:59:59.9999999" &&
				  dto.type().id() == LogicalTypeId::TIMESTAMP_TZ &&
				  dto.GetValue<timestamp_tz_t>().value ==
					  Timestamp::FromDatetime(Date::FromDate(2026, 1, 31), dtime_t(22LL * 3600 * 1000000)).value &&
				  show(dt) == "TIMESTAMP 2026-01-31 23:59:59.996666" && show(sdt) == "TIMESTAMP 2026-01-31 23:59:00" &&
				  show(tm) == "TIME 12:00:00.5" && show(dec) == "DECIMAL(10,2) -5.25" &&
				  show(guid) == "UUID 6f9619ff-8b86-d011-b42d-00c04fc964ff" && bin.type().id() == LogicalTypeId::BLOB &&
				  bin.GetValueUnsafe<string_t>().GetString() == std::string("\xff\x00\xff", 3) &&
				  show(vc) == "VARCHAR G" && show(nvc) == "VARCHAR \xd0\x96\xd1\x83\xd0\xba" &&
				  show(tiny) == "UTINYINT 255",
			  "sql_variant values as D6 types: datetime2(7) to the 100 ns, datetimeoffset at its UTC instant, "
			  "datetime and smalldatetime in record form, time(3), a negative decimal, a GUID in SQL Server's "
			  "order, varbinary, a code-page varchar, a Cyrillic nvarchar, tinyint (" +
				  show(dt2) + ", " + show(tm) + ")");
		// Refused by name, never a wrong or NULL value (D13): a datetime2(7) past TIMESTAMP_NS's 1677-2262
		// (9999-12-31, a common sentinel; 0001-01-01), a decimal with more digits than its precision.
		Check(ErrorOf([&]() {
				  text({0x2a, 0x01, 0x07, 0, 0, 0, 0, 0, 0xda, 0xb9, 0x37});
			  }).find("1677-2262") != std::string::npos &&
				  ErrorOf([&]() {
					  text({0x2a, 0x01, 0x07, 0, 0, 0, 0, 0, 0, 0, 0});
				  }).find("1677-2262") != std::string::npos &&
				  ErrorOf([&]() {
					  text({0x6a, 0x01, 0x02, 0x00, 0x01, 0x64, 0, 0, 0});
				  }).find("more than 2 digits") != std::string::npos &&
				  text({0x6a, 0x01, 0x02, 0x00, 0x01, 0x63, 0, 0, 0}).ToString() == "99",
			  "a datetime2(7) at 9999-12-31 or 0001-01-01 and a decimal(2, 0) of 100 are refused; 99 is not");
		Check(ErrorOf([&]() {
				  text({0xa7, 0x01, 0x01, 0x00, 0x05, 0x00, 0x00, 0x7f, 0x47});
			  }).find("does not know") != std::string::npos,
			  "a varchar of an unknown collation is refused by name");
		// A double-byte page is decoded on the client like any other (a backup has no server to do it).
		const CollationRow *japanese = mssql::codec::code_page::FindCollationByName("Japanese_CI_AS", 14);
		std::string got = "no Japanese_CI_AS";
		if (japanese) {
			const uint32_t id = static_cast<uint32_t>(japanese->collation_id);
			got = text({0xa7, 0x01, 0x04, 0x00, static_cast<int>(id & 0xff), static_cast<int>((id >> 8) & 0xff),
						static_cast<int>((id >> 16) & 0xff), static_cast<int>(id >> 24), 0x93, 0xfa, 0x96, 0x7b})
					  .ToString();
		}
		Check(got == "\xE6\x97\xA5\xE6\x9C\xAC", "a varchar of code page 932 (Japanese_CI_AS) decodes: " + got);
	}

	// More sql_variant values: money in record form (a plain little-endian i64 of 1/10,000; TDS sends the
	// high word first), and the values outside a type's range, refused by name (review of #551).
	{
		auto text = [](const std::vector<int> &bytes) {
			return SqlVariantValue(ParseSqlVariant(Bytes(bytes), "v"), "v");
		};
		const Value money = text({0x3c, 0x01, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f});
		const Value small = text({0x7a, 0x01, 0x30, 0x75, 0x00, 0x00});
		Check(money.ToString() == "922337203685477.5807" && small.ToString() == "3.0000",
			  "money and smallmoney: the record form's little-endian integer of 1/10,000 (" + money.ToString() + ", " +
				  small.ToString() + ")");
		// time(0) is 3 bytes of seconds: 86400 is midnight of the next day, past the type.
		Check(ErrorOf([&]() {
				  text({0x29, 0x01, 0x00, 0x80, 0x51, 0x01});
			  }).find("past midnight") != std::string::npos &&
				  text({0x29, 0x01, 0x00, 0x7f, 0x51, 0x01}).ToString() == "23:59:59",
			  "a time past midnight is refused (86400 s), 86399 s is not");
		// time(7): units of 100 ns; 1 has a 100 ns digit, 10 does not.
		Check(ErrorOf([&]() {
				  text({0x29, 0x01, 0x07, 0x01, 0, 0, 0, 0});
			  }).find("100 ns") != std::string::npos &&
				  text({0x29, 0x01, 0x07, 0x0a, 0, 0, 0, 0}).ToString() == "00:00:00.000001",
			  "a time(7) with a 100 ns digit is refused, not cut to the microsecond");
		// datetimeoffset(7) at 2026-01-31 (day 739646 = 0x0b4a3e) +00:00, with a 100 ns digit; and one at
		// +15:00 (900 minutes).
		Check(ErrorOf([&]() {
				  text({0x2b, 0x01, 0x07, 0x01, 0, 0, 0, 0, 0x3e, 0x4a, 0x0b, 0x00, 0x00});
			  }).find("100 ns") != std::string::npos &&
				  ErrorOf([&]() {
					  text({0x2b, 0x01, 0x07, 0x0a, 0, 0, 0, 0, 0x3e, 0x4a, 0x0b, 0x84, 0x03});
				  }).find("-14:00..+14:00") != std::string::npos,
			  "a datetimeoffset(7) with a 100 ns digit, and one at +15:00, are refused");
		// nvarchar: a high surrogate alone (D800) is refused; a pair (D83D DE00) is one character.
		Check(ErrorOf([&]() {
				  text({0xe7, 0x01, 0x10, 0x00, 0x08, 0xd0, 0x00, 0x34, 0x00, 0xd8, 0x41, 0x00});
			  }).find("unpaired surrogate") != std::string::npos &&
				  text({0xe7, 0x01, 0x10, 0x00, 0x08, 0xd0, 0x00, 0x34, 0x3d, 0xd8, 0x00, 0xde}).ToString() ==
					  "\xf0\x9f\x98\x80",
			  "an nvarchar with an unpaired surrogate is refused, not given U+FFFD; a pair is read");
		// date: 3652058 (0x37b9da) is 9999-12-31, 3652059 past it.
		Check(ErrorOf([&]() {
				  text({0x28, 0x01, 0xdb, 0xb9, 0x37});
			  }).find("past 9999-12-31") != std::string::npos &&
				  text({0x28, 0x01, 0xda, 0xb9, 0x37}).ToString() == "9999-12-31",
			  "a date past 9999-12-31 is refused, 9999-12-31 is not");
	}

	// Partitioning from a synthetic catalog (no .bak): one table on a scheme of a function of int with
	// fanout 3, and every way its catalog can be wrong -- each a reason, with no list and no NEXT USED
	// beside it (D13; review of #551).
	{
		auto int_variant = [](int32_t v) {
			PartitionBoundary b;
			b.bytes = std::string("\x38\x01", 2) + std::string(reinterpret_cast<const char *>(&v), 4);
			return b;
		};
		PartitionCatalog good;
		good.functions[65536] = PartitionCatalog::Function{"pf", true, 3};
		PartitionCatalog::Parameter param;
		param.xtype = 56;
		param.user_type = 56;
		param.length = 4;
		param.precision = 10;
		good.parameters[65536] = param;
		good.boundaries[65536][1] = int_variant(100);
		good.boundaries[65536][2] = int_variant(200);
		good.schemes[65601] = "ps";
		good.filegroups[1] = "PRIMARY";
		good.filegroups[2] = "FG2";
		good.scheme_function[65601] = 65536;
		good.destinations[65601] = {{1, 1}, {2, 2}, {3, 1}, {4, 2}};
		good.data_space[std::make_pair(500, 1)] = 65601;
		good.partition_column[std::make_pair(500, 1)] = 1;
		good.data_spaces_read = true;
		const std::unordered_map<int32_t, std::string> types = {{56, "int"}, {127, "bigint"}, {167, "varchar"}};
		auto table = [](size_t partitions) {
			CatalogTable t;
			t.object_id = 500;
			CatalogColumn k;
			k.column_id = 1;
			k.name = "k";
			t.columns.push_back(k);
			for (size_t i = 0; i < partitions; i++) {
				CatalogPartition p;
				p.partition_number = static_cast<int32_t>(i + 1);
				t.partitions.push_back(p);
			}
			return t;
		};
		auto describe = [&](const PartitionCatalog &pc, size_t partitions, const BoundaryCheck &check) {
			CatalogTable t = table(partitions);
			DescribePartitioning(pc, types, 1, check, t);
			return t;
		};
		const CatalogTable ok = describe(good, 3, BoundaryCheck());
		const CatalogPartitioning &p = ok.partitioning;
		Check(ok.partitioned && p.reason.empty() && p.function_name == "pf" && p.scheme_name == "ps" &&
				  p.column_name == "k" && p.range_right && p.boundaries.size() == 2 && !p.boundaries[0].is_null &&
				  p.destinations == std::vector<std::string>({"PRIMARY", "FG2", "PRIMARY"}) && p.next_used == "FG2" &&
				  p.parameter_type_name == "int",
			  "a table on a scheme: its function, column, two boundaries, three destinations and NEXT USED");
		PartitionCatalog null_boundary = good;
		null_boundary.boundaries[65536][1] = PartitionBoundary();
		null_boundary.boundaries[65536][1].is_null = true;
		const CatalogTable with_null = describe(null_boundary, 3, BoundaryCheck());
		Check(with_null.partitioning.reason.empty() && with_null.partitioning.boundaries.size() == 2 &&
				  with_null.partitioning.boundaries[0].is_null,
			  "a NULL boundary is a boundary (the lowest value), not a count mismatch");
		const CatalogTable filegroup = [&]() {
			PartitionCatalog pc = good;
			pc.data_space[std::make_pair(500, 1)] = 2;
			return describe(pc, 1, BoundaryCheck());
		}();
		const CatalogTable default_space = [&]() {
			PartitionCatalog pc = good;
			pc.data_space.clear();
			return describe(pc, 1, BoundaryCheck());
		}();
		Check(!filegroup.partitioned && !default_space.partitioned,
			  "on a filegroup, or with no data-space row (the default one, as measured), a table is not partitioned");

		// Each wrong catalog: its reason, and nothing else given.
		struct Wrong {
			std::string what;
			std::function<void(PartitionCatalog &)> change;
			size_t partitions;
			std::string expect;
		};
		std::vector<Wrong> wrongs = {
			{"no data-space row while syssingleobjrefs could not be read",
			 [](PartitionCatalog &pc) {
				 pc.data_space.clear();
				 pc.data_spaces_read = false;
				 pc.reason = "the partitioning tables: x";
			 },
			 3, "the partitioning tables"},
			{"the data-space row twice",
			 [](PartitionCatalog &pc) {
				 pc.data_space_twice.insert({500, 1});
			 },
			 3, "data space twice"},
			{"the scheme's id twice in sysclsobjs", [](PartitionCatalog &pc) { pc.data_spaces_twice.insert(65601); }, 3,
			 "twice"},
			{"the function twice", [](PartitionCatalog &pc) { pc.functions_twice.insert(65536); }, 3, "listed twice"},
			{"the parameter's type not in sysscalartypes",
			 [](PartitionCatalog &pc) { pc.parameters[65536].user_type = 999; }, 3, "not in sysscalartypes"},
			{"no partitioning column", [](PartitionCatalog &pc) { pc.partition_column.clear(); }, 3,
			 "no partitioning column"},
			{"two partitioning columns",
			 [](PartitionCatalog &pc) {
				 pc.partition_column_twice.insert({500, 1});
			 },
			 3, "two partitioning columns"},
			{"a fanout of 0", [](PartitionCatalog &pc) { pc.functions[65536].fanout = 0; }, 3, "fanout of 0"},
			{"a fanout past 15000", [](PartitionCatalog &pc) { pc.functions[65536].fanout = 15001; }, 3,
			 "fanout of 15001"},
			{"no destination at all", [](PartitionCatalog &pc) { pc.destinations.clear(); }, 3, "no destination 1"},
			{"a destination missing", [](PartitionCatalog &pc) { pc.destinations[65601].erase(2); }, 3,
			 "no destination 2"},
			{"a destination twice",
			 [](PartitionCatalog &pc) {
				 pc.destinations_twice.insert({65601, 3});
			 },
			 3, "destination 3 is listed twice"},
			{"two partitions where the fanout says three", [](PartitionCatalog &) {}, 2, "2 partitions"},
		};
		// Boundaries the reader cannot give: the reason, no boundaries, the destinations and NEXT USED
		// still given (they do not depend on the values; owner, review of #551).
		std::vector<Wrong> boundary_wrongs = {
			{"a boundary off-row", [](PartitionCatalog &pc) { pc.boundaries_off_row.insert(65536); }, 3, "off-row"},
			{"one boundary fewer", [](PartitionCatalog &pc) { pc.boundaries[65536].erase(2); }, 3, "has 1 boundaries"},
			{"a boundary at the wrong id",
			 [&](PartitionCatalog &pc) {
				 pc.boundaries[65536].erase(2);
				 pc.boundaries[65536][3] = int_variant(300);
			 },
			 3, "boundary 2 is missing"},
			{"a boundary twice",
			 [](PartitionCatalog &pc) {
				 pc.boundaries_twice.insert({65536, 2});
			 },
			 3, "listed twice"},
			{"a boundary of another type",
			 [](PartitionCatalog &pc) {
				 pc.boundaries[65536][2].bytes = std::string("\x7f\x01\x01\0\0\0\0\0\0\0", 10);
			 },
			 3, "not of its parameter's type"},
			{"a boundary that does not parse",
			 [](PartitionCatalog &pc) { pc.boundaries[65536][2].bytes = std::string("\x38", 1); }, 3, "boundary 2"},
		};
		bool kept = true;
		std::string kept_failed;
		for (auto &w : boundary_wrongs) {
			PartitionCatalog pc = good;
			w.change(pc);
			const CatalogTable t = describe(pc, w.partitions, BoundaryCheck());
			const CatalogPartitioning &q = t.partitioning;
			if (!(t.partitioned && q.reason.find(w.expect) != std::string::npos && q.boundaries.empty() &&
				  q.destinations.size() == 3 && q.next_used == "FG2")) {
				kept = false;
				kept_failed += " [" + w.what + ": '" + q.reason + "']";
			}
		}
		Check(kept, std::to_string(boundary_wrongs.size()) +
						" boundaries the reader cannot give: named, no boundaries, destinations and NEXT USED kept" +
						kept_failed);
		const CatalogTable later_failure = [&]() {
			PartitionCatalog pc = good;
			pc.data_space.clear();
			pc.data_spaces_read = true;
			pc.reason = "the partitioning tables: sysobjvalues failed";
			return describe(pc, 1, BoundaryCheck());
		}();
		Check(!later_failure.partitioned,
			  "with syssingleobjrefs read whole, no data-space row is the default data space whatever failed later");
		bool all = true;
		std::string failed;
		for (auto &w : wrongs) {
			PartitionCatalog pc = good;
			w.change(pc);
			const CatalogTable t = describe(pc, w.partitions, BoundaryCheck());
			const CatalogPartitioning &q = t.partitioning;
			const bool right = t.partitioned && q.reason.find(w.expect) != std::string::npos && q.boundaries.empty() &&
							   q.destinations.empty() && q.next_used.empty() && q.reason.compare(0, 8, "backup: ") != 0;
			if (!right) {
				all = false;
				failed += " [" + w.what + ": '" + q.reason + "']";
			}
		}
		Check(all, std::to_string(wrongs.size()) + " wrong catalogs, each named, with no list beside it" + failed);

		// A varchar(10) parameter: a boundary of another collation, or longer than 10 bytes, is refused.
		PartitionCatalog strings = good;
		strings.parameters[65536].xtype = 167;
		strings.parameters[65536].user_type = 167;
		strings.parameters[65536].length = 10;
		strings.parameters[65536].collation = 0x0000D008;
		auto varchar = [](uint32_t collation, const std::string &v) {
			PartitionBoundary b;
			b.bytes = std::string("\xa7\x01", 2) + std::string("\x0a\x00", 2) +
					  std::string(reinterpret_cast<const char *>(&collation), 4) + v;
			return b;
		};
		strings.boundaries[65536][1] = varchar(0x0000D008, "b");
		strings.boundaries[65536][2] = varchar(0x0000D008, "m");
		const CatalogTable strings_ok = describe(strings, 3, BoundaryCheck());
		PartitionCatalog other_collation = strings;
		other_collation.boundaries[65536][2] = varchar(0x0000D009, "m");
		PartitionCatalog too_long = strings;
		too_long.boundaries[65536][2] = varchar(0x0000D008, "mmmmmmmmmmmm");
		Check(
			strings_ok.partitioning.reason.empty() &&
				describe(other_collation, 3, BoundaryCheck()).partitioning.reason.find("collation") !=
					std::string::npos &&
				describe(too_long, 3, BoundaryCheck()).partitioning.reason.find("holds 12 bytes") != std::string::npos,
			"a varchar boundary of its parameter's collation and length passes; another collation, or 12 bytes "
			"for a varchar(10), is refused: " +
				strings_ok.partitioning.reason);

		// The check decides whether a boundary decodes: its refusal is the model's reason.
		const CatalogTable refused = describe(good, 3, [](const SqlVariant &, const std::string &what) {
			throw FormatError("backup: " + what + " is not given");
		});
		Check(refused.partitioning.reason == "boundary 1 of pf is not given" &&
				  refused.partitioning.boundaries.empty() && refused.partitioning.destinations.size() == 3,
			  "a boundary the check refuses is the reason, without the backup: prefix: " + refused.partitioning.reason);
	}

	{
		// mssql_backup_columns' states (task 16d): each from ReadingOf, the off-row ones by type and by
		// what the table's row can hold.
		auto column = [](int32_t id, uint8_t type, int16_t max_length) {
			CatalogColumn c;
			c.column_id = id;
			c.name = "c" + std::to_string(id);
			c.system_type_id = type;
			c.max_length = max_length;
			c.collation_id = type == XTYPE_VARCHAR || type == XTYPE_TEXT ? 53256 : 0;
			c.system_type_name = type == XTYPE_INT			 ? "int"
								 : type == XTYPE_VARCHAR	 ? "varchar"
								 : type == XTYPE_VARBINARY	 ? "varbinary"
								 : type == XTYPE_TEXT		 ? "text"
								 : type == XTYPE_SQL_VARIANT ? "sql_variant"
															 : "";
			c.type_name = c.system_type_name;
			return c;
		};
		CatalogTable narrow;
		narrow.decodable = true;
		narrow.columns = {column(1, XTYPE_INT, 4),	 column(2, XTYPE_VARCHAR, 20), column(3, XTYPE_VARBINARY, -1),
						  column(4, XTYPE_TEXT, 16), column(5, XTYPE_INT, 4),	   column(6, XTYPE_INT, 4)};
		narrow.columns[4].is_computed = true;
		narrow.columns[5].is_sparse = true;
		auto state = [](const CatalogTable &t, size_t i) { return std::string(StateOf(t, t.columns[i]).state); };
		Check(state(narrow, 0) == "converted" && StateOf(narrow, narrow.columns[0]).reason.empty() &&
				  state(narrow, 1) == "converted" && state(narrow, 2) == "off_row_pending" &&
				  state(narrow, 3) == "off_row_pending" && state(narrow, 4) == "not_stored" &&
				  state(narrow, 5) == "storage_format",
			  "states: converted (an int, a varchar(20)), a MAX and a text off_row_pending, a computed column "
			  "not_stored, a sparse one storage_format");
		CatalogTable variant;
		variant.decodable = true;
		variant.columns = {column(1, XTYPE_INT, 4), column(2, XTYPE_SQL_VARIANT, 8016), column(3, XTYPE_VARCHAR, 100)};
		const ColumnState v = StateOf(variant, variant.columns[1]);
		Check(std::string(v.state) == "stored_bytes" && v.reason.find("sql_variant") != std::string::npos &&
				  v.reason.find("8,060") != std::string::npos && state(variant, 2) == "off_row_pending",
			  "a sql_variant: stored_bytes, and (8,016 bytes declared) its row can pass 8,060, so it and a "
			  "varchar(100) beside it can go off row: " +
				  v.reason);
		CatalogTable wide;
		wide.decodable = true;
		wide.columns = {column(1, XTYPE_INT, 4), column(2, XTYPE_VARCHAR, 6000), column(3, XTYPE_VARCHAR, 6000),
						column(4, XTYPE_VARCHAR, 20)};
		Check(state(wide, 1) == "off_row_pending" && state(wide, 2) == "off_row_pending" &&
				  state(wide, 3) == "converted" && state(wide, 0) == "converted",
			  "a row that can pass 8,060 bytes: its varchar(6000)s off_row_pending, a varchar(20) (no wider than "
			  "the off-row pointer) and an int converted");
		CatalogTable with_max;
		with_max.decodable = true;
		with_max.columns = {column(1, XTYPE_VARCHAR, 4000), column(2, XTYPE_VARBINARY, -1),
							column(3, XTYPE_VARCHAR, 20)};
		Check(state(with_max, 0) == "off_row_pending" && state(with_max, 2) == "converted",
			  "a MAX column counts as its 8,000-byte in-row share: a varchar(4000) beside it can be pushed off row "
			  "(the server moves the widest value, not the MAX one first), a varchar(20) cannot");
		CatalogTable unread = with_max;
		unread.decodable = false;
		unread.reason = "PAGE compression";
		Check(StateOf(unread, unread.columns[2]).reason == "the table is not read: PAGE compression",
			  "a table not read says so in each column's reason");
	}

	std::cout << (failures ? "FAILED" : "PASSED") << " (" << failures << " failure(s))" << std::endl;
	return failures ? 1 : 0;
}
