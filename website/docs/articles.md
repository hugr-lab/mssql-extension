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

Related resources:

- [DuckDB Community Extensions: mssql](https://duckdb.org/community_extensions/extensions/mssql.html)
- [Hugr Lab](https://hugr-lab.github.io/) — the data-mesh platform this extension grew out of.
