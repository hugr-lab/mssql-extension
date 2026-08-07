---
title: Articles
sidebar_position: 7
---

# Articles

Deep dives on the extension's design and performance, published on Medium:

- **[Bringing Microsoft SQL Server to DuckDB: A Native TDS Extension](https://medium.com/@gribanov.vladimir/bringing-microsoft-sql-server-to-duckdb-a-native-tds-extension-d069bf49d8d2)**
  — why a pure C++ TDS 7.4 implementation instead of ODBC/FreeTDS, and what the
  catalog integration looks like from the inside.
- **[DuckDB SQL Server extension: UPDATE, DELETE, Transactions, and 1.2M Rows/Sec Bulk Loading](https://medium.com/@gribanov.vladimir/duckdb-sql-server-extension-update-delete-transactions-and-1-2m-rows-sec-bulk-loading-a1a921fce647)**
  — the DML layer, transaction pinning, and how the columnar BCP write path
  reaches seven-figure row rates.
- **[From 51 Seconds to 1 Second: How We Made DuckDB ↔ SQL Server 49x Faster with ORDER BY Pushdown](https://medium.com/@gribanov.vladimir/from-51-seconds-to-1-second-how-we-made-duckdb-sql-server-49x-faster-with-order-by-pushdown-8274fa0ee594)**
  — pushing `ORDER BY` + `LIMIT` into T-SQL as `SELECT TOP N ... ORDER BY`,
  and why sorting on the server changes everything for top-N queries.
- **[Querying Azure SQL and Microsoft Fabric from DuckDB with Azure Entra ID Authentication](https://medium.com/@gribanov.vladimir/querying-azure-sql-and-microsoft-fabric-from-duckdb-with-azure-entra-id-authentication-56e0a67a0d9b)**
  — service principals, Azure CLI and device-code flows against Azure SQL and
  Fabric warehouses, end to end.

Related resources:

- [DuckDB Community Extensions: mssql](https://duckdb.org/community_extensions/extensions/mssql.html)
- [Hugr Lab](https://hugr-lab.github.io/) — the data-mesh platform this extension grew out of.
