//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// backup/backup_schema_changes.hpp
//
// Spec 085 task 24c (PR 4c-3, DQ-47): the schema changes between two states of a database -- two catalog
// models -- by object id and column id, so a rename is a rename. What an importer applies before the
// rows. DuckDB-free.
//===----------------------------------------------------------------------===//

#pragma once

#include "backup/backup_catalog_model.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace duckdb {
namespace mssql {
namespace backup {

//! One change. `object_type` is "table" or "view"; a column's change carries its table's names and the
//! column's. Old and new describe the object (or column) before and after; empty where it has none.
struct SchemaChange {
	std::string change_type;  // table_created, table_dropped, table_renamed, table_reload, table_truncated,
							  // partition_switched_out, column_added,
							  // column_dropped, column_renamed, column_type_changed,
							  // column_nullability_changed, view_created, view_dropped, view_renamed,
							  // view_definition_changed, view_indexed, view_unindexed
	std::string object_type;
	//! sys.objects' type of the object -- a table's `U`, `S` or `IT`, a view's `V` -- and is_ms_shipped, so a
	//! consumer that mirrors only the user's objects filters the system and internal tables out (review of #612:
	//! every table is compared, and a history break reloads ~115 tables of schema sys).
	std::string type;
	bool is_ms_shipped = false;
	int32_t object_id = 0;	// the new state's; the old state's for a drop
	int32_t old_object_id = 0;
	std::string schema_name, name;
	std::string old_schema_name, old_name;
	int32_t column_id = 0;	// 0: not a column's
	std::string column_name, old_column_name;
	std::string sql_type, old_sql_type;
	int8_t is_nullable = -1, old_is_nullable = -1;	// -1: not a column's
	//! A view's T-SQL, read only for its definition events: `has_definition` false leaves it NULL.
	bool has_definition = false, has_old_definition = false;
	std::string definition, old_definition;
	std::string reason;	 // table_reload's, and a definition that could not be read
	//! table_truncated's / partition_switched_out's partitions (by number); empty otherwise.
	std::vector<int32_t> partitions;
};

//! The text of a view's definition in the old (`old_state`) or new state, or a FormatError's reason.
using ViewDefinition = std::function<std::string(bool old_state, const CatalogView &view)>;

//! The changes from `old_model` to `new_model`, in the order an importer applies them: the tables'
//! reloads and drops, then each kept table's rename, reload, truncation, switch out and columns (renames,
//! drops, additions, type and nullability changes), then the tables created; the views after the tables.
//! Not events: a column's collation, identity, computed definition, sparseness or alias type schema
//! alone; a table recreated under another case of its name (names compare as bytes). `history_break`: the two states
//! are of a database restored or recreated between them -- object ids compare as nothing, so every table of the new
//! state is a `table_reload` and one the old state alone has is dropped, by name. `old_owners` /
//! `new_owners`: the models a rowset's owner is found in, every object's where the models compared are
//! narrowed by an ATTACH's filters (review of #580: a partition switched in from a table they leave out
//! is still a reload); none: the models compared.
std::vector<SchemaChange> CompareCatalogs(const CatalogModel &old_model, const CatalogModel &new_model,
										  bool history_break, const ViewDefinition &definition,
										  const CatalogModel *old_owners = nullptr,
										  const CatalogModel *new_owners = nullptr);

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb
