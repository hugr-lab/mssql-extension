---
title: Overview
sidebar_position: 1
slug: /
---

# DuckDB MSSQL Extension

[![GitHub stars](https://img.shields.io/github/stars/hugr-lab/mssql-extension?style=social)](https://github.com/hugr-lab/mssql-extension)
[![GitHub release](https://img.shields.io/github/v/release/hugr-lab/mssql-extension)](https://github.com/hugr-lab/mssql-extension/releases)
[![DuckDB Community Extension](https://img.shields.io/badge/DuckDB-community%20extension-fff100)](https://duckdb.org/community_extensions/extensions/mssql.html)
[![License](https://img.shields.io/github/license/hugr-lab/mssql-extension)](https://github.com/hugr-lab/mssql-extension/blob/main/LICENSE)


Connect DuckDB to Microsoft SQL Server, Azure SQL and Microsoft Fabric over a
**native TDS 7.4 implementation** — no ODBC, no JDBC, no FreeTDS. Attach a
database and it becomes a DuckDB catalog: stream reads with filter and ORDER BY
pushdown, bulk-load writes over the BCP protocol at millions of rows per
second, full DML and transactions.

```sql
INSTALL mssql FROM community;
LOAD mssql;

ATTACH 'Server=localhost;Database=AdventureWorks;User Id=sa;Password=...' AS mssql (TYPE mssql);
SELECT * FROM mssql.dbo.SalesOrderHeader LIMIT 10;
```

→ **[Getting Started](./getting-started.md)** · **[Settings Reference](./reference/settings.md)** · **[Download metrics](https://hugr-lab.github.io/mssql-extension/metrics/)**

## Community Extension Downloads

[![Weekly downloads of the mssql DuckDB community extension](https://hugr-lab.github.io/mssql-extension/download-metrics.svg)](https://duckdb.org/community_extensions/download_metrics)

📊 **[Interactive chart](https://hugr-lab.github.io/mssql-extension/metrics/)** — queried live in your browser with DuckDB-Wasm. Regenerated weekly; counts are a Cloudflare estimate of `INSTALL mssql FROM community` events across DuckDB versions and platforms.

## Features

- Native TDS protocol implementation (no external dependencies)
- Stream query results directly into DuckDB without buffering
- Full DuckDB catalog integration with three-part naming and lazy metadata loading
- Row identity (`rowid`) support for tables with primary keys
- Connection pooling with configurable limits and automatic session reset
- TLS/SSL encrypted connections
- Full DML support: INSERT (with RETURNING), UPDATE, DELETE
- CREATE TABLE AS SELECT (CTAS) with streaming and type mapping
- High-performance COPY TO via TDS BulkLoadBCP protocol
- Transaction support: BEGIN/COMMIT/ROLLBACK with connection pinning
- Multi-statement SQL batches via `mssql_scan()` (e.g., temp table workflows)
- DuckDB secret management for secure credential storage
- [Azure AD authentication](/connection/azure/) (service principal, CLI, interactive device code flow)
- [Kerberos / Windows SSPI integrated authentication](/connection/kerberos/) — POSIX (`kinit` / keytab / raw credentials) and Windows (current logon session via `secur32.dll`)
- Custom `Application Name` propagated to SQL Server `APP_NAME()` / `sys.dm_exec_sessions.program_name`
- ATTACH-time credential validation (opt-out via `lazy_validation true`)
- **Experimental**: ORDER BY pushdown to SQL Server (opt-in via `mssql_order_pushdown` setting)


## Platform Support

| Platform | Status | Notes |
|----------|--------|-------|
| macOS ARM64 | Primary development | Active development and testing |
| Linux x86_64 | CI-validated | Automated builds and tests in CI |
| Linux ARM64 | CI-validated | Automated builds and tests in CI |
| Windows x64 | CI-validated | Automated builds and tests in CI |


## Roadmap

**Current release: v0.2.3** — the performance series. Reads stage column-major
with batch decode (−14…−47% per family), writes encode column-major over
parallel bulk-load sessions (up to 6.6×), UTF-8 end to end, declared string
types for table creation. Details in the
[release notes](https://github.com/hugr-lab/mssql-extension/releases).

**v0.3.0 — DML pushdown and federation** (reconnaissance complete, specs in
progress):

| Feature | What it does | Status |
|---------|-------------|--------|
| **Direct UPDATE/DELETE** | When the whole `WHERE` pushes down, emit one `UPDATE/DELETE ... WHERE` statement — no scan, no rowid round trip. Also lifts the primary-key requirement for pushable statements (#140) | Spec drafted |
| **INSERT over BCP** | `INSERT INTO ... SELECT` switches from batched VALUES text to the bulk-load protocol (2–10×) | Spec drafted |
| **UPDATE/DELETE via staging** | Affected rows bulk-load into a session `#temp`, one server-side `UPDATE ... JOIN #stage` replaces per-batch VALUES joins | Spec drafted |
| **MERGE INTO + upsert** | `MERGE INTO` planning (DuckDB lowers `INSERT ... ON CONFLICT` to MERGE, so both arrive together); full T-SQL MERGE pushdown for same-catalog sources | Recon complete |
| **Same-catalog read-then-write in transactions** | Materialize the extension's own scans before a bulk-load sink starts (the duckdb-postgres pattern), removing a documented limitation (#239) | Recon complete |
| **JOIN / aggregate pushdown** | Semi-join reduction (always-safe), shipping small local tables to the server via BCP for server-side joins, and pushed GROUP BY / aggregates with exact type mapping | Recon complete |
| **ORDER BY pushdown by default** | Collation-aware safety predicate; native server semantics as the default contract | Spec drafted |

### Under consideration

| Topic | What we're thinking about |
|-------|----------------------------|
| **VARIANT fallback** | UDT / `SQL_VARIANT` columns as DuckDB `VARIANT` instead of today's `NVARCHAR(MAX)` auto-CAST text form. Opt-in via a setting. |
| **TRUNCATE optimization** | Auto-detect `DELETE FROM t` without `WHERE` and emit `TRUNCATE TABLE` when safe (no triggers/FK/CCI). |
| **DuckLake metadata backend** | SQL Server as a DuckLake catalog store (#129). |

## Support MSSQL Extension

MSSQL Extension is an open-source DuckDB extension maintained in spare time.

If it helps you or your organization, consider sponsoring its development through GitHub Sponsors.

Your support helps fund maintenance, bug fixes, testing, and new features.


