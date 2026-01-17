# MSSQL Extension Tests

This directory contains tests for the DuckDB MSSQL extension.

## Requirements

- **SQL Server 2022+** running on localhost:1433 (for GENERATE_SERIES support)
- **Docker** (recommended) or a local SQL Server installation

### Starting SQL Server with Docker

```bash
make docker-up
# Or manually:
docker compose -f docker/docker-compose.yml up -d
```

This starts a SQL Server 2022 instance on port 1433 with credentials:
- Username: `sa`
- Password: `TestPassword1`
- Database: `master`

## TLS Support

**Important:** TLS (encrypted connections) is only available in the **loadable extension** (`.duckdb_extension`). The static extension (built into the test runner) has a TLS stub.

See [TLS_TESTING.md](TLS_TESTING.md) for detailed TLS testing instructions.

## Running Tests

### Build the Extension

```bash
make
```

### Run All Tests

```bash
make test
```

### Run Integration Tests Only

Integration tests require a running SQL Server instance:

```bash
export MSSQL_TEST_DSN="Server=localhost,1433;Database=master;User Id=sa;Password=TestPassword1"
make integration-test
```

Or run directly:

```bash
MSSQL_TEST_DSN="Server=localhost,1433;Database=master;User Id=sa;Password=TestPassword1" \
  ./build/release/test/unittest "[integration]" --force-reload
```

### Run Specific Test Groups

```bash
# Run query tests
./build/release/test/unittest "[query]" --force-reload

# Run TDS connection tests
./build/release/test/unittest "[tds_connection]" --force-reload

# Run a specific test file
./build/release/test/unittest "*basic_queries*" --force-reload
```

## Test Structure

### Integration Tests (`test/sql/integration/`)

Tests that require a running SQL Server:

| Test File | Description |
|-----------|-------------|
| `basic_queries.test` | Basic SELECT queries and pool statistics |
| `connection_pool.test` | Connection pool ATTACH/DETACH and statistics |
| `pool_limits.test` | Connection reuse and pool limit enforcement |
| `parallel_queries.test` | Multiple CTEs with GENERATE_SERIES |
| `large_data.test` | Large datasets and big row data (VARCHAR, VARBINARY) |
| `query_cancellation.test` | Query cancellation with LIMIT |
| `diagnostic_functions.test` | mssql_open, mssql_ping, mssql_close with connection strings |
| `tls_connection.test` | TLS connection tests (requires loadable extension) |
| `tls_queries.test` | Data type tests over TLS (requires loadable extension) |
| `tls_parallel.test` | Parallel query tests over TLS (requires loadable extension) |

**Note:** TLS tests (`tls_*.test`) are skipped by default. See [TLS_TESTING.md](TLS_TESTING.md) for TLS testing instructions.

### Query Tests (`test/sql/query/`)

Tests for query execution and type handling:

| Test File | Description |
|-----------|-------------|
| `basic_select.test` | Simple SELECT statements |
| `type_mapping.test` | SQL Server to DuckDB type conversions |
| `error_handling.test` | SQL errors and invalid queries |
| `info_messages.test` | INFO message handling |
| `cancellation.test` | Query cancellation infrastructure |

### TDS Connection Tests (`test/sql/tds_connection/`)

Low-level TDS protocol tests:

| Test File | Description |
|-----------|-------------|
| `open_close.test` | mssql_open/mssql_close functions |
| `ping.test` | Connection validation |
| `pool_stats.test` | Pool statistics function |
| `settings.test` | Extension settings |

### Unit Tests (`test/sql/`)

Tests that don't require SQL Server:

| Test File | Description |
|-----------|-------------|
| `mssql_version.test` | Extension version function |
| `mssql_attach.test` | ATTACH syntax validation |
| `mssql_execute.test` | mssql_execute function |
| `mssql_scan.test` | mssql_scan function |
| `mssql_secret.test` | Secret management |
| `tls_secret.test` | TLS secret creation (no actual TLS connection) |

### C++ Tests (`test/cpp/`)

| Test File | Description |
|-----------|-------------|
| `test_connection_pool.cpp` | Connection pool integration tests |

#### Running C++ Connection Pool Tests

The C++ tests require manual compilation:

```bash
# Compile the test
clang++ -std=c++17 -I src/include -I duckdb/src/include \
    test/cpp/test_connection_pool.cpp \
    src/tds/connection_pool.cpp \
    src/tds/tds_connection.cpp \
    src/tds/tds_protocol.cpp \
    src/tds/tds_types.cpp \
    src/tds/tds_socket.cpp \
    src/tds/tds_packet.cpp \
    src/encoding/utf16.cpp \
    -o build/debug/test_connection_pool \
    -pthread

# Run the test
MSSQL_TEST_HOST=localhost \
MSSQL_TEST_PORT=1433 \
MSSQL_TEST_USER=sa \
MSSQL_TEST_PASS=TestPassword1 \
MSSQL_TEST_DB=master \
./build/debug/test_connection_pool
```

The test verifies:
- Basic acquire/release from pool
- Connection reuse efficiency
- Pool limit enforcement
- Parallel connection acquisition
- Connection validation

## Environment Variables

| Variable | Description | Default |
|----------|-------------|---------|
| `MSSQL_TEST_DSN` | Connection string for integration tests | (required) |
| `MSSQL_TEST_DSN_TLS` | TLS connection string for TLS tests | (not exported) |
| `MSSQL_TEST_HOST` | SQL Server hostname | localhost |
| `MSSQL_TEST_PORT` | SQL Server port | 1433 |
| `MSSQL_TEST_USER` | SQL Server username | sa |
| `MSSQL_TEST_PASS` | SQL Server password | (required for C++ tests) |
| `MSSQL_TEST_DB` | Database name | master |

Connection parameters are defined in `.env`. See [TLS_TESTING.md](TLS_TESTING.md) for TLS testing.

## Test Categories

Tests are organized into groups using DuckDB's sqllogictest format:

- `[integration]` - Requires SQL Server connection
- `[query]` - Query execution tests
- `[tds_connection]` - TDS protocol tests

Use these groups with the `--force-reload` flag:

```bash
./build/release/test/unittest "[integration]" --force-reload
```

## Adding New Tests

1. Create a `.test` file in the appropriate directory
2. Use the sqllogictest format:

```sql
# name: test/sql/integration/my_test.test
# description: Description of the test
# group: [integration]

require mssql
require-env MSSQL_TEST_DSN

statement ok
ATTACH '${MSSQL_TEST_DSN}' AS testdb (TYPE mssql);

query I
SELECT * FROM mssql_scan('testdb', 'SELECT 1 AS val');
----
1

statement ok
DETACH testdb;
```

## Troubleshooting

### SQL Server not running

```bash
docker ps  # Check if container is running
docker logs mssql-dev  # Check container logs
```

### GENERATE_SERIES not found

Ensure SQL Server 2022+ is running. GENERATE_SERIES is not available in earlier versions.

### Connection timeout

Check firewall settings and ensure port 1433 is accessible:

```bash
nc -zv localhost 1433
```

### Test hangs

Kill running tests and check SQL Server logs:

```bash
pkill -f unittest
docker logs mssql-dev --tail 50
```
