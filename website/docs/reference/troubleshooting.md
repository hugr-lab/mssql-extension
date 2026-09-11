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

### Certificate Verification Failed

```text
TLS handshake failed: certificate verification failed for localhost: self-signed certificate. Set TrustServerCertificate=yes to accept this server's certificate without verification, or HostNameInCertificate=<name> if the certificate is valid but issued for a different name
```

The server's certificate could not be verified (spec 074): the chain does not
lead to a root in the platform trust store (`self-signed certificate`, `unable
to get local issuer certificate`), or its subject is not the host you connected
to (`hostname mismatch`).

**Solutions:**

- A self-signed certificate you know (docker, an on-prem instance with none
  installed): add `TrustServerCertificate=yes` / `trust_server_certificate true`
- The certificate is valid but you connect through a tunnel, an IP or an alias:
  add `HostNameInCertificate=<the name in the certificate>`
- `unable to get local issuer certificate` on a minimal Linux image: install
  `ca-certificates`, or point `SSL_CERT_FILE` at a bundle; a private CA goes
  into the platform store or `SSL_CERT_FILE`

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

- Check the [Type Mapping](../reading/types.md) section for supported types
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
- **Keytab / raw Kerberos credentials on macOS**: macOS's `GSS.framework` lacks MIT extensions for `gss_acquire_cred_from` keytab and raw-password paths. macOS supports credential-cache mode only (via `kinit`); keytab and raw-credentials modes are Linux-only. See [Kerberos.md](../connection/kerberos.md) for the WSL2 / Docker testing path.
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

1. **Catalog queries only**: This conversion applies only to catalog-based queries (three-part naming like `db.schema.table`). When using `mssql_scan()` with raw SQL, you must add the CAST yourself — see [Raw `mssql_scan()` does not transcode code pages](#raw-mssql_scan-does-not-transcode-code-pages) below for what happens if you do not.

```sql
-- No CAST: the raw code-page bytes are returned as-is. For a non-UTF8
-- collation that is NOT valid UTF-8, and string functions will mangle it.
FROM mssql_scan('db', 'SELECT name FROM dbo.customers');

-- With CAST: SQL Server transcodes to UTF-16 and the extension decodes it.
FROM mssql_scan('db', 'SELECT CAST(name AS NVARCHAR(MAX)) AS name FROM dbo.customers');
```

### Raw `mssql_scan()` does not transcode code pages

**Symptom.** A string read through `mssql_scan()` looks right in the output but
behaves as if it were truncated. The classic shape is a string function
returning less than it was given:

```sql
SELECT upper(name) FROM mssql_scan('db', 'SELECT name FROM dbo.t');
-- 'naïve' comes back as 'NA'
```

**Cause.** SQL Server's `CHAR` / `VARCHAR` / `TEXT` carry bytes in the
**column's own code page** — `Latin1_General_CI_AS` is CP1252, `Cyrillic_General_CI_AS`
is CP1251, and so on. DuckDB `VARCHAR` is UTF-8 by contract. The extension does
not transcode legacy code pages: it hands those bytes to DuckDB unchanged, and
for anything outside ASCII the result is not valid UTF-8. It is not rejected —
it is stored, displayed, and then mangled by whatever touches it. `ï` in CP1252
is the single byte `0xEF`, which UTF-8 reads as the start of a three-byte
sequence, so `upper()` consumes the two bytes after it and returns `NA`.

The transcoding SQL Server *will* do for you is a `CAST` to a Unicode type, and
that is what the fix is.

**Fix — one of:**

```sql
-- 1. CAST in the query. NVARCHAR(MAX), not NVARCHAR(n): SQL Server does not
--    raise on a narrowing character CAST, so a fixed width silently truncates.
SELECT * FROM mssql_scan('db',
    'SELECT id, CAST(name AS NVARCHAR(MAX)) AS name FROM dbo.customers');

-- 2. Read through the attached catalog instead, which adds that CAST for you.
SELECT id, name FROM db.dbo.customers;
```

**When this cannot happen:**

- **Catalog scans** (three-part names, and therefore `COPY`/`CREATE TABLE AS`
  reading from them) — the generated `SELECT` casts every non-Unicode string
  column server-side. The one exception is a declared `VARCHAR(MAX)` when
  `mssql_convert_varchar_max = false`, which opts out of that cast on purpose.
- **`NCHAR` / `NVARCHAR` / `NTEXT` / `XML`** — always UTF-16 on the wire,
  always decoded.
- **UTF-8 collations** (`..._UTF8`, SQL Server 2019 and later). Those columns
  are already UTF-8, so the bytes are correct with or without a cast. Note that
  the *default* collation of a SQL Server installation is not one of these —
  `SQL_Latin1_General_CP1_CI_AS` is CP1252 — so a `varchar` column is in a
  legacy code page unless somebody chose otherwise.
- **ASCII-only data**, in any code page: every byte is already valid UTF-8.

### Why this is not validated for you

Tracked as [issue #224](https://github.com/hugr-lab/mssql-extension/issues/224),
and deliberately answered with documentation rather than a runtime check.

The obvious fix is to validate every value as it is decoded and raise on the
first one that is not UTF-8. It was considered and rejected, because of where
that check would have to live.

**The string codec is built to avoid exactly that call.** Its ASCII fast path
exists for one reason, recorded next to it in `codec/string_codec.hpp`: measured
on 12-byte values, *a simdutf call costs ~3.1 ns of pure overhead* against *~3 ns
of actual byte work* — the call is as expensive as the transcoding it performs.
Adding validation to the read path puts one back, per value, and as a **second
pass over bytes the decoder has already copied**. That is a real cost on every
scan, paid in full by the UTF-8 and `NVARCHAR` columns that cannot be affected by
this in the first place.

**And it would be paid on the wrong path.** The catalog scan — three-part names,
and the `COPY` / `CREATE TABLE AS` built on them — is the path that carries
volume, and it is already safe here: it casts non-Unicode string columns
server-side, so the bytes it decodes are Unicode by construction. `mssql_scan()`
is the escape hatch for hand-written T-SQL. Slowing the main path to guard the
escape hatch is the wrong trade.

**What the escape hatch gets instead is this page.** Someone writing raw T-SQL is
already choosing the exact columns and can add `CAST(col AS NVARCHAR(MAX))`; what
they were missing was any statement that they had to. That is what changed.


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
- Raw `mssql_scan()` returns non-Unicode `CHAR`/`VARCHAR`/`TEXT` bytes in the column's code page without transcoding them, so a non-ASCII value is not valid UTF-8 and string functions mangle it silently — add `CAST(col AS NVARCHAR(MAX))`, or read through the catalog. See [Raw `mssql_scan()` does not transcode code pages](#raw-mssql_scan-does-not-transcode-code-pages)
- XML columns in INSERT/UPDATE are limited to 4096 bytes per value — use COPY TO with BCP protocol for larger documents
- Very large DECIMAL values may lose precision at extreme scales
- Connection pool statistics reset when all connections close
- Microsoft Fabric Warehouse has no `nvarchar` type at all, and no `datetimeoffset` — so a `TIMESTAMP WITH TIME ZONE` column cannot be created there. `varchar(max)` is supported. See [AZURE.md](../connection/azure.md#microsoft-fabric)
- A `#temp` table cannot be a BCP target on Microsoft Fabric: the load fails inside Fabric with an I/O error against a parquet file. Load into a permanent table

