# Feature Specification: DML UPDATE/DELETE using PK-based rowid (MSSQL)

**Feature Branch**: `002-dml-update-delete`
**Created**: 2026-01-25
**Status**: Draft
**Input**: User description: "Spec 05.04b - Support UPDATE and DELETE on SQL Server tables through DuckDB by implementing two-phase DML semantics based on the rowid mapping from Spec 05.04a."

## Overview

This specification enables UPDATE and DELETE operations on MSSQL tables through DuckDB using the PK-based rowid mechanism from Spec 001 (05.04a). DuckDB identifies affected rows via rowid, then the extension executes batched DML statements against SQL Server using primary key predicates.

**Scope boundaries**:
- UPDATE and DELETE are supported; MERGE/upsert is out of scope
- Primary key modifications are rejected by default
- RETURNING clause is not supported in this specification
- Optimistic concurrency control is not implemented

**Dependencies**:
- Spec 001-pk-rowid-semantics: Provides rowid-to-PK mapping
- Existing INSERT implementation: Reuses value serialization and type binding patterns

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Update Rows via Standard SQL UPDATE (Priority: P1)

A DuckDB user wants to update rows in an MSSQL table using standard SQL UPDATE syntax. The system translates the update operation into efficient batched MSSQL UPDATE statements using primary key predicates.

**Why this priority**: UPDATE is one of the two core DML operations. Users expect standard SQL UPDATE syntax to work on attached MSSQL tables.

**Independent Test**: Can be fully tested by running `UPDATE mssql.dbo.products SET price = price * 1.1 WHERE category = 'electronics'` and verifying the affected rows have updated values.

**Acceptance Scenarios**:

1. **Given** an MSSQL table with scalar PK `id` and columns `name`, `price`, **When** user runs `UPDATE mssql.dbo.products SET price = 99.99 WHERE id = 1`, **Then** the row with id=1 has price=99.99 and the operation reports 1 row affected.

2. **Given** an MSSQL table with 1000 rows matching a filter, **When** user runs `UPDATE mssql.dbo.orders SET status = 'archived' WHERE created_date < '2024-01-01'`, **Then** all 1000 rows are updated using batched statements (not row-by-row).

3. **Given** an MSSQL table with composite PK `(tenant_id, order_id)`, **When** user runs `UPDATE mssql.dbo.tenant_orders SET amount = 500 WHERE tenant_id = 1 AND amount < 100`, **Then** the correct rows are updated using composite key predicates.

---

### User Story 2 - Delete Rows via Standard SQL DELETE (Priority: P1)

A DuckDB user wants to delete rows from an MSSQL table using standard SQL DELETE syntax. The system translates the delete operation into efficient batched MSSQL DELETE statements using primary key predicates.

**Why this priority**: DELETE is one of the two core DML operations. Users expect standard SQL DELETE syntax to work on attached MSSQL tables.

**Independent Test**: Can be fully tested by running `DELETE FROM mssql.dbo.logs WHERE created_date < '2024-01-01'` and verifying the affected rows no longer exist.

**Acceptance Scenarios**:

1. **Given** an MSSQL table with scalar PK `id`, **When** user runs `DELETE FROM mssql.dbo.products WHERE id = 5`, **Then** the row with id=5 is deleted and the operation reports 1 row affected.

2. **Given** an MSSQL table with 5000 rows matching a filter, **When** user runs `DELETE FROM mssql.dbo.audit_logs WHERE log_date < '2023-01-01'`, **Then** all 5000 rows are deleted using batched statements.

3. **Given** an MSSQL table with composite PK, **When** user runs `DELETE FROM mssql.dbo.tenant_data WHERE tenant_id = 'old_tenant'`, **Then** all matching rows are deleted using composite key predicates.

---

### User Story 3 - Partial Predicate Pushdown (Priority: P2)

A DuckDB user runs UPDATE/DELETE with predicates that cannot be fully pushed down to SQL Server (e.g., DuckDB-specific functions). DuckDB filters rows locally and only the identified rowids are sent for modification.

**Why this priority**: Real-world queries often mix pushable and non-pushable predicates. The system must handle this gracefully.

**Independent Test**: Can be tested by using a DuckDB function in the WHERE clause and verifying only filtered rows are modified.

**Acceptance Scenarios**:

1. **Given** an MSSQL table and a predicate using a DuckDB-only function, **When** user runs `UPDATE mssql.dbo.items SET flag = true WHERE regexp_matches(name, '^test.*')`, **Then** DuckDB filters locally and only affected rowids are updated remotely.

2. **Given** a complex predicate mixing pushable and non-pushable conditions, **When** user runs `DELETE FROM mssql.dbo.data WHERE col1 > 10 AND duckdb_function(col2)`, **Then** the pushable predicate (col1 > 10) is pushed, DuckDB filters the rest locally, and DELETE targets only the final rowid set.

---

### User Story 4 - Tables Without Primary Key (Priority: P2)

A DuckDB user attempts UPDATE/DELETE on an MSSQL table without a primary key. The system must reject the operation with a clear error.

**Why this priority**: Clear error messages prevent confusion when operations cannot be supported.

**Independent Test**: Can be tested by attempting UPDATE/DELETE on a PK-less table and verifying the error message.

**Acceptance Scenarios**:

1. **Given** an MSSQL table without a primary key, **When** user runs `UPDATE mssql.dbo.heap_table SET col = 'value'`, **Then** the system returns error "MSSQL: UPDATE/DELETE requires a primary key".

2. **Given** an MSSQL table without a primary key, **When** user runs `DELETE FROM mssql.dbo.heap_table WHERE col = 'x'`, **Then** the system returns error "MSSQL: UPDATE/DELETE requires a primary key".

---

### User Story 5 - Reject Primary Key Updates (Priority: P2)

A DuckDB user attempts to update primary key columns. The system must reject this operation to preserve row identity integrity.

**Why this priority**: PK modifications require special handling (DELETE+INSERT) which is out of scope for this specification.

**Independent Test**: Can be tested by attempting to update a PK column and verifying the error message.

**Acceptance Scenarios**:

1. **Given** an MSSQL table with PK column `id`, **When** user runs `UPDATE mssql.dbo.products SET id = id + 1000`, **Then** the system returns error "MSSQL: updating primary key columns is not supported".

2. **Given** an MSSQL table with composite PK `(a, b)`, **When** user runs `UPDATE mssql.dbo.items SET a = 'new_value'`, **Then** the system returns error "MSSQL: updating primary key columns is not supported".

---

### Edge Cases

- What happens when UPDATE/DELETE affects zero rows? System reports 0 rows affected (no error).
- How are concurrent modifications handled? Each batch is atomic; no cross-batch atomicity is guaranteed (SQL Server transaction semantics apply).
- What happens on batch failure mid-operation? Error is raised with operation type, batch number, and SQL Server error; already-committed batches are not rolled back.
- How are NULL values in update expressions handled? NULL binding must work correctly per existing type mapping.
- What about very large updates (millions of rows)? Batching ensures no single statement exceeds parameter limits.
- How are string PK values with special characters handled? Prepared statements with typed parameters avoid SQL injection; collation is server-handled.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: System MUST use rowid mapping from Spec 001 (05.04a) exclusively for row identification. Scalar PK maps to scalar rowid; composite PK maps to STRUCT rowid.

- **FR-002**: System MUST reject UPDATE/DELETE on tables without primary keys with error "MSSQL: UPDATE/DELETE requires a primary key".

- **FR-003**: System MUST execute UPDATE/DELETE as two-phase operations where DuckDB identifies rows (via rowid) and the extension applies changes using PK predicates.

- **FR-004**: System MUST use PK-based predicates exclusively for remote DML. Physical row locators (%%physloc%%) are forbidden.

- **FR-005**: System MUST batch UPDATE/DELETE operations controlled by setting `mssql_dml_batch_size` (INT, default 500).

- **FR-006**: System MUST use prepared statements with typed parameter binding controlled by setting `mssql_dml_use_prepared` (BOOL, default true).

- **FR-007**: System MUST enforce a maximum parameter count per statement controlled by setting `mssql_dml_max_parameters` (INT, default 2000).

- **FR-008**: System MUST generate batched UPDATE statements using VALUES join pattern:
  ```sql
  UPDATE t SET t.[col1] = v.[col1], t.[col2] = v.[col2]
  FROM [schema].[table] AS t
  JOIN (VALUES (@k1_1, @v1_1, @v2_1), (@k1_2, @v1_2, @v2_2)) AS v([pk], [col1], [col2])
  ON t.[pk] = v.[pk]
  ```

