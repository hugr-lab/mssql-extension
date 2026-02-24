# Research: XML Data Type Support

**Feature**: 041-xml-type-support
**Date**: 2026-02-24

## R1: XML Wire Format in TDS 7.4

**Decision**: XML uses PLP (Partially Length-Prefixed) encoding with UTF-16LE data, identical to NVARCHAR(MAX).

**Rationale**: TDS 7.4 specification defines XML (type 0xF1) as a PLP type with no additional COLMETADATA fields (no collation, no max_length, no precision/scale). Data is always UTF-16LE on the wire. This matches the existing NVARCHAR(MAX) reading infrastructure.

**Alternatives considered**:
- Treating XML as a fixed-length type: Incorrect, XML uses PLP chunked encoding.
- Adding XML-specific parsing: Unnecessary, identical to NVARCHAR(MAX) wire format.

## R2: DuckDB Type Mapping for XML

**Decision**: Map XML to `LogicalType::VARCHAR`.

**Rationale**: DuckDB has no native XML type. VARCHAR is the natural fit for text data. Users can process XML strings with DuckDB string functions or export for XML-specific processing.

**Alternatives considered**:
- BLOB: Less suitable since XML is text, not binary. Would lose UTF-8 readability.
- Custom type: Over-engineering for this use case; no XML parsing is in scope.

## R3: BCP Write Path for XML Columns

**Decision**: For COPY TO existing tables, map "xml" → `TDS_TYPE_XML` (0xF1) in `SQLServerTypeToTDSToken`. Add XML case in BCP COLMETADATA builder. Use same PLP encoding as NVARCHAR(MAX).

**Rationale**: When COPY TO targets an existing SQL Server table with an XML column, `target_resolver.cpp` reads the column's `sql_type_name` ("xml") from sys.columns metadata. Currently `SQLServerTypeToTDSToken("xml")` falls back to `TDS_TYPE_NVARCHAR`. While SQL Server may accept NVARCHAR→XML implicit conversion in INSERT BULK, declaring the correct type (0xF1) is more correct and avoids potential conversion errors for edge cases (typed XML schemas, XML validation).

**Key code paths**:
- `target_resolver.cpp:633-674` — `SQLServerTypeToTDSToken()`: Add `"xml"` → `TDS_TYPE_XML`
- `target_resolver.cpp:680-740` — `SQLServerTypeMaxLength()`: Add `"xml"` → `0xFFFF` (PLP indicator)
- `bcp_writer.cpp:375-429` — `BuildColmetadataToken()`: Add `TDS_TYPE_XML` case (no additional metadata, like DATE)
- `bcp_row_encoder.cpp` — Row encoding: XML maps to `duckdb_type=VARCHAR`, already routes through `EncodeNVarcharPLP()` via the VARCHAR/NVARCHAR path

**Alternatives considered**:
- Keep NVARCHAR fallback: Works in most cases but incorrect type declaration may fail with typed XML schemas.

## R4: CTAS with XML

**Decision**: CTAS creates `nvarchar(max)` columns (not XML) when source is VARCHAR. No changes needed.

**Rationale**: CTAS maps DuckDB types to SQL Server types via `GetSQLServerTypeDeclaration()`. DuckDB VARCHAR → `nvarchar(max)`. There's no mechanism to create XML columns via CTAS since DuckDB has no XML type. This is acceptable and expected behavior.

**Alternatives considered**:
- Adding XML type hint: Out of scope. Users can ALTER the column type after CTAS if needed.

## R5: DML Guard Strategy

**Decision**: Check `mssql_type` in `SerializeRow()` (INSERT) and `Build()` (UPDATE) to reject XML columns before serialization.

**Rationale**: Both code paths have access to `col.mssql_type` (string, e.g., "xml") via `MSSQLInsertColumn` and `MSSQLUpdateColumn` structs. The check happens at the point where SQL literal serialization is about to occur, so:
- It does NOT affect BCP/COPY TO (uses `BCPRowEncoder`, separate code path)
- It does NOT affect CTAS (uses BCP by default)
- If INSERT without RETURNING later moves to BCP, the guard naturally stops applying

**Key code paths**:
- `mssql_batch_builder.cpp:59-73` — `SerializeRow()`: Has `col.mssql_type` via `target_.columns[col_idx]`
- `mssql_update_statement.cpp:53-58` — `Build()`: Has `target_.update_columns[col_idx].mssql_type`

**Error message format**: `"MSSQL Error: XML column '%s' cannot be used in %s via SQL literals. Use COPY TO with BCP protocol instead (FORMAT bcp)."`

**Alternatives considered**:
- Check in `MSSQLValueSerializer::Serialize()`: Would require changing the serializer interface to pass mssql_type. Rejected — simpler to check at the caller level.
- Reject at plan time (`PlanInsert`/`PlanUpdate`): Too early — would reject all INSERT/UPDATE to tables with XML columns even when XML columns aren't targeted.

## R6: XML Column Metadata Parsing

**Decision**: Add `case TDS_TYPE_XML: break;` in `ParseTypeInfo()` with no additional metadata reading.

**Rationale**: Per TDS 7.4 spec, XML type has no additional metadata in COLMETADATA token (no collation, no length, no precision/scale). The column's `max_length` should be set to indicate PLP mode.

**Key finding**: The `IsPLPType()` check in `tds_column_metadata.cpp:118-135` tests `max_length == 0xFFFF`. For XML, we need to ensure `max_length` is set to `0xFFFF` in the ParseTypeInfo case, since there's no length field in the wire format to read.

## R7: NBC (Null Bitmap Compressed) Row Support

**Decision**: XML is handled identically to NVARCHAR(MAX) in NBC rows — add XML case in both `ReadValueNBC()` and `SkipValueNBC()`.

**Rationale**: NBC rows use a null bitmap prefix, then the same value-reading logic. The PLP encoding for XML is identical in both regular and NBC row formats.
