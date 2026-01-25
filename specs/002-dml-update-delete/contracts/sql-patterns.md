# SQL Patterns: DML UPDATE/DELETE

**Feature**: 002-dml-update-delete
**Date**: 2026-01-25

This document defines the T-SQL patterns generated for UPDATE and DELETE operations.

---

## 1. UPDATE Patterns

### 1.1 Scalar PK UPDATE

**Input**: Table with single-column PK, updating multiple columns.

```sql
-- Table: [dbo].[products]
-- PK: id (INT)
-- Updating: name, price

UPDATE t
SET t.[name] = v.[name], t.[price] = v.[price]
FROM [dbo].[products] AS t
JOIN (VALUES
  (@p1, @p2, @p3),
  (@p4, @p5, @p6),
  (@p7, @p8, @p9)
) AS v([id], [name], [price])
ON t.[id] = v.[id]
```

**Parameter binding order**: pk1, val1, val2, pk2, val3, val4, ...

### 1.2 Composite PK UPDATE

**Input**: Table with multi-column PK.

```sql
-- Table: [dbo].[order_items]
-- PK: order_id (INT), item_id (INT)
-- Updating: quantity, unit_price

UPDATE t
SET t.[quantity] = v.[quantity], t.[unit_price] = v.[unit_price]
FROM [dbo].[order_items] AS t
JOIN (VALUES
  (@p1, @p2, @p3, @p4),
  (@p5, @p6, @p7, @p8)
) AS v([order_id], [item_id], [quantity], [unit_price])
ON t.[order_id] = v.[order_id] AND t.[item_id] = v.[item_id]
```

**Parameter binding order**: pk1_1, pk1_2, val1, val2, pk2_1, pk2_2, val3, val4, ...

### 1.3 Single Column UPDATE

**Input**: Updating only one column.

```sql
-- Table: [dbo].[users]
-- PK: id (BIGINT)
-- Updating: status only

UPDATE t
SET t.[status] = v.[status]
FROM [dbo].[users] AS t
JOIN (VALUES
  (@p1, @p2),
  (@p3, @p4),
  (@p5, @p6)
) AS v([id], [status])
ON t.[id] = v.[id]
```

### 1.4 UPDATE with NULL Values

**Input**: Setting column to NULL.

```sql
-- Parameter @p2 bound as NULL with correct type
UPDATE t
SET t.[description] = v.[description]
FROM [dbo].[products] AS t
JOIN (VALUES
  (@p1, @p2)
) AS v([id], [description])
ON t.[id] = v.[id]

-- Note: @p2 is NULL but retains type (NVARCHAR, etc.)
```

---

## 2. DELETE Patterns

### 2.1 Scalar PK DELETE

**Input**: Table with single-column PK.

```sql
-- Table: [dbo].[logs]
-- PK: id (BIGINT)

DELETE t
FROM [dbo].[logs] AS t
JOIN (VALUES
  (@p1),
  (@p2),
  (@p3)
) AS v([id])
ON t.[id] = v.[id]
```

**Parameter binding order**: pk1, pk2, pk3, ...

### 2.2 Composite PK DELETE

**Input**: Table with multi-column PK.

```sql
-- Table: [dbo].[tenant_data]
-- PK: tenant_id (VARCHAR), record_id (INT)

DELETE t
FROM [dbo].[tenant_data] AS t
JOIN (VALUES
  (@p1, @p2),
  (@p3, @p4),
  (@p5, @p6)
) AS v([tenant_id], [record_id])
ON t.[tenant_id] = v.[tenant_id] AND t.[record_id] = v.[record_id]
```

### 2.3 Single Row DELETE

**Input**: Deleting exactly one row.

```sql
DELETE t
FROM [dbo].[users] AS t
JOIN (VALUES
  (@p1)
) AS v([id])
ON t.[id] = v.[id]
```

---

## 3. Identifier Escaping

All identifiers use bracket quoting for safety:

| Identifier Type | Example Input | Escaped Output |
|-----------------|---------------|----------------|
| Schema | `dbo` | `[dbo]` |
| Table | `user data` | `[user data]` |
| Column | `SELECT` | `[SELECT]` |
| Column with bracket | `col]name` | `[col]]name]` |

**Escaping rule**: Replace `]` with `]]` then wrap in `[]`.

---

## 4. Parameter Type Binding

Parameters are bound with SQL Server types matching the source column:

| DuckDB Type | SQL Server Type | TDS Type |
|-------------|-----------------|----------|
| INTEGER | INT | INTN (4 bytes) |
| BIGINT | BIGINT | INTN (8 bytes) |
| VARCHAR | NVARCHAR(max) | NVARCHAR |
| DOUBLE | FLOAT | FLTN |
| DECIMAL(p,s) | DECIMAL(p,s) | DECIMAL |
| DATE | DATE | DATEN |
| TIMESTAMP | DATETIME2 | DATETIME2 |
| BOOLEAN | BIT | BIT |
| BLOB | VARBINARY(max) | VARBINARY |
| UUID | UNIQUEIDENTIFIER | GUID |

---

## 5. Batch Size Calculation

```
params_per_row = pk_column_count + update_column_count  (for UPDATE)
params_per_row = pk_column_count                        (for DELETE)

effective_batch_size = min(
    mssql_dml_batch_size,
    floor(mssql_dml_max_parameters / params_per_row)
)
```

**Examples**:

| Operation | PK Cols | Update Cols | Params/Row | Max Batch (2000 limit) |
|-----------|---------|-------------|------------|------------------------|
| UPDATE | 1 | 2 | 3 | 666 |
| UPDATE | 2 | 3 | 5 | 400 |
| DELETE | 1 | 0 | 1 | 2000 |
| DELETE | 3 | 0 | 3 | 666 |

---

## 6. Error Response Format

On batch failure, the error message follows this format:

```
MSSQL UPDATE failed: batch 3 of 10: [SQL Server error message]
MSSQL DELETE failed: batch 1 of 5: [SQL Server error message]
```

Components:
- Operation type: UPDATE or DELETE
- Batch number (1-indexed)
- Total batches (if known)
- SQL Server native error message

---

## 7. Reserved Word Handling

SQL Server reserved words as identifiers are automatically escaped:

```sql
-- Table named "SELECT" with column "FROM"
UPDATE t
SET t.[FROM] = v.[FROM]
FROM [dbo].[SELECT] AS t
JOIN (VALUES
  (@p1, @p2)
) AS v([id], [FROM])
ON t.[id] = v.[id]
```

The bracket quoting handles all reserved words without maintaining a reserved word list.
