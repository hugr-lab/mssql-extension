# Data Model: VARCHAR to NVARCHAR Conversion

**Date**: 2026-02-02
**Feature**: 026-varchar-nvarchar-conversion

## Overview

This feature involves query transformation, not data model changes. The existing `MSSQLColumnInfo` entity already contains the required metadata. This document describes the relevant entities and their relationships.

## Entities

### MSSQLColumnInfo (Existing)

Column metadata including SQL Server-specific information. Already contains all fields needed for conversion logic.

**Location**: `src/include/catalog/mssql_column_info.hpp`

| Field | Type | Description | Used For |
|-------|------|-------------|----------|
| name | string | Column name | Query generation |
| sql_type_name | string | "varchar", "char", "nvarchar", etc. | Type detection |
| max_length | int16_t | Max length (-1 for MAX types) | NVARCHAR length calc |
| is_unicode | bool | True for NVARCHAR/NCHAR/NTEXT | Skip conversion |
| is_utf8 | bool | Derived from _UTF8 collation suffix | Skip conversion |
| collation_name | string | Full collation name | Debug/logging |

**Conversion Decision Matrix**:

| sql_type_name | is_unicode | is_utf8 | Needs Conversion? |
|---------------|------------|---------|-------------------|
| varchar | false | false | YES |
| varchar | false | true | NO |
| char | false | false | YES |
| char | false | true | NO |
| nvarchar | true | N/A | NO |
| nchar | true | N/A | NO |
| int, date, etc. | N/A | N/A | NO |

### MSSQLCatalogScanBindData (Modified)

Bind data passed from `MSSQLTableEntry::GetScanFunction()` to `TableScanInitGlobal()`.

**Location**: `src/include/mssql_functions.hpp`

**New Field**:

| Field | Type | Description |
|-------|------|-------------|
| mssql_columns | vector<MSSQLColumnInfo> | Full column metadata for conversion logic |

**Data Flow**:

```text
MSSQLTableEntry
    │
    ├── mssql_columns_ (private member)
    │
    └── GetScanFunction()
            │
            ├── GetMSSQLColumns() → vector<MSSQLColumnInfo>
            │
            └── Creates MSSQLCatalogScanBindData
                    │
                    └── mssql_columns (NEW) ← copied from GetMSSQLColumns()
                            │
                            └── TableScanInitGlobal()
                                    │
                                    └── Uses mssql_columns[col_idx] for conversion check
```

## Relationships

### Column Index Mapping

In `TableScanInitGlobal()`, `column_ids` contains indices into the table's column list:

```text
column_ids[output_position] = table_column_index

bind_data.all_column_names[table_column_index] = column_name
bind_data.all_types[table_column_index] = duckdb_type
bind_data.mssql_columns[table_column_index] = MSSQLColumnInfo (NEW)
```

All three vectors are parallel (same indices refer to same column).

## Validation Rules

### Conversion Criteria

```cpp
bool NeedsNVarcharConversion(const MSSQLColumnInfo &col) {
    // Rule 1: Not Unicode (NCHAR/NVARCHAR/NTEXT already Unicode)
    if (col.is_unicode) return false;

    // Rule 2: Must be text type (CHAR or VARCHAR)
    if (!MSSQLColumnInfo::IsTextType(col.sql_type_name)) return false;

    // Rule 3: Not UTF-8 collation
    if (col.is_utf8) return false;

    return true;
}
```

### Length Calculation

```cpp
std::string GetNVarcharLength(int16_t max_length) {
    // Rule 1: MAX types stay MAX
    if (max_length == -1) return "MAX";

    // Rule 2: Cap at NVARCHAR limit
    if (max_length > 4000) return "4000";

    // Rule 3: Preserve original length
    return std::to_string(max_length);
}
```

## State Transitions

N/A - This feature does not involve state machines. It's a stateless query transformation.
