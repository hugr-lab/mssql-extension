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

### DuckDB UI: "Expected field id 65535 but got 103"

```text
Error: Expected field id 65535 but got 103 (offset=50)
```

Raised by the DuckDB UI extension (`CALL start_ui_server()`) — and only
there — when a result contains a column read from an attached table whose
type is `MSSQL_VARCHAR(n)` / `MSSQL_NVARCHAR(n)`. The UI serializes the
result's column types for the browser, and its reader does not know the
`extension_info` block (field 103) that DuckDB writes for an extension type
with modifiers; the CLI, Python and JDBC clients never serialize result
types, so the same query runs there. Tracked as
[issue #297](https://github.com/hugr-lab/mssql-extension/issues/297).

**Solutions:**

- Report plain `VARCHAR` for attached string columns, then drop the entries
  the catalog has already loaded with the native type:

  ```sql
  SET mssql_catalog_native_types = false;
  SELECT mssql_invalidate_cache('mydb');
  ```

  Or for one catalog only, at ATTACH:
  `ATTACH '…' AS mydb (TYPE mssql, native_types false)`.

  The cost is what the setting's name says: `DESCRIBE` shows `VARCHAR`
  instead of the declared length and collation, and a CTAS or COPY back to
  SQL Server no longer inherits them
  ([Catalog-Reported String Types](../reading/types.md#catalog-reported-string-types)).
- Per query, `CAST(col AS VARCHAR)` yields a plain `VARCHAR` as well.

### Slow Query Performance

**Solutions:**

- Verify filter pushdown is working (check query plan)
- Reduce result set size with LIMIT or WHERE clauses
- Increase connection pool size for concurrent queries
- Check network latency to SQL Server
- Consider using `mssql_scan()` for complex queries with explicit SQL


## Limitations

### Unsupported Features

- **UPDATE / DELETE … RETURNING on Fabric Warehouse and Azure Synapse**: refused by name; both lack the `OUTPUT … INTO` the rows are read back through. On SQL Server and Azure SQL they work. See [DML](../writing/dml.md).
- **UPDATE / DELETE on a table with no usable key**, when the statement cannot go to the server whole: such a table is keyed by every column, which needs the server to select the same rows as DuckDB, and is refused by name when it cannot (a WHERE that runs in DuckDB, a volatile function, `UPDATE … FROM`). A primary key or a unique index lifts it. See [DML](../writing/dml.md).
- **A SET of a key column** goes only when the UPDATE goes to the server whole (`mssql_dml_pushdown`); the row paths select rows by that key.
- **MERGE INTO an MSSQL table runs on the server or is refused**: more WHEN clauses than T-SQL takes, `ERROR`, an INSERT naming the identity column, `RETURNING`, a subquery, an expression over the target T-SQL cannot write (a string function, unary minus), and any MERGE with DuckDB's optimizer off are refused by name. `SET x = DEFAULT` writes NULL. A source value compared with a temporal column of the target is rounded to that column's precision first, as the server compares. See [MERGE INTO](../writing/dml.md#merge-into).
- **UPDATE / DELETE / MERGE on Azure Synapse**: refused; its keys are NOT ENFORCED and its MERGE is in preview.
- **Keytab / raw Kerberos credentials on macOS**: macOS's `GSS.framework` lacks MIT extensions for `gss_acquire_cred_from` keytab and raw-password paths. macOS supports credential-cache mode only (via `kinit`); keytab and raw-credentials modes are Linux-only. See [Kerberos.md](../connection/kerberos.md) for the WSL2 / Docker testing path.
- **Multiple result sets**: Only one result-producing statement per `mssql_scan()` batch is allowed
- **Stored Procedures with Output Parameters**: Use `mssql_scan()` for stored procedures
- **rowid for views and tables without a usable key**: `rowid` is the primary key, else a usable unique index (not filtered, not disabled, NOT NULL columns that a literal matches); a view or a table with neither has no `rowid`

### VARCHAR Encoding

A `CHAR` or `VARCHAR` column under a code-page collation (e.g. `Latin1_General_CI_AS`) holds bytes in that code page, while DuckDB `VARCHAR` is UTF-8. How it reaches DuckDB depends on the page:

| Column, read through the catalog | Sent as | Converted by |
|---|---|---|
| `CHAR`, `VARCHAR`, `VARCHAR(MAX)` on a single-byte page (1250-1258, 874, 437, 850) | itself: one byte a character | the client |
| The same on a double-byte page (932, 936, 949, 950) | `NVARCHAR(n)`, `NCHAR(n)`, or `NVARCHAR(MAX)` past 4000 | the server |
| `TEXT`, any page | `NVARCHAR(MAX)` | the server |
| A collation with a UTF-8 code page (`..._UTF8`) | itself | nothing: it is UTF-8 |

The client reads a byte exactly as the server's `CAST` would: the tables come from a SQL Server, and include the bytes a code page leaves undefined. A `CHAR(n)` arrives without its padding either way.

**Settings:**

| Setting | Type | Default | Description |
|---------|------|---------|-------------|
| `mssql_client_transcode` | BOOLEAN | true | Read code-page text as bytes and convert it on the client; read at ATTACH. `false` makes the server convert every column, and leaves a raw `mssql_scan` with the column's bytes (below) |
| `mssql_convert_varchar_max` | BOOLEAN | true | Convert a declared `VARCHAR(MAX)` on the server, where the server converts. `false` on a double-byte page drops that cast, and the client converts the column. No effect on a single-byte page, read as itself either way |

`ATTACH ... (TYPE mssql, client_transcode false)` sets it for one catalog.

### When code-page bytes reach you unconverted

A raw `mssql_scan()` returns correct UTF-8 for every code page, and a catalog read for every column. Two cases hand a `CHAR` / `VARCHAR` / `TEXT` column's bytes over as they are:

- **`client_transcode` is off** for the catalog (`mssql_client_transcode = false`, or the ATTACH option), on a raw `mssql_scan()`, or on a catalog read of a declared `VARCHAR(MAX)` with `mssql_convert_varchar_max = false`;
- **the collation is newer than this build's tables**, on a raw `mssql_scan()`.

**Symptom.** A string looks right in the output but behaves as if it were truncated:

```sql
SELECT upper(name) FROM mssql_scan('db', 'SELECT name FROM dbo.t');
-- 'naïve' comes back as 'NA'
```

**Cause.** `ï` in CP1252 is the single byte `0xEF`, which UTF-8 reads as the start of a three-byte sequence, so `upper()` consumes the two bytes after it. DuckDB `VARCHAR` is UTF-8 by contract, and these values are not validated on the way in: that would cost every scan a second pass over bytes it has already copied.

Both cases log one `WARNING` per column per query into `duckdb_logs` (`mssql_warn_non_utf8_collation` turns it off).

**Fix.** Attach with `client_transcode true`, the default. Or `CAST` in the query, to `NVARCHAR(MAX)` and not `NVARCHAR(n)`: SQL Server does not raise on a narrowing character `CAST`, so a fixed width silently truncates:

```sql
SELECT * FROM mssql_scan('db',
    'SELECT id, CAST(name AS NVARCHAR(MAX)) AS name FROM dbo.customers');
```

**To get the stored bytes on purpose,** `CAST(name AS VARBINARY(MAX))` in the T-SQL, on any setting.

### A warning that a column "is in the catalog under collation"

The column's collation changed on the server after the catalog loaded it. The values are read correctly, by the collation the server sent, but `DESCRIBE` shows the old one. Run `mssql_invalidate_cache()`. For a view, `sp_refreshview` first: its `sys.columns` row stays old until then.

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
- **A `SQL_VARIANT` column loses data when read, without a warning** ([#354](https://github.com/hugr-lab/mssql-extension/issues/354)). The auto-CAST to `NVARCHAR(MAX)` uses conversion style 0: a `datetime2` / `datetime` / `date` / `time` value loses its seconds and fractions (`Sep 16 2026 10:11AM`), and a `varbinary` value arrives as its bytes read as UTF-16. No single style serves every base type a variant column can hold. Read such a column through `mssql_scan()` with a conversion per base type:

  ```sql
  SELECT * FROM mssql_scan('db', 'SELECT id,
      CASE WHEN CAST(SQL_VARIANT_PROPERTY(v, ''BaseType'') AS sysname)
                IN (''datetime2'', ''datetime'', ''smalldatetime'', ''date'', ''time'', ''datetimeoffset'')
           THEN CONVERT(nvarchar(max), v, 126)
           WHEN CAST(SQL_VARIANT_PROPERTY(v, ''BaseType'') AS sysname) IN (''binary'', ''varbinary'')
           THEN CONVERT(nvarchar(max), CAST(v AS varbinary(max)), 1)
           ELSE CAST(v AS nvarchar(max)) END AS v
    FROM dbo.t');
  ```

  It returns `2026-09-16T10:11:12.1234567`, `10:11:12`, `0x0102`, `2026-09-16` where the auto-CAST lost them. Reading the column as DuckDB's own `VARIANT` is planned after 0.3.0.
- **A serverless Azure SQL database that is resuming** answers a login with error 40613 ("not available yet ... retry shortly") until it is up, typically 20 to 60 s. The extension tries the login again, as it does for 40197, 40501 and 49918 ([#481](https://github.com/hugr-lab/mssql-extension/issues/481)): the pause starts at 1 s and doubles to 10 s, within `mssql_connection_timeout` (for the ATTACH, `mssql_attach_validation_timeout` when set), each attempt an INFO line in `duckdb_logs`, and Ctrl-C ends it. An extra parallel bulk-load writer makes one attempt; the connections `min_connections` opens at ATTACH retry but cannot be interrupted. The 30 s default can be short for a resume: raise the timeout (`SET mssql_connection_timeout = 90`) if the ATTACH still fails with `tried N time(s)`.
- Under `client_transcode false`, raw `mssql_scan()` returns non-Unicode `CHAR`/`VARCHAR`/`TEXT` bytes in the column's code page without transcoding them; see [When code-page bytes reach you unconverted](#when-code-page-bytes-reach-you-unconverted)
- The DuckDB UI cannot render a result with an `MSSQL_VARCHAR(n)` / `MSSQL_NVARCHAR(n)` column (`Expected field id 65535 but got 103`) — set `mssql_catalog_native_types = false` and invalidate the cache. See [DuckDB UI](#duckdb-ui-expected-field-id-65535-but-got-103)
- XML columns in INSERT/UPDATE are limited to 4096 bytes per value — use COPY TO with BCP protocol for larger documents
- Very large DECIMAL values may lose precision at extreme scales
- Connection pool statistics reset when all connections close
- Microsoft Fabric Warehouse has no `nvarchar` type at all, and no `datetimeoffset` — so a `TIMESTAMP WITH TIME ZONE` column cannot be created there. `varchar(max)` is supported. See [AZURE.md](../connection/azure.md#microsoft-fabric)
- A `#temp` table cannot be a BCP target on Microsoft Fabric: the load fails inside Fabric with an I/O error against a parquet file. Load into a permanent table

