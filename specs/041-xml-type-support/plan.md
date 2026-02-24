# Implementation Plan: XML Data Type Support

**Branch**: `041-xml-type-support` | **Date**: 2026-02-24 | **Spec**: [spec.md](spec.md)
**Input**: Feature specification from `/specs/041-xml-type-support/spec.md`

## Summary

Add SQL Server XML type (0xF1) support to the extension. XML is read as DuckDB VARCHAR via existing PLP + UTF-16LE infrastructure. Write is supported via BCP/COPY TO and CTAS. INSERT with RETURNING and UPDATE reject XML columns with clear error messages since SQL literal serialization cannot handle XML's 2 GB limit.

## Technical Context

**Language/Version**: C++17 (C++11-compatible for ODR on Linux)
**Primary Dependencies**: DuckDB (main branch), OpenSSL (vcpkg), existing TDS protocol layer
**Storage**: N/A (remote SQL Server via TDS protocol)
**Testing**: SQLLogicTest (integration, requires SQL Server), C++ unit tests (no SQL Server)
**Target Platform**: Linux (GCC), macOS (Clang), Windows (MSVC, MinGW)
**Project Type**: Single (DuckDB extension)
**Performance Goals**: No measurable overhead vs existing NVARCHAR(MAX) read/write paths
**Constraints**: Must use existing PLP/UTF-16LE infrastructure; no new dependencies
**Scale/Scope**: ~8 case additions across 4 source files + BCP type mapping + DML guards

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Pre-Research | Post-Design | Notes |
|-----------|-------------|-------------|-------|
| I. Native and Open | PASS | PASS | Uses native TDS implementation, no external libraries |
| II. Streaming First | PASS | PASS | XML data streamed via existing PLP chunked reader into DuckDB vectors |
| III. Correctness over Convenience | PASS | PASS | XML DML via SQL literals explicitly rejected with clear errors; correct TDS_TYPE_XML token used in BCP (not NVARCHAR fallback) |
| IV. Explicit State Machines | PASS | PASS | No new connection states; uses existing TDS state machine |
| V. DuckDB-Native UX | PASS | PASS | XML columns appear as VARCHAR in DuckDB catalog, queryable via standard SQL |
| VI. Incremental Delivery | PASS | PASS | Read support (P1) is independently testable; BCP write (P2) and DML guards (P3) are additive |

## Project Structure

### Documentation (this feature)

```text
specs/041-xml-type-support/
├── spec.md
├── plan.md              # This file
├── research.md          # Phase 0 output
├── data-model.md        # Phase 1 output
├── quickstart.md        # Phase 1 output
└── checklists/
    └── requirements.md
```

### Source Code (repository root)

```text
src/
├── tds/
│   ├── tds_column_metadata.cpp    # Add XML case in ParseTypeInfo()
│   ├── tds_row_reader.cpp         # Add XML case in 4 dispatch methods
│   └── encoding/
│       ├── type_converter.cpp     # Map XML→VARCHAR, add to whitelist, route through ConvertString
│       └── bcp_row_encoder.cpp    # (verify PLP encoding works for XML, no changes expected)
├── copy/
│   ├── bcp_writer.cpp             # Add TDS_TYPE_XML case in BuildColmetadataToken
│   └── target_resolver.cpp        # Add "xml" mapping in SQLServerTypeToTDSToken + SQLServerTypeMaxLength
├── dml/
│   ├── insert/
│   │   └── mssql_batch_builder.cpp  # Add XML column guard in SerializeRow()
│   └── update/
│       └── mssql_update_statement.cpp  # Add XML column guard in Build()
└── include/
    └── tds/
        └── tds_types.hpp          # (already has TDS_TYPE_XML = 0xF1, no changes)

test/
├── sql/
│   └── xml/                       # New test directory
│       ├── xml_read.test          # SELECT with XML columns
│       └── xml_dml_error.test     # INSERT/UPDATE error messages
└── cpp/
    └── (unit tests if applicable)
```

**Structure Decision**: Extension convention — changes distributed across existing files in existing directories. Only new directory is `test/sql/xml/` for integration tests.

## Integration Tests

### Test Data Setup (`docker/init/init.sql`)

Add XML test table to the Docker init script (alongside existing `AllDataTypes`, `TestSimplePK`, etc.):

