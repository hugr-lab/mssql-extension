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
		Check(in_row == 1 && items->layout.columns.size() == 7 &&
				  items->layout.rowset_id == items->partitions[0].rowset_id,
			  form.front() + ": items' clustered index has one in-row unit, and its layout seven physical columns");
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
		const CollationRow *japanese = mssql::codec::code_page::FindCollationByName("Japanese_CI_AS", 14);
		std::string error = "no Japanese_CI_AS";
		if (japanese) {
			const uint32_t id = static_cast<uint32_t>(japanese->collation_id);
			error = ErrorOf([&]() {
				text({0xa7, 0x01, 0x01, 0x00, static_cast<int>(id & 0xff), static_cast<int>((id >> 8) & 0xff),
					  static_cast<int>((id >> 16) & 0xff), static_cast<int>(id >> 24), 0x47});
			});
		}
		Check(error.find("code page 932") != std::string::npos && error.find("086") != std::string::npos,
			  "a varchar of a double-byte code page is refused, naming spec 086: " + error);
	}

	std::cout << (failures ? "FAILED" : "PASSED") << " (" << failures << " failure(s))" << std::endl;
	return failures ? 1 : 0;
}
