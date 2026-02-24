# Feature Specification: XML Data Type Support

**Feature Branch**: `041-xml-type-support`
**Created**: 2026-02-24
**Status**: Draft
**Input**: User description: "Add XML data type support to the mssql-extension"

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Read XML columns from SQL Server tables (Priority: P1)

A user has a SQL Server database with tables containing XML columns. They attach the database in DuckDB and query those tables. XML column values are returned as text strings that can be processed with DuckDB string functions.

**Why this priority**: Reading data is the most fundamental operation. Without XML read support, XML columns cause errors that block querying entire tables.

**Independent Test**: Can be fully tested by creating a SQL Server table with an XML column, inserting sample XML data, and running SELECT queries from DuckDB. Delivers the ability to read XML data without workarounds.

**Acceptance Scenarios**:

1. **Given** a SQL Server table with an XML column containing valid XML documents, **When** user runs `SELECT * FROM mssql_db.schema.table`, **Then** XML values are returned as VARCHAR strings containing the XML content.
2. **Given** a SQL Server table with an XML column containing NULL values, **When** user queries the table, **Then** NULL XML values are returned as DuckDB NULL.
3. **Given** a SQL Server table with an XML column containing large XML documents (multi-MB), **When** user queries the table, **Then** the full XML content is returned without truncation.
4. **Given** a SQL Server table with an XML column containing Unicode characters, **When** user queries the table, **Then** Unicode content is correctly preserved in the returned VARCHAR.

---

### User Story 2 - Write XML data via COPY TO / BCP (Priority: P2)

A user wants to load XML data into a SQL Server table that has XML columns using COPY TO with BCP protocol. The BCP protocol handles large values natively via PLP encoding, supporting XML documents up to 2 GB.

**Why this priority**: BCP is the most efficient write path and supports the full XML size range. Users who need to bulk-load XML data should use this path.

**Independent Test**: Can be tested by creating a target table with an XML column in SQL Server, then using COPY TO from DuckDB to load XML data. Verify the data arrives correctly in SQL Server.

**Acceptance Scenarios**:

1. **Given** a SQL Server table with an XML column, **When** user runs `COPY source_table TO mssql_db.schema.target (FORMAT bcp)`, **Then** VARCHAR values from the source are written as XML values in the target.
2. **Given** a SQL Server table with an XML column, **When** user copies NULL values, **Then** NULL is correctly stored in the XML column.

---

### User Story 3 - Write XML data via CTAS (Priority: P2)

A user creates a new SQL Server table from a DuckDB query using CREATE TABLE AS SELECT. When the target table has XML columns (or the source data maps to XML), the data is transferred via BCP protocol by default.

**Why this priority**: CTAS is a common data migration pattern and uses BCP internally, sharing the same write path as COPY TO.

**Independent Test**: Can be tested by running `CREATE TABLE mssql_db.schema.new_table AS SELECT xml_col FROM source` and verifying data integrity.

**Acceptance Scenarios**:

1. **Given** a DuckDB source with VARCHAR data destined for an XML column, **When** user runs CTAS, **Then** data is correctly written to the SQL Server XML column via BCP.

---

### User Story 4 - Clear error for INSERT/UPDATE with XML columns (Priority: P3)

A user attempts to INSERT or UPDATE rows in a SQL Server table that has XML columns using the SQL literal path (INSERT with RETURNING, or UPDATE). The system provides a clear, actionable error message explaining that XML columns are not supported via SQL literal serialization and recommending COPY TO with BCP protocol instead.

**Why this priority**: While XML cannot be written via SQL literals due to size constraints, users need a clear explanation rather than a confusing failure.

**Independent Test**: Can be tested by attempting INSERT with RETURNING or UPDATE on a table with XML columns and verifying the error message content.

**Acceptance Scenarios**:

