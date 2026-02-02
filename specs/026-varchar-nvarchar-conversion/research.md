# Research: VARCHAR to NVARCHAR Conversion

**Date**: 2026-02-02
**Feature**: 026-varchar-nvarchar-conversion

## Research Questions

### 1. Where is table scan query generation located?

**Decision**: Query generation occurs in `src/table_scan/table_scan.cpp` within `TableScanInitGlobal()` function.

**Rationale**: The function builds the SELECT column list at lines 68-208, constructing column expressions like `[column_name]`. This is the exact location to inject CAST wrapping.

**Key Code Locations**:
- Line 139: `column_list += "[" + FilterEncoder::EscapeBracketIdentifier(col_name) + "]";` (with rowid)
- Line 204: `column_list += "[" + FilterEncoder::EscapeBracketIdentifier(bind_data.all_column_names[col_idx]) + "]";` (no rowid)
- Line 155: PK columns for rowid (same pattern)

**Alternatives considered**:
- Modifying TypeConverter to decode CP1252 client-side → Rejected (requires iconv/ICU dependency)
- Adding a query builder class → Overkill for this change

### 2. How is column metadata available at query generation time?

**Decision**: Use `MSSQLColumnInfo` vector from `MSSQLTableEntry`, pass it via `MSSQLCatalogScanBindData`.

**Rationale**:
- `MSSQLTableEntry::GetMSSQLColumns()` returns `vector<MSSQLColumnInfo>` with full column metadata
- `MSSQLColumnInfo` already has `is_unicode`, `is_utf8`, `sql_type_name`, `max_length` fields
- `MSSQLColumnInfo::IsUTF8Collation()` static method already exists

**Current Flow**:
1. `MSSQLTableEntry::GetScanFunction()` creates `MSSQLCatalogScanBindData`
2. Bind data has `all_types` and `all_column_names` but NOT `MSSQLColumnInfo`
3. Need to add `vector<MSSQLColumnInfo> mssql_columns` to bind data

**Alternatives considered**:
- Storing collation in TDS ColumnMetadata → Available but requires catalog query at scan time
- Re-querying column metadata at InitGlobal → Wasteful, data already cached

### 3. What is the algorithm for determining conversion need?

**Decision**: Convert if column is VARCHAR/CHAR AND not UTF-8 collation.

**Rationale**:
```cpp
bool NeedsNVarcharConversion(const MSSQLColumnInfo &col) {
    // Only CHAR/VARCHAR need conversion (not NCHAR/NVARCHAR)
    if (col.is_unicode) {
        return false;  // Already Unicode
    }
    // Check if it's a text type
    if (!MSSQLColumnInfo::IsTextType(col.sql_type_name)) {
        return false;  // Not a string type
    }
    // Check if UTF-8 collation
    if (col.is_utf8) {
        return false;  // UTF-8 is safe
    }
    return true;  // Non-UTF8 CHAR/VARCHAR needs conversion
}
```

**Alternatives considered**:
- Convert ALL VARCHAR unconditionally → Slight overhead for UTF-8 tables, but simpler
- Detect at runtime from TDS collation bytes → Complex, collation name not available

### 4. What is the NVARCHAR length calculation?

**Decision**: Use `MIN(original_length, 4000)` for fixed-length VARCHAR, `MAX` for VARCHAR(MAX).

**Rationale**:
- NVARCHAR maximum non-MAX length is 4000 characters
- VARCHAR(MAX) is indicated by `max_length == -1` in MSSQLColumnInfo
- VARCHAR(8000) at max → must cap at NVARCHAR(4000) with silent truncation

**Length Mapping**:
| VARCHAR Length | NVARCHAR Length |
|----------------|-----------------|
| 1-4000 | Same as VARCHAR |
| 4001-8000 | 4000 (truncated) |
| MAX (-1) | MAX |

