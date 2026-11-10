//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// backup/backup_system_tables.hpp
//
// Spec 085 PR 2b step 5 (task 11): the database's system base tables, read from the backup's pages.
//
// A rowset's physical layout -- per column its rscolid, type, fixed offset or variable index, null
// bit and bit position (sysrscols) and its name (syscolpars) -- and ONE decoder of a FixedVar record
// through it (spec 085 § Rules, records): dropped columns skipped, a column added after the record
// was written (its null bit past the record's column count) its default or NULL, the null bitmap,
// fixed bytes at an offset, a variable column by index (empty past the record's variable count),
// a complex variable column kept as its in-row bytes and marked off-row, a forwarded record's back
// pointer left out, a uniquifier left out.
//
// The five bootstrap tables -- sysallocunits, sysrowsets, sysschobjs, syscolpars, sysrscols -- cannot
// be described before they are read, so their layouts are a table of literals (the fields the reader
// takes), decoded by the same decoder. sysallocunits is read along its leaf chain from the boot page's
// dbi_firstSysIndexes, and its own row's IAM chain must list exactly those level-0 data pages; the other four
// through their IAM chains (DQ-40), found by their fixed allocation unit ids. Once read, every
// literal is checked against the tables' own description of themselves (sysrscols + syscolpars), so
// a version that moves a field is refused by name rather than misread. Measured on the lane's
// 2017-2025 sets of the samples' database in R2 (2026-11-10): the fields sit where the literals put them on all four;
// 2022 added a trailing sysrscols column (ordlock), which the literals do not take. Any other system table is read
// through its own description (ReadSystemTable). DuckDB-free (D15).
//===----------------------------------------------------------------------===//

#pragma once

#include "backup/backup_boot.hpp"
#include "backup/backup_page.hpp"
#include "backup/backup_record.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace duckdb {
namespace mssql {
namespace backup {

//! The xtype values the system-table decoder reads (sys.types' system_type_id).
enum SystemXtype : uint8_t {
	XTYPE_UNIQUEIDENTIFIER = 36,
	XTYPE_TINYINT = 48,
	XTYPE_SMALLINT = 52,
	XTYPE_INT = 56,
	XTYPE_SMALLDATETIME = 58,
	XTYPE_DATETIME = 61,
	XTYPE_SQL_VARIANT = 98,
	XTYPE_BIT = 104,
	XTYPE_BIGINT = 127,
	XTYPE_VARBINARY = 165,
	XTYPE_VARCHAR = 167,
	XTYPE_BINARY = 173,
	XTYPE_CHAR = 175,
	XTYPE_NVARCHAR = 231,
	XTYPE_NCHAR = 239,
};

//! One physical column of a rowset (a sysrscols row, named by syscolpars).
struct PhysicalColumn {
	std::string name;
	int32_t rscolid = 0;
	int32_t hbcolid = 0;
	uint8_t xtype = 0;
	uint16_t length = 0;  // bytes, for binary / char / nchar and the variable types (ti >> 8)
	uint8_t precision = 0;
	uint8_t scale = 0;
	uint32_t collation = 0;	 // a CollationId
	uint32_t status = 0;	 // 0x2 dropped, 0x10 uniquifier, 0x80 NOT NULL, 0x200 has a default
	int16_t offset = 0;		 // > 0 a fixed offset in the record, -n the n-th variable column
	uint16_t null_bit = 0;	 // 1-based, in the null bitmap
	uint8_t bit_position = 0;
	int16_t ordkey = 0;	 // its place in the clustered index key, 1-based; 0 when not a key column
	//! The value of a column added after a record was written (sysseobjvalues; the model's to give).
	bool has_default = false;
	std::string default_bytes;

	bool Dropped() const {
		return (status & 0x2) != 0;
	}
	bool Uniquifier() const {
		return (status & 0x10) != 0;
	}
	bool HasDefaultStatus() const {
		return (status & 0x200) != 0;
	}
	//! Bytes the column takes in the fixed part; a FormatError for a type this reader does not read.
	uint16_t FixedLength(const std::string &table) const;
};

//! A rowset's physical columns, in rscolid order.
struct RowsetLayout {
	std::string table;	// for errors
	uint64_t rowset_id = 0;
	std::vector<PhysicalColumn> columns;

	//! The column named `name`, or null.
	const PhysicalColumn *Find(const std::string &name) const;
	//! Its index in `columns`; a FormatError naming it when the layout has none.
	size_t IndexOf(const std::string &name) const;
};

//! One column's value in a decoded record.
struct SystemValue {
	bool is_null = true;
	bool off_row = false;  // a complex column: `bytes` is its in-row pointer, not the value
	std::string bytes;	   // fixed bytes, a variable column's bytes; a bit as one byte 0 / 1
};

//! A record's values, one per column of its layout (dropped and uniquifier columns stay null).
struct SystemRow {
	std::vector<SystemValue> values;
	PagePointer page;
	uint16_t slot = 0;
};

//! The record `record` through `layout`: one value per column. Every byte read is bounded by the
//! record's parts; a column the record does not hold where its layout says is a FormatError naming
//! the column. A column added after the record with the default status and no default given is a
//! FormatError too (it would read as NULL where the server reads its default).
std::vector<SystemValue> DecodeRecord(const RecordView &record, const RowsetLayout &layout);

//! Value `value` of column `column` of `layout` as an integer (tinyint / smallint / int / bigint /
//! bit; tinyint and bit unsigned, the others sign-extended), a string (nvarchar / nchar as UTF-16, every unit kept;
//! char / varchar bytes as they are), a binary(6) page pointer. NULL is `null_value`, "" and the null pointer. A value
//! of another type, or of another size than its type's, is a FormatError naming the column.
int64_t ValueInt(const RowsetLayout &layout, size_t column, const SystemValue &value, int64_t null_value = 0);
std::string ValueText(const RowsetLayout &layout, size_t column, const SystemValue &value);
PagePointer ValuePointer(const RowsetLayout &layout, size_t column, const SystemValue &value);

//! A table's rows and the layout they were decoded through.
struct SystemTable {
	RowsetLayout layout;
	std::vector<SystemRow> rows;

	//! Column `name`'s index (IndexOf).
	size_t Column(const std::string &name) const {
		return layout.IndexOf(name);
	}
	//! ValueInt / ValueText / ValuePointer of row `row`'s column `column`.
	int64_t Int(size_t row, size_t column, int64_t null_value = 0) const {
		return ValueInt(layout, column, rows[row].values[column], null_value);
	}
	std::string Text(size_t row, size_t column) const {
		return ValueText(layout, column, rows[row].values[column]);
	}
	PagePointer Pointer(size_t row, size_t column) const {
		return ValuePointer(layout, column, rows[row].values[column]);
	}
	bool IsNull(size_t row, size_t column) const {
		return rows[row].values[column].is_null;
	}
};

struct SysAllocUnit {
	uint64_t auid = 0;
	uint8_t type = 0;		// 1 IN_ROW_DATA, 2 LOB_DATA, 3 ROW_OVERFLOW_DATA; 0 a deferred drop
	uint64_t owner_id = 0;	// the rowset
	int32_t status = 0;
	int16_t filegroup_id = 0;
	PagePointer first_page;
	PagePointer root_page;
	PagePointer first_iam;
};

struct SysRowset {
	uint64_t rowset_id = 0;
	uint8_t owner_type = 0;
	int32_t object_id = 0;		   // idmajor
	int32_t index_id = 0;		   // idminor: 0 heap, 1 clustered
	int32_t partition_number = 0;  // numpart
	int32_t status = 0;
	int64_t rows = 0;		  // rcrows
	uint8_t compression = 0;  // cmprlevel: 0 none, 1 row, 2 page, 3 columnstore
};

struct SysObject {
	int32_t id = 0;
	std::string name;
	int32_t schema_id = 0;	// nsid
	int32_t status = 0;
	std::string type;  // char(2): 'U ', 'S ', 'V ', ...
	int32_t parent_id = 0;
	std::string created;  // datetime, 8 raw bytes
	std::string modified;
};

struct SysColumn {
	int32_t object_id = 0;
	int16_t number = 0;	 // 0 a table's column
	int32_t column_id = 0;
	std::string name;
	uint8_t xtype = 0;
	int32_t user_type = 0;
	int16_t length = 0;	 // -1 MAX
	uint8_t precision = 0;
	uint8_t scale = 0;
	uint32_t collation_id = 0;
	int32_t status = 0;	 // bit 0: NOT NULL
};

struct SysRsCol {
	uint64_t rowset_id = 0;
	int32_t rscolid = 0;
	int32_t hbcolid = 0;
	uint32_t ti = 0;  // low byte the xtype, then the length, or precision and scale
	uint32_t collation = 0;
	int16_t ordkey = 0;
	uint32_t status = 0;
	int16_t offset = 0;
	uint16_t null_bit = 0;
	uint8_t bit_position = 0;
};

//! The five bootstrap tables, decoded, with the indexes every lookup goes through (BuildIndexes).
struct BootstrapTables {
	std::vector<SysAllocUnit> allocation_units;
	std::vector<SysRowset> rowsets;
	std::vector<SysObject> objects;
	std::vector<SysColumn> columns;
	std::vector<SysRsCol> rscols;

