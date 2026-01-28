# Quickstart: CTAS for MSSQL

**Branch**: `022-mssql-ctas` | **Date**: 2026-01-28

## Overview

This guide covers implementing CREATE TABLE AS SELECT (CTAS) for MSSQL-attached databases.

---

## Prerequisites

1. Working mssql-extension build environment
2. SQL Server 2019+ test instance (Docker or native)
3. Familiarity with existing DDL and INSERT paths

---

## Implementation Order

### Phase 1: Settings & Type Mapping

**Files to modify**:
- `src/connection/mssql_settings.cpp`
- `src/include/connection/mssql_settings.hpp`

**Tasks**:
1. Add `mssql_ctas_drop_on_failure` setting (BOOLEAN, default false)
2. Add `mssql_ctas_text_type` setting (VARCHAR, default "NVARCHAR")
3. Add `CTASConfig::Load()` helper function

**Validation**:
```sql
-- In DuckDB CLI
SET mssql_ctas_text_type = 'NVARCHAR';
SET mssql_ctas_drop_on_failure = true;
SELECT current_setting('mssql_ctas_text_type');  -- Should return 'NVARCHAR'
```

---

### Phase 2: DDL Translation

**Files to modify**:
- `src/include/catalog/mssql_ddl_translator.hpp`
- `src/catalog/mssql_ddl_translator.cpp`

**Tasks**:
1. Add `TranslateCreateTableFromSchema(schema, table, column_defs)` method
2. Add `MapLogicalTypeToCTAS(LogicalType, CTASConfig)` method
3. Add unsupported type detection with clear error messages

**Unit Test**:
```cpp
// test/cpp/test_ctas_type_mapping.cpp
TEST_CASE("CTAS type mapping", "[ctas]") {
    SECTION("INTEGER maps to int") {
        auto result = MSSQLDDLTranslator::MapLogicalTypeToCTAS(LogicalType::INTEGER, config);
        REQUIRE(result == "int");
    }
    SECTION("VARCHAR maps to nvarchar(max) by default") {
        CTASConfig config;
        config.text_type = CTASConfig::TextType::NVARCHAR;
        auto result = MSSQLDDLTranslator::MapLogicalTypeToCTAS(LogicalType::VARCHAR, config);
        REQUIRE(result == "nvarchar(max)");
    }
    SECTION("HUGEINT throws error") {
        REQUIRE_THROWS_WITH(
            MSSQLDDLTranslator::MapLogicalTypeToCTAS(LogicalType::HUGEINT, config),
            Catch::Contains("Unsupported DuckDB type")
        );
    }
}
```

---

### Phase 3: CTAS Planner

**Files to create**:
- `src/include/dml/ctas/mssql_ctas_planner.hpp`
- `src/dml/ctas/mssql_ctas_planner.cpp`

**Tasks**:
1. Create `CTASTarget` struct
2. Create `CTASColumnDef` struct
3. Create `CTASPlanner::Plan()` method that:
   - Extracts target table info from `CreateStatement`
   - Gets output types from source plan
   - Maps types to SQL Server
   - Returns `MSSQLPhysicalCreateTableAs` operator

**Key Pattern**:
```cpp
unique_ptr<PhysicalOperator> CTASPlanner::Plan(
    ClientContext &context,
    LogicalCreateTable &op,
    PhysicalOperator &source_plan) {

    // 1. Extract target info
    CTASTarget target = ExtractTarget(op);

    // 2. Get source column types
    auto source_types = source_plan.GetTypes();
    auto source_names = op.info->columns.GetColumnNames();

    // 3. Map types
    CTASConfig config = CTASConfig::Load(context);
    vector<CTASColumnDef> columns;
    for (idx_t i = 0; i < source_types.size(); i++) {
        columns.push_back(MapColumn(source_names[i], source_types[i], config));
    }

    // 4. Create physical operator
    return make_uniq<MSSQLPhysicalCreateTableAs>(target, columns, config);
}
```

---

### Phase 4: CTAS Executor

**Files to create**:
- `src/include/dml/ctas/mssql_ctas_executor.hpp`
- `src/dml/ctas/mssql_ctas_executor.cpp`

**Tasks**:
1. Create `CTASExecutionState` struct
2. Create `MSSQLPhysicalCreateTableAs` operator with:
   - `GetGlobalSinkState()` — initialize executor
   - `Sink()` — accumulate rows, execute INSERT batches
   - `Finalize()` — flush remaining rows, report count

**Execution Flow**:
```cpp
// In Sink(), on first chunk:
if (gstate.phase == Phase::PENDING) {
    // OR REPLACE: drop existing table
    if (target.or_replace && TableExists(context, target)) {
        ExecuteDrop(context, target);
    }

    // Execute CREATE TABLE
    gstate.ddl_sql = GenerateDDL(target, columns);
    ExecuteDDL(context, gstate.ddl_sql);
    gstate.phase = Phase::DDL_DONE;

    // Initialize insert executor
    gstate.insert_executor = CreateInsertExecutor(context, target, columns);
    gstate.phase = Phase::INSERT_EXECUTING;
}

// Accumulate rows
gstate.insert_executor->Execute(chunk);
gstate.rows_produced += chunk.size();
```

---

### Phase 5: Catalog Integration

**Files to modify**:
- `src/catalog/mssql_catalog.cpp`

**Tasks**:
1. Override `PlanCreateTableAs()` to route to CTAS planner
2. Add write access check

**Pattern**:
```cpp
unique_ptr<PhysicalOperator> MSSQLCatalog::PlanCreateTableAs(
    ClientContext &context,
    Planner &planner,
    LogicalCreateTable &op,
    PhysicalOperator &source_plan) {

    CheckWriteAccess("CREATE TABLE AS SELECT");
    return CTASPlanner::Plan(context, op, source_plan);
}
```

---

### Phase 6: Integration Tests

**Files to create** in `test/sql/ctas/`:

1. `ctas_basic.test` — Simple CTAS, row count, schema validation
2. `ctas_types.test` — All supported type mappings
3. `ctas_large.test` — 1M row CTAS, memory stability
4. `ctas_or_replace.test` — OR REPLACE behavior
5. `ctas_failure.test` — Error cases, cleanup behavior
6. `ctas_transaction.test` — Transaction semantics

**Example Test**:
```sql
# name: test/sql/ctas/ctas_basic.test
# group: [ctas]

require mssql

statement ok
ATTACH 'mssql://...' AS mssql;

statement ok
CREATE TABLE mssql.dbo.ctas_test AS SELECT 1 AS id, 'hello' AS name;

query II
SELECT * FROM mssql.dbo.ctas_test;
----
1	hello

statement ok
DROP TABLE mssql.dbo.ctas_test;
```

---

## Debug Tips

Enable debug logging:
```bash
export MSSQL_DEBUG=2
./build/release/duckdb
```

Check generated DDL:
```
[MSSQL CTAS] DDL: CREATE TABLE [dbo].[test] ([id] int NOT NULL, [name] nvarchar(max) NULL);
[MSSQL CTAS] DDL executed: 0 rows, 15 ms
[MSSQL CTAS] INSERT phase: 1000 rows, 3 batches, 245 ms
```

---

## Common Issues

1. **"Table already exists"** — Use `CREATE OR REPLACE TABLE` or drop first
2. **"Unsupported type"** — Check type mapping table, avoid HUGEINT/INTERVAL
3. **"Schema does not exist"** — Create schema first or use existing one
4. **Memory growth on large CTAS** — Check batch size settings, ensure streaming
