//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// backup/backup_catalog_model.hpp
//
// Spec 085 PR 2b step 6 (task 12): the database's catalog as the backup holds it -- schemas, user
// tables (their columns, rowsets, allocation units, layout and the defaults of columns added after
// rows were written) and views (their definitions) -- derived from the system base tables
// (backup_system_tables). What mssql_backup_tables / _views report, and what the scan binds.
//
// Pinned on the catalog fixture (2022; spec 085 § Rules, catalog, lists them):
//  - a user table is a sysschobjs row of type 'U ' without status bit 0x1 (the MS-shipped
//    trace_xe_* tables every database lists have it, and no rowset); a view one of type 'V ', bit
//    0x10000 when WITH ENCRYPTION;
//  - syscolpars.status: 0x1 NOT NULL, 0x4 identity, 0x8 ROWGUIDCOL, 0x10 computed, 0x40 persisted,
//    0x1000000 sparse, 0x2000000 a column set; a type's name is its sysscalartypes row (by utype:
//    the system types are listed there too);
//  - schemas are sysclsobjs class 50;
//  - a base rowset's owner type: 1 the rowset of a partition (a heap's, a clustered index's, a
//    clustered columnstore's compressed rowgroups), 2 and 3 a columnstore's other two (its delta
//    store holds rcrows of the rows not yet compressed);
//  - a module's text (sysobjvalues valclass 1 by object, valclass 2 by (table, column) for a computed
//    column) is stored as Latin-1 bytes when sysobjvalues.value (a sql_variant int) has 0x2 -- every
//    character in U+0000-U+00FF: a definition with a euro sign is UTF-16 --, else as UTF-16LE;
//  - a metadata-only ADD COLUMN's default is sysseobjvalues valclass 1 by (rowset, hbcolid), a
//    sql_variant whose value is in the column's record form.
// DuckDB-free (D15).
//===----------------------------------------------------------------------===//

#pragma once

#include "backup/backup_sql_variant.hpp"
#include "backup/backup_system_tables.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace duckdb {
namespace mssql {
namespace backup {

struct CatalogSchema {
	int32_t id = 0;
	std::string name;
};

//! sysschobjs.status / syscolpars.status bits the catalog reads (pinned on the catalog fixture, 2022).
static constexpr int32_t OBJECT_MS_SHIPPED = 0x1;
static constexpr int32_t VIEW_ENCRYPTED = 0x10000;
static constexpr int32_t COLUMN_NOT_NULL = 0x1;
static constexpr int32_t COLUMN_IDENTITY = 0x4;
static constexpr int32_t COLUMN_ROWGUIDCOL = 0x8;
static constexpr int32_t COLUMN_COMPUTED = 0x10;
static constexpr int32_t COLUMN_PERSISTED = 0x40;
static constexpr int32_t COLUMN_SPARSE = 0x1000000;
static constexpr int32_t COLUMN_SET = 0x2000000;

//! A user table: type 'U ', not MS-shipped (the trace_xe_* tables every database lists are).
bool IsUserTable(const SysObject &o);
//! A view, not MS-shipped (encrypted or not: VIEW_ENCRYPTED tells).
bool IsUserView(const SysObject &o);
//! A view whose definition may be read: a user view not WITH ENCRYPTION.
bool IsReadableView(const SysObject &o);
//! A default constraint (type 'D '), not MS-shipped.
bool IsUserDefault(const SysObject &o);

//! The sysobjvalues rows that may leave the reader (DQ-11: module source never does), each checked
//! against the object it belongs to: valclass 1 of a readable view or a default, with subobjid and
//! valnum 0; valclass 2 (a computed column's definition) of a computed column of a user table -- not by
//! the class alone, since a multi-statement function's return table can have a computed column and its
//! definition is a valclass 2 row of the function (measured: an encrypted TVF's); valclass 30 of a
//! partition function (sysclsobjs class 30). Any other row is kept back.
class ObjectValueFilter {
public:
	ObjectValueFilter(const BootstrapTables &tables, std::set<int32_t> partition_functions);
	bool Keep(int64_t valclass, int32_t object_id, int64_t sub_id, int64_t valnum) const;

private:
	std::set<int32_t> definitions_;
	std::set<std::pair<int32_t, int32_t>> computed_;
	std::set<int32_t> partition_functions_;
};

//! The partition functions of the database: sysclsobjs class 30, read through `store`.
std::set<int32_t> ReadPartitionFunctions(const PageStore &store, const BootstrapTables &tables);

//! A module's text (a view's definition, a computed column's): read when in row; an off-row one is
//! kept as its LOB root and read on demand (ReadModuleDefinition), so listing a catalog never reads
//! every long definition, and one that cannot be read is named on its object alone (D13).
struct ModuleDefinition {
	enum class State : uint8_t { Absent, Ready, Deferred, Unreadable };
	State state = State::Absent;
	std::string text;	 // Ready: UTF-8
	std::string reason;	 // Unreadable
	std::string root;	 // Deferred: the in-row LOB root
	int64_t flags = 0;	 // Deferred: sysobjvalues.value
	uint64_t lob_unit = 0;
};

//! The text of `definition` (`what` names it): a Ready one's, a Deferred one read through `store`
//! (at most MAX_MODULE_TEXT_BYTES); a FormatError naming the reason for an Unreadable or Absent one.
std::string ReadModuleDefinition(const PageStore &store, const ModuleDefinition &definition, const std::string &what);

struct CatalogColumn {
	int32_t column_id = 0;
	std::string name;
	uint8_t system_type_id = 0;
	int32_t user_type_id = 0;
	std::string type_name;	// TYPE_NAME(user_type_id): an alias type's own name
	//! The system type underneath: sysscalartypes' row of id system_type_id (a CLR type's by its
	//! user type: hierarchyid, geometry, geography; "" for a user-defined CLR type).
	std::string system_type_name;
	int16_t max_length = 0;	 // bytes; -1 MAX
	uint8_t precision = 0;
	uint8_t scale = 0;
	uint32_t collation_id = 0;	// 0 for a type without one
	bool is_nullable = false;
	bool is_identity = false;
	bool is_computed = false;
	bool is_persisted = false;
	bool is_sparse = false;
	bool is_column_set = false;
	bool is_rowguidcol = false;
	//! A computed column's definition (sysobjvalues valclass 2).
	ModuleDefinition definition;
};

struct CatalogPartition {
	uint64_t rowset_id = 0;
	int32_t partition_number = 0;
	//! rcrows of the partition's rowsets of owner types 1 and 3 (a columnstore's delta stores), -1 unknown.
	//! A clustered columnstore's delta stores include those a REORGANIZE left as tombstones until the
	//! tuple mover drops them, which sysrowsets cannot tell from an open one: the sum is what
	//! sys.partitions.rows says up to SQL Server 2022 (measured on 2019 and 2022; 2025's leaves
	//! tombstones out, the live count); spec 088 reads the rowgroup metadata that makes it exact (#551).
	int64_t rows = 0;
	uint8_t compression = 0;   // sysrowsets.cmprlevel: 0 none, 1 row, 2 page, 3 columnstore, 4 archive
	int16_t filegroup_id = 0;  // its in-row unit's (= the scheme's destination, E7)
	std::string filegroup;	   // its name (sysclsobjs class 31), "" when not listed
	//! Its layout, an index into CatalogTable::layouts (CatalogTable::Layout); only when the table is
	//! decodable. Rows are decoded per rowset: partitions may differ (in compression, after a SWITCH).
	size_t layout = 0;
};

//! A partition function's boundary as sysobjvalues holds it: a sql_variant, or NULL (a legal
//! boundary, the lowest value: sys.partition_range_values shows it so).
struct PartitionBoundary {
	bool is_null = false;
	std::string bytes;
};

//! A partitioned table's partitioning (E7; spec 085 § Rules, catalog, partitioning).
struct CatalogPartitioning {
	int32_t function_id = 0;
	std::string function_name;
	int32_t scheme_id = 0;
	std::string scheme_name;
	int32_t column_id = 0;	// the partitioning column (sysiscols' partition ordinal 1)
	std::string column_name;
	//! The function's parameter (systypedsubobjs class 30): its type as a column's (TYPE_NAME by utype).
	uint8_t parameter_xtype = 0;
	std::string parameter_type_name;
	std::string parameter_system_type_name;
	int16_t parameter_length = 0;
	uint8_t parameter_precision = 0;
	uint8_t parameter_scale = 0;
	uint32_t parameter_collation = 0;
	bool range_right = false;
	int32_t fanout = 0;	 // partitions; boundaries = fanout - 1
	//! The boundaries, by boundary id 1..fanout-1 (positional: SPLIT / MERGE renumber them): each a
	//! sql_variant in the parameter's own type (a varbinary one without its trailing zero bytes, as
	//! the server stores and prints it).
	std::vector<PartitionBoundary> boundaries;
	std::vector<std::string> destinations;	// filegroup names by destination (= partition number) 1..fanout
	std::string next_used;					// the destination past the fanout, "" when none
	//! Why the partitioning cannot be given exactly (D13: named, never a guess); empty otherwise. With a
	//! reason the boundaries are empty (never a partial list); the destinations and NEXT USED are given
	//! when only the boundaries could not be (they do not depend on the values), empty otherwise. The
	//! rows are read either way: partitioning is reference only.
	std::string reason;
};

//! An allocation unit of one of the table's rowsets (any index), with its rowset's fields.
struct CatalogUnit {
	SysAllocUnit unit;
	int32_t index_id = 0;
	int32_t partition_number = 0;
	uint8_t compression = 0;
};

enum class TableShape : uint8_t { Heap, Clustered, Columnstore };

struct CatalogTable {
	int32_t object_id = 0;
	int32_t schema_id = 0;
	std::string schema_name;
	std::string name;
	std::string created;  // datetime, 8 bytes in record form (ticks, then days)
	std::string modified;
	TableShape shape = TableShape::Heap;
	std::vector<CatalogPartition> partitions;  // owner type 1, by partition number
	//! Its base index's data space is a partition scheme; `partitioning` then describes it.
	bool partitioned = false;
	CatalogPartitioning partitioning;
	int64_t row_count = 0;				 // the partitions' rows summed (CatalogPartition::rows: tombstones too)
	bool row_count_known = true;		 // false: a count negative, or the sum past int64
	std::vector<CatalogColumn> columns;	 // by column id
	std::vector<CatalogUnit> units;
	//! Whether the scan can read the table; `reason` names what it cannot when not (spec 085 D13).
	bool decodable = false;
	std::string reason;
	//! The layouts of its partitions, each distinct one once (a table of 15000 partitions has one
	//! layout, not 15000 copies of it, review of #551); only when decodable. A shared layout's
	//! `rowset_id` is 0: a partition's own is CatalogPartition::rowset_id (a unit looked up by the
	//! layout's would be the first partition's for every partition).
	std::vector<RowsetLayout> layouts;
	const RowsetLayout &Layout(const CatalogPartition &p) const {
		if (p.layout >= layouts.size()) {
			throw FormatError("backup: " + schema_name + "." + name + " has no layout: it is not decodable");
		}
		return layouts[p.layout];
	}
};

struct CatalogView {
	int32_t object_id = 0;
	int32_t schema_id = 0;
	std::string schema_name;
	std::string name;
	std::string created;
	std::string modified;
	bool is_encrypted = false;
	//! The T-SQL text; Absent for an encrypted view (its bytes are obfuscated, never read).
	ModuleDefinition definition;
	//! Why the view's own row cannot be trusted (its schema is not one); empty otherwise.
	std::string reason;
};

struct CatalogModel {
	std::vector<CatalogSchema> schemas;	 // by id
	std::vector<CatalogTable> tables;	 // by schema name, then name (bytes)
	std::vector<CatalogView> views;		 // by schema name, then name (bytes)

