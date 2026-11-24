//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// backup/backup_schema_changes.cpp
//
// Spec 085 task 24c (PR 4c-3, DQ-47): two catalog models compared.
//===----------------------------------------------------------------------===//

#include "backup/backup_schema_changes.hpp"

#include "backup/backup_bytes.hpp"

#include <map>
#include <set>

namespace duckdb {
namespace mssql {
namespace backup {

namespace {

std::string TypeOf(const CatalogColumn &c) {
	return SqlTypeText(c.type_name, c.system_type_name, c.system_type_id, c.max_length, c.precision, c.scale);
}

SchemaChange TableChange(const std::string &type, const CatalogTable *now, const CatalogTable *old) {
	SchemaChange c;
	c.change_type = type;
	c.object_type = "table";
	const CatalogTable *any = now ? now : old;
	c.type = any->type;
	c.is_ms_shipped = any->is_ms_shipped;
	if (now) {
		c.object_id = now->object_id;
		c.schema_name = now->schema_name;
		c.name = now->name;
	}
	if (old) {
		c.old_object_id = old->object_id;
		c.old_schema_name = old->schema_name;
		c.old_name = old->name;
		if (!now) {
			c.object_id = old->object_id;
		}
	}
	return c;
}

//! Every rowset of a model's tables, and the object it belongs to.
std::map<uint64_t, int32_t> RowsetOwners(const CatalogModel &model) {
	std::map<uint64_t, int32_t> owners;
	for (const auto &t : model.tables) {
		for (const auto &p : t.partitions) {
			owners[p.rowset_id] = t.object_id;
		}
	}
	return owners;
}

//! Whether a rowset kept from `old_table` to `new_table` was truncated, from its allocation units (in-row,
//! LOB, row-overflow), measured on the oracle fixture schema_changes.sql: a TRUNCATE frees every page, so
//! a unit's first IAM page goes when nothing was inserted since, and its first page is another when
//! something was (allocation starts anew); past 128 extents (a deferred drop) the unit itself is replaced. A DBCC
//! SHRINKFILE moves the IAM and root pages and leaves the unit's recorded first page as it was, and so does a DELETE of
//! every row: neither is a truncation.
bool Truncated(const CatalogTable &old_table, const CatalogTable &new_table, uint64_t rowset) {
	// A rowset has one unit of each type.
	std::map<uint8_t, const SysAllocUnit *> old_units;
	for (const auto &u : old_table.units) {
		if (u.unit.owner_id == rowset) {
			old_units[u.unit.type] = &u.unit;
		}
	}
	for (const auto &u : new_table.units) {
		if (u.unit.owner_id != rowset) {
			continue;
		}
		auto o = old_units.find(u.unit.type);
		if (o == old_units.end()) {
			continue;
		}
		const auto &before = *o->second;
		// A table past 128 extents is truncated by a deferred drop: its old unit goes, a new one takes its place.
		const bool unit_replaced = before.auid != u.unit.auid;
		const bool chain_gone = !before.first_iam.IsNull() && u.unit.first_iam.IsNull();
		const bool restarted =
			!before.first_page.IsNull() && !u.unit.first_page.IsNull() && !(before.first_page == u.unit.first_page);
		if (unit_replaced || chain_gone || restarted) {
			return true;
		}
	}
	return false;
}

//! What happened to a table's partitions from `old_table` to `new_table` (DQ-47, DQ-49), judged by rowset
//! -- a MERGE or SPLIT renumbers partitions, a rowset is the partition's data (review: by number, a
//! sliding window read as false switches):
//! - a rowset kept that was truncated (Truncated: one of its units' IAM chain gone, or its first page
//!   another): `truncated`;
//! - a new rowset that was another table's in the old state: switched in, its pages unwritten since, so
//!   their LSNs say nothing of the rows -- a reload;
//! - an old rowset that is another table's now: switched out, reported by its old partition number;
//! - a new rowset otherwise (a rebuild, a split, the empty partition a switch leaves), or an old one gone
//!   (rebuilt, merged, or switched out to a table since dropped): written pages, which the row changes
//!   see -- no event.
struct PartitionEvents {
	std::vector<int32_t> truncated, switched_out;
	std::string switched_in_from;  // a reload's reasons, empty when none
};

PartitionEvents ComparePartitions(const CatalogTable &old_table, const CatalogTable &new_table,
								  const std::map<uint64_t, int32_t> &old_owners,
								  const std::map<uint64_t, int32_t> &new_owners) {
	PartitionEvents events;
	std::map<uint64_t, const CatalogPartition *> old_by_rowset, new_by_rowset;
	for (const auto &p : old_table.partitions) {
		old_by_rowset[p.rowset_id] = &p;
	}
	for (const auto &p : new_table.partitions) {
		new_by_rowset[p.rowset_id] = &p;
	}
	for (const auto &n : new_by_rowset) {
		const auto &p = *n.second;
		auto kept = old_by_rowset.find(n.first);
		if (kept != old_by_rowset.end()) {
			if (Truncated(old_table, new_table, n.first)) {
				events.truncated.push_back(p.partition_number);
			}
			continue;
		}
		auto was = old_owners.find(n.first);
		if (was != old_owners.end() && was->second != new_table.object_id) {
			if (!events.switched_in_from.empty()) {
				events.switched_in_from += "; ";
			}
			events.switched_in_from += "partition " + std::to_string(p.partition_number) +
									   " was switched in from another table (object " + std::to_string(was->second) +
									   "): its pages are unwritten since, so their LSNs say nothing of its rows";
		}
	}
	for (const auto &o : old_by_rowset) {
		if (new_by_rowset.count(o.first)) {
			continue;
		}
		auto now = new_owners.find(o.first);
		if (now != new_owners.end() && now->second != new_table.object_id) {
			events.switched_out.push_back(o.second->partition_number);
		}
	}
	return events;
}

void CompareColumns(const CatalogTable &old_table, const CatalogTable &new_table, std::vector<SchemaChange> &out) {
	std::map<int32_t, const CatalogColumn *> old_columns, new_columns;
	for (const auto &c : old_table.columns) {
		old_columns[c.column_id] = &c;
	}
	for (const auto &c : new_table.columns) {
		new_columns[c.column_id] = &c;
	}
	auto column = [&](const std::string &type, const CatalogColumn *now, const CatalogColumn *old) {
		SchemaChange c = TableChange(type, &new_table, &old_table);
		c.change_type = type;
		const CatalogColumn *any = now ? now : old;
		c.column_id = any->column_id;
		if (now) {
			c.column_name = now->name;
			c.sql_type = TypeOf(*now);
			c.is_nullable = now->is_nullable ? 1 : 0;
		}
		if (old) {
			c.old_column_name = old->name;
			c.old_sql_type = TypeOf(*old);
			c.old_is_nullable = old->is_nullable ? 1 : 0;
		}
		out.push_back(std::move(c));
	};
	// DQ-49's order: renames, drops, additions, then type and nullability changes.
	for (const auto &o : old_columns) {
		auto n = new_columns.find(o.first);
		if (n != new_columns.end() && n->second->name != o.second->name) {
			column("column_renamed", n->second, o.second);
		}
	}
	for (const auto &o : old_columns) {
		if (!new_columns.count(o.first)) {
			column("column_dropped", nullptr, o.second);
		}
	}
	for (const auto &n : new_columns) {
		if (!old_columns.count(n.first)) {
			column("column_added", n.second, nullptr);
		}
	}
	for (const auto &o : old_columns) {
		auto n = new_columns.find(o.first);
		if (n == new_columns.end()) {
			continue;
		}
		if (TypeOf(*n->second) != TypeOf(*o.second)) {
			column("column_type_changed", n->second, o.second);
		}
		if (n->second->is_nullable != o.second->is_nullable) {
			column("column_nullability_changed", n->second, o.second);
		}
	}
}

SchemaChange ViewChange(const std::string &type, const CatalogView *now, const CatalogView *old) {
	SchemaChange c;
	c.change_type = type;
	c.object_type = "view";
	c.type = "V";
	c.is_ms_shipped = (now ? now : old)->is_ms_shipped;
	if (now) {
		c.object_id = now->object_id;
		c.schema_name = now->schema_name;
		c.name = now->name;
	}
	if (old) {
		c.old_object_id = old->object_id;
		c.old_schema_name = old->schema_name;
		c.old_name = old->name;
		if (!now) {
			c.object_id = old->object_id;
		}
	}
	return c;
}

std::string NameKey(const std::string &schema, const std::string &name) {
	return schema + '\x1f' + name;
}

//! A model's tables by object id and by schema and name.
struct TableIndex {
	explicit TableIndex(const CatalogModel &model) {
		for (const auto &t : model.tables) {
			by_id[t.object_id] = &t;
			by_name[NameKey(t.schema_name, t.name)] = &t;
		}
	}
	std::map<int32_t, const CatalogTable *> by_id;
	std::map<std::string, const CatalogTable *> by_name;
};

//! Across a history break, where object ids compare as nothing: tables by name, a kept or new name a reload.
void CompareTablesByName(const TableIndex &old_tables, const TableIndex &new_tables, std::vector<SchemaChange> &out) {
	for (const auto &o : old_tables.by_name) {
		if (!new_tables.by_name.count(o.first)) {
			out.push_back(TableChange("table_dropped", nullptr, o.second));
		}
	}
	for (const auto &n : new_tables.by_name) {
		auto o = old_tables.by_name.find(n.first);
		auto c = TableChange("table_reload", n.second, o == old_tables.by_name.end() ? nullptr : o->second);
		c.reason = "the database was restored or recreated between the two states";
		out.push_back(std::move(c));
	}
}

//! A table dropped and another created under its name (a rebuild under a new object id) is one reload of
//! that name, not a drop and a creation: the object ids of both, old and new.
void ReloadsUnderNewId(const TableIndex &old_tables, const TableIndex &new_tables, std::set<int32_t> &reloaded_old,
					   std::set<int32_t> &reloaded_new, std::vector<SchemaChange> &out) {
	for (const auto &o : old_tables.by_id) {
		if (new_tables.by_id.count(o.first)) {
			continue;
		}
		auto same_name = new_tables.by_name.find(NameKey(o.second->schema_name, o.second->name));
		if (same_name != new_tables.by_name.end() && !old_tables.by_id.count(same_name->second->object_id)) {
			auto c = TableChange("table_reload", same_name->second, o.second);
			c.reason = "recreated under a new object id";
			out.push_back(std::move(c));
			reloaded_old.insert(o.first);
			reloaded_new.insert(same_name->second->object_id);
		}
	}
}

//! A table both states hold: its rename, what happened to its partitions, its columns.
void CompareKeptTable(const CatalogTable &old_table, const CatalogTable &new_table,
					  const std::map<uint64_t, int32_t> &old_owners, const std::map<uint64_t, int32_t> &new_owners,
					  std::vector<SchemaChange> &out) {
	if (old_table.name != new_table.name || old_table.schema_name != new_table.schema_name) {
		out.push_back(TableChange("table_renamed", &new_table, &old_table));
	}
	const auto events = ComparePartitions(old_table, new_table, old_owners, new_owners);
	if (!events.switched_in_from.empty()) {
		auto c = TableChange("table_reload", &new_table, &old_table);
		c.reason = events.switched_in_from;
		out.push_back(std::move(c));
	}
	if (!events.truncated.empty()) {
		auto c = TableChange("table_truncated", &new_table, &old_table);
		c.partitions = events.truncated;
		out.push_back(std::move(c));
	}
	if (!events.switched_out.empty()) {
		auto c = TableChange("partition_switched_out", &new_table, &old_table);
		c.partitions = events.switched_out;
		out.push_back(std::move(c));
	}
	CompareColumns(old_table, new_table, out);
}

//! Tables by object id: reloads under a new id, drops, the kept ones, creations.
void CompareTablesById(const TableIndex &old_tables, const TableIndex &new_tables,
					   const std::map<uint64_t, int32_t> &old_owners, const std::map<uint64_t, int32_t> &new_owners,
					   std::vector<SchemaChange> &out) {
	std::set<int32_t> reloaded_old, reloaded_new;
	ReloadsUnderNewId(old_tables, new_tables, reloaded_old, reloaded_new, out);
	for (const auto &o : old_tables.by_id) {
		if (!new_tables.by_id.count(o.first) && !reloaded_old.count(o.first)) {
			out.push_back(TableChange("table_dropped", nullptr, o.second));
		}
	}
	for (const auto &o : old_tables.by_id) {
		auto n = new_tables.by_id.find(o.first);
		if (n != new_tables.by_id.end()) {
			CompareKeptTable(*o.second, *n->second, old_owners, new_owners, out);
		}
	}
	for (const auto &n : new_tables.by_id) {
		if (!old_tables.by_id.count(n.first) && !reloaded_new.count(n.first)) {
			out.push_back(TableChange("table_created", n.second, nullptr));
		}
	}
}

//! A view's text into `text` (`has` set), or why it could not be read added to `c`'s reason; an encrypted
//! view's is not asked for.
void ReadViewDefinition(const ViewDefinition &definition, bool old_state, const CatalogView &v, SchemaChange &c,
						std::string &text, bool &has) {
	if (v.is_encrypted) {
		return;
	}
	try {
		text = definition(old_state, v);
		has = true;
	} catch (FormatError &e) {
		std::string why = e.what();
		if (why.compare(0, 8, "backup: ") == 0) {
			why = why.substr(8);
		}
		c.reason = c.reason.empty() ? why : c.reason + "; " + why;
	}
}

void WithDefinitions(const ViewDefinition &definition, SchemaChange &c, const CatalogView *now,
					 const CatalogView *old) {
	if (now) {
		ReadViewDefinition(definition, false, *now, c, c.definition, c.has_definition);
	}
	if (old) {
		ReadViewDefinition(definition, true, *old, c, c.old_definition, c.has_old_definition);
	}
}

//! A view both states hold: its rename, its definition, whether it is indexed.
void CompareKeptView(const CatalogView &old_view, const CatalogView &new_view, const ViewDefinition &definition,
					 std::vector<SchemaChange> &out) {
	if (old_view.name != new_view.name || old_view.schema_name != new_view.schema_name) {
		out.push_back(ViewChange("view_renamed", &new_view, &old_view));
	}
	// The definitions are compared as stored; reading them is what costs (an off-row one), so only a
	// modification date that moved asks.
	if (old_view.modified != new_view.modified || old_view.definition.state != new_view.definition.state) {
		auto c = ViewChange("view_definition_changed", &new_view, &old_view);
		WithDefinitions(definition, c, &new_view, &old_view);
		const bool both_read = c.has_definition && c.has_old_definition;
		// A text that could not be read on either side (encrypted, unreadable) is a change the
		// modification date vouches for, said with NULL texts and the reason (review: else it was lost).
		if (!both_read || c.definition != c.old_definition) {
			if (!both_read && (new_view.is_encrypted || old_view.is_encrypted)) {
				c.reason = c.reason.empty() ? "the view is encrypted (WITH ENCRYPTION): its text is not stored readably"
											: c.reason;
			}
			out.push_back(std::move(c));
		}
	}
	if (old_view.indexed != new_view.indexed) {
		out.push_back(ViewChange(new_view.indexed ? "view_indexed" : "view_unindexed", &new_view, &old_view));
	}
}

//! Views, by object id (by name across a break): drops, the kept ones, creations.
void CompareViews(const CatalogModel &old_model, const CatalogModel &new_model, bool history_break,
				  const ViewDefinition &definition, std::vector<SchemaChange> &out) {
	std::map<std::string, const CatalogView *> old_views, new_views;
	for (const auto &v : old_model.views) {
		old_views[history_break ? NameKey(v.schema_name, v.name) : std::to_string(v.object_id)] = &v;
	}
	for (const auto &v : new_model.views) {
		new_views[history_break ? NameKey(v.schema_name, v.name) : std::to_string(v.object_id)] = &v;
	}
	for (const auto &o : old_views) {
		if (!new_views.count(o.first)) {
			auto c = ViewChange("view_dropped", nullptr, o.second);
			WithDefinitions(definition, c, nullptr, o.second);
			out.push_back(std::move(c));
		}
	}
	for (const auto &o : old_views) {
		auto n = new_views.find(o.first);
		if (n != new_views.end()) {
			CompareKeptView(*o.second, *n->second, definition, out);
		}
	}
	for (const auto &n : new_views) {
		if (!old_views.count(n.first)) {
			auto c = ViewChange("view_created", n.second, nullptr);
			WithDefinitions(definition, c, n.second, nullptr);
			out.push_back(std::move(c));
		}
	}
}

}  // namespace

std::vector<SchemaChange> CompareCatalogs(const CatalogModel &old_model, const CatalogModel &new_model,
										  bool history_break, const ViewDefinition &definition,
										  const CatalogModel *old_owners_model, const CatalogModel *new_owners_model) {
	std::vector<SchemaChange> out;
	// Tables, by object id -- by name across a break, where object ids compare as nothing.
	const TableIndex old_tables(old_model), new_tables(new_model);
	if (history_break) {
		CompareTablesByName(old_tables, new_tables, out);
	} else {
		const auto old_owners = RowsetOwners(old_owners_model ? *old_owners_model : old_model);
		const auto new_owners = RowsetOwners(new_owners_model ? *new_owners_model : new_model);
		CompareTablesById(old_tables, new_tables, old_owners, new_owners, out);
	}
	CompareViews(old_model, new_model, history_break, definition, out);
	return out;
}

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb
