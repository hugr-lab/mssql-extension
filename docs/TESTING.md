# MSSQL Extension Testing Guide

This guide provides comprehensive instructions for testing the DuckDB MSSQL Extension.

## Table of Contents

1. [Prerequisites](#prerequisites)
2. [Test Environment Setup](#test-environment-setup)
3. [Running Tests](#running-tests)
4. [Test Structure](#test-structure)
5. [Writing New Tests](#writing-new-tests)
6. [Test Data Reference](#test-data-reference)
7. [Troubleshooting](#troubleshooting)

---

## Prerequisites

### Required Software

- **Docker** and **Docker Compose** - For running SQL Server test container
- **CMake** (3.21+) - Build system
- **Ninja** - Build tool (recommended)
- **C++17 compiler** - GCC 9+, Clang 10+, or MSVC 2019+
- **vcpkg** - Automatically bootstrapped by the build system

### System Requirements

- 4GB+ RAM (SQL Server container requires ~2GB)
- 10GB+ disk space (for SQL Server image and build artifacts)

---

## Test Environment Setup

### 1. Start SQL Server Container

```bash
# Start SQL Server and initialize test database
make docker-up
```

This command:
- Pulls the SQL Server 2022 Docker image (if not present)
- Starts SQL Server on `localhost:1433`
- Waits for SQL Server to be healthy
- Runs `docker/init/init.sql` to create test database and data

### 2. Verify Container Status

```bash
make docker-status
```

Expected output:
```
SQL Server container status:
NAME        STATUS
mssql-dev   healthy

Testing connection...
Connection OK
```

### 3. Stop SQL Server Container

```bash
make docker-down
```

---

## Running Tests

### Quick Reference

| Command | Description |
|---------|-------------|
| `make test` | Run unit tests (no SQL Server required) |
| `make integration-test` | Run integration tests (requires SQL Server) |
| `make test-all` | Run all tests |

### Unit Tests

Unit tests do not require SQL Server and test isolated components:

```bash
make test
```

### Integration Tests

Integration tests require a running SQL Server instance:

```bash
# Ensure SQL Server is running
make docker-up

# Run integration tests
make integration-test
```

This runs two test suites:
1. `[integration]` - Tests in `test/sql/integration/` folder
2. `[sql]` - Tests with `# group: [sql]` tag (includes catalog tests)

### Running Specific Tests

Run tests matching a pattern:

```bash
# Run only catalog tests
build/release/test/unittest "/path/to/mssql-extension/test/sql/catalog/*" --force-reload

# Run tests containing "basic" in name
build/release/test/unittest "*basic*" --force-reload

# Run a single test file
build/release/test/unittest "test/sql/integration/basic_queries.test" --force-reload
```

### Environment Variables

Tests require these environment variables (automatically set by `make integration-test`):

| Variable | Default | Description |
|----------|---------|-------------|
| `MSSQL_TEST_HOST` | `localhost` | SQL Server hostname |
| `MSSQL_TEST_PORT` | `1433` | SQL Server port |
| `MSSQL_TEST_USER` | `sa` | SQL Server username |
| `MSSQL_TEST_PASS` | `TestPassword1` | SQL Server password |
| `MSSQL_TEST_DB` | `master` | Default database |
| `MSSQL_TEST_DSN` | (computed) | ADO.NET connection string for master |
| `MSSQL_TESTDB_DSN` | (computed) | ADO.NET connection string for TestDB |

**Debug Environment Variables:**

| Variable | Values | Description |
|----------|--------|-------------|
| `MSSQL_DEBUG` | `1`, `2`, `3` | TDS protocol debug level (1=basic, 3=trace) |
| `MSSQL_DML_DEBUG` | `1` | Enable DML operation debugging (INSERT/UPDATE/DELETE) |

To run tests manually with custom environment:

```bash
export MSSQL_TEST_HOST=localhost
export MSSQL_TEST_PORT=1433
export MSSQL_TEST_USER=sa
export MSSQL_TEST_PASS=TestPassword1
export MSSQL_TEST_DSN="Server=localhost,1433;Database=master;User Id=sa;Password=TestPassword1"
export MSSQL_TESTDB_DSN="Server=localhost,1433;Database=TestDB;User Id=sa;Password=TestPassword1"

build/release/test/unittest "[sql]" --force-reload
```

---

## Test Structure

### Directory Layout

```
test/
├── sql/
│   ├── catalog/                    # Catalog integration tests
│   │   ├── catalog_parsing.test    # Schema/table/column discovery
│   │   ├── select_queries.test     # SELECT query tests
│   │   └── data_types.test         # Data type handling tests
│   ├── dml/                        # UPDATE and DELETE tests
│   │   ├── update_scalar.test      # Single-row UPDATE tests
│   │   ├── update_bulk.test        # Bulk UPDATE tests
│   │   ├── update_types.test       # UPDATE data type handling
│   │   ├── update_errors.test      # UPDATE error handling
│   │   ├── delete_basic.test       # Basic DELETE tests
│   │   ├── delete_bulk.test        # Bulk DELETE tests
│   │   └── delete_errors.test      # DELETE error handling
│   ├── insert/                     # INSERT tests
│   │   ├── insert_basic.test       # Basic INSERT tests
│   │   ├── insert_bulk.test        # Bulk INSERT tests
│   │   ├── insert_types.test       # INSERT data type handling
│   │   ├── insert_errors.test      # INSERT error handling
│   │   └── insert_returning.test   # INSERT with RETURNING clause
│   ├── integration/                # Core integration tests
│   │   ├── basic_queries.test      # Basic query functionality
│   │   ├── large_data.test         # Large dataset handling
│   │   ├── parallel_queries.test   # Concurrent query tests
│   │   ├── connection_pool.test    # Connection pool tests
│   │   ├── query_cancellation.test # Query cancellation tests
│   │   ├── tls_connection.test     # TLS connection tests
│   │   ├── tls_queries.test        # TLS query tests
│   │   └── tls_parallel.test       # TLS parallel tests
│   ├── tds_connection/             # TDS protocol tests
│   │   ├── open_close.test
│   │   ├── ping.test
│   │   ├── pool_stats.test
│   │   └── settings.test
│   ├── query/                      # Query-level tests
│   │   ├── basic_select.test
│   │   ├── cancellation.test
│   │   ├── error_handling.test
│   │   ├── info_messages.test
│   │   └── type_mapping.test
│   ├── mssql_attach.test           # ATTACH/DETACH tests
│   ├── mssql_scan.test             # mssql_scan() function tests
│   ├── mssql_secret.test           # Secret management tests
│   ├── mssql_version.test          # Version info tests
│   └── tls_secret.test             # TLS secret tests
└── cpp/                            # C++ unit tests
    └── test_simple_query.cpp
```

### Test File Format

Tests use DuckDB's SQLLogicTest format:

```sql
# name: test/sql/example.test
# description: Example test file
# group: [sql]

require mssql

require-env MSSQL_TEST_DSN

# Setup
statement ok
ATTACH '${MSSQL_TEST_DSN}' AS testdb (TYPE mssql);

# Test with expected result
query II
SELECT 1 AS a, 2 AS b;
----
1	2

# Test with multiple rows
query IT
SELECT id, name FROM testdb.dbo.test ORDER BY id;
----
1	A
2	B
3	C

# Test for error
statement error
SELECT * FROM nonexistent_table;
----
does not exist

# Cleanup
statement ok
DETACH testdb;
```

### Query Result Type Codes

| Code | Type | Description |
|------|------|-------------|
| `I` | INTEGER | Any integer type |
| `T` | TEXT | VARCHAR, NVARCHAR, CHAR |
| `R` | REAL | FLOAT, DOUBLE, DECIMAL |
| `B` | BOOLEAN | BIT |
| `D` | DATE | DATE type |
| `!` | ANY | Any type (flexible) |

### Test Groups

| Group | Description | Requires SQL Server |
|-------|-------------|---------------------|
| `[sql]` | General SQL tests | Yes |
| `[integration]` | Integration tests | Yes |
| `[mssql]` | MSSQL-specific tests (catalog, DML) | Yes |
| `[dml]` | DML operations (INSERT/UPDATE/DELETE) | Yes |

---

## Writing New Tests

### 1. Choose the Right Location

- **Catalog tests** → `test/sql/catalog/`
- **DML tests (UPDATE/DELETE)** → `test/sql/dml/`
- **INSERT tests** → `test/sql/insert/`
- **Integration tests** → `test/sql/integration/`
- **TDS protocol tests** → `test/sql/tds_connection/`
- **Query tests** → `test/sql/query/`

### 2. Create Test File

```sql
# name: test/sql/catalog/my_new_test.test
# description: Description of what this test covers
# group: [sql]

require mssql

require-env MSSQL_TESTDB_DSN

statement ok
ATTACH '${MSSQL_TESTDB_DSN}' AS mydb (TYPE mssql);

# Your tests here...

statement ok
DETACH mydb;
```

### 3. Use Unique Context Names

Each test file should use a unique database alias to avoid conflicts:

```sql
# Good - unique names
ATTACH '...' AS testdb_catalog (TYPE mssql);
ATTACH '...' AS testdb_select (TYPE mssql);
ATTACH '...' AS testdb_types (TYPE mssql);

# Bad - may conflict with other tests
ATTACH '...' AS testdb (TYPE mssql);
```

### 4. Test Naming Conventions

- Use descriptive test comments
- Group related tests with section headers
- Number tests for easy reference

```sql
# =============================================================================
# Schema Tests
# =============================================================================

# Test 1: Check dbo schema exists
query I
SELECT COUNT(*) > 0 FROM information_schema.schemata WHERE schema_name = 'dbo';
----
true

# Test 2: Check reserved word schema
query I
SELECT COUNT(*) > 0 FROM information_schema.schemata WHERE schema_name = 'SELECT';
----
true
```

### 5. Handle Special Characters

For tables/columns with special characters, use proper quoting:

```sql
# Reserved words - use double quotes in DuckDB
SELECT "COLUMN", "INDEX" FROM testdb."SELECT"."TABLE";

# Embedded quotes - double the quote character
SELECT "col""quote" FROM testdb."schema""quote"."table""quote";

# Spaces - use double quotes
SELECT "My Column" FROM testdb."My Schema"."My Table";
```

### 6. Writing DML Tests (INSERT/UPDATE/DELETE)

DML tests require careful setup and cleanup to ensure test isolation:

```sql
# name: test/sql/dml/my_update_test.test
# description: Test UPDATE operations
# group: [mssql]

require mssql

require-env MSSQL_TESTDB_DSN

# Use unique context name to avoid conflicts
statement ok
ATTACH '${MSSQL_TESTDB_DSN}' AS mssql_upd_mytest (TYPE mssql);

# Create test table with PRIMARY KEY (required for rowid-based UPDATE/DELETE)
# Use mssql_exec for CREATE TABLE with constraints
statement ok
DROP TABLE IF EXISTS mssql_upd_mytest.dbo.my_update_test;

statement ok
SELECT mssql_exec('mssql_upd_mytest', 'CREATE TABLE dbo.my_update_test (id INT PRIMARY KEY, name NVARCHAR(100), value DECIMAL(10,2))');

# IMPORTANT: Refresh cache after DDL via mssql_exec
statement ok
SELECT mssql_refresh_cache('mssql_upd_mytest');

# Insert test data
statement ok
INSERT INTO mssql_upd_mytest.dbo.my_update_test (id, name, value) VALUES (1, 'Test', 100.00);

# Test UPDATE
statement ok
UPDATE mssql_upd_mytest.dbo.my_update_test SET name = 'Updated' WHERE id = 1;

# Verify UPDATE worked
query IT
SELECT id, name FROM mssql_upd_mytest.dbo.my_update_test WHERE id = 1;
----
1	Updated

# Test DELETE
statement ok
DELETE FROM mssql_upd_mytest.dbo.my_update_test WHERE id = 1;

# Verify DELETE worked
query I
SELECT COUNT(*) FROM mssql_upd_mytest.dbo.my_update_test WHERE id = 1;
----
0

# Cleanup
statement ok
DROP TABLE mssql_upd_mytest.dbo.my_update_test;

statement ok
DETACH mssql_upd_mytest;
```

**DML Test Best Practices:**

1. **Always use unique context names** - Prefix with operation type (e.g., `mssql_upd_`, `mssql_del_`, `mssql_ins_`)
2. **Tables need PRIMARY KEY for UPDATE/DELETE** - rowid is derived from PK columns
3. **Call `mssql_refresh_cache()` after `mssql_exec()`** - Required for DuckDB to see DDL changes
4. **Clean up test data** - Delete or drop tables at the end of tests
5. **RETURNING clause** - Only supported for INSERT, not for UPDATE/DELETE

**DML Settings for Tests:**

```sql
# Configure batch size (default: 1000)
SET mssql_dml_batch_size = 100;

# Configure max parameters per batch (default: 2100)
SET mssql_dml_max_parameters = 500;

# Enable/disable prepared statements (default: true)
SET mssql_dml_use_prepared = false;
```

---

## Test Data Reference

### Databases

| Database | Description |
|----------|-------------|
| `master` | SQL Server system database |
| `TestDB` | Test database with comprehensive test data |

### Schemas in TestDB

| Schema | Description |
|--------|-------------|
| `dbo` | Default schema with most test tables |
| `test` | Additional test schema |
| `SELECT` | Schema with reserved word name |
| `My Schema` | Schema with space in name |
| `schema"quote` | Schema with quote in name |

### Tables in TestDB.dbo

| Table | Rows | Description |
|-------|------|-------------|
| `TestSimplePK` | 5 | Simple table with INT PK, NVARCHAR, DECIMAL |
| `TestCompositePK` | 7 | Table with composite primary key |
| `LargeTable` | 150,000 | Large dataset for performance testing |
| `AllDataTypes` | 6 | Table with all supported SQL Server types |
| `NullableTypes` | 5 | Table with nullable columns and NULL patterns |
| `SELECT` | 3 | Table with reserved word name |
| `Space Column Table` | 3 | Table with spaces in column names |

### Tables in Other Schemas

| Table | Description |
|-------|-------------|
| `test.test` | Simple test table with special column name |
| `SELECT.TABLE` | Table with reserved word schema and table names |
| `My Schema.My Table` | Table with spaces in schema/table/column names |
| `schema"quote.table"quote` | Table with quotes in names |

### Views in TestDB

| View | Description |
|------|-------------|
| `dbo.LargeTableView` | View over LargeTable with computed status column |
| `dbo.AllTypesView` | Subset of AllDataTypes columns |
| `SELECT.VIEW` | View with reserved word names |
| `dbo.SELECT VIEW` | View with space in name |

### Sample Data

**TestSimplePK:**
```
id | name           | value   | created_at
---|----------------|---------|------------
1  | First Record   | 100.50  | (datetime)
2  | Second Record  | 200.75  | (datetime)
3  | Third Record   | NULL    | (datetime)
4  | Fourth Record  | 400.00  | (datetime)
5  | Fifth Record   | 500.25  | (datetime)
```

**LargeTable formula:**
```sql
id = 1 to 150000
category = (id % 100) + 1  -- 1 to 100
name = 'Item_' + id
value = id * 1.5 + (id % 1000) / 100.0
created_date = '2024-01-01' + (id % 365) days
is_active = (id % 10 != 0)  -- 90% active
```

---

## Troubleshooting

### Common Issues

#### 1. "SQL Server is not running or not healthy"

```bash
# Check container status
docker ps -a | grep mssql

# View container logs
docker logs mssql-dev

# Restart container
make docker-down
make docker-up
```

#### 2. "Context already exists"

Tests are sharing state. Ensure each test file:
- Uses a unique database alias
- Properly detaches at the end

#### 3. "Expected vector of type INT8, but found vector of type UINT8"

This is a known issue with TINYINT type mapping. SQL Server TINYINT (0-255) maps to UINT8, but the type converter may expect INT8. Avoid querying tables with TINYINT columns through the catalog, or cast to INTEGER:

```sql
SELECT CAST(col_tinyint AS INTEGER) FROM table;
```

#### 4. "Virtual columns require projection pushdown"

Don't use `COUNT(*)` on MSSQL tables. Instead, count a specific column:

```sql
-- Won't work
SELECT COUNT(*) FROM testdb.dbo.table;

-- Works
SELECT COUNT(id) FROM testdb.dbo.table;
```

#### 5. Tests not being discovered

Ensure:
- Test file has `.test` extension
- Test file has correct `# group: [sql]` or `# group: [integration]` tag
- Test file is in `test/sql/` directory tree
- Run `cmake` to regenerate test registration

#### 6. Environment variable not found

```bash
# Check if variable is exported
echo $MSSQL_TESTDB_DSN

# Set manually if needed
export MSSQL_TESTDB_DSN="Server=localhost,1433;Database=TestDB;User Id=sa;Password=TestPassword1"
```

#### 7. UPDATE/DELETE fails with "Table has no primary key"

UPDATE and DELETE operations require tables to have a PRIMARY KEY. The extension uses PK columns to generate a synthetic `rowid` for targeting specific rows:

```sql
-- Won't work - table without primary key
UPDATE testdb.dbo.table_without_pk SET col = 'value' WHERE id = 1;

-- Solution: Add primary key to the table or use mssql_exec for direct SQL
SELECT mssql_exec('testdb', 'UPDATE dbo.table_without_pk SET col = ''value'' WHERE id = 1');
```

#### 8. INSERT values going to wrong columns

This can happen if INSERT column order doesn't match table column order. The extension should preserve INSERT statement column order, but verify:

```sql
-- Explicit column order (recommended)
INSERT INTO testdb.dbo.table (col_b, col_a) VALUES ('b_val', 'a_val');

-- Verify the values are correct
SELECT col_a, col_b FROM testdb.dbo.table WHERE ...;
```

#### 9. DML changes not visible after mssql_exec

After using `mssql_exec()` for DDL operations (CREATE TABLE, ALTER TABLE, etc.), refresh the catalog cache:

```sql
-- Create table via mssql_exec
SELECT mssql_exec('testdb', 'CREATE TABLE dbo.new_table (id INT PRIMARY KEY)');

-- REQUIRED: Refresh cache to see the new table
SELECT mssql_refresh_cache('testdb');

-- Now you can use the table
INSERT INTO testdb.dbo.new_table (id) VALUES (1);
```

#### 10. RETURNING clause not working for UPDATE/DELETE

RETURNING is only supported for INSERT operations (implemented via SQL Server's `OUTPUT INSERTED`). UPDATE and DELETE do not support RETURNING:

```sql
-- Works - INSERT with RETURNING
INSERT INTO testdb.dbo.table (id, name) VALUES (1, 'test') RETURNING *;

-- Does NOT work - UPDATE with RETURNING
UPDATE testdb.dbo.table SET name = 'updated' WHERE id = 1 RETURNING *;
-- Error: RETURNING is not supported for UPDATE

-- Workaround: Query after UPDATE
UPDATE testdb.dbo.table SET name = 'updated' WHERE id = 1;
SELECT * FROM testdb.dbo.table WHERE id = 1;
```

### Debug Mode

Enable debug logging by setting environment variable:

```bash
export MSSQL_DEBUG=1  # Basic debug
export MSSQL_DEBUG=2  # Verbose debug
export MSSQL_DEBUG=3  # Trace level

# Then run tests
build/release/test/unittest "[sql]" --force-reload
```

### Running Tests with Verbose Output

```bash
# Show test progress
build/release/test/unittest "[sql]" --force-reload -d yes

# Show all output including passing tests
build/release/test/unittest "[sql]" --force-reload -s
```

---

## CI/CD Integration

### GitHub Actions Example

```yaml
name: Tests

on: [push, pull_request]

jobs:
  test:
    runs-on: ubuntu-latest

    services:
      sqlserver:
        image: mcr.microsoft.com/mssql/server:2022-latest
        env:
          ACCEPT_EULA: Y
          SA_PASSWORD: TestPassword1
        ports:
          - 1433:1433
        options: >-
          --health-cmd "/opt/mssql-tools18/bin/sqlcmd -S localhost -U sa -P TestPassword1 -C -Q 'SELECT 1'"
          --health-interval 10s
          --health-timeout 5s
          --health-retries 10

    steps:
      - uses: actions/checkout@v4
        with:
          submodules: recursive

      - name: Build
        run: make release

      - name: Initialize Test Database
        run: |
          docker exec sqlserver /opt/mssql-tools18/bin/sqlcmd \
            -S localhost -U sa -P TestPassword1 -C \
            -i /path/to/init.sql

      - name: Run Tests
        env:
          MSSQL_TEST_HOST: localhost
          MSSQL_TEST_PORT: 1433
          MSSQL_TEST_USER: sa
          MSSQL_TEST_PASS: TestPassword1
          MSSQL_TEST_DSN: "Server=localhost,1433;Database=master;User Id=sa;Password=TestPassword1"
          MSSQL_TESTDB_DSN: "Server=localhost,1433;Database=TestDB;User Id=sa;Password=TestPassword1"
        run: |
          build/release/test/unittest "[integration]" --force-reload
          build/release/test/unittest "[sql]" --force-reload
```

---

## Summary

1. **Setup**: `make docker-up` to start SQL Server
2. **Run all tests**: `make integration-test`
3. **Run specific tests**: Use `build/release/test/unittest` with filters
4. **Write new tests**: Follow SQLLogicTest format in `test/sql/` directory
5. **Cleanup**: `make docker-down` to stop SQL Server

For questions or issues, see the project's GitHub issues page.