	//! The table `name` of schema `schema`, or null (a binary search).
	const CatalogTable *FindTable(const std::string &schema, const std::string &name) const;
	//! The schema `id`'s name, or "" when the catalog has none (a binary search).
	std::string SchemaName(int32_t id) const;
};

//! A type as T-SQL declares it: a system type with its length, precision or scale (nchar / nvarchar
//! counted in characters); an alias or a CLR type by its own name.
std::string SqlTypeText(const std::string &type_name, const std::string &system_type_name, uint8_t system_type_id,
						int16_t max_length, uint8_t precision, uint8_t scale);

//! The most bytes of one module's text the catalog reads (a view's definition is nvarchar(max)).
static constexpr uint64_t MAX_MODULE_TEXT_BYTES = 64ull << 20;
//! The most bytes of off-row values one call of a catalog function reads in all (each within
//! MAX_MODULE_TEXT_BYTES): a crafted file whose rows all point to one large LOB must not cost its size
//! once per row.
static constexpr uint64_t MAX_CALL_OFF_ROW_BYTES = 1ull << 30;

//! Refuses a value the reader cannot give (a FormatError naming `what`), or returns: how the model
//! asks whether a partition boundary decodes, so that the model's `reason` and the value
//! mssql_backup_tables gives are one verdict (review of #551). The extension passes the DuckDB-side
//! decoder (SqlVariantValue); without one, a boundary is checked against its parameter only.
using BoundaryCheck = std::function<void(const SqlVariant &value, const std::string &what)>;

//! The base tables partitioning is read from (E7), each read through its own description. A key listed
//! twice is kept in a `*_twice` set and refuses only what uses it (D13).
struct PartitionCatalog {
	struct Function {
		std::string name;
		bool range_right = false;
		int32_t fanout = 0;
	};
	struct Parameter {
		uint8_t xtype = 0;
		int32_t user_type = 0;
		int16_t length = 0;
		uint8_t precision = 0, scale = 0;
		uint32_t collation = 0;
	};
	std::map<int32_t, Function> functions;								 // sysclsobjs class 30
	std::map<int32_t, Parameter> parameters;							 // systypedsubobjs class 30, subid 1
	std::map<int32_t, std::map<int32_t, PartitionBoundary>> boundaries;	 // sysobjvalues valclass 30
	std::set<std::pair<int32_t, int32_t>> boundaries_twice;				 // (function, boundary id)
	std::map<int32_t, std::string> filegroups, schemes;					 // sysclsobjs class 31
	std::map<int32_t, int32_t> scheme_function;							 // syssingleobjrefs class 31
	std::map<int32_t, std::map<int32_t, int32_t>> destinations;			 // class 30: scheme -> dest -> fg
	std::map<std::pair<int32_t, int32_t>, int32_t> data_space;			 // class 7: (object, index) -> space
	std::map<std::pair<int32_t, int32_t>, int32_t> partition_column;	 // sysiscols, partition ordinal 1
	std::set<int32_t> functions_twice, data_spaces_twice, parameters_twice, schemes_twice;
	std::set<std::pair<int32_t, int32_t>> destinations_twice, data_space_twice, partition_column_twice;
	std::set<int32_t> boundaries_off_row;  // functions with a boundary stored off-row
	//! A table of these could not be read: the partitioning of a table it leaves unknown is named so.
	std::string reason;
	//! syssingleobjrefs was read whole: a table with no data-space row is then on the default one,
	//! whatever table failed after it.
	bool data_spaces_read = false;
};

//! The partitioning catalog: `classes` is sysclsobjs, read already by the model; the other tables are
//! read only when it lists a partition scheme (a database without one reads none of them).
PartitionCatalog ReadPartitionCatalog(const PageStore &store, const BootstrapTables &tables,
									  const SystemTable &classes);

//! Table `t`'s partitioning from `pc` into t.partitioned / t.partitioning, or its reason (D13): named,
//! never a guess and never a partial list. `base_index` is its heap's 0 or clustered 1; `check`, when
//! given, decides whether each boundary decodes. `type_names` is sysscalartypes by id.
void DescribePartitioning(const PartitionCatalog &pc, const std::unordered_map<int32_t, std::string> &type_names,
						  int32_t base_index, const BoundaryCheck &check, CatalogTable &t);

//! The catalog model of the database whose base tables are `tables`, its other system tables read
//! through `store`. What one object's rows get wrong is named on that object (D13): a table the scan
//! cannot read -- its schema or a column's type not in the catalog, an added column's default it
//! cannot use, a feature not read yet -- is listed with `decodable` false and its reason; a
//! definition that cannot be read is Unreadable with its reason. Coarser than D13 for now: a sparse
//! column or an unusable default refuses the table, not the column (the scan's column-level refusal
//! is PR 2d's). A FormatError only for what the model as a whole rests on (a system table missing,
//! a malformed row of sysclsobjs or sysscalartypes).
CatalogModel BuildCatalogModel(const PageStore &store, const BootstrapTables &tables,
							   const BoundaryCheck &check = BoundaryCheck());

//! The flag of a module's sysobjvalues `value` that says its text is Latin-1, not UTF-16LE.
constexpr int64_t MODULE_TEXT_LATIN1 = 0x2;

//! A module's text from its sysobjvalues bytes and flags (`value`, a sql_variant int): Latin-1 bytes
//! when the flags have MODULE_TEXT_LATIN1, else UTF-16LE; UTF-8 either way.
std::string ModuleText(const std::string &bytes, int64_t flags, const std::string &what);

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb
