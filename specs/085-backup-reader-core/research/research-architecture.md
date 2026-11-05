# MSSQL backup reader: architecture research

2026-11-05. mssql-extension `4c65cfb`; mssql-ducklake `8fe10a1` (`duckdb/` `d54cbbe3df1`, `ducklake/` `dcf71031`). Paths are repo-relative.

## Verdict

- **Reader:** build a separate extension (`mssql_backup`) with its own small read-only catalog. Share the decoders as source code, not through linking.
- **Import:** a PRAGMA that expands into COPY SCHEMA, COPY DATA, a commit message and a provenance row. DuckDB runs an expanded PRAGMA as one query in one transaction, and DuckLake writes one snapshot per commit, so each backup file gives **one snapshot**.
- **Differential import:** replace changed tables first; a PK-keyed delta later.

## 1. mssql-extension

**Storage hook.** `TYPE mssql` is a `StorageExtension` with `attach = MSSQLAttach` and `create_transaction_manager` (`src/mssql_storage.cpp:1232,1650,1661-1668`; `duckdb/src/include/duckdb/storage/storage_extension.hpp:26-50`). Register `mssql_backup` the same way and force READ_ONLY.

**Catalog.** Use a separate light catalog, not a subclass of `MSSQLCatalog`:

- `MSSQLCatalog` owns the pool and a connection-loaded cache (`src/include/catalog/mssql_catalog.hpp:301-310,630-631`).
- About ten sites check `GetCatalogType()=="mssql"` and then use the pool, the optimizer or DML (`src/table_scan/table_scan.cpp:1110,1424`, `src/table_scan/mssql_optimizer.cpp:84,883,947`, `src/connection/mssql_connection_provider.cpp:143`, `src/dml/mssql_dml_function.cpp:168`).
- Proposed: `MSSQLBackupCatalog` with type `mssql_backup`. Load metadata once at ATTACH from sysschobjs, syscolpars, sysrowsets/sysallocunits, sysidxstats and sysobjvalues. Use a trivial transaction manager.
- Reuse only the data shapes `MSSQLTableMetadata` (`src/include/catalog/mssql_metadata_cache.hpp:46`) and `MSSQLColumnInfo` (`mssql_column_info.hpp:13`).

**Reusable for decoding:**

- **Type map:** `MapSQLServerTypeToDuckDB` (`src/catalog/mssql_column_info.cpp:199-336`) is pure. It needs a system_type_id → name table.
- **Strings:** report plain VARCHAR, not `NativeDuckDBType` (`:72`). DuckLake rejects alias types and collated VARCHAR (`ducklake/src/common/ducklake_types.cpp:158-163,181-184`), and `MSSQL_VARCHAR` is registered by mssql itself (`src/mssql_extension.cpp:207-215`).
- **Byte decoders, same layout on disk as on the wire:**
  - `ConvertDatetime2` / `ConvertDatetimeOffset` (`src/tds/encoding/datetime_encoding.cpp:120,158`), and date/time;
  - `ConvertDecimal` (`decimal_encoding.cpp:33`);
  - `ConvertGuid` (`guid_encoding.cpp:39`);
  - `Utf16LEDecodeValidInto` (`utf16.cpp:502`).
- **Batch kernels:** `DecodeChunkFromStaging` (e.g. `src/include/codec/datetime_codec.hpp:79`) reads a `ColumnStaging` buffer, which has no TDS dependency (`codec/staging/column_staging.hpp:195`). A reader can fill it from page rows and build a synthetic `tds::ColumnMetadata`.
- **Not reusable:**
  - `ResolveColumnOps` and the row stager, which assume TDS framing (`codec/staging/column_ops.hpp:40-164`);
  - the per-value `DecodeFromTds`.
- **Code pages:** the `CodePageCanEncode` tables are sorted code-point sets (`src/catalog/mssql_code_page.cpp:24-174`): they check encoding but cannot decode.
  - The server supplies the code page (`COLLATIONPROPERTY`, `mssql_column_info.hpp:31-35`) and does the transcoding (`BuildReadExpression` casts a non-UTF-8 varchar to NVARCHAR, `mssql_column_info.cpp:526-575`).
  - Offline, the reader needs a collation id → code page table and byte → Unicode tables (1250-1258, 874, 932/936/949/950).
  - UTF-8 collations need nothing.
- **Geometry:** there is no parser for the native format; the server converts with `.STAsBinary()` (`mssql_column_info.cpp:532-534`). The reader needs an MS-SSCLRT → WKB converter. DuckLake stores `geometry` (`ducklake_types.cpp:173`).
- **Filters:** copy the client-side filter net (`table_scan.cpp:379-412`).

**On disk ≠ wire** (verify each on samples):

- **`datetime` / `smalldatetime`:** time first on disk, while the extension decodes days first (`datetime_encoding.cpp:66-71,142-147`).
- **`money`:** a plain little-endian int64 on disk; TDS sends two halves (`decimal_encoding.cpp:64`).
- **`bit`:** packed in the row.
- **`xml`:** binary XML (MS-BINXML), not UTF-16 text.
- **`json` (2025):** a native binary format, undocumented. The wire sends varchar(max) (`mssql_column_info.cpp:307-316`).
- **`sql_variant`:** a header followed by the value. Map it to VARIANT, which DuckLake stores (`ducklake_types.cpp:171`), instead of today's NVARCHAR cast (`mssql_column_info.cpp:544-549`). mssql-ducklake will not inline VARIANT (`src/mssql_metadata_manager.cpp:100-107`).
- **`hierarchyid` / UDTs:** CLR-serialized.
- **Physical:** compression, vardecimal, sparse, LOB/row-overflow, ghost/versioned and forwarded records, columnstore (refuse by name).

**Scan:**

- **Function:** a catalog-scan `TableFunction` with `projection_pushdown`, `filter_pushdown` and `filter_prune` (`duckdb/src/include/duckdb/function/table_function.hpp:554-565`), and `cardinality` from sysrowsets.
- **Filters:** DuckDB 2.0 requires the scan to apply every filter it accepts. Decode the filter columns first and the rest only for rows that pass. Seek the B-tree on a clustered-key range.
- **Parallelism:** batches of extents from the IAM chain, filtered to leaf pages. Key order is not kept. (`MaxThreads()` is 1 today, `src/table_scan/table_scan_state.cpp:40`.)
- **LOBs:** read lazily. A **PageSource** under the scan maps (file_id, page_id) to bytes in layers: full, then diff, then log redo. Its page index is built in one pass at ATTACH. Reads go through DuckDB's FileSystem, so S3 works.

**Live-connection dependencies to avoid:**

- `MSSQLCatalog::Initialize` (pool, login, collation; `src/catalog/mssql_catalog.cpp:153,421,465`);
- the metadata loaders, which take `tds::TdsConnection&` (`mssql_metadata_cache.hpp:158-256,401`);
- transaction pinning (`src/catalog/mssql_transaction.cpp:262,410`; `table_scan.cpp:140`);
- the statistics DMV (`mssql_catalog.cpp:968`);
- remote pushdown (`src/catalog/mssql_catalog_pushdown.cpp:134`);
- the optimizer (`src/mssql_extension.cpp:239-242`).

**Separate extension.**

- **Pros:**
  - WASM is possible (no sockets, TLS or Kerberos).
  - Smaller than mssql's 46 MB loadable; DuckDB's mbedtls and miniz (`duckdb/third_party/`) may cover crypto, if the host exports them.
  - Its own release cadence, outside the "ships with DuckDB 2.0" gate.
  - The format risk is isolated.
  - It can stay MIT (clean-room; OrcaMDF is reportedly GPL, verify, so do not port it).
- **Cons:**
  - Code is shared by source only, because hidden symbols prevent linking across extensions (mssql-ducklake `CLAUDE.md`, spec 002). Use a submodule for `tds/encoding`, the type map and code pages.
  - Two installs; type mappings can drift.
- **Recommendation:** separate.

## 2. mssql-ducklake

**Writes today:**

- **Data:** DuckLake's own path, parquet in DATA_PATH. Rows up to 10 are inlined into SQL Server (`ducklake/src/ducklake_extension.cpp:36-38`).
- **Metadata:** each commit-batch statement is matched exactly and sent as T-SQL through `mssql_exec` on the pinned connection (`src/mssql_commit_batch.cpp:1369`).
- **Data-files-only commits** are applied server-side (`src/mssql_server_commit.cpp:241-282`). DDL takes DuckLake's client loop.

**Snapshots (verified).** The path is `Commit` → `FlushChanges` (`ducklake/src/storage/ducklake_transaction.cpp:799-837,1426-1456`). Each attempt does `snapshot_id++` once, and DDL, data and snapshot_changes go in **one batch** (`ducklake_transaction_state.cpp:2674-2686`). The test `ducklake/test/sql/functions/ducklake_snapshots.test:108-133` confirms one snapshot for CREATE SCHEMA, CREATE TABLE and INSERT in one transaction. Limits:

- **Compaction** cannot share a transaction with changes (`ducklake_transaction_state.cpp:419-421`).
- **`set_option`** is refused for tables created in the same transaction (`ducklake/src/functions/ducklake_set_option.cpp:96-105`). Set lake-level options first.
- **ALTER** is refused on a table with inlined data from the same transaction (`ducklake_table_entry.cpp:1473`).
- **Inlined inserts** run after the DDL (`ducklake_transaction.cpp:1517-1525`): on a SQL Server catalog, one table per small imported table. Consider an inlining limit of 0.
- **Large transactions:** parquet is written as the statement runs, metadata at commit; rollback deletes the files (`ducklake_transaction.cpp:839-853`).

**Building the import:**

- A table function cannot run statements: `CALL` binds as `SELECT * FROM f()` (`duckdb/src/planner/binder/statement/bind_call.cpp`).
- **The precedent is `PRAGMA copy_database`,** which expands into COPY SCHEMA + COPY DATA (`duckdb/src/function/pragma/pragma_queries.cpp:191-200`).
  - An expanded PRAGMA runs "as one query in one transaction", with no BEGIN/COMMIT allowed (`duckdb/src/planner/statement_preprocessor.cpp:56-63`; `duckdb/src/main/client_context_execution.cpp:48-50,390-391`).
  - COPY DATA binds every table's INSERT as one UNION ALL plan (`bind_copy_database.cpp:52-104`).
- DuckLake's own functions work differently: `ducklake_add_data_files` appends to the caller's transaction (`ducklake/src/functions/ducklake_add_data_files.cpp:1212-1217`), and compaction uses `bind_operator` (`ducklake_compaction_functions.cpp:916-925`).

Proposed: `PRAGMA ducklake_import_mssql_backup('lake','b')`.

- Its callback runs in a transaction (it can read the backup header) and returns:
  1. COPY SCHEMA;
  2. COPY DATA;
  3. `CALL ducklake_set_commit_message('lake',…, extra_info := '{json}')` (`ducklake/src/functions/ducklake_set_commit_message.cpp:27-48`);
  4. `INSERT INTO lake.__mssql_backup.imports …`.
- Attach the backup before the PRAGMA; a DETACH inside it is refused (`duckdb/src/execution/operator/schema/physical_detach.cpp:20-25`).
- Put decryption parameters in a SECRET: the generated SQL reaches query logs.
- The import is pure SQL, so it also works with stock DuckLake. Putting it in mssql-ducklake is a packaging choice.

**Differential import:**

- **Replace each changed table:**
  - Find changed allocation units from page headers and IAM pages, and map them to objects through sysallocunits/sysrowsets.
  - `DELETE FROM t` + INSERT. DuckLake drops fully deleted files rather than writing delete files (`ducklake/src/storage/ducklake_delete.cpp:408-419`).
  - Simple and exact, but `ducklake_table_changes` shows the whole table replaced.
- **PK-keyed delta:**
  - K = the keys on changed leaf pages, in the old and new images.
  - Delete K, then insert the rows on the new changed pages. This is exact because a moved row dirties its destination page.
  - The old image comes from the previous chain, or from a page id stored with each row.
  - Fall back to replace when LOB or row-overflow units changed, for heaps, and on schema changes.
- DuckLake's MERGE allows only one UPDATE/DELETE action (`ducklake/src/storage/ducklake_merge_into.cpp:11-23`). Use DELETE + INSERT. It is the same transaction, so **one snapshot**.

**Provenance:**

- **`commit_extra_info` JSON:** backup_set_guid, database guid, backup type, first/last/checkpoint LSNs, differential_base_lsn/guid. The struct is at `ducklake_metadata_info.hpp:555-564`. `ducklake_snapshots()` stops listing a snapshot after it expires, though the change rows are kept (`ducklake_metadata_manager.cpp:5475`).
- **A versioned lake table, `__mssql_backup.imports`,** written in the same snapshot, for chain checks: the diff's base guid must match the full's, and checkpoint_lsn must increase.

## 3. Views

**Today:**

- Views are table entries of type `MSSQLObjectType::VIEW` (`src/catalog/mssql_metadata_cache.cpp:546,949`), with no rowid (`mssql_table_entry.cpp:153-157`).
- The server evaluates them, and no definition is read (no `sql_modules` in `src/`).
- CREATE VIEW is not implemented (`mssql_schema_entry.cpp:295-300`).

**For backups:**

- Definitions are in sysobjvalues.
- Store all of them as text in `__mssql_backup.views`.
- Translate a SELECT-only subset (brackets, TOP, ISNULL, CONVERT, `+` concatenation) into DuckDB views, either in the backup catalog (COPY SCHEMA carries them) or in the lake.
- Raw T-SQL cannot be a lake view: DuckLake writes `dialect='duckdb'` (`ducklake_transaction.cpp:1264`) and parses the text as DuckDB SQL on load (`ducklake_view_entry.cpp:115-119`).
- DuckDB's `GrammarExtension` changes the global grammar, so it is unsuitable.

## Risks

- **Fuzzy full backups:** a page-level read can see uncommitted rows and half-done splits. Exact results need redo/undo of the log included in the backup, not only log-backup support.
- **Compression (MS_XPRESS/QAT), encryption and TDE:** undocumented containers; decryption needs the certificate and its private key.
- **Columnstore and native json:** undocumented formats.
