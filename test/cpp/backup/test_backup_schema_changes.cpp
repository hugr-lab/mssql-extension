// Spec 085 task 24c (PR 4c-3, DQ-47): CompareCatalogs over synthetic catalog models -- the committed
// samples' states share one schema, so every change an importer applies is pinned here: tables created,
// dropped, renamed and reloaded (recreated under a new object id, rowsets changed, a history break),
// columns renamed, dropped, added, retyped and of another nullability, in DQ-49's order, and the views'
// events with their definitions.

#include <iostream>
#include <string>
#include <vector>

#include "backup/backup_bytes.hpp"
#include "backup/backup_schema_changes.hpp"

using namespace duckdb::mssql::backup;

namespace {

int failures = 0;

void Check(bool ok, const std::string &what) {
	std::cout << (ok ? "  ok   " : "  FAIL ") << what << std::endl;
	if (!ok) {
		failures++;
	}
}

CatalogColumn Column(int32_t id, const std::string &name, const std::string &type, uint8_t system_type_id,
					 bool nullable) {
	CatalogColumn c;
	c.column_id = id;
	c.name = name;
	c.type_name = type;
	c.system_type_name = type;
	c.system_type_id = system_type_id;
	c.max_length = system_type_id == 56 ? 4 : 8;
	c.is_nullable = nullable;
	return c;
}

CatalogTable Table(int32_t id, const std::string &schema, const std::string &name, uint64_t rowset,
				   std::vector<CatalogColumn> columns) {
	CatalogTable t;
	t.object_id = id;
	t.schema_name = schema;
	t.name = name;
	CatalogPartition p;
	p.rowset_id = rowset;
	p.partition_number = 1;
	p.rows = 10;
	t.partitions.push_back(p);
	CatalogUnit u;
	u.unit.auid = rowset * 10;
	u.unit.owner_id = rowset;
	u.unit.type = 1;
	u.unit.first_iam.file = 1;
	u.unit.first_iam.page = static_cast<uint32_t>(rowset);
	u.unit.first_page.file = 1;
	u.unit.first_page.page = static_cast<uint32_t>(rowset) + 1;
	t.units.push_back(u);
	t.columns = std::move(columns);
	return t;
}

CatalogView View(int32_t id, const std::string &name, const std::string &modified, const std::string &text,
				 bool indexed = false) {
	CatalogView v;
	v.object_id = id;
	v.schema_name = "dbo";
	v.name = name;
	v.modified = modified;
	v.definition.state = ModuleDefinition::State::Ready;
	v.definition.text = text;
	v.indexed = indexed;
	return v;
}

const ViewDefinition DEFINITION = [](bool, const CatalogView &v) { return v.definition.text; };

std::vector<std::string> Types(const std::vector<SchemaChange> &changes) {
	std::vector<std::string> out;
	for (const auto &c : changes) {
		out.push_back(c.change_type +
					  (c.column_id ? ":" + std::to_string(c.column_id) : ":" + (c.name.empty() ? c.old_name : c.name)));
	}
	return out;
}

void Print(const std::vector<SchemaChange> &changes) {
	for (const auto &t : Types(changes)) {
		std::cout << "    " << t << std::endl;
	}
}

}  // namespace

