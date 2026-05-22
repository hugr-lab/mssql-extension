# Data Model: DML Rewrite (spec 050)

Spec 050 is a refactor + new feature. There is no business data
model; the "entities" are the new C++ types, the per-table
capability record, the session settings surface, and the T-SQL
templates emitted by the new operators.

## E1. `RowIdStrategy` enum + `ResolveRowIdStrategy`

- **Header**: `src/include/dml/mssql_rowid_strategy.hpp`
- **Source**: `src/dml/mssql_dml_capabilities.cpp`

```cpp
namespace duckdb {

enum class RowIdStrategy {
    PrimaryKey,    // join on PK columns
    PhysLoc,       // join on %%physloc%% VARBINARY(8)
};

RowIdStrategy ResolveRowIdStrategy(
    ClientContext &context,
    const MSSQLTableEntry &table
);

} // namespace duckdb
```

`ResolveRowIdStrategy` algorithm (locked in research.md §R2):

1. Read `mssql_rowid_strategy` session setting (default `"auto"`).
2. If `"pk"`: require `caps.has_primary_key`, else throw
   `BinderException`.
3. If `"physloc"`: require `caps.supports_physloc`, else throw
   `BinderException` quoting `caps.physloc_unavailable_reason`.
4. If `"auto"`: prefer PK; fall back to physloc; else throw
   `BinderException` listing both reasons.

The exception messages are fixed and tested per FR-002 / SC-001 /
acceptance scenarios US3 #3, #4.

## E2. `TableDmlCapabilities`

- **Header**: `src/include/dml/mssql_dml_capabilities.hpp`
- **Stored on**: `MSSQLTableEntry`

```cpp
namespace duckdb {

struct PKColumnInfo;  // already defined elsewhere

struct TableDmlCapabilities {
    bool has_primary_key = false;
    vector<PKColumnInfo> pk_columns;        // copied/aliased from existing PK metadata
    bool supports_physloc = false;
    string physloc_unavailable_reason;      // empty when supports_physloc == true
};

} // namespace duckdb
```

`physloc_unavailable_reason` value set (FR-004) — exact strings,
case-sensitive, used in error messages:

| Condition | String |
|---|---|
| EngineEdition = 6 | `engine is Azure Synapse Dedicated (no physloc support)` |
| EngineEdition = 11 | `engine is Fabric Warehouse (no physloc support)` |
| `is_memory_optimized = 1` | `table is memory-optimized` |
| `is_external = 1` | `table is external (PolyBase)` |
| `has_columnstore = 1` (CCI or NCCI base) | `table has clustered columnstore index` |

If multiple conditions apply, the engine-level reason wins (it
applies catalog-wide and is more diagnostic for the user).

`MSSQLTableEntry::GetDmlCapabilities()` returns a `const
TableDmlCapabilities &`. Populated during the existing per-table
metadata fetch (extra columns added to the existing query — see
research.md §R3). The PK column data already lives on
`MSSQLTableEntry::GetPrimaryKeyInfo(context)` (defined in
`src/include/catalog/mssql_table_entry.hpp`); `pk_columns` here is a
reference to / copy of that existing data, not a parallel load.

## E3. Engine edition enum + connection-state cache

- **Header**: `src/include/connection/mssql_engine_edition.hpp` (new)
- **Stored on**: `TdsConnection` (the TDS-layer connection object at
  `src/include/tds/tds_connection.hpp:18`). The extension does not
  have an `MSSQLConnection` class; the TDS connection is the
  long-lived, per-pool-handle state and is the natural cache home.

```cpp
namespace duckdb {

enum class MSSQLEngineEdition : int {
    Personal = 1,
    Standard = 2,
    Enterprise = 3,
    Express = 4,
    AzureSqlDatabase = 5,
    AzureSynapseDedicated = 6,
    AzureSqlManagedInstance = 8,
    AzureSqlEdge = 9,
    FabricWarehouse = 11,
    Unknown = -1,
};

MSSQLEngineEdition GetCachedEngineEdition(tds::TdsConnection &conn);

} // namespace duckdb
```

