# Data Model: ORDER BY Pushdown

**Branch**: `039-order-pushdown`
**Date**: 2026-02-16

## Entities

### OrderByColumnInfo

Represents a single ORDER BY column that has been validated as pushable to SQL Server.

| Field | Type | Description |
| ----- | ---- | ----------- |
| sql_fragment | string | T-SQL ORDER BY fragment (e.g., `[col1] ASC`, `YEAR([date_col]) DESC`) |
| is_descending | bool | Sort direction |
| source_column_name | string | Original column name (for nullability lookup) |

### MSSQLCatalogScanBindData (extended)

Existing struct — new fields added for ORDER BY pushdown:

| Field | Type | Default | Description |
| ----- | ---- | ------- | ----------- |
| order_by_clause | string | "" | Complete T-SQL ORDER BY fragment (comma-separated columns) |
| top_n | int64_t | 0 | TOP N value (0 = no TOP pushdown) |

### Pushdown Configuration

The effective pushdown state for a given database, resolved at optimizer time.

| Source | Priority | Field |
| ------ | -------- | ----- |
| ATTACH option `order_pushdown` | 1 (highest) | Stored in catalog attach info |
| Global setting `mssql_order_pushdown` | 2 (fallback) | Retrieved via `context.TryGetCurrentSetting()` |

Resolution: ATTACH option > global setting. If ATTACH option is not specified, fall back to global setting.

## Relationships

- `MSSQLCatalogScanBindData` contains `vector<MSSQLColumnInfo> mssql_columns` — used to check `is_nullable` for NULL order validation.
- `OrderByColumnInfo` is a transient struct used during optimizer processing — it is serialized into `order_by_clause` string before being stored in bind data.
- The optimizer reads `LogicalOrder.orders` (vector of `BoundOrderByNode`) and produces `OrderByColumnInfo` entries.
