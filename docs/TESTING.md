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
| `MSSQL_TEST_URI` | (computed) | URI connection string for master |
| `MSSQL_TESTDB_DSN` | (computed) | ADO.NET connection string for TestDB |
| `MSSQL_TESTDB_URI` | (computed) | URI connection string for TestDB |
| `MSSQL_TEST_DSN_TLS` | (not exported) | TLS URI string (export manually for TLS tests) |

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
│   ├── attach/                     # ATTACH/DETACH tests
│   │   ├── attach_trust_cert.test  # TLS trust certificate tests
│   │   └── attach_validation.test  # Connection validation tests
│   ├── catalog/                    # Catalog integration tests
│   │   ├── catalog_parsing.test    # Schema/table/column discovery
│   │   ├── collation_filter.test   # Collation-aware filter pushdown
│   │   ├── data_types.test         # Data type handling tests
│   │   ├── datetimeoffset.test     # DATETIMEOFFSET type tests
│   │   ├── ddl_alter.test          # ALTER TABLE tests
│   │   ├── ddl_schema.test         # CREATE/DROP SCHEMA tests
│   │   ├── ddl_table.test          # CREATE/DROP TABLE tests
│   │   ├── filter_pushdown.test    # Filter pushdown tests
│   │   ├── read_only.test          # Read-only catalog tests
│   │   ├── select_queries.test     # SELECT query tests
│   │   └── statistics.test         # Statistics provider tests
│   ├── dml/                        # UPDATE and DELETE tests
│   │   ├── delete_bulk.test        # Bulk DELETE tests
│   │   ├── delete_composite_pk.test # DELETE with composite PK
│   │   ├── delete_scalar_pk.test   # DELETE with scalar PK
│   │   ├── update_bulk.test        # Bulk UPDATE tests
│   │   ├── update_composite_pk.test # UPDATE with composite PK
│   │   └── update_scalar_pk.test   # UPDATE with scalar PK
│   ├── insert/                     # INSERT tests
│   │   ├── insert_basic.test       # Basic INSERT tests
│   │   ├── insert_bulk.test        # Bulk INSERT tests
│   │   ├── insert_config.test      # INSERT configuration tests
│   │   ├── insert_errors.test      # INSERT error handling
│   │   ├── insert_returning.test   # INSERT with RETURNING clause
│   │   └── insert_types.test       # INSERT data type handling
│   ├── integration/                # Core integration tests
│   │   ├── basic_queries.test      # Basic query functionality
│   │   ├── connection_pool.test    # Connection pool tests
│   │   ├── diagnostic_functions.test # Diagnostic function tests
│   │   ├── filter_pushdown.test    # Integration filter pushdown
│   │   ├── large_data.test         # Large dataset handling
│   │   ├── max_types.test          # MAX type tests (VARCHAR(MAX), etc.)
│   │   ├── parallel_queries.test   # Concurrent query tests
│   │   ├── pool_limits.test        # Connection pool limit tests
│   │   ├── query_cancellation.test # Query cancellation tests
│   │   ├── tls_connection.test     # TLS connection tests
│   │   ├── tls_multipacket.test    # TLS multi-packet tests
│   │   ├── tls_parallel.test       # TLS parallel tests
│   │   └── tls_queries.test        # TLS query tests
│   ├── query/                      # Query-level tests
│   │   ├── basic_select.test
│   │   ├── cancellation.test
│   │   ├── error_handling.test
│   │   ├── info_messages.test
│   │   └── type_mapping.test
│   ├── rowid/                      # Rowid pseudo-column tests
│   │   ├── composite_pk_rowid.test # Composite PK rowid behavior
│   │   ├── no_pk_rowid.test        # Tables without PK
│   │   ├── rowid_filter_pushdown.test # Rowid in filters
│   │   ├── scalar_pk_rowid.test    # Scalar PK rowid behavior
│   │   └── view_rowid.test         # Views and rowid
│   ├── tds_connection/             # TDS protocol tests
│   │   ├── open_close.test
│   │   ├── ping.test
│   │   ├── pool_stats.test
│   │   └── settings.test
│   ├── transaction/                # Transaction management tests
│   │   ├── transaction_autocommit.test       # Autocommit mode behavior
│   │   ├── transaction_catalog_restriction.test # Catalog scans in transactions
│   │   ├── transaction_commit.test           # DML commit operations
│   │   ├── transaction_mssql_exec.test       # mssql_exec in transactions
│   │   ├── transaction_mssql_scan.test       # mssql_scan in transactions
│   │   └── transaction_rollback.test         # Rollback operations
│   ├── copy/                       # COPY TO MSSQL (BulkLoadBCP) tests
│   │   ├── copy_basic.test         # Basic COPY with URL and catalog syntax
│   │   ├── copy_types.test         # Data type handling in COPY
│   │   ├── copy_large.test         # Large dataset COPY performance
│   │   ├── copy_errors.test        # Error handling (missing table, bad data)
│   │   ├── copy_overwrite.test     # REPLACE option (drop and recreate)
│   │   ├── copy_temp.test          # Session-scoped temp table COPY
│   │   ├── copy_transaction.test   # COPY within transactions
│   │   └── copy_column_mapping.test # Name-based column mapping tests
│   ├── catalog_discovery.test      # Catalog discovery tests
│   ├── mssql_attach.test           # ATTACH/DETACH tests
│   ├── mssql_exec.test             # mssql_exec() function tests
│   ├── mssql_scan.test             # mssql_scan() function tests
│   ├── mssql_secret.test           # Secret management tests
│   ├── mssql_version.test          # Version info tests
│   ├── multipacket.test            # Multi-packet query tests
│   └── tls_secret.test             # TLS secret tests
└── cpp/                            # C++ unit tests
    ├── test_batch_builder.cpp      # INSERT batch building
    ├── test_connection_pool.cpp    # Connection pooling
    ├── test_ddl_translator.cpp     # DDL translation
    ├── test_insert_executor.cpp    # INSERT execution
    ├── test_multi_connection_transactions.cpp # Multi-connection transaction isolation
    ├── test_simple_query.cpp       # Basic query execution
    ├── test_statistics_provider.cpp # Statistics collection
    ├── test_tls_connection.cpp     # TLS connection support
    └── test_value_serializer.cpp   # Type serialization
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
| `[mssql]` | MSSQL-specific tests (catalog, DML, transactions) | Yes |
| `[dml]` | DML operations (INSERT/UPDATE/DELETE) | Yes |
| `[transaction]` | Transaction management tests | Yes |
| `[copy]` | COPY TO MSSQL (BulkLoadBCP) tests | Yes |

---

## Writing New Tests

### 1. Choose the Right Location

- **Attach/detach tests** → `test/sql/attach/`
- **Catalog tests** → `test/sql/catalog/`
- **DML tests (UPDATE/DELETE)** → `test/sql/dml/`
- **INSERT tests** → `test/sql/insert/`
- **Integration tests** → `test/sql/integration/`
- **Query tests** → `test/sql/query/`
- **Rowid tests** → `test/sql/rowid/`
- **TDS protocol tests** → `test/sql/tds_connection/`
- **Transaction tests** → `test/sql/transaction/`

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
# Configure batch size (default: 500)
SET mssql_dml_batch_size = 100;

# Configure max parameters per batch (default: 2000)
SET mssql_dml_max_parameters = 500;

# Enable/disable prepared statements (default: true)
SET mssql_dml_use_prepared = false;
```

### 7. Writing Transaction Tests

Transaction tests verify BEGIN/COMMIT/ROLLBACK behavior with the MSSQL extension:

```sql
# name: test/sql/transaction/my_transaction_test.test
# description: Test transaction behavior
# group: [transaction]

require mssql

require-env MSSQL_TESTDB_DSN

statement ok
ATTACH '${MSSQL_TESTDB_DSN}' AS txdb (TYPE mssql);

# Test COMMIT: changes persist
statement ok
BEGIN;

statement ok
INSERT INTO txdb.dbo.TxTestOrders (id, customer, amount, status)
VALUES (100, 'Test Customer', 50.00, 'pending');

statement ok
COMMIT;

query I
SELECT COUNT(id) FROM txdb.dbo.TxTestOrders WHERE id = 100;
----
1

# Test ROLLBACK: changes discarded
statement ok
BEGIN;

statement ok
DELETE FROM txdb.dbo.TxTestOrders WHERE id = 100;

statement ok
ROLLBACK;

query I
SELECT COUNT(id) FROM txdb.dbo.TxTestOrders WHERE id = 100;
----
1

# Cleanup
statement ok
DELETE FROM txdb.dbo.TxTestOrders WHERE id = 100;

statement ok
DETACH txdb;
```

**Transaction Test Best Practices:**

1. **Use dedicated transaction test tables** (TxTestOrders, TxTestProducts, TxTestCounter, TxTestLogs)
2. **Always clean up** rows inserted during tests to restore initial state
3. **Test both COMMIT and ROLLBACK** to verify atomicity
4. **Tables need PRIMARY KEY** for UPDATE/DELETE within transactions
5. **mssql_exec and mssql_scan work inside transactions** using the pinned connection

### 8. Writing COPY Tests

COPY tests verify bulk data transfer using the TDS BulkLoadBCP protocol:

```sql
# name: test/sql/copy/my_copy_test.test
# description: Test COPY TO MSSQL functionality
# group: [copy]

require mssql

require-env MSSQL_TESTDB_DSN

statement ok
ATTACH '${MSSQL_TESTDB_DSN}' AS copydb (TYPE mssql);

# Create local source data
statement ok
CREATE TABLE local_source AS SELECT i::BIGINT AS id, ('Item ' || i)::VARCHAR AS name FROM range(1, 101) t(i);

# Clean up target if exists
statement ok
DROP TABLE IF EXISTS copydb.dbo.copy_test_target;

# Test COPY with CREATE_TABLE option
statement ok
COPY local_source TO 'mssql://copydb/dbo/copy_test_target' (FORMAT 'bcp', CREATE_TABLE true);

# Verify data was transferred
query I
SELECT COUNT(*) FROM copydb.dbo.copy_test_target;
----
100

# Test REPLACE option (drop and recreate with new data)
statement ok
CREATE TABLE local_replace AS SELECT 1::BIGINT AS new_col;

statement ok
COPY local_replace TO 'mssql://copydb/dbo/copy_test_target' (FORMAT 'bcp', REPLACE true);

query I
SELECT COUNT(*) FROM copydb.dbo.copy_test_target;
----
1

# Cleanup
statement ok
DROP TABLE IF EXISTS copydb.dbo.copy_test_target;

statement ok
DROP TABLE local_source;

statement ok
DROP TABLE local_replace;

statement ok
DETACH copydb;
```

**COPY Test Best Practices:**

1. **Use unique context names** - Prefix with `copy` (e.g., `copydb`, `copytx`)
2. **Clean up target tables** - Use `DROP TABLE IF EXISTS` before and after tests
3. **Test both URL and catalog syntax**:
   - URL: `'mssql://catalog/schema/table'`
   - Catalog: `'catalog.schema.table'`
4. **Test temp tables within transactions** - Temp tables require transaction context:
   ```sql
   BEGIN;
   COPY data TO 'mssql://db/#temp_table' (FORMAT 'bcp', CREATE_TABLE true);
   SELECT * FROM mssql_scan('db', 'SELECT * FROM #temp_table');
   COMMIT;
   ```
5. **Use mssql_scan for in-transaction verification** - Catalog metadata queries use separate connections, so use `mssql_scan` to verify data within a transaction

**COPY Options:**

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `CREATE_TABLE` | BOOLEAN | true | Auto-create table if not exists |
| `REPLACE` | BOOLEAN | false | Drop and recreate table |
| `FLUSH_ROWS` | BIGINT | 100000 | Rows before flush (bounded memory) |
| `TABLOCK` | BOOLEAN | true | Use TABLOCK for faster inserts |

**Note**: Use `REPLACE` instead of `OVERWRITE` - DuckDB intercepts `OVERWRITE` as a built-in file operation.

### 9. Writing Column Mapping Tests (COPY TO)

When copying to existing tables with `CREATE_TABLE false`, the extension uses name-based column mapping:

```sql
# name: test/sql/copy/my_column_mapping_test.test
# description: Test column mapping for COPY TO existing tables
# group: [copy]

require mssql

require-env MSSQL_TESTDB_DSN

statement ok
ATTACH '${MSSQL_TESTDB_DSN}' AS db (TYPE mssql);

statement ok
BEGIN TRANSACTION;

# Create target table with specific columns
statement ok
SELECT mssql_exec('db', 'CREATE TABLE #test_mapping (id INT, name NVARCHAR(50), value FLOAT)');

# Create source with different column order
statement ok
CREATE TABLE source_reorder AS SELECT 3.5::DOUBLE AS value, 'Bob' AS name, 2 AS id;

# Copy with reordered columns - should map by name, not position
statement ok
COPY source_reorder TO 'mssql://db//#test_mapping' (FORMAT 'bcp', CREATE_TABLE false);

# Verify - values should be correctly mapped by name
query III
SELECT id, name, value FROM mssql_scan('db', 'SELECT * FROM #test_mapping ORDER BY id');
----
2	Bob	3.5

statement ok
ROLLBACK;

statement ok
DETACH db;
```

**Column Mapping Test Scenarios:**

1. **Exact column match** - Source and target have same columns in same order (backward compatibility)
2. **Subset of columns** - Source has fewer columns; missing target columns receive NULL
3. **Column reordering** - Source columns in different order; mapped by name
4. **Extra source columns** - Source has columns not in target; extra columns ignored
5. **Combined subset + reorder** - Source has fewer columns in different order
6. **Case-insensitive matching** - Source `id` matches target `ID`
7. **No matching columns** - Error expected when no source columns match target
8. **Multiple rows** - Verify mapping works correctly for multi-row inserts

**Column Mapping Best Practices:**

1. **Target columns allowing NULL**: Unmapped target columns must allow NULL values
2. **At least one match required**: COPY fails if no source columns match any target columns
3. **Use transactions for temp tables**: Temp table tests require transaction context
4. **Verify with mssql_scan**: Use `mssql_scan()` to query temp tables within transactions

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
| `TxTestOrders` | 3 | Transaction test: INSERT/UPDATE/DELETE with orders |
| `TxTestProducts` | 5 | Transaction test: combined DML operations |
| `TxTestLogs` | 0 | Transaction test: mssql_exec logging (IDENTITY PK) |
| `TxTestCounter` | 2 | Transaction test: numeric UPDATE operations |
| `tx_test` | 3 | Multi-connection transaction isolation tests |

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

#### 4. Tests not being discovered

Ensure:
- Test file has `.test` extension
- Test file has correct `# group: [sql]` or `# group: [integration]` tag
- Test file is in `test/sql/` directory tree
- Run `cmake` to regenerate test registration

#### 5. Environment variable not found

```bash
# Check if variable is exported
echo $MSSQL_TESTDB_DSN

# Set manually if needed
export MSSQL_TESTDB_DSN="Server=localhost,1433;Database=TestDB;User Id=sa;Password=TestPassword1"
```

#### 6. UPDATE/DELETE fails with "Table has no primary key"

UPDATE and DELETE operations require tables to have a PRIMARY KEY. The extension uses PK columns to generate a synthetic `rowid` for targeting specific rows:

```sql
-- Won't work - table without primary key
UPDATE testdb.dbo.table_without_pk SET col = 'value' WHERE id = 1;

-- Solution: Add primary key to the table or use mssql_exec for direct SQL
SELECT mssql_exec('testdb', 'UPDATE dbo.table_without_pk SET col = ''value'' WHERE id = 1');
```

#### 7. INSERT values going to wrong columns

This can happen if INSERT column order doesn't match table column order. The extension should preserve INSERT statement column order, but verify:

```sql
-- Explicit column order (recommended)
INSERT INTO testdb.dbo.table (col_b, col_a) VALUES ('b_val', 'a_val');

-- Verify the values are correct
SELECT col_a, col_b FROM testdb.dbo.table WHERE ...;
```

#### 8. DML changes not visible after mssql_exec

After using `mssql_exec()` for DDL operations (CREATE TABLE, ALTER TABLE, etc.), refresh the catalog cache:

```sql
-- Create table via mssql_exec
SELECT mssql_exec('testdb', 'CREATE TABLE dbo.new_table (id INT PRIMARY KEY)');

-- REQUIRED: Refresh cache to see the new table
SELECT mssql_refresh_cache('testdb');

-- Now you can use the table
INSERT INTO testdb.dbo.new_table (id) VALUES (1);
```

#### 9. RETURNING clause not working for UPDATE/DELETE

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