`GetCachedEngineEdition` lazily issues
`SELECT SERVERPROPERTY('EngineEdition')` once per `TdsConnection`
lifetime and caches the result. Subsequent calls return the cached
value. Thread-safe (single probe protected by a once-flag stored on
the `TdsConnection`).

Per-table capability detection consumes the cached value via the
connection that the catalog metadata fetch is running on.

## E4. New physical operators

Four new operators replace the v0.1.x `MSSQLPhysical{Update,Delete}`
+ `MSSQLRowIdExtractor` pipeline.

### E4a. `MSSQLDirectDelete` / `MSSQLDirectUpdate`

- **Headers**:
  `src/include/dml/delete/mssql_direct_delete.hpp`,
  `src/include/dml/update/mssql_direct_update.hpp`
- **Sink/Source**: neither. Single statement at Finalize.

Common members:

```cpp
struct MSSQLDirectDml : public PhysicalOperator {
    MSSQLTableEntry &target;
    string filter_clause;       // pre-formatted WHERE via spec 044
    bool has_returning;
    vector<idx_t> returning_columns;
    // …operator boilerplate…
};
```

`MSSQLDirectDelete::Finalize` emits:

```sql
DELETE FROM <quoted_schema>.<quoted_table>
WHERE <filter_clause>
[OUTPUT DELETED.<col1>, DELETED.<col2>, …];
```

`MSSQLDirectUpdate::Finalize` emits:

```sql
UPDATE <quoted_schema>.<quoted_table>
SET <col1> = <literal1>, <col2> = <literal2>, …
WHERE <filter_clause>
[OUTPUT INSERTED.<col1>, INSERTED.<col2>, …];
```

Single round-trip. No staging. No rowid SELECT. `@@ROWCOUNT` is
captured from the response; OUTPUT rows are parsed via
`MSSQLDmlReturningParser`.

### E4b. `MSSQLStagingDelete` / `MSSQLStagingUpdate`

- **Headers**:
  `src/include/dml/delete/mssql_staging_delete.hpp`,
  `src/include/dml/update/mssql_staging_update.hpp`
- **Sink/Source**: full Sink/Source pattern over `DataChunk`.

Sink phase:

1. (First chunk only.) Probe `@@TRANCOUNT` → branch transaction
   wrap (`BEGIN TRAN` + `SET XACT_ABORT ON` in autocommit;
   `SAVE TRANSACTION mssql_dml_<uuid>` in user-txn). Probe
   `XACT_STATE()` if `@@TRANCOUNT > 0`; throw on `2` (DTC) or
   `-1` (doomed).
2. (First chunk only.) Create `#upd_<uuid>` with DDL from
   `data-model.md` §E5.
3. Per chunk: forward to the spec 046 pipelined BCP writer for
   upload into `#upd_<uuid>`.

Finalize phase:

1. Issue the JOIN statement (one of the templates in §E6).
2. Parse OUTPUT rows via `MSSQLDmlReturningParser` if RETURNING is
   set.
3. `DROP TABLE #upd_<uuid>`.
4. `COMMIT` (autocommit case) or savepoint discard (user-txn case).

On failure anywhere in Sink or Finalize, `ROLLBACK` (autocommit) or
`ROLLBACK TRANSACTION mssql_dml_<uuid>` (user-txn), then re-throw.

## E5. Staging table DDL templates

DDL types come from spec 044's `encoding::AppendDdlColumnType(col)`.

### DELETE — PK strategy

```sql
CREATE TABLE #upd_<uuid> (
    <pk_col1> <pk_type1>,
    <pk_col2> <pk_type2>,
    -- …each PK column from caps.pk_columns…
);
```

### DELETE — PhysLoc strategy

```sql
CREATE TABLE #upd_<uuid> (
    __physloc VARBINARY(8)
);
```

### UPDATE — PK strategy

```sql
CREATE TABLE #upd_<uuid> (
    <pk_col1> <pk_type1>, …,                    -- rowid columns
    <upd_col1> <upd_type1>, <upd_col2> <upd_type2>, …  -- updated columns
);
```