**Code Pattern**:
```cpp
std::string GetNVarcharLength(int16_t max_length) {
    if (max_length == -1) {
        return "MAX";  // VARCHAR(MAX) → NVARCHAR(MAX)
    }
    if (max_length > 4000) {
        return "4000";  // Truncate
    }
    return std::to_string(max_length);
}
```

### 5. How to generate the CAST expression?

**Decision**: Build expression string `CAST([column] AS NVARCHAR(n)) AS [column]`.

**Rationale**:
- Must preserve column alias for result set mapping
- Use bracket escaping for column names (already done via `FilterEncoder::EscapeBracketIdentifier`)

**Code Pattern**:
```cpp
std::string BuildColumnExpression(const MSSQLColumnInfo &col) {
    std::string escaped_name = "[" + FilterEncoder::EscapeBracketIdentifier(col.name) + "]";

    if (NeedsNVarcharConversion(col)) {
        std::string nvarchar_len = GetNVarcharLength(col.max_length);
        return "CAST(" + escaped_name + " AS NVARCHAR(" + nvarchar_len + ")) AS " + escaped_name;
    }
    return escaped_name;
}
```

**Generated SQL Example**:
```sql
-- Before
SELECT [id], [name], [description] FROM [dbo].[products]

-- After (name is VARCHAR(100), description is VARCHAR(MAX))
SELECT [id], CAST([name] AS NVARCHAR(100)) AS [name], CAST([description] AS NVARCHAR(MAX)) AS [description] FROM [dbo].[products]
```

### 6. Which code paths need modification?

**Decision**: Three code paths in TableScanInitGlobal() need the same change.

**Rationale**: The column list is built in three places:
1. **Lines 134-141**: Projected columns when rowid is requested
2. **Lines 143-159**: PK columns added for rowid construction
3. **Lines 198-207**: Standard projection without rowid

All three use the same pattern: build `[column_name]` and append to `column_list`.

**Modification Strategy**:
- Create helper function `BuildColumnExpression(const MSSQLColumnInfo &col)`
- Replace direct string concatenation with helper call
- Helper returns either `[column]` or `CAST([column] AS NVARCHAR(n)) AS [column]`

### 7. How to test the implementation?

**Decision**: Create integration test with SQL Server using extended ASCII characters.

**Rationale**:
- Unit tests cannot verify SQL Server encoding behavior
- Need actual non-UTF8 collation data to trigger the bug
- SQLLogicTest format matches existing test patterns

**Test Setup**:
```sql
-- Create table with explicit non-UTF8 collation
CREATE TABLE test_encoding (
    id INT PRIMARY KEY,
    ascii_only VARCHAR(100) COLLATE Latin1_General_CI_AS,
    extended_ascii VARCHAR(100) COLLATE Latin1_General_CI_AS,
    unicode_text NVARCHAR(100),
    large_varchar VARCHAR(8000) COLLATE Latin1_General_CI_AS
);

-- Insert extended ASCII characters (via NCHAR codes for test portability)
INSERT INTO test_encoding VALUES
    (1, 'Hello', 'Caf' + NCHAR(233) + ' r' + NCHAR(233) + 'sum' + NCHAR(233), N'Hello', REPLICATE('x', 5000));
```

**Test Assertions**:
1. Query returns data without UTF-8 errors
2. Extended ASCII characters decoded correctly
3. NULL values preserved
4. Large VARCHAR (>4000) truncated to 4000

## Summary of Decisions

| Question | Decision |
|----------|----------|
| Code location | `table_scan.cpp:TableScanInitGlobal()` |
| Metadata source | `MSSQLColumnInfo` via `MSSQLCatalogScanBindData.mssql_columns` |
| Conversion criteria | `!is_unicode && IsTextType && !is_utf8` |
| Length mapping | `MIN(original, 4000)` or `MAX` |
| Expression format | `CAST([col] AS NVARCHAR(n)) AS [col]` |
| Code paths | 3 places in TableScanInitGlobal |
| Testing | SQLLogicTest with Latin1 collation |
