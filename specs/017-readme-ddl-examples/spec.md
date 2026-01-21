# Feature Specification: README DDL Examples

**Feature Branch**: `017-readme-ddl-examples`
**Created**: 2026-01-21
**Status**: Complete
**Input**: User description: "Add to the readme ddl operation examples (doc)"

## Problem Statement

The README documentation lacks examples of DDL (Data Definition Language) operations. Users need guidance on how to execute CREATE TABLE, ALTER TABLE, DROP TABLE, CREATE SCHEMA, and other DDL statements against SQL Server through the extension.

## Requirements

### Functional Requirements

- **FR-001**: Add a dedicated DDL section to the README demonstrating common DDL operations
- **FR-002**: Show examples using standard DuckDB DDL syntax for implemented operations (CREATE/DROP TABLE, CREATE/DROP SCHEMA, ALTER TABLE columns)
- **FR-003**: Show examples using `mssql_exec()` for SQL Server-specific features (IDENTITY, constraints, indexes)

## Success Criteria

- **SC-001**: README contains clear DDL examples that users can copy and adapt
- **SC-002**: Examples follow the existing README style and formatting
- **SC-003**: Documentation accurately reflects what is implemented (spec 008) vs what requires mssql_exec()

## Out of Scope

- Code changes (documentation only)
- New functionality