### UPDATE — PhysLoc strategy

```sql
CREATE TABLE #upd_<uuid> (
    __physloc VARBINARY(8),                     -- rowid
    <upd_col1> <upd_type1>, …                   -- updated columns
);
```

## E6. JOIN execution templates

### Staging DELETE — PK strategy

```sql
DELETE target
FROM <schema>.<target_table> AS target
INNER JOIN #upd_<uuid> AS s
    ON target.<pk1> = s.<pk1> AND target.<pk2> = s.<pk2> AND …
[OUTPUT DELETED.<col1>, DELETED.<col2>, …];
```

### Staging DELETE — PhysLoc strategy

```sql
DELETE target
FROM <schema>.<target_table> AS target
INNER JOIN #upd_<uuid> AS s
    ON target.%%physloc%% = s.__physloc
[OUTPUT DELETED.<col1>, …];
```

### Staging UPDATE — PK strategy

```sql
UPDATE target
SET target.<col1> = s.<upd_col1>, target.<col2> = s.<upd_col2>, …
FROM <schema>.<target_table> AS target
INNER JOIN #upd_<uuid> AS s
    ON target.<pk1> = s.<pk1> AND …
[OUTPUT INSERTED.<col1>, …];
```

### Staging UPDATE — PhysLoc strategy

```sql
UPDATE target
SET target.<col1> = s.<upd_col1>, …
FROM <schema>.<target_table> AS target
INNER JOIN #upd_<uuid> AS s
    ON target.%%physloc%% = s.__physloc
[OUTPUT INSERTED.<col1>, …];
```

## E7. Scan-side rowid emission

`bind_data` (existing struct) gains two flags:

```cpp
struct MSSQLScanBindData {
    // …existing fields…
    bool emit_rowid = false;
    RowIdStrategy rowid_strategy = RowIdStrategy::PrimaryKey;
};
```

When `emit_rowid = true`, the scan operator's SELECT-list builder
appends:

- PK strategy: each PK column from `caps.pk_columns` (already in
  the table's column list; we ensure they are projected even when
  the user's query doesn't reference them).
- PhysLoc strategy: `%%physloc%% AS __physloc` as a trailing
  column.