1. **Given** a SQL Server table with an XML column, **When** user runs an INSERT with RETURNING that includes the XML column, **Then** the system returns an error message stating XML columns are not supported for INSERT via SQL literals and recommends using COPY TO with BCP.
2. **Given** a SQL Server table with an XML column, **When** user runs an UPDATE that sets an XML column value, **Then** the system returns an error message stating XML columns are not supported for UPDATE via SQL literals.
3. **Given** a SQL Server table with an XML column and other non-XML columns, **When** user runs INSERT with RETURNING that only targets non-XML columns, **Then** the INSERT succeeds normally (XML column is not involved).

---

### Edge Cases

- What happens when an XML column contains an empty XML document (`<root/>`)? It should be returned as a VARCHAR containing that string.
- What happens when an XML column value is an empty string? It should be returned as an empty VARCHAR.
- What happens when XML data contains XML declarations (`<?xml version="1.0"?>`)? The declaration should be preserved in the returned string.
- What happens when a user tries to INSERT into a table with XML columns on a Fabric endpoint? Fabric does not support BCP, so neither the SQL literal path nor the BCP path is available. The SQL literal error should still apply.
- What happens when XML data uses NBC (Null Bitmap Compressed) row format? XML columns should be correctly handled in NBC rows, same as regular rows.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: System MUST read SQL Server XML type columns and return values as DuckDB VARCHAR type.
- **FR-002**: System MUST handle XML NULL values, returning DuckDB NULL.
- **FR-003**: System MUST correctly decode XML wire data from UTF-16LE encoding to UTF-8 VARCHAR strings.
- **FR-004**: System MUST support XML values in PLP (Partially Length-Prefixed) wire format, including large documents up to the SQL Server XML limit (2 GB).
- **FR-005**: System MUST support XML columns in NBC (Null Bitmap Compressed) row format.
- **FR-006**: System MUST support writing XML data via COPY TO with BCP protocol.
- **FR-007**: System MUST support writing XML data via CTAS when BCP mode is enabled (default).
- **FR-008**: System MUST reject INSERT operations that serialize XML columns as SQL literals (INSERT with RETURNING) with a clear error message recommending COPY TO with BCP.
- **FR-009**: System MUST reject UPDATE operations that target XML columns with a clear error message recommending COPY TO with BCP.
- **FR-010**: System MUST allow INSERT and UPDATE operations on tables with XML columns when the XML columns are not part of the operation (e.g., INSERT into non-XML columns only).
- **FR-011**: Error messages for rejected XML DML operations MUST include the column name and recommend using `COPY TO` with BCP protocol as an alternative.

### Key Entities

- **XML Column**: A SQL Server column of type `xml` (TDS type ID 0xF1). Stored as UTF-16LE on the wire using PLP encoding. Maps to DuckDB VARCHAR. Maximum size 2 GB.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: Users can query SQL Server tables with XML columns without errors -- XML values appear as readable text strings.
- **SC-002**: XML data round-trips correctly through COPY TO / BCP -- data written from DuckDB is readable back from SQL Server.
- **SC-003**: NULL XML values are correctly handled in both read and write paths.
- **SC-004**: Users attempting unsupported XML write operations (INSERT with RETURNING, UPDATE) receive an actionable error message that names the XML column and suggests COPY TO with BCP.
- **SC-005**: Existing queries and operations on tables without XML columns are unaffected by this change.
- **SC-006**: XML columns work correctly alongside other column types in the same table (mixed-type queries).

## Assumptions

- XML data is mapped to VARCHAR because DuckDB has no native XML type. No XML parsing or validation is performed by the extension.
- The SQL Server XML type uses the same PLP wire encoding as NVARCHAR(MAX), with UTF-16LE data encoding. No additional metadata (collation, length, precision) is sent in the COLMETADATA token for XML.
- BCP encoder already supports PLP types (NVARCHAR(MAX), VARBINARY(MAX)) and the same mechanism works for XML without modification, or with minimal type-routing changes.
- INSERT without RETURNING currently uses SQL literal serialization (same as INSERT with RETURNING), so the XML rejection applies to both paths equally. If INSERT without RETURNING is later moved to BCP, the rejection will naturally stop applying for that path.
- Fabric endpoints do not support BCP/INSERT BULK, so XML write operations are not available on Fabric at all.
