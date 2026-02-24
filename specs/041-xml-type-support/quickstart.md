# Quickstart: XML Data Type Support

**Feature**: 041-xml-type-support
**Date**: 2026-02-24

## Changed Files Summary

### TDS Protocol Layer (Reading)

| File | Change | Complexity |
|------|--------|------------|
| `src/tds/tds_column_metadata.cpp` | Add `TDS_TYPE_XML` case in `ParseTypeInfo()` — set `max_length = 0xFFFF`, no extra metadata | 3 lines |
| `src/tds/tds_row_reader.cpp` | Add `TDS_TYPE_XML` case in 4 methods: `ReadValue`, `SkipValue`, `ReadValueNBC`, `SkipValueNBC` — route through `ReadPLPType`/`SkipPLPType` | 4×2 lines |
| `src/tds/encoding/type_converter.cpp` | Map XML→VARCHAR in `GetDuckDBType()`, add to `IsSupported()`, route through `ConvertString()` with UTF-16LE decode | 6 lines |

### BCP Protocol Layer (Writing)

| File | Change | Complexity |
|------|--------|------------|
| `src/copy/bcp_writer.cpp` | Add `TDS_TYPE_XML` case in `BuildColmetadataToken()` — no additional metadata (like DATE) | 3 lines |
| `src/copy/target_resolver.cpp` | Add `"xml"` mapping in `SQLServerTypeToTDSToken()` and `SQLServerTypeMaxLength()` | 4 lines |

### DML Guards

| File | Change | Complexity |
|------|--------|------------|
| `src/dml/insert/mssql_batch_builder.cpp` | Add XML column check in `SerializeRow()` before serialization | 6 lines |
| `src/dml/update/mssql_update_statement.cpp` | Add XML column check in `Build()` before serialization | 6 lines |

### Test Data Setup

| File | Change | Complexity |
|------|--------|------------|
| `docker/init/init.sql` | Add `dbo.XmlTestTable` with XML column, sample data (simple, NULL, Unicode, large) | ~15 lines |

### Integration Tests

| File | Change | Complexity |
|------|--------|------------|
| `test/sql/xml/xml_read.test` | New — SELECT XML columns: simple, NULL, empty, Unicode, large PLP, mixed types, mssql_scan() | ~60 lines |
| `test/sql/xml/xml_copy_bcp.test` | New — COPY TO existing table with XML column, NULL round-trip, Unicode round-trip | ~50 lines |
| `test/sql/xml/xml_dml_error.test` | New — INSERT/UPDATE error messages for XML columns, non-XML columns still work | ~40 lines |
| `test/sql/xml/xml_nbc.test` | New — XML columns in NBC (Null Bitmap Compressed) row format | ~20 lines |

## Build & Test

```bash
# Build
GEN=ninja make

# Run unit tests (no SQL Server needed)
GEN=ninja make test

# Start SQL Server container
make docker-up

# Run integration tests
GEN=ninja make integration-test
```

## Implementation Order

1. **Docker init** — add `XmlTestTable` to `docker/init/init.sql` (test data setup)
2. **Type converter** — map XML→VARCHAR (unblocks everything else)
3. **Column metadata parser** — handle XML in COLMETADATA
4. **Row reader** — add XML to all 4 dispatch methods
5. **Read tests** — `xml_read.test` + `xml_nbc.test` (validate read path)
6. **BCP writer** — add XML type in COLMETADATA builder
7. **Target resolver** — add "xml" type mapping
8. **BCP tests** — `xml_copy_bcp.test` (validate write path)
9. **DML guards** — reject XML in INSERT/UPDATE serialization
10. **DML error tests** — `xml_dml_error.test` (validate error messages)

## Key Code Patterns to Follow

### Adding a type case (row reader)

```cpp
// Pattern from NVARCHAR(MAX) in ReadValue():
case TDS_TYPE_NVARCHAR:
    if (col.IsPLPType()) {
        return ReadPLPType(data, length, value, is_null);
    }
    return ReadVariableLengthType(data, length, value, is_null);

// XML is ALWAYS PLP, so simpler:
case TDS_TYPE_XML:
    return ReadPLPType(data, length, value, is_null);
```

### Adding type in ConvertString()

```cpp
// Current:
if (column.type_id == TDS_TYPE_NCHAR || column.type_id == TDS_TYPE_NVARCHAR) {
    str = Utf16LEDecode(value.data(), value.size());
}

// Add XML:
if (column.type_id == TDS_TYPE_NCHAR || column.type_id == TDS_TYPE_NVARCHAR || column.type_id == TDS_TYPE_XML) {
    str = Utf16LEDecode(value.data(), value.size());
}
```

### DML guard pattern

```cpp
// In SerializeRow() after getting col:
if (col.mssql_type == "xml") {
    throw InvalidInputException(
        "MSSQL Error: XML column '%s' cannot be used in INSERT via SQL literals. "
        "Use COPY TO with BCP protocol instead (FORMAT bcp).",
        col.name.c_str());
}
```