int main() {
	std::cout << "schema changes (spec 085 task 24c)" << std::endl;
	{
		CatalogModel old_model, new_model;
		old_model.tables.push_back(Table(10, "dbo", "orders", 100,
										 {Column(1, "id", "int", 56, false), Column(2, "amount", "int", 56, true),
										  Column(3, "note", "int", 56, true), Column(4, "qty", "int", 56, true)}));
		old_model.tables.push_back(Table(20, "dbo", "gone", 200, {Column(1, "id", "int", 56, false)}));
		new_model.tables.push_back(Table(10, "sales", "orders2", 100,
										 {Column(1, "id", "int", 56, false), Column(2, "total", "bigint", 127, true),
										  Column(4, "qty", "int", 56, false), Column(5, "added", "int", 56, true)}));
		new_model.tables.push_back(Table(30, "dbo", "fresh", 300, {Column(1, "id", "int", 56, false)}));
		const auto changes = CompareCatalogs(old_model, new_model, false, DEFINITION);
		const std::vector<std::string> want = {"table_dropped:gone",
											   "table_renamed:orders2",
											   "column_renamed:2",
											   "column_dropped:3",
											   "column_added:5",
											   "column_type_changed:2",
											   "column_nullability_changed:4",
											   "table_created:fresh"};
		Check(Types(changes) == want,
			  "drops, then the kept table's rename and columns in DQ-49's order, then creations");
		if (Types(changes) != want) {
			Print(changes);
		}
		if (changes.size() == want.size()) {
			Check(changes[1].old_schema_name == "dbo" && changes[1].old_name == "orders" &&
					  changes[1].schema_name == "sales",
				  "a rename carries the old and new names");
			Check(changes[2].old_column_name == "amount" && changes[2].column_name == "total",
				  "a column renamed by its id");
			Check(changes[5].old_sql_type == "int" && changes[5].sql_type == "bigint", "a type change, old and new");
			Check(changes[6].old_is_nullable == 1 && changes[6].is_nullable == 0, "a nullability change");
			Check(changes[0].object_id == 20 && changes[0].old_name == "gone", "a drop carries the old object");
		}
	}
	{
		// A table rebuilt under a new object id: one reload, not a drop and a creation.
		CatalogModel old_model, new_model;
		old_model.tables.push_back(Table(10, "dbo", "t", 100, {Column(1, "id", "int", 56, false)}));
		new_model.tables.push_back(Table(12, "dbo", "t", 120, {Column(1, "id", "int", 56, false)}));
		const auto changes = CompareCatalogs(old_model, new_model, false, DEFINITION);
		Check(Types(changes) == std::vector<std::string>({"table_reload:t"}) && changes[0].old_object_id == 10 &&
				  changes[0].object_id == 12 && changes[0].reason.find("new object id") != std::string::npos,
			  "recreated under a new object id: a reload naming both ids");
	}
	{
		// Partitions (DQ-47 / DQ-49), judged by rowset: a rebuild is nothing (its pages are written anew), a
		// TRUNCATE keeps the rowset and starts its IAM chain anew -- or leaves none when nothing was inserted
		// since --, a partition switched in from another table is a reload and its source switched out.
		CatalogModel old_model, new_model;
		old_model.tables.push_back(Table(10, "dbo", "rebuilt", 100, {Column(1, "id", "int", 56, false)}));
		new_model.tables.push_back(Table(10, "dbo", "rebuilt", 101, {Column(1, "id", "int", 56, false)}));
		// Measured (schema_changes.sql): a TRUNCATE then an INSERT starts the unit at another first page; a
		// shrink moves the IAM page and leaves the recorded first page; past 128 extents the unit is replaced.
		auto truncated = Table(11, "dbo", "truncated", 110, {Column(1, "id", "int", 56, false)});
		old_model.tables.push_back(truncated);
		truncated.units[0].unit.first_page.page = 999;
		new_model.tables.push_back(truncated);
		auto shrunk = Table(19, "dbo", "shrunk", 190, {Column(1, "id", "int", 56, false)});
		old_model.tables.push_back(shrunk);
		shrunk.units[0].unit.first_iam.page = 7;
		new_model.tables.push_back(shrunk);
		auto deferred = Table(22, "dbo", "deferred", 220, {Column(1, "id", "int", 56, false)});
		old_model.tables.push_back(deferred);
		deferred.units[0].unit.auid = 1;
		deferred.units[0].unit.first_iam = PagePointer();
		deferred.units[0].unit.first_page = PagePointer();
		new_model.tables.push_back(deferred);
		auto emptied = Table(17, "dbo", "emptied", 170, {Column(1, "id", "int", 56, false)});
		old_model.tables.push_back(emptied);
		emptied.units[0].unit.first_iam = PagePointer();  // truncated, nothing inserted since
		emptied.partitions[0].rows = 0;
		new_model.tables.push_back(emptied);
		// A clustered columnstore: its in-row unit holds no pages, its LOB unit (the segments) does.
		auto columnstore = Table(18, "dbo", "columnstore", 180, {Column(1, "id", "int", 56, false)});
		columnstore.units[0].unit.first_iam = PagePointer();
		columnstore.units[0].unit.first_page = PagePointer();
		CatalogUnit lob = columnstore.units[0];
		lob.unit.type = 2;
		lob.unit.auid = 1801;
		lob.unit.first_iam.file = 1;
		lob.unit.first_iam.page = 1985;
		lob.unit.first_page.file = 1;
		lob.unit.first_page.page = 1000;
		columnstore.units.push_back(lob);
		old_model.tables.push_back(columnstore);
		columnstore.units[1].unit.first_iam = PagePointer();
		columnstore.units[1].unit.first_page = PagePointer();
		new_model.tables.push_back(columnstore);
		old_model.tables.push_back(Table(12, "dbo", "target", 120, {Column(1, "id", "int", 56, false)}));
		old_model.tables.push_back(Table(13, "dbo", "staging", 130, {Column(1, "id", "int", 56, false)}));
		new_model.tables.push_back(Table(12, "dbo", "target", 130, {Column(1, "id", "int", 56, false)}));
		new_model.tables.push_back(Table(13, "dbo", "staging", 131, {Column(1, "id", "int", 56, false)}));
		auto purged_new = Table(16, "dbo", "purged", 161, {Column(1, "id", "int", 56, false)});
		purged_new.partitions[0].rows = 0;
		old_model.tables.push_back(Table(16, "dbo", "purged", 160, {Column(1, "id", "int", 56, false)}));
		new_model.tables.push_back(purged_new);
		const auto changes = CompareCatalogs(old_model, new_model, false, DEFINITION);
		const std::vector<std::string> want = {"table_truncated:truncated",		 "table_reload:target",
											   "partition_switched_out:staging", "table_truncated:emptied",
											   "table_truncated:columnstore",	 "table_truncated:deferred"};
		Check(Types(changes) == want,
			  "by rowset: a rebuild and a shrink nothing, truncates (restarted, emptied, columnstore, a deferred "
			  "drop), a "
			  "switch in (a reload, its source switched out), a rowset replaced by an empty one "
			  "nothing (the row changes say what left)");
		if (Types(changes) != want) {
			Print(changes);
		} else {
			Check(changes[0].partitions == std::vector<int32_t>({1}), "the partitions concerned");
			Check(changes[1].reason.find("switched in") != std::string::npos, "the switch in says why");
		}
	}
	{
		// A sliding window: p1 switched out to archive, MERGE at the low end, SPLIT at the top -- the
		// partitions renumbered, which by number read as false switches (review).
		auto partitioned = [](int32_t id, std::vector<std::pair<int32_t, uint64_t>> parts) {
			CatalogTable t = Table(id, "dbo", "events", parts.front().second, {Column(1, "id", "int", 56, false)});
			t.partitions.clear();
			t.units.clear();
			for (const auto &p : parts) {
				CatalogPartition partition;
				partition.partition_number = p.first;
				partition.rowset_id = p.second;
				partition.rows = 10;
				t.partitions.push_back(partition);
				CatalogUnit u;
				u.unit.owner_id = p.second;
				u.unit.type = 1;
				u.unit.first_iam.file = 1;
				u.unit.first_iam.page = static_cast<uint32_t>(p.second);
				t.units.push_back(u);
			}
			return t;
		};
		CatalogModel old_model, new_model;
		old_model.tables.push_back(partitioned(20, {{1, 201}, {2, 202}, {3, 203}, {4, 204}}));
		new_model.tables.push_back(partitioned(20, {{1, 202}, {2, 203}, {3, 204}, {4, 205}}));
		new_model.tables.push_back(Table(21, "dbo", "archive", 201, {Column(1, "id", "int", 56, false)}));
		const auto changes = CompareCatalogs(old_model, new_model, false, DEFINITION);
		Check(Types(changes) == std::vector<std::string>({"partition_switched_out:events", "table_created:archive"}),
			  "renumbered partitions: one switch out, no false truncate or reload");
		if (changes.size() == 2) {
			Check(changes[0].partitions == std::vector<int32_t>({1}), "reported by its old partition number");
		} else {
			Print(changes);
		}
	}
	{
		// Across a history break object ids compare as nothing: every table a reload, by name.
		CatalogModel old_model, new_model;
		old_model.tables.push_back(Table(10, "dbo", "t", 100, {Column(1, "id", "int", 56, false)}));
		old_model.tables.push_back(Table(11, "dbo", "only_old", 110, {Column(1, "id", "int", 56, false)}));
		new_model.tables.push_back(Table(10, "dbo", "u", 100, {Column(1, "id", "int", 56, false)}));
		new_model.tables.push_back(Table(55, "dbo", "t", 120, {Column(1, "id", "int", 56, false)}));
		const auto changes = CompareCatalogs(old_model, new_model, true, DEFINITION);
		Check(
			Types(changes) == std::vector<std::string>({"table_dropped:only_old", "table_reload:t", "table_reload:u"}),
			"a history break: the old-only table dropped, every new table a reload, by name");
		if (Types(changes).size() != 3) {
			Print(changes);
		}
	}
	{
		// Views: created, dropped, renamed, redefined (and a modification date alone is no change),
		// indexed.
		CatalogModel old_model, new_model;
		old_model.views.push_back(View(40, "v_old", "m1", "SELECT 1"));
		old_model.views.push_back(View(41, "v_rename", "m1", "SELECT 2"));
		old_model.views.push_back(View(42, "v_redef", "m1", "SELECT 3"));
		old_model.views.push_back(View(43, "v_touched", "m1", "SELECT 4"));
		old_model.views.push_back(View(44, "v_index", "m1", "SELECT 5"));
		new_model.views.push_back(View(41, "v_renamed", "m1", "SELECT 2"));
		new_model.views.push_back(View(42, "v_redef", "m2", "SELECT 33"));
		new_model.views.push_back(View(43, "v_touched", "m2", "SELECT 4"));
		new_model.views.push_back(View(44, "v_index", "m1", "SELECT 5", true));
		new_model.views.push_back(View(45, "v_new", "m1", "SELECT 6"));
		const auto changes = CompareCatalogs(old_model, new_model, false, DEFINITION);
		Check(Types(changes) == std::vector<std::string>({"view_dropped:v_old", "view_renamed:v_renamed",
														  "view_definition_changed:v_redef", "view_indexed:v_index",
														  "view_created:v_new"}),
			  "views: dropped, renamed, redefined, indexed, created; a date alone no change");
		if (Types(changes).size() != 5) {
			Print(changes);
		}
		if (changes.size() == 5) {
			Check(changes[0].old_name == "v_old" && changes[0].has_old_definition &&
					  changes[0].old_definition == "SELECT 1",
				  "a dropped view keeps its old T-SQL");
			Check(changes[2].old_definition == "SELECT 3" && changes[2].definition == "SELECT 33",
				  "a redefinition carries both texts");
			Check(changes[4].has_definition && changes[4].definition == "SELECT 6", "a created view its T-SQL");
		}
	}
	{
		// A definition that cannot be read is said, not thrown.
		CatalogModel old_model, new_model;
		new_model.views.push_back(View(50, "v", "m1", "SELECT 7"));
		const ViewDefinition broken = [](bool, const CatalogView &) -> std::string {
			throw FormatError("backup: the definition is off row past the limit");
		};
		const auto changes = CompareCatalogs(old_model, new_model, false, broken);
		Check(changes.size() == 1 && !changes[0].has_definition &&
				  changes[0].reason.find("past the limit") != std::string::npos,
			  "an unreadable definition: NULL, with the reason");
	}
	{
		// An encrypted view altered: its date moved, its text unreadable on both sides -- the change said, with
		// NULL texts and the reason (review: it was lost).
		CatalogModel old_model, new_model;
		auto v_old = View(60, "secret", "m1", "");
		v_old.is_encrypted = true;
		auto v_new = View(60, "secret", "m2", "");
		v_new.is_encrypted = true;
		old_model.views.push_back(v_old);
		new_model.views.push_back(v_new);
		const auto changes = CompareCatalogs(old_model, new_model, false, DEFINITION);
		Check(changes.size() == 1 && changes[0].change_type == "view_definition_changed" &&
				  !changes[0].has_definition && changes[0].reason.find("encrypted") != std::string::npos,
			  "an encrypted view altered: the change, NULL texts, the reason");
	}
	{
		CatalogModel same;
		same.tables.push_back(Table(10, "dbo", "t", 100, {Column(1, "id", "int", 56, false)}));
		same.views.push_back(View(40, "v", "m1", "SELECT 1"));
		Check(CompareCatalogs(same, same, false, DEFINITION).empty(), "one state: no change");
	}

	if (failures) {
		std::cout << failures << " FAILED" << std::endl;
		return 1;
	}
	std::cout << "all passed" << std::endl;
	return 0;
}