The chunk output gains a corresponding trailing column (or a
dedicated rowid slot per DuckDB's plan convention).

`MaterializeMssqlScans(plan, strategy)`:

```cpp
void MaterializeMssqlScans(PhysicalOperator &plan, RowIdStrategy strategy);
```

Walks the plan tree depth-first; for each MSSQL scan
(`PhysicalTableScan` whose `function.name` matches the MSSQL
scan function name), sets `emit_rowid = true`,
`rowid_strategy = strategy`, `requires_materialization = true`,
`max_threads = 1`.

## E8. `MSSQLDmlReturningParser`

- **Headers**:
  `src/include/dml/mssql_dml_returning_parser.hpp`
- **Source**:
  `src/dml/mssql_dml_returning_parser.cpp` (renamed from
  `src/dml/insert/mssql_returning_parser.cpp`)

Old constructor:

```cpp
MSSQLReturningParser(const MSSQLInsertTarget &target, …);
```

New constructor:

```cpp
MSSQLDmlReturningParser(
    const vector<TdsColumnMetadata> &output_columns,
    const vector<idx_t> &returning_column_ids,
    /* …existing args… */
);
```

INSERT-side shim (in `src/dml/insert/mssql_insert.cpp`):

```cpp
auto parser = make_uniq<MSSQLDmlReturningParser>(
    insert_target.GetTdsColumns(),
    insert_target.GetReturningColumnIds(),
    /* … */);
```

Direct UPDATE / DELETE and staging UPDATE / DELETE construct the
parser from their own column metadata.

## E9. Settings registration

Three new session settings (registered alongside existing
`mssql_*` settings) and routed through the existing
`MSSQLDMLConfig` loader (`src/include/dml/mssql_dml_config.hpp` +
`LoadDMLConfig(context)`) so all UPDATE/DELETE-relevant settings
land in one struct:

| Name | Type | Default | Allowed values | Effect |
|---|---|---|---|---|
| `mssql_rowid_strategy` | string | `auto` | `auto`, `pk`, `physloc` | Per spec.md FR-021 / research.md §R2. |
| `mssql_direct_dml` | boolean | `true` | — | When `false`, force staging path even for direct-eligible plans. Debug aid. |
| `mssql_dml_log_transactions` | boolean | `false` | — | When `true`, log every `BEGIN` / `SAVE` / `COMMIT` / `ROLLBACK` issued by the staging operators. |

`MSSQLDMLConfig` gains three fields (`rowid_strategy`,
`direct_dml`, `dml_log_transactions`); `LoadDMLConfig` is extended
to populate them. The existing `batch_size` / `max_parameters` /
`use_prepared` fields are retained for any code path that still
needs row-batching (currently INSERT VALUES batching; the staging
path uses BCP and does not consult them).

Validation happens at consumption — `ResolveRowIdStrategy` rejects
unknown strings via `BinderException`. No validation at
registration.

## E10. MERGE guard

- **Header**: `src/include/dml/merge/mssql_merge_guard.hpp`
- **Source**: `src/dml/merge/mssql_merge_guard.cpp`

Single function:

```cpp
namespace duckdb {

[[noreturn]] void ThrowMssqlMergeNotImplemented(const string &target_name);

} // namespace duckdb
```

Body:

```cpp
throw NotImplementedException(
    "MERGE INTO against MSSQL tables (" + target_name + ") is "
    "temporarily disabled in v0.2.0. The previous implementation "
    "(in v0.1.x) produced incorrect results for WHEN NOT MATCHED "
    "clauses and did not support WHEN NOT MATCHED BY SOURCE or "
    "RETURNING merge_action. A proper MERGE implementation is "
    "planned for a future release. In the meantime, use separate "
    "INSERT and UPDATE statements:\n"
    "  -- INSERT new rows:\n"
    "  INSERT INTO mssql.<schema>.<target> (...)\n"
    "  SELECT ... FROM source\n"
    "  WHERE NOT EXISTS (SELECT 1 FROM mssql.<schema>.<target> WHERE ...);\n"
    "  -- UPDATE matched rows:\n"
    "  UPDATE mssql.<schema>.<target> SET ... FROM source WHERE ..."
);
```

Called from `MSSQLCatalog::PlanMergeInto` (FR-040).

## E11. Files removed

```text
src/dml/delete/mssql_delete_executor.cpp
src/dml/delete/mssql_delete_statement.cpp
src/dml/delete/mssql_delete_target.cpp
src/dml/delete/mssql_physical_delete.cpp
src/dml/update/mssql_physical_update.cpp
src/dml/update/mssql_update_executor.cpp
src/dml/update/mssql_update_statement.cpp
src/dml/update/mssql_update_target.cpp
src/dml/mssql_rowid_extractor.cpp
```

And their corresponding headers under `src/include/dml/`.

## E12. Test fixtures

`test/sql/dml/dml_capabilities.test` populates a fixture catalog
with ≥ 7 table shapes (FR-001 / SC-001):

| Fixture name | Shape | `has_primary_key` | `supports_physloc` | `physloc_unavailable_reason` |
|---|---|---|---|---|
| `T_HEAP` | heap, no PK | false | true | "" |
| `T_CB` | clustered B-tree | true | true | "" |
| `T_NCCI` | rowstore + NCCI | true | false | "table has clustered columnstore index" |
| `T_CCI` | clustered columnstore | false | false | "table has clustered columnstore index" |
| `T_INMEM` | memory-optimized | true | false | "table is memory-optimized" |
| `T_EXT` | external (PolyBase) | false | false | "table is external (PolyBase)" |
| `T_SYNAPSE` | (mocked EngineEdition = 6) | true | false | "engine is Azure Synapse Dedicated (no physloc support)" |

If a live SQL Server instance in CI lacks the in-memory or external
features, the fixture is built behind a `mode skipif` guard with
the reason recorded — `data-model.md` is the authoritative reference
for what the test expects.
