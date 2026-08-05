<!-- METRICS-BADGES:START -->

[![CI](https://github.com/hugr-lab/mssql-extension/actions/workflows/ci.yml/badge.svg)](https://github.com/hugr-lab/mssql-extension/actions/workflows/ci.yml)
[![DuckDB](https://img.shields.io/static/v1?label=duckdb&message=v1.4.1%2B&color=blue)](https://github.com/duckdb/duckdb/releases)
[![Latest release](https://img.shields.io/github/v/release/hugr-lab/mssql-extension?label=release&color=blue)](https://github.com/hugr-lab/mssql-extension/releases/latest)
[![Community downloads per week](https://img.shields.io/badge/dynamic/json?url=https%3A%2F%2Fcommunity-extensions.duckdb.org%2Fdownloads-last-week.json&query=%24.mssql&label=downloads%2Fweek&color=brightgreen)](https://duckdb.org/community_extensions/download_metrics)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![GitHub stars](https://img.shields.io/github/stars/hugr-lab/mssql-extension?style=flat&color=informational)](https://github.com/hugr-lab/mssql-extension/stargazers)

<!-- METRICS-BADGES:END -->

# DuckDB MSSQL Extension

A DuckDB extension for connecting to Microsoft SQL Server databases using native TDS protocol - no ODBC, JDBC, or external drivers required.

<!-- METRICS-CHART:START -->

### 📈 Community Extension Downloads

[![Weekly downloads of the mssql DuckDB community extension](https://hugr-lab.github.io/mssql-extension/download-metrics.svg)](https://duckdb.org/community_extensions/download_metrics)

📊 **[Interactive chart](https://hugr-lab.github.io/mssql-extension/metrics/)** — queried live in your browser with DuckDB-Wasm.

> Regenerated weekly and served from GitHub Pages, so it stays current without a commit. Counts are a Cloudflare estimate of `INSTALL mssql FROM community` events, aggregated across DuckDB versions and platforms. Source: [DuckDB Community Extensions download metrics](https://duckdb.org/community_extensions/download_metrics).

<!-- METRICS-CHART:END -->

> **Experimental**: This extension is under active development. APIs and behavior may change between releases. We welcome contributions, bug reports, and testing feedback!


## Documentation

**Full documentation lives at [hugr-lab.github.io/mssql-extension](https://hugr-lab.github.io/mssql-extension/)** —
connections and authentication (SQL, Azure AD / Entra ID, Kerberos, Windows SSPI),
catalog integration, query pushdown, bulk loading over BCP, DML, transactions,
the complete settings reference, and performance tuning.

Quick pointers:

| | |
|---|---|
| [Getting Started](https://hugr-lab.github.io/mssql-extension/getting-started/) | install, ATTACH, first queries |
| [Connections & Auth](https://hugr-lab.github.io/mssql-extension/connection/) | connection strings, secrets, Azure AD, Kerberos |
| [Writing Data](https://hugr-lab.github.io/mssql-extension/writing/copy/) | COPY (bulk load), CTAS, INSERT/UPDATE/DELETE |
| [Settings Reference](https://hugr-lab.github.io/mssql-extension/reference/settings/) | every `mssql_*` setting with defaults |
| [Troubleshooting](https://hugr-lab.github.io/mssql-extension/reference/troubleshooting/) | common errors and limitations |
| [Articles](https://hugr-lab.github.io/mssql-extension/articles/) | Medium deep dives on design and performance |

Contributor docs stay in the repository: [DATAMODEL.md](DATAMODEL.md) (layered
architecture), [docs/](docs/) (internals), [docs/TESTING.md](docs/TESTING.md),
[CLAUDE.md](CLAUDE.md) (development guidelines).

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
- [Azure AD authentication](AZURE.md) (service principal, CLI, interactive device code flow)
- [Kerberos / Windows SSPI integrated authentication](Kerberos.md) — POSIX (`kinit` / keytab / raw credentials) and Windows (current logon session via `secur32.dll`)
- Custom `Application Name` propagated to SQL Server `APP_NAME()` / `sys.dm_exec_sessions.program_name`
- ATTACH-time credential validation (opt-out via `lazy_validation true`)
- **Experimental**: ORDER BY pushdown to SQL Server (opt-in via `mssql_order_pushdown` setting)


## Quick Start

### Prerequisites

- DuckDB v1.4.1 or later (minimum supported version)
- SQL Server 2019 or later accessible on network

### Step 1: Install Extension

```sql
INSTALL mssql FROM community;
LOAD mssql;
```

### Step 2: Connect to SQL Server

#### Option A: Using a Secret (Recommended)

```sql
CREATE SECRET my_sqlserver (
    TYPE mssql,
    host 'localhost',
    port 1433,
    database 'master',
    user 'sa',
    password 'YourPassword123'
);

ATTACH '' AS sqlserver (TYPE mssql, SECRET my_sqlserver);
```

#### Option B: Using Connection String

```sql
ATTACH 'Server=localhost,1433;Database=master;User Id=sa;Password=YourPassword123'
    AS sqlserver (TYPE mssql);
```

### Step 3: Query Data

```sql
-- List schemas
SELECT schema_name FROM duckdb_schemas() WHERE database_name = 'sqlserver';

-- List tables in dbo schema
SELECT table_name FROM duckdb_tables() WHERE database_name = 'sqlserver' AND schema_name = 'dbo';

-- Query a table
FROM sqlserver.dbo.my_table LIMIT 10;
```

### Step 4: Disconnect

```sql
DETACH sqlserver;
DROP SECRET my_sqlserver;
```


## Support MSSQL Extension

MSSQL Extension is an open-source DuckDB extension maintained in spare time.

If it helps you or your organization, consider sponsoring its development through GitHub Sponsors.

Your support helps fund maintenance, bug fixes, testing, and new features.


## Contributing

Issues and pull requests are welcome. Build and test instructions:
[Development](https://hugr-lab.github.io/mssql-extension/development/) ·
[docs/TESTING.md](docs/TESTING.md).

## Third-Party Licenses

The hugr-lab/mssql-extension release binaries statically link the
following third-party components:

- **OpenSSL** (TLS / cryptography), licensed under the Apache
  License, Version 2.0. © The OpenSSL Project Authors and
  contributors. Version pinned via `vcpkg.json`.
  License text: [LICENSES/openssl-LICENSE.txt](LICENSES/openssl-LICENSE.txt)
- The **simdutf** library (Unicode transcoding via SIMD), used
  under the MIT License option of its dual Apache-2.0/MIT
  licensing. © Daniel Lemire and simdutf contributors.
  License text: [LICENSES/simdutf-LICENSE-MIT.txt](LICENSES/simdutf-LICENSE-MIT.txt)

## License

This project is licensed under the MIT License. See the [LICENSE](LICENSE) file for details.
