---
title: Troubleshooting & Limitations
sidebar_position: 3
---

# Troubleshooting

### Connection Refused

```text
Error: Failed to connect to SQL Server: Connection refused
```

**Solutions:**

- Verify SQL Server hostname and port are correct
- Check firewall allows TCP connections on port 1433
- Ensure SQL Server is configured for TCP/IP connections (SQL Server Configuration Manager)
- Test connectivity: `telnet hostname 1433`

### Login Failed

```text
Error: Login failed for user 'username'
```

**Solutions:**

- Verify username and password are correct
- Ensure SQL Server authentication mode is enabled (not Windows-only)
- Check user has access to the specified database
- Verify user account is not locked or disabled

### TLS Required

```text
Error: Server requires encryption but TLS is not available
```

**Solutions:**

- Enable encryption in connection: `use_encrypt true` or `Encrypt=yes`
- Ensure extension was built with OpenSSL (default for vcpkg builds)

### TLS Handshake Failed

```text
Error: TLS handshake failed
```

**Solutions:**

- Verify server certificate is valid
- Check TLS version compatibility (TLS 1.2+ required)
- Set `MSSQL_DEBUG=1` for detailed TLS debugging output
- Verify server hostname matches certificate

### Type Conversion Error

```text
Error: Unsupported SQL Server type 'UDT' (0xF0) for column 'col_name'
```

**Solutions:**

- Check the [Type Mapping](/reading/types/) section for supported types
- Cast unsupported columns to supported types in your query
- Exclude unsupported columns from SELECT

### Slow Query Performance

**Solutions:**

- Verify filter pushdown is working (check query plan)
- Reduce result set size with LIMIT or WHERE clauses
- Increase connection pool size for concurrent queries
- Check network latency to SQL Server
- Consider using `mssql_scan()` for complex queries with explicit SQL


## Limitations

### Unsupported Features

- **RETURNING for UPDATE/DELETE**: Only INSERT supports RETURNING clause; UPDATE/DELETE do not
- **UPDATE/DELETE without PK**: Tables must have primary keys for UPDATE/DELETE operations
- **Updating primary key columns**: UPDATE cannot modify primary key columns (used for row identification)
- **Keytab / raw Kerberos credentials on macOS**: macOS's `GSS.framework` lacks MIT extensions for `gss_acquire_cred_from` keytab and raw-password paths. macOS supports credential-cache mode only (via `kinit`); keytab and raw-credentials modes are Linux-only. See [Kerberos.md](/connection/kerberos/) for the WSL2 / Docker testing path.
- **Multiple result sets**: Only one result-producing statement per `mssql_scan()` batch is allowed
- **Stored Procedures with Output Parameters**: Use `mssql_scan()` for stored procedures
- **rowid for views/tables without PK**: Only tables with primary keys expose `rowid`

### VARCHAR Encoding

VARCHAR and CHAR columns with non-UTF8 collations (e.g., `Latin1_General_CI_AS`) are automatically converted to NVARCHAR in generated queries to ensure proper UTF-8 decoding in DuckDB. This allows extended ASCII characters (é, ñ, ü, etc.) to be correctly returned.

**Behavior:**

| VARCHAR Length | Converted To | Notes |
|----------------|--------------|-------|
| VARCHAR(n) where n ≤ 4000 | NVARCHAR(n) | Full data preserved |
| VARCHAR(n) where n > 4000 | NVARCHAR(MAX) | Full data preserved. NVARCHAR has no inline length above 4000 characters, so these columns are sent as PLP. |
| CHAR(n) | same as VARCHAR(n) | Full data preserved (blank-padded to `n` by SQL Server) |
| TEXT | NVARCHAR(MAX) | Full data preserved. Always converted — TEXT has no decodable unconverted wire form, so `mssql_convert_varchar_max` does not apply to it. |
| VARCHAR(MAX) | NVARCHAR(MAX) | Converted by default; disable with `mssql_convert_varchar_max = false` |

`mssql_convert_varchar_max` governs *declared* `VARCHAR(MAX)` columns only. A `VARCHAR(4001..8000)` is converted to `NVARCHAR(MAX)` regardless of the setting, because no shorter NVARCHAR can hold it.

> **Note**: Previously, `VARCHAR(n > 4000)` was converted to `NVARCHAR(4000)` and `TEXT` to `NVARCHAR(16)`, both of which silently truncated the value on read. Both now convert to `NVARCHAR(MAX)` and return the full value.

**VARCHAR Encoding Setting:**

| Setting | Type | Default | Description |
|---------|------|---------|-------------|
| `mssql_convert_varchar_max` | BOOLEAN | true | Convert VARCHAR(MAX) to NVARCHAR(MAX) in catalog queries |


To preserve the full buffer capacity at the cost of potential encoding errors with extended ASCII:

```sql
SET mssql_convert_varchar_max = false;
```

**Notes:**

1. **Catalog queries only**: This conversion applies only to catalog-based queries (three-part naming like `db.schema.table`). When using `mssql_scan()` with raw SQL, you must manually add CAST expressions:

```sql
-- Without CAST: may fail with UTF-8 validation error for extended ASCII
FROM mssql_scan('db', 'SELECT name FROM dbo.customers');

-- With CAST: properly handles extended ASCII characters
FROM mssql_scan('db', 'SELECT CAST(name AS NVARCHAR(100)) AS name FROM dbo.customers');
```


### Unicode Transcoding (simdutf)

DuckDB strings are UTF-8 internally; the TDS wire protocol carries character data as UTF-16LE. Every NVARCHAR / NCHAR / NTEXT / XML value the extension reads from SQL Server is decoded from UTF-16LE to UTF-8 before being handed to DuckDB, and every string the extension sends back (LOGIN7 fields, INSERT/UPDATE parameters, T-SQL identifiers in BCP metadata, etc.) is encoded the other direction. On a SELECT that returns millions of rows, that's a lot of bytes — and the codec sits squarely in the hot path.

The extension uses [**simdutf**](https://github.com/simdutf/simdutf) for that conversion. simdutf is a SIMD-accelerated Unicode validation and transcoding library by Daniel Lemire and contributors; on modern x86_64 (AVX-512 / AVX2 / SSE) and ARM64 (NEON / SVE) cores it typically transcodes UTF-8 ↔ UTF-16 at 2–5 GB/s, an order of magnitude faster than the hand-rolled byte-at-a-time loops it replaced. The library is dual-licensed MIT / Apache-2.0; the extension links it statically under the MIT terms.

**Where simdutf is called** (`src/tds/encoding/utf16.cpp`):

| Direction | simdutf entry points | Used by |
|-----------|----------------------|---------|
| UTF-8 → UTF-16LE (encode) | `simdutf::validate_utf8`, `simdutf::utf16_length_from_utf8`, `simdutf::convert_valid_utf8_to_utf16le` | LOGIN7 client/host/app/server/library/language/database/SSPI fields; T-SQL batches built by INSERT/UPDATE/DELETE; BCP column metadata; identifier quoting |
| UTF-16LE → UTF-8 (decode) | `simdutf::validate_utf16le`, `simdutf::utf8_length_from_utf16le`, `simdutf::convert_valid_utf16le_to_utf8` | NVARCHAR / NCHAR / NTEXT / SQL_VARIANT-string / XML column results; ENVCHANGE token payloads; ERROR/INFO token messages |

The codec validates the input first and falls back to a slower scalar implementation only when input is malformed — so well-formed UTF-8/UTF-16 (the overwhelming majority of real traffic) hits the SIMD fast path every time. There is a microbenchmark (`make bench-utf16`) and an end-to-end before/after benchmark (`test/bench/bench_codec_e2e.sh`) recorded in `bench_results.md`.

**Dependency surface:** simdutf is pulled in statically via `vcpkg.json` (`"simdutf"` dependency, version resolved from the `builtin-baseline` pin). Release binaries embed it; downstream builds that don't use vcpkg need to provide `simdutfConfig.cmake` some other way (e.g., the `test/kerberos/test-client` Docker image builds it from upstream at a pinned tag).

### Known Issues

- Catalog scans auto-CAST server-specific types (`SQL_VARIANT`, `hierarchyid`, CLR UDTs) to `NVARCHAR(MAX)`, so three-part-name queries succeed; raw `mssql_scan()` needs an explicit CAST for such columns
- XML columns in INSERT/UPDATE are limited to 4096 bytes per value — use COPY TO with BCP protocol for larger documents
- Very large DECIMAL values may lose precision at extreme scales
- Connection pool statistics reset when all connections close
- Microsoft Fabric Warehouse has no `nvarchar` type at all, and no `datetimeoffset` — so a `TIMESTAMP WITH TIME ZONE` column cannot be created there. `varchar(max)` is supported. See [AZURE.md](/connection/azure/#microsoft-fabric)
- A `#temp` table cannot be a BCP target on Microsoft Fabric: the load fails inside Fabric with an I/O error against a parquet file. Load into a permanent table

