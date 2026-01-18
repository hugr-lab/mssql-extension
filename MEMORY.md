# MSSQL Extension - Quick Reference Memory

## Testing Commands

### Start Test Environment
```bash
make docker-up          # Start SQL Server + initialize TestDB
make docker-status      # Check SQL Server health
make docker-down        # Stop SQL Server
```

### Run Tests
```bash
make test               # Unit tests (no SQL Server needed)
make integration-test   # Integration tests (needs SQL Server)
make test-all           # All tests
```

### Run Specific Tests
```bash
# By pattern
build/release/test/unittest "*catalog*" --force-reload
build/release/test/unittest "*basic*" --force-reload

# By file path
build/release/test/unittest "/path/to/test/sql/catalog/*" --force-reload

# By group
build/release/test/unittest "[sql]" --force-reload
build/release/test/unittest "[integration]" --force-reload
```

### Manual Test Run with Environment
```bash
export MSSQL_TEST_DSN="Server=localhost,1433;Database=master;User Id=sa;Password=TestPassword1"
export MSSQL_TESTDB_DSN="Server=localhost,1433;Database=TestDB;User Id=sa;Password=TestPassword1"
build/release/test/unittest "[sql]" --force-reload
```

---

## Test Database Structure

### Connection Strings
| Database | DSN Variable |
|----------|--------------|
| master | `MSSQL_TEST_DSN` |
| TestDB | `MSSQL_TESTDB_DSN` |

### TestDB Schemas
- `dbo` - Default schema
- `test` - Test schema
- `SELECT` - Reserved word schema
- `My Schema` - Space in name
- `schema"quote` - Quote in name

### Key Test Tables
| Table | Rows | Notes |
|-------|------|-------|
| `dbo.TestSimplePK` | 5 | INT, NVARCHAR, DECIMAL |
| `dbo.LargeTable` | 150k | Performance testing |
| `dbo.AllDataTypes` | 6 | All SQL Server types |
| `SELECT.TABLE` | 3 | Reserved word names |
| `My Schema.My Table` | 3 | Space in names |

---

## Test File Format

```sql
# name: test/sql/catalog/example.test
# description: Test description
# group: [sql]

require mssql
require-env MSSQL_TESTDB_DSN

statement ok
ATTACH '${MSSQL_TESTDB_DSN}' AS testdb_example (TYPE mssql);

# Test query with expected result
query II
SELECT id, name FROM testdb_example.dbo.TestSimplePK WHERE id = 1;
----
1	First Record

statement ok
DETACH testdb_example;
```

### Query Type Codes
- `I` = INTEGER
- `T` = TEXT/VARCHAR
- `R` = REAL/DECIMAL
- `B` = BOOLEAN

---

## Quoting Rules

```sql
-- Reserved words (use double quotes in DuckDB)
SELECT "COLUMN" FROM testdb."SELECT"."TABLE";

-- Embedded quotes (double the quote)
SELECT "col""quote" FROM testdb."schema""quote"."table""quote";

-- Spaces
SELECT "My Column" FROM testdb."My Schema"."My Table";
```

---

## Known Issues

1. **TINYINT type mismatch**: Cast to INTEGER when querying TINYINT columns
   ```sql
   SELECT CAST(col_tinyint AS INTEGER) FROM table;
   ```

2. **COUNT(*) not supported**: Use specific column
   ```sql
   SELECT COUNT(id) FROM table;  -- Works
   SELECT COUNT(*) FROM table;   -- Fails
   ```

3. **Context collision**: Use unique alias names per test file

---

## Debug Mode

```bash
export MSSQL_DEBUG=1   # Basic debug
export MSSQL_DEBUG=2   # Verbose
export MSSQL_DEBUG=3   # Trace
```

---

## Build Commands

```bash
make release           # Build release
make debug             # Build debug
make clean             # Clean build
```

---

## File Locations

| File | Purpose |
|------|---------|
| `docker/init/init.sql` | Test database initialization |
| `test/sql/catalog/` | Catalog integration tests |
| `test/sql/integration/` | Core integration tests |
| `docs/TESTING.md` | Full testing guide |