```sql
-- XML data type tests
IF OBJECT_ID('dbo.XmlTestTable', 'U') IS NOT NULL DROP TABLE dbo.XmlTestTable;
CREATE TABLE dbo.XmlTestTable (
    id INT NOT NULL PRIMARY KEY,
    xml_col XML NULL,
    name NVARCHAR(100) NULL
);

INSERT INTO dbo.XmlTestTable VALUES
(1, '<root><item id="1">Hello</item></root>', 'simple'),
(2, NULL, 'null_xml'),
(3, '<root/>', 'empty_element'),
(4, '<?xml version="1.0" encoding="utf-8"?><doc><p>Unicode: привет мир 你好世界</p></doc>', 'unicode'),
(5, '<root>' + REPLICATE(CAST('<item>data</item>' AS NVARCHAR(MAX)), 1000) + '</root>', 'large_xml'),
(6, '', 'empty_string');
```

### Test File: `test/sql/xml/xml_read.test`

**Group**: `[xml]`
**Requires**: SQL Server (integration test)

| Test | Description | Pattern | Validates |
|------|-------------|---------|-----------|
| T1 | SELECT simple XML document | `query IT` | FR-001: XML returns as VARCHAR |
| T2 | SELECT NULL XML value | `query IT` | FR-002: NULL handling |
| T3 | SELECT empty XML element | `query IT` | Edge case: `<root/>` |
| T4 | SELECT XML with Unicode | `query IT` | FR-003: UTF-16LE→UTF-8 decoding |
| T5 | SELECT large XML (PLP multi-chunk) | `query I` + length check | FR-004: PLP streaming for large docs |
| T6 | SELECT * from mixed table (XML + other types) | `query ITT` | SC-006: Mixed-type queries work |
| T7 | SELECT with WHERE on non-XML column | `query IT` | Filter pushdown alongside XML |
| T8 | COUNT(*) on table with XML columns | `query I` | Basic aggregation not blocked by XML |
| T9 | SELECT XML column via mssql_scan() | `query T` | Raw query path also works |

### Test File: `test/sql/xml/xml_copy_bcp.test`

**Group**: `[xml]`
**Requires**: SQL Server (integration test)

| Test | Description | Pattern | Validates |
|------|-------------|---------|-----------|
| T1 | COPY TO existing table with XML column | `statement ok` + read-back | FR-006: BCP write path |
| T2 | COPY NULL values to XML column | `statement ok` + read-back | FR-002: NULL round-trip |
| T3 | COPY Unicode data to XML column | `statement ok` + read-back | FR-003: UTF-8→UTF-16LE encoding |
| T4 | Read back copied XML to verify round-trip | `query IT` | SC-002: Data round-trip |

### Test File: `test/sql/xml/xml_dml_error.test`

**Group**: `[xml]`
**Requires**: SQL Server (integration test)

| Test | Description | Pattern | Validates |
|------|-------------|---------|-----------|
| T1 | INSERT with RETURNING targeting XML column | `statement error` | FR-008: Reject with error |
| T2 | Error message mentions column name | Error substring match | FR-011: Column name in error |
| T3 | Error message recommends COPY TO | Error substring match | FR-011: BCP recommendation |
| T4 | UPDATE setting XML column value | `statement error` | FR-009: Reject UPDATE with error |
| T5 | INSERT into non-XML columns succeeds | `statement ok` | FR-010: Non-XML INSERT works |
| T6 | UPDATE on non-XML columns succeeds | `statement ok` | FR-010: Non-XML UPDATE works |

### Test File: `test/sql/xml/xml_nbc.test`

**Group**: `[xml]`
**Requires**: SQL Server (integration test)

| Test | Description | Pattern | Validates |
|------|-------------|---------|-----------|
| T1 | SELECT from table with many nullable columns including XML | `query` | FR-005: NBC row format |
| T2 | NULL XML in NBC row | `query` | FR-005 + FR-002 combined |

### Existing Test Verification

These existing tests must continue to pass unchanged (regression):

- `test/sql/catalog/data_types.test` — existing type mapping not broken
- `test/sql/copy/copy_types.test` — existing BCP types not broken
- `test/sql/insert/insert_errors.test` — existing error handling not broken
- `test/sql/integration/max_types.test` — NVARCHAR(MAX) still works

## Complexity Tracking

No constitution violations. No complexity justifications needed.
