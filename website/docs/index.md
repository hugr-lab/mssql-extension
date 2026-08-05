---
title: Overview
sidebar_position: 1
slug: /
---

# DuckDB MSSQL Extension

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

→ **[Getting Started](/getting-started/)** · **[Settings Reference](/reference/settings/)** · **[Download metrics](https://hugr-lab.github.io/mssql-extension/metrics/)**

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

The following features are planned for future releases:

| Feature | Description | Status |
|---------|-------------|--------|
| **Row Identity** | `rowid` pseudo-column mapping to primary keys | ✅ Implemented |
| **UPDATE/DELETE** | DML support with PK-based row identification, batched execution | ✅ Implemented |
| **Transactions** | BEGIN/COMMIT/ROLLBACK with connection pinning | ✅ Implemented |
| **Multi-Statement Batches** | Temp table workflows via `mssql_scan()` with session reset | ✅ Implemented |
| **CTAS** | CREATE TABLE AS SELECT with two-phase execution (DDL + INSERT) | ✅ Implemented |
| **BCP/COPY** | High-throughput bulk insert via TDS BCP protocol (10M+ rows) | ✅ Implemented |
| **Integrated Auth** | Kerberos on POSIX (kinit / keytab / raw); SSPI on Windows | ✅ Implemented |
| **Custom Application Name** | LOGIN7 `program_name` from connection string / URI / secret / ATTACH option | ✅ Implemented |
| **ATTACH credential validation** | Fail-fast on wrong password / unreachable host (`lazy_validation true` to opt out) | ✅ Implemented |
| **BCP throughput** | LOGIN7 32 KB packet size, single-connection encoder/sender pipelining, column-batch encoding, parallel multi-connection BCP for heap targets | Planned |
| **CTAS quality** | Bounded `NVARCHAR(N)` text defaults, `PAGE` compression, primary-key propagation, per-query overrides via COPY options | Planned |
| **TRUNCATE optimization** | Auto-detect `DELETE FROM t` without `WHERE` and emit `TRUNCATE TABLE` when safe (no triggers/FK/CCI) | Planned |
| **COPY TO TRUNCATE mode** | Atomic replace via TRUNCATE + BCP (distinct from current `OVERWRITE` = DROP+CREATE) | Planned |

### Under consideration

Items being designed; surface and implementation strategy not committed yet.

| Topic | What we're thinking about |
|-------|----------------------------|
| **Direct DML pushdown** | Skip the `rowid` round trip on UPDATE/DELETE when the entire `WHERE` filter is pushable — emit a single `UPDATE target SET ... WHERE <pushdown>` / `DELETE FROM target WHERE <pushdown>` statement instead. |
| **MERGE INTO** | Native SQL Server `MERGE` push-down. Pipelined BCP upload of the USING source to a session `#tmp`, then emit `MERGE INTO target USING #tmp WITH (HOLDLOCK)`. RETURNING via `OUTPUT $action`. |
| **VARIANT fallback for unsupported types** | UDT / `SQL_VARIANT` / legacy `IMAGE` / `TEXT` / `NTEXT` columns mapped to DuckDB `VARIANT` instead of raising. Opt-in via a setting. |

### Feature Details

**Row Identity**: Tables with primary keys expose a virtual `rowid` column. Scalar PKs map to their native type; composite PKs map to DuckDB STRUCT. This enables UPDATE/DELETE support.

**UPDATE/DELETE**: Supports `UPDATE ... SET ... WHERE` and `DELETE FROM ... WHERE` through DuckDB catalog integration. Uses `rowid` for row identification. Batched execution for large operations. Note: RETURNING clause is not supported for UPDATE/DELETE (only for INSERT).

**Transactions**: DML transactions with connection pinning. Each explicit transaction pins a single TDS connection for the transaction's duration, using SQL Server's 8-byte transaction descriptor in ALL_HEADERS. Connections are flagged for session reset (RESET_CONNECTION) on pool return.

**Multi-Statement Batches**: `mssql_scan()` supports batches where intermediate statements (DML/DDL) don't return result sets. Only one result-producing statement per batch is allowed. Session state (temp tables, variables) is reset via TDS RESET_CONNECTION flag when connections return to the pool.

**CTAS**: `CREATE TABLE mssql.schema.table AS SELECT ...` with two-phase execution: CREATE TABLE DDL followed by batched INSERT. Supports CREATE OR REPLACE, configurable text type (NVARCHAR/VARCHAR), and streaming for large result sets. Type mapping from DuckDB to SQL Server with clear errors for unsupported types.

**BCP/COPY**: Binary bulk copy protocol for maximum throughput. Streaming execution with bounded memory. No RETURNING support (use regular INSERT for that).


## Support MSSQL Extension

MSSQL Extension is an open-source DuckDB extension maintained in spare time.

If it helps you or your organization, consider sponsoring its development through GitHub Sponsors.

Your support helps fund maintenance, bug fixes, testing, and new features.