	//! Builds the lookups over the five vectors (ReadBootstrapTables does), and refuses by name what
	//! a database cannot hold twice: an allocation unit, an object id, a system table's name, a
	//! table's column (number 0) by colid, a rowset's physical column by rscolid, two in-row units of
	//! one rowset. Call it again after changing a vector's membership; positions are indexed, so an
	//! element changed in place is seen.
	void BuildIndexes();

	//! The allocation unit `auid`, or null.
	const SysAllocUnit *AllocationUnit(uint64_t auid) const;
	//! The object `id`, or null.
	const SysObject *Object(int32_t id) const;
	//! The system base table (type 'S ') named `name`, or null.
	const SysObject *SystemObject(const std::string &name) const;
	//! The layout of object `object_id`'s base rowset (index 0 heap or 1 clustered; one partition),
	//! from sysrscols and syscolpars: rscolid names the column syscolpars lists as colid. A base
	//! rowset with no in-row unit (a deferred drop, spec 085 § Rules) is not counted. A FormatError
	//! naming the object when it has no live base rowset, more than one (a partitioned table, PR 2c,
	//! or a columnstore), a physical column syscolpars does not name, or more physical columns than a
	//! null bit can number.
	RowsetLayout BaseLayout(int32_t object_id) const;
	//! The layout of rowset `rowset` of object `object_id` (BaseLayout's, for any rowset of it).
	RowsetLayout LayoutOf(int32_t object_id, const SysRowset &rowset) const;
	//! Every sysrowsets row of object `object_id`, in sysrowsets' order.
	std::vector<const SysRowset *> RowsetsOf(int32_t object_id) const;
	//! Every allocation unit rowset `rowset_id` owns, in sysallocunits' order.
	std::vector<const SysAllocUnit *> UnitsOf(uint64_t rowset_id) const;
	//! The in-row allocation unit (type 1) of rowset `rowset_id`, or null.
	const SysAllocUnit *FindInRowUnit(uint64_t rowset_id) const;
	//! The LOB_DATA allocation unit (type 2) of rowset `rowset_id`, or null.
	const SysAllocUnit *FindLobUnit(uint64_t rowset_id) const;
	//! FindInRowUnit, with none a FormatError naming `table`.
	const SysAllocUnit &InRowUnit(uint64_t rowset_id, const std::string &table) const;

private:
	std::unordered_map<uint64_t, size_t> au_index_;
	std::unordered_map<int32_t, size_t> object_index_;
	std::unordered_map<std::string, size_t> system_object_index_;
	std::unordered_map<int32_t, std::vector<size_t>> rowsets_by_object_;
	std::map<std::pair<int32_t, int32_t>, size_t> column_index_;  // (object, colid), number 0
	std::vector<size_t> rscol_order_;							  // by (rowset, rscolid)
	std::unordered_map<uint64_t, size_t> in_row_index_;			  // rowset -> its in-row unit
	std::unordered_map<uint64_t, std::vector<size_t>> units_by_owner_;
};

//! The bootstrap tables of the database whose boot page is `boot`, every page through `store`.
//! FormatError naming the table and the page when a page is not one of the table's, a chain loops,
//! the IAM chain of sysallocunits lists other level-0 data pages than its leaf chain, a table's allocation unit
//! is missing, or a literal layout disagrees with the tables' own description (CheckBootstrapLayouts).
BootstrapTables ReadBootstrapTables(const PageStore &store, const BootPage &boot);

//! One row of bootstrap table `table` (decoded through BootstrapLayout(table)) appended to `out`'s
//! vector of it; a NOT NULL field that is NULL, or a value of the wrong size, is a FormatError naming
//! it. ReadBootstrapTables' own conversion (the fuzz harness drives it over synthetic pages); call
//! BuildIndexes after.
void AddBootstrapRow(BootstrapTables &out, const std::string &table, const SystemRow &row);

//! The reader's literal layouts of the five against their description in `tables`: per field the
//! name, type, fixed offset or variable index, null bit, and a fixed type's length.
void CheckBootstrapLayouts(const BootstrapTables &tables);

//! The literal layout the reader decodes bootstrap table `name` with (the fields it takes).
const RowsetLayout &BootstrapLayout(const std::string &name);

//! The system base table `name` (sysclsobjs, sysseobjvalues, sysobjvalues, ...), read through its
//! own description: its base rowset's in-row unit's IAM chain, every live level-0 data page of it,
//! records of types 0 and 1. A FormatError when the database has no such system table.
SystemTable ReadSystemTable(const PageStore &store, const BootstrapTables &tables, const std::string &name);

//! The most physical columns the system-table decoder takes: it keeps a value per column of every
//! row, so a crafted description of a million columns must not multiply into every record. System
//! tables have a few dozen; a user table's rows are PR 2d's columnar decoder's.
static constexpr size_t MAX_SYSTEM_TABLE_COLUMNS = 1024;

//! Calls `row` for each record of the rowset whose in-row allocation unit is `unit`, through
//! `layout`: the unit's IAM chain, its live data pages of level 0 (each naming the unit in its
//! header), records of types 0 and 1 (freed slots, ghosts, stubs and index records skipped; two
//! slots of one page at one offset refused). A page's error names the table. A layout of more than
//! MAX_SYSTEM_TABLE_COLUMNS columns is refused.
void ForEachRowsetRow(const PageStore &store, const SysAllocUnit &unit, const RowsetLayout &layout,
					  const std::function<void(const SystemRow &)> &row);
//! The pages the IAM chain of `unit` lists, in chain order (its single pages, then every page of its
//! extents): each may hold none of the unit's rows (not live, an IAM page, an upper index level).
std::vector<PagePointer> UnitPages(const PageStore &store, const SysAllocUnit &unit);
//! Page `at` into `out` when it is a live level-0 data page (false otherwise); one naming another unit
//! than `unit` is a FormatError naming `table`, as is a page the store refuses.
bool ReadUnitDataPage(const PageStore &store, const SysAllocUnit &unit, const PagePointer &at, const std::string &table,
					  Page &out);
//! The records of types 0 and 1 of `page` through `layout`, each handed to `row` (ForEachRowsetRow's
//! rules for one page: freed slots, ghosts, stubs and index records skipped; two slots at one record
//! and a layout past MAX_SYSTEM_TABLE_COLUMNS refused).
void ForEachPageRow(const Page &page, const RowsetLayout &layout, const std::function<void(const SystemRow &)> &row);
//! ForEachRowsetRow's rows, kept.
std::vector<SystemRow> ReadRowsetRows(const PageStore &store, const SysAllocUnit &unit, const RowsetLayout &layout);

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb
