# Testing TLS Connections

This guide explains how to test TLS (encrypted) connections with the MSSQL extension.

## TLS Architecture

Both static and loadable extensions have **full TLS support** via OpenSSL (statically linked with symbol visibility control).

| Build Type | TLS Support | Use Case |
|------------|-------------|----------|
| Static extension | Full TLS (OpenSSL) | Built-in test runner, embedded DuckDB |
| Loadable extension | Full TLS (OpenSSL) | Standalone DuckDB CLI |

Symbol visibility is controlled using version scripts (Linux) or exported_symbols_list (macOS) to prevent runtime conflicts with other OpenSSL libraries.

## Prerequisites

1. **SQL Server** running with TLS enabled (SQL Server 2022 has TLS by default)
2. **Docker** for the test SQL Server container

### Start SQL Server

```bash
make docker-up
```

Connection parameters are defined in `.env`:

```bash
# .env file contents
MSSQL_TEST_HOST=localhost
MSSQL_TEST_PORT=1433
MSSQL_TEST_USER=sa
MSSQL_TEST_PASS=TestPassword1
MSSQL_TEST_DB=master
```

## Step 1: Check DuckDB Version

The loadable extension must match the DuckDB version. Check the submodule version:

```bash
cd duckdb && git describe --tags && cd ..
```

Example output: `v1.4.3`

## Step 2: Install Matching DuckDB

Download from [DuckDB Releases](https://github.com/duckdb/duckdb/releases):

### macOS (Apple Silicon / Intel)

```bash
# Get version from submodule
DUCKDB_VERSION=$(cd duckdb && git describe --tags)

# Download universal binary
curl -L -o duckdb.zip "https://github.com/duckdb/duckdb/releases/download/${DUCKDB_VERSION}/duckdb_cli-osx-universal.zip"
unzip duckdb.zip
mkdir -p ~/.local/bin
mv duckdb ~/.local/bin/
rm duckdb.zip

# Verify
~/.local/bin/duckdb --version
```

### Linux (x86_64)

```bash
DUCKDB_VERSION=$(cd duckdb && git describe --tags)

curl -L -o duckdb.zip "https://github.com/duckdb/duckdb/releases/download/${DUCKDB_VERSION}/duckdb_cli-linux-amd64.zip"
unzip duckdb.zip
mkdir -p ~/.local/bin
mv duckdb ~/.local/bin/
rm duckdb.zip

~/.local/bin/duckdb --version
```

### Linux (ARM64)

```bash
DUCKDB_VERSION=$(cd duckdb && git describe --tags)

curl -L -o duckdb.zip "https://github.com/duckdb/duckdb/releases/download/${DUCKDB_VERSION}/duckdb_cli-linux-aarch64.zip"
unzip duckdb.zip
mkdir -p ~/.local/bin
mv duckdb ~/.local/bin/
rm duckdb.zip

~/.local/bin/duckdb --version
```

## Step 3: Build the Extension

```bash
make release
```

The loadable extension is created at:
```
build/release/extension/mssql/mssql.duckdb_extension
```

## Step 4: Verify TLS Connection

Load connection parameters from `.env` and test:

```bash
# Load environment
source .env

# Start DuckDB with unsigned extension support
~/.local/bin/duckdb -unsigned
```

In DuckDB SQL:

```sql
-- Load the extension
LOAD 'build/release/extension/mssql/mssql.duckdb_extension';

-- Connect with TLS (encrypt=true)
ATTACH 'mssql://sa:TestPassword1@localhost:1433/master?encrypt=true' AS db (TYPE mssql);

-- Test query
SELECT * FROM mssql_scan('db', 'SELECT 1 AS test_value');

-- Verify connection pool
SELECT * FROM mssql_pool_stats();

-- Cleanup
DETACH db;
```

## Step 5: Run TLS Tests

### Quick Test Script

Create and run a test script that uses `.env` parameters:

```bash
#!/bin/bash
# test_tls.sh

source .env

~/.local/bin/duckdb -unsigned -c "
LOAD 'build/release/extension/mssql/mssql.duckdb_extension';

-- TLS connection
ATTACH 'mssql://${MSSQL_TEST_USER}:${MSSQL_TEST_PASS}@${MSSQL_TEST_HOST}:${MSSQL_TEST_PORT}/${MSSQL_TEST_DB}?encrypt=true' AS tls_db (TYPE mssql);

-- Basic tests
SELECT * FROM mssql_scan('tls_db', 'SELECT 1 AS test_value');
SELECT * FROM mssql_scan('tls_db', 'SELECT 42 AS answer, 123 AS number');
SELECT * FROM mssql_scan('tls_db', 'SELECT ''Hello TLS'' AS greeting');

-- Pool stats
SELECT * FROM mssql_pool_stats();

DETACH tls_db;
SELECT 'All TLS tests passed!' AS result;
"
```

Run:
```bash
chmod +x test_tls.sh
./test_tls.sh
```

### Full TLS Test Suite

The following test files in `test/sql/integration/` test TLS functionality:

| Test File | Description |
|-----------|-------------|
| `tls_connection.test` | Basic TLS connection, queries, pool stats |
| `tls_queries.test` | Various data types over TLS |
| `tls_parallel.test` | Parallel queries, UNION ALL, JOINs over TLS |

These tests are **skipped by default** in `make integration-test` because the test runner uses the static extension.

## Verifying TLS is Active

### Method 1: Connection String

If the connection succeeds with `?encrypt=true`, TLS is working:

```sql
ATTACH 'mssql://sa:TestPassword1@localhost:1433/master?encrypt=true' AS db (TYPE mssql);
-- If this succeeds, TLS handshake completed
```

### Method 2: Pool Statistics

Check the pool stats for connection info:

```sql
SELECT * FROM mssql_pool_stats();
```

### Method 3: SQL Server Logs

Check SQL Server for encrypted connections:

```bash
docker logs mssql-dev 2>&1 | grep -i encrypt
```

## Troubleshooting

### "TLS not available in static build"

**Cause:** Using the static extension (test runner or embedded DuckDB)

**Solution:** Use the loadable extension with standalone DuckDB:
```bash
~/.local/bin/duckdb -unsigned
LOAD 'build/release/extension/mssql/mssql.duckdb_extension';
```

### Extension version mismatch

**Error:** Extension fails to load or crashes

**Solution:** Ensure DuckDB version matches:
```bash
# Check extension was built for
cd duckdb && git describe --tags

# Check installed DuckDB
~/.local/bin/duckdb --version
```

### "unsigned extension" error

**Cause:** DuckDB won't load unsigned extensions by default

**Solution:** Use the `-unsigned` flag:
```bash
~/.local/bin/duckdb -unsigned
```

### Connection timeout

**Cause:** SQL Server not running or port blocked

**Solution:**
```bash
# Check container status
make docker-status

# Check port
nc -zv localhost 1433

# Check container logs
docker logs mssql-dev --tail 50
```

### Certificate verification failed

**Cause:** SQL Server uses self-signed certificate

**Note:** The extension accepts self-signed certificates by default (trust_server_certificate is implicit with encrypt=true).

## Connection String Options

TLS is enabled via the `encrypt` query parameter:

```
mssql://user:password@host:port/database?encrypt=true
```

| Parameter | Description | Default |
|-----------|-------------|---------|
| `encrypt` | Enable TLS encryption | `false` |

Example using `.env` variables:
```bash
source .env
CONN="mssql://${MSSQL_TEST_USER}:${MSSQL_TEST_PASS}@${MSSQL_TEST_HOST}:${MSSQL_TEST_PORT}/${MSSQL_TEST_DB}?encrypt=true"
```

## See Also

- [README.md](README.md) - General testing guide
- [.env](../.env) - Connection parameters
- [specs/005-tls-connection-support/](../specs/005-tls-connection-support/) - TLS implementation spec