- **FR-009**: System MUST generate batched DELETE statements using VALUES join pattern:
  ```sql
  DELETE t FROM [schema].[table] AS t
  JOIN (VALUES (@k1), (@k2)) AS v([pk])
  ON t.[pk] = v.[pk]
  ```

- **FR-010**: System MUST bind PK parameters with correct MSSQL types matching the source column types from catalog metadata.

- **FR-011**: System MUST bind update value parameters using existing value serialization rules from INSERT implementation.

- **FR-012**: System MUST bind string PK columns as Unicode (NVARCHAR) where possible and rely on server-side collation semantics.

- **FR-013**: System MUST NOT embed literal values into SQL text; all values MUST be bound as parameters.

- **FR-014**: System MUST reject attempts to update primary key columns with error "MSSQL: updating primary key columns is not supported".

- **FR-015**: System MUST fail the operation on batch error with: operation type (UPDATE/DELETE), batch context, SQL Server error message/code.

- **FR-016**: System MUST NOT provide cross-batch atomicity guarantees; each batch executes as an independent SQL statement.

- **FR-017**: When operating within a DuckDB transaction on a pinned MSSQL connection, errors MUST propagate to trigger DuckDB transaction rollback.

### Non-Functional Requirements

- **NFR-001**: Batch operations MUST NOT degrade to per-row execution; a single SQL statement handles each batch of rows.

- **NFR-002**: DML operations MUST emit debug-level logs when MSSQL_DEBUG is enabled showing generated SQL and parameter counts.

### Key Entities

- **MSSQLUpdateExecutor**: Orchestrates batched UPDATE execution using rowid-to-PK mapping.

- **MSSQLDeleteExecutor**: Orchestrates batched DELETE execution using rowid-to-PK mapping.

- **MSSQLDMLStatementBuilder**: Generates batched UPDATE/DELETE SQL using VALUES join pattern.

- **MSSQLDMLParameterBinder**: Binds PK and value parameters with correct types.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: UPDATE operations on tables with scalar PK modify exactly the expected rows with 100% accuracy.

- **SC-002**: DELETE operations on tables with scalar PK remove exactly the expected rows with 100% accuracy.

- **SC-003**: UPDATE/DELETE operations on tables with composite PK correctly identify rows using all PK components.

- **SC-004**: Batch operations process N rows in ceil(N/batch_size) SQL statements, not N statements.

- **SC-005**: Tables without primary keys remain fully queryable for SELECT operations while rejecting UPDATE/DELETE with clear error messages.

- **SC-006**: Attempts to modify PK columns are rejected before any data modification occurs.

- **SC-007**: Large-scale operations (100,000+ rows) complete successfully using batched execution without per-row overhead.

## Testing

### Test Environment

Tests require a running SQL Server instance. See [TESTING.md](../../docs/TESTING.md) for setup instructions.

```bash
# Start SQL Server container
make docker-up

# Run integration tests
make integration-test

# Run specific DML tests
build/release/test/unittest "test/sql/dml/*" --force-reload
```

### Test Categories

**T1 - Scalar PK UPDATE/DELETE**: Basic operations on tables with single-column PK.

**T2 - Composite PK UPDATE/DELETE**: Operations on tables with multi-column PK.

**T3 - Partial Predicate Pushdown**: Verify correct behavior when predicates cannot be fully pushed.

**T4 - Missing PK Rejection**: Confirm clear error messages for PK-less tables.

**T5 - PK Modification Rejection**: Confirm rejection of PK column updates.

**T6 - Batching Verification**: Verify batch size settings are respected.

**T7 - NULL Handling**: Verify NULL values in update expressions work correctly.

**T8 - Large Scale**: Performance tests with 100K+ row operations.

## Assumptions

- Spec 001 (05.04a) rowid implementation is complete and provides correct PK-to-rowid mapping.
- Existing INSERT implementation provides reusable value serialization and type binding code.
- SQL Server parameter limits (approximately 2100) are well-known and accommodated by batch size calculations.
- DuckDB provides rowid values in the DataChunk for UPDATE/DELETE physical operators.
- Connection pooling provides stable connections for batched operations within a single logical operation.
