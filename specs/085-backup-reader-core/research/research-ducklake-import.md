# Importing backups into DuckLake: Parquet first, one commit per file

2026-11-05. Read-only research. Pins: mssql-ducklake `8fe10a1`, its `ducklake/` at `dcf71031` (catalog
format 1.1), `duckdb/` at v2.0.0 `d54cbbe3df1`; duckdb-iceberg `b02e34f`, cloned into `work/duckdb-iceberg`.
Path prefixes used below:

- `DL/` = `mssql-ducklake/ducklake/src/`
- `DD/` = `mssql-ducklake/duckdb/`
- `MD/` = `mssql-ducklake/src/`
- `IB/` = `work/duckdb-iceberg/src/function/ducklake/`

Builds on RESEARCH.md §5 and §6.

## 0. Verdict

1. **Write the files first, then commit once.** Parallel workers write the data Parquet and the page
   runs. They can be threads, processes or machines, and they work outside any lake transaction.
   Then ONE short transaction per backup file holds everything else: DDL, file registration,
   positional deletes, `__mssql_sys`, the commit message and provenance. That transaction is the
   file's one snapshot.
2. **Register through DuckLake's own transaction, not by writing metadata rows.** mssql-ducklake
   compiles DuckLake in (its CLAUDE.md), so it can call `AppendFiles`, `AddDeletes`, `DropFile` and
   the delete-file writer without editing DuckLake. DuckLake keeps ids, row ids, stats and conflict
   checks, and mssql-ducklake's commit batch carries the result to SQL Server. v1 can be plain SQL
   (`ducklake_add_data_files` plus native `DELETE`).
3. **Deletes come from a page map, with no data scan.** A page's rows are contiguous in their file, so
   the rows of changed or freed pages become position ranges. DuckLake's cumulative delete-file
   format supports this against files from any earlier import.
4. **Most catalog changes map to DuckLake ALTERs, with stable field ids.** Schema transfer,
   non-widening type changes and non-persisted computed columns need a rewrite inside the same
   commit.
5. **Partitioning:**
   - Files are always split per SQL Server partition (rowset).
   - A DuckLake transform is declared only where the boundaries align.
   - SWITCH cannot be metadata-only: DuckLake has no file move, and its cleanup does not count path
     references.

## 1. What iceberg_to_ducklake does

- **Output.** Metadata rows for the EXISTING Iceberg Parquet files, with zero copy: paths are absolute
  (`IB/ducklake_data_file.cpp:66-70`) and only Parquet is accepted (`:15-19`).
- **Snapshots.** One snapshot per distinct Iceberg snapshot timestamp, across tables
  (`IB/iceberg_to_ducklake.cpp:112-116,137,785-786`). Ids come from its own counter
  (`IB/ducklake_snapshot.cpp:12-28`).
- **Field ids, schemas, partitions.** Column ids are the Iceberg field ids (`IB/ducklake_column.cpp:13`);
  column versions follow schema changes (`iceberg_to_ducklake.cpp:146-187`); partitions come from
  the specs (`:207-213`).
- **Statistics.** From the manifest bounds, not from footers (`:462-536`). `next_row_id =
  record_count` is a FIXME (`:609-620`).
- **Delete files.** Positional Parquet files referencing ONE data file, or Puffin deletion vectors at
  offset 0 (`IB/ducklake_delete_file.cpp:17-60`). One active delete file per data file
  (`IB/ducklake_table.cpp:112-119`). Equality deletes are refused (`iceberg_to_ducklake.cpp:238-241`).
- **How it writes.** `BEGIN`, `DELETE FROM ducklake_table/snapshot/snapshot_changes`, then INSERTs
  into a dozen metadata tables (`:304-751`), run on a new `Connection` to `__ducklake_metadata_<lake>`
  (`:941-943,955-967`). It bypasses DuckLake's catalog and commit path.
- **What it assumes:**
  - format 0.4 only (`:893-896`), while our lake is 1.1 (`DL/storage/ducklake_metadata_manager.cpp:627`);
  - an empty catalog (`:899-924`);
  - NULL `row_id_start`, `footer_size` and `mapping_id` (`ducklake_data_file.cpp:77-85`);
  - NULL extra_info (`:746`);
  - no concurrent writer.

**What to take from it:**
- zero-copy registration;
- statistics from the producer;
- one delete file per data file.

**What not to take:** writing the metadata rows. That code is frozen to one format and an empty lake,
and has already drifted: its data-file row has 16 values, the 1.1 layout has 17
(`MD/mssql_commit_batch.cpp:62`).

## 2. `ducklake_add_data_files`

- **Signature.** `(catalog, table_name, path | [paths])`, globs accepted. Options: `allow_missing`,
  `ignore_extra_columns`, `hive_partitioning`, `schema` (`DL/functions/ducklake_add_data_files.cpp:1220-1235`).
- **Mapping is BY NAME**, case-insensitive (`:1012-1019`).
  - Extra or missing columns are errors unless allowed; missing ones read as `initial_default`
    (`:1020-1050`).
  - Parquet field ids are only checked to be live (`:1066-1082`).
  - Every file gets a name map (`:1140-1146`), de-duplicated (`DL/storage/ducklake_transaction.cpp:2370-2386`).
  - Types are checked by `DuckLakeParquetTypeChecker`, NOT NULL against footer null counts.
- **Statistics:** read from each footer through `parquet_full_metadata`: min/max, exactness, nulls,
  sizes, geo bbox (`:236-268,389-469`).
- **Partition values:** only from Hive paths (`:1084-1120,1148-1198`).
- **Ownership:**
  - `created_by_ducklake=false` (`:471`) only means a rollback does not delete the file
    (`ducklake_transaction.cpp:76-81,101-116`).
  - After commit the file is DuckLake's: compaction merges it and cleanup deletes the original
    (`DL/../test/sql/add_files/add_files_compaction.test:62-98`).
  - There is no "external" flag.
- **Transactions:** appends to the CALLER's transaction (`:1212-1218`). CREATE TABLE plus add-files
  in one transaction works (`add_files_transaction_local.test:17-36`). DuckLake makes one snapshot
  per commit (`ducklake_snapshots.test:108-133`), so DDL and many tables give ONE snapshot.
- **Limits:** data files only, no delete files; encrypted catalogs refused (`:45-48`).

## 3. DuckLake's own write paths

- **INSERT.** Rows become Parquet as they arrive; the metadata is written at commit.
  - The plan is a `PhysicalCopyToFile` with `rotate`, a size target and `WRITTEN_FILE_STATISTICS`
    (`DL/storage/ducklake_insert.cpp:551-612`).
  - Sizes: 512 MB files (`DL/include/storage/ducklake_catalog.hpp:141`), 122,880-row row groups
    (`DD/src/include/duckdb/storage/storage_info.hpp:26`).
  - Rotation turns off order preservation (`DD/src/execution/physical_plan/plan_copy_to_file.cpp:28-36`),
    so threads append their own row groups to shared files.
  - `SORTED BY` adds a full sort (`ducklake_insert.cpp:715-744`).
  - So the producer knows neither page contiguity nor positions.
- **Several tables.** `COPY FROM DATABASE … (DATA)` is a UNION ALL of INSERTs
  (`DD/src/planner/binder/statement/bind_copy_database.cpp:52-101`). Children that saturate the
  threads get OPTIONAL dependencies (`DD/src/execution/operator/set/physical_union.cpp:35-84`), so
  big tables run one after another.
- **DELETE.** The scan feeds `(filename, file_index, file_row_number)` (`DL/storage/ducklake_delete.cpp:363-391`).
  Per file, DuckLake then:
  - drops the file if all its rows go (`:408-420`);
  - inlines up to `data_inlining_row_limit` positions (`:523-537`);
  - otherwise writes a positional Parquet file `file_path, pos[, _ducklake_internal_snapshot_id]` (`:37-155`).
  - Puffin deletion vectors (`:166-186`) are EXPERIMENTAL (`DL/functions/ducklake_options.cpp:40`).
- **Deletes against an older file.** The delete file is cumulative: old positions keep their snapshot
  ids (`:422-478`). The commit drops the superseded row and schedules its file
  (`ducklake_metadata_manager.cpp:4581-4603`); it rewrites the file if the snapshot id moved
  (`ducklake_delete.cpp:245-278`).

## 4. Design: export, then one registering commit

### 4.1 What the commit must write

**For the snapshot:**
- `ducklake_snapshot` (id, time, schema_version, next_catalog_id, next_file_id);
- `ducklake_snapshot_changes` (changes, author, message, `commit_extra_info`).

**DDL:**
- schema, table and column rows, with `end_snapshot` on old versions;
- `ducklake_schema_versions`;
- partition and sort info;
- name maps.

**Per data file:**
- `ducklake_data_file`, including a `row_id_start` taken from the table's `next_row_id`;
- per-column `ducklake_file_column_stats`;
- `ducklake_file_partition_value`.

**Per delete file:** its row, and the deletion of the superseded one together with
`ducklake_files_scheduled_for_deletion`.

**Fully deleted data files:** `end_snapshot`.

**Per touched table:** merged table stats and table column stats.

The DDL is at `ducklake_metadata_manager.cpp:247-330`.

### 4.2 Through DuckLake, not metadata rows

**Writing those rows ourselves means T-SQL against the catalog in SQL Server.** It would be an
extended `CommitBatchSql` (`MD/mssql_server_commit.cpp:31-80`) that reproduces:
- snapshot allocation under UPDLOCK;
- row ids and the stats merge;
- delete-file supersession and name-map de-duplication;
- re-validation on every DuckLake bump.

The caches would learn of it only by reloading, and the metadata volume is identical either way.

**The DuckLake path:**
- (a) SQL: `ducklake_add_data_files`, DDL, native DELETE.
- (b) C++ in mssql-ducklake:
  1. turn the writer's `WRITTEN_FILE_STATISTICS` rows into `DuckLakeDataFile`s
     (`DL/include/common/ducklake_data_file.hpp:65-83`), as `DuckLakeInsert::AddWrittenFiles` does
     (`ducklake_insert.cpp:157-258`): no footer re-read, partition values set directly;
  2. `AppendFiles` with `created_by_ducklake=false`, so a failed commit keeps the files for a retry;
  3. deletes as in §5.

**The commit:**
- `FlushChanges` runs through mssql-ducklake's client loop as T-SQL in 1000-row VALUES chunks
  (`MD/mssql_commit_batch.cpp:14-27,290-292,462`).
- The server-side apply covers data-files-only commits (`MD/mssql_server_commit.cpp:22-26,236-262`),
  and an import has DDL, deletes and name maps.
- Estimate: 2,000 files × 40 columns ≈ 80k rows ≈ 80 statements, so seconds.
- If it proves slow, extend the apply to DuckLake's staged `DELETE_FILE` / `DROPPED_FILE` /
  `NAME_MAP` tables (`DL/include/storage/ducklake_staged_commit.hpp:39-58`). That is the "direct"
  route done safely.

### 4.3 Options compared

| | (A) `INSERT … SELECT FROM mssql_backup_scan`, one txn | **(B) workers write files, one short commit** | (C) iceberg-style rows |
|---|---|---|---|
| Snapshots | 1 | **1** | 1 per source snapshot |
| Parallelism | within a table; big tables sequential (§3) | all units (partition × key or extent range) at once, any machine | n/a |
| Memory | threads × row group, plus sort spill | 1 row group per writer | small |
| Lake txn length | the WHOLE import (hours); a conflict at commit loses all | **registration only** | short |
| Failure | rollback deletes all files | per-unit retry; names `mssql-<BackupSetGUID>-<objid>-p<part>-u<unit>-<seq>.parquet`; orphans → `ducklake_delete_orphaned_files` | manual |
| Stats / ids | native | writer stats or footer; name map, or `FIELD_IDS` (`DD/extension/parquet/parquet_extension.cpp:149,210`) | hand-made |
| Order / positions | unknown | **writer controls and records them** | n/a |
| Commit path | client loop | client loop (server-side later) | bypasses DuckLake |

Inlining: set `data_inlining_row_limit=0` everywhere, so that small DELETEs and ALTERs do not hit
inlined data (`DL/storage/ducklake_table_entry.cpp:1473`).

## 5. Positional deletes from a page map

**Writer contract (`mssql_backup_export`):**
- one thread per file;
- a page's rows emitted together;
- a page splits only at a rotation or a DuckLake partition boundary;
- each file reports its runs `(page, first_pos, row_count)`.

**Physical order:**
- **Clustered rowstore: leaf-chain order**, key order and contiguity together. Split the work by the
  level-1 index pages, so each unit is a disjoint key range: tight key min/max, and `SORTED BY (key)`.
- **Heap: IAM order**, ascending page ids, so `_mssql_page` stats prune too.
- **Columnstore: rowgroup order.** The run is `(rowgroup, first_pos, count)` and tuple t sits at
  `first_pos + t`. The delete bitmap is directly DuckLake positions; the tuple mover shows up as delta
  pages freed plus a new rowgroup.

**The page map.**
- Lake table `__mssql_sys.page_map(table, page, au_id, data_file_path, first_pos, row_count)`,
  `SORTED BY (page)`, written by the export and registered like data.
- The path is the key, because file ids exist only after commit; `ducklake_list_files` maps paths
  to ids.
- About one row per 8 KB page.

**Diff:**
1. Collect the changed, freed and dead-AU pages in EVERY table (E1 rule 5).
2. Look them up in the page map; stats prune that lookup.
3. Group the hits by data file.
4. For each file:
   - read its current deletes (`DuckLakeDeleteFilter::ScanDeleteFile`, `DL/include/storage/ducklake_delete_filter.hpp:51`);
   - merge, and write a cumulative file with per-position snapshot ids through
     `DuckLakeDeleteFileWriter`;
   - `AddDeletes` with `overwritten_delete_file`. This is `FlushDeleteWithSnapshots`
     (`ducklake_delete.cpp:422-478`), driven from mssql-ducklake;
   - all rows gone → `DropFile` (`:408-420`).
5. The page map's stale rows go by a plain `DELETE … WHERE page IN`.

The cost is O(changed rows). The native `DELETE … WHERE _mssql_page IN (…)` is the oracle and the v1
fallback; it reads `_mssql_page` of every file.

**Compaction moves positions.** `auto_compact` defaults to true (`DL/storage/ducklake_catalog.cpp:1306`),
and compaction cannot share a transaction with changes (`DL/storage/ducklake_transaction_state.cpp:421-423`).
- Set `auto_compact=false` and let the importer compact.
- The next import rebuilds the page-map entries of files that are no longer live from their
  `_mssql_page, file_row_number`. That contiguity survives a merge is to be verified.

## 6. Schema evolution, all inside the one commit

Field ids are DuckLake column ids: kept across RENAME and promotion, new on DROP + ADD. `__mssql_sys`
maps (object_id, colid) → (table_id, field id). Name-mapped files map a Parquet name to a field id,
so renames do not affect them.

| SQL Server | Lake | Rewrite |
|---|---|---|
| new / dropped table | CREATE (with PARTITIONED BY / SORTED BY) / DROP | no |
| rename (object_id stable) | `RENAME TABLE`, table_id kept (`ducklake_table_entry.cpp:1478`) | no |
| `ALTER SCHEMA … TRANSFER` | no SET SCHEMA in DuckDB (`DD/src/include/duckdb/parser/parsed_data/alter_table_info.hpp:72-92`): CREATE in the target, `INSERT … SELECT`, DROP; or copy files (§7.4) | yes |
| ADD COLUMN NULL | `ADD COLUMN` (`:823-855`) | no |
| ADD COLUMN NOT NULL DEFAULT k (metadata-only; k in `sysseobjvalues`) | `ADD COLUMN … DEFAULT k`: old files read k (`DL/storage/ducklake_field_data.cpp:80-90,115`); literal only (`:88`), so pass the stored constant | no |
| DROP COLUMN | `DROP COLUMN`; reset the partition or sort spec on it first (`:864-900`) | no |
| `sp_rename` column | `RENAME COLUMN`, field id kept (`:785-810`) | no |
| int → bigint (size-of-data) | `ALTER … TYPE`, allowed when DuckDB's max type is the target (`:956-967,1106-1130`) | rows in diff anyway |
| varchar(n) widening (metadata-only) | nothing: no length in the lake; n kept in the mirror | no |
| decimal(p,s) → (p2>p,s) | promotion | no |
| scale change, int → varchar … | ADD new, DROP old, RENAME; SQL Server rewrote every row | yes |
| NULL ↔ NOT NULL | `DROP NOT NULL` is cheap; `SET NOT NULL` scans (`:77-91`), or keep lake columns nullable | scan |
| computed | generated columns refused (`:115`): persisted ones as plain columns, others as text in the mirror | — |
| identity | plain column; seed and increment in the mirror | no |
| PK / clustered / partitioning rebuilt | new rowsets, every page in the diff: drop the old files whole, insert, `SET SORTED BY` / `SET PARTITIONED BY` (new spec, new files only) | yes |

Table options go in `CREATE … WITH (…)` or at schema scope beforehand: `set_option` is refused for a
table created in the same transaction (research-architecture.md).

## 7. Partitioning

### 7.1 DuckLake today

- **Transforms:** identity, `bucket(n,col)`, `year`/`month`/`day`/`hour` (calendar parts), and on 1.1
  `epoch_year`/`epoch_month`/`epoch_day`/`epoch_hour` (`DL/include/storage/ducklake_partition_data.hpp:20-31`;
  `ducklake_table_entry.cpp:598-658`). There is no truncate transform.
- **Evolution:** `SET PARTITIONED BY` starts a new `ducklake_partition_info`; old files keep theirs
  (`ducklake_metadata_manager.cpp:311-316`).
- **One partition value set per file.** add-files takes it from Hive paths (key = column name for
  identity, else the transform name, `DL/storage/ducklake_partition_data.cpp:43-59`); the C++ path
  sets it directly.

### 7.2 SQL Server partitions in the backup

- Each partition of each index is a rowset: `sysrowsets` (idmajor, idminor, **numpart**), with its
  own allocation units. `numpart` is already decoded in `experiments/E5/cs_decode.py:748`.
- One SQL Server partition gives one unit of writers, with files of its own.
- **To verify (E7)**, by reading `OBJECT_DEFINITION` of `sys.partition_functions`,
  `partition_range_values` and `partition_schemes` on the DAC:
  - functions and schemes are class rows in `sysclsobjs`;
  - boundary values are sql_variant-like values in `sysobjvalues`, typed by `sys.partition_parameters`;
  - scheme → function and scheme → filegroup links are in `syssingleobjrefs` / `sysmultiobjrefs`;
  - the index → scheme link is `sysidxstats.dataspace`;
  - the partitioning column is in `sysiscols`;
  - `sysprufiles` holds database files, which are unrelated.
- **(a) User parameter:** `partitioning := MAP {'dbo.orders': 'epoch_month(order_date)'}`, plus
  `sorting :=` and `target_file_size`.

### 7.3 The derived default

**Temporal boundaries** (date, datetime, datetime2; datetimeoffset in UTC):
- Choose the finest T such that every inner SQL Server partition lies within ONE T value.
  - RANGE RIGHT: boundaries are period starts.
  - RANGE LEFT: boundaries are period ends (`…-31`, `23:59:59.997`, `.9999999`).
- Examples:
  - monthly → `epoch_month`;
  - quarterly → `epoch_year`, because `epoch_month` would split partitions;
  - tiny daily partitions → coarser, to avoid tiny files.
- Each file has one value and no page splits. Only the open-ended first and last partitions are split
  by T, and only their pages get several runs.

**Other boundary types:** no DuckLake spec.
- Files stay per rowset, and min/max on the column prunes.
- Optionally, identity on a service column `_mssql_partition_low` (the lower boundary) keeps
  compaction inside partitions and survives SPLIT/MERGE of other ranges. The partition number would
  shift.
- Avoid `bucket`: it scatters a page over n files.

**Clustered key** → `SORTED BY (key)`; ordered CCI `ORDER (col)` → `SORTED BY (col)`; segment min/max
needs nothing.

**Writers:** one writer per (partition × range) unit rotates at the target size and holds one open
file per partition value.

### 7.4 Maintenance between backups (confirm in E7)

**SWITCH in or out** swaps rowset ownership.
- Data pages are untouched (no DCM bit, header AU ids unchanged); only catalog pages are in the diff.
- Rule: an AU whose owning (object, index, partition) changed in `__mssql_sys` → MOVE its rows, found
  by `page_map.au_id`. They come from the lake.
- DuckLake cannot move files:
  - `ducklake_data_file.table_id` is fixed and there is no move change;
  - expiring the source's ended row schedules the PATH for deletion without reference counting
    (`ducklake_metadata_manager.cpp:5476-5530`), so a shared path would be deleted under the target.
- So, with per-rowset files: object-store copy into the target's path, `add_data_files`, the same
  delete positions, then `DropFile` the sources. Otherwise `INSERT … SELECT` the pages' rows and
  delete them.

**SPLIT / MERGE RANGE.** Rows that change partition are physically moved, fully logged, so their pages
are in the diff and the general delete-then-insert applies. New files take the new boundaries. An
empty range is catalog-only.

**TRUNCATE … WITH (PARTITIONS (n)).** The AUs are deallocated or deferred-dropped (E2); their pages'
rows are deleted, and with per-rowset files those are whole-file drops.

## 8. Where things live, and worker connections

**mssql** (knows no lake):
- `mssql_backup_export(file, units :=, out :=, since_lsn :=, target_file_size :=, partition_values :=)`
  is a PARALLEL table function on DuckDB's scheduler.
  - Each thread writes its own files through the Parquet `CopyFunction` API, as DuckLake writes its
    delete files (`ducklake_delete.cpp:54-135`).
  - It returns one `WRITTEN_FILE_STATISTICS`-shaped row per file, plus (object, partition, unit,
    page-run file).
  - It also writes the changed/freed-page, pending-page and LOB-node side files.
- `mssql_backup_export_plan(file)` lists the units, so other processes or machines can take subsets.

**mssql-ducklake** (orchestration):
- `PRAGMA ducklake_import_mssql_backup(…)` expands into one query in one transaction (`DD/src/planner/statement_preprocessor.cpp:56-66`;
  `DD/src/main/client_context_execution.cpp:48-50`). Its callback reads the header and the
  provenance through a side `Connection` and generates the DDL.
- `mssql_ducklake_apply_backup(…)` runs the export on a side `Connection`, or takes manifests from
  workers, then registers into `DuckLakeTransaction::Get(context, catalog)` as add-files does.

**Worker connections are legal.** Precedents: `IB/iceberg_to_ducklake.cpp:941-967`,
`DL/storage/ducklake_checkpoint.cpp:15`, `DL/storage/ducklake_transaction.cpp:858`,
`DD/src/function/table/system/create_external_resource.cpp:177`.
- The rule: a side connection writes files only. Its lake writes would be another snapshot.
- Temp-table writes are exempt from the one-database rule (`DD/src/transaction/meta_transaction.cpp:259-284`).

## 9. Pipelines

```sql
-- once, lake-wide (or schema := … after the schemas exist)
CALL ducklake_set_option('lake', 'data_inlining_row_limit', 0);
CALL ducklake_set_option('lake', 'auto_compact', false);

-- FULL: one transaction, one snapshot
PRAGMA ducklake_import_mssql_backup('lake', 's3://bak/sales_full.bak',
       partitioning := MAP {'dbo.orders': 'epoch_month(order_date)'});
-- expands to:
CREATE SCHEMA lake.__mssql_sys;
CREATE SCHEMA lake.__mssql_backup;
CREATE SCHEMA lake.dbo;
CREATE TABLE lake.dbo.orders (id BIGINT NOT NULL, order_date TIMESTAMP, …,
       _mssql_rid BLOB, _mssql_page BIGINT, _mssql_lob_map …)
       PARTITIONED BY (epoch_month(order_date)) SORTED BY (id);
--   … every table, the __mssql_sys mirrors, page_map, pending_pages
SELECT * FROM mssql_ducklake_apply_backup('lake', 's3://bak/sales_full.bak', mode := 'full');
--   export of all units → AppendFiles: data, page_map, mirrors, pending pages
CALL ducklake_set_commit_message('lake', 'mssql-ducklake', 'full sales_full.bak',
       extra_info := '{"backup_set_guid": …, "type": "full", "first_lsn": …, "checkpoint_lsn": …,
                       "last_lsn": …, "backup_finish_date": …, "database": …, "file": …}');
INSERT INTO lake.__mssql_backup.imports VALUES (…);

-- DIFFERENTIAL: the callback checks the chain (DifferentialBaseGUID, W; E1 rule 1), diffs the
-- catalog pages against __mssql_sys (→ §6 DDL), and refreshes page-map rows of compacted files
PRAGMA ducklake_import_mssql_diff_backup('lake', 's3://bak/sales_diff_2026-11-05.bak');
-- expands to: DDL;
-- SELECT * FROM mssql_ducklake_apply_backup(…, since_lsn := W)
--     -- exports the new images of changed pages; positional deletes for changed / freed / dead-AU
--     -- pages in every table (§5) and SWITCH moves (§7.4); registers the files; updates
--     -- page_map, mirrors and pending pages
-- then the commit message and the provenance row
```

**Side state:**
- **Pending pages:** `__mssql_sys.pending_pages(page, xact_id, lsn, image BLOB)`, resolved by
  RESEARCH §4's rules with native deletes; the table is small.
- **LOB maps:** live next to the values (`_mssql_lob_map`). The old nodes a diff lacks are read from
  the lake by mssql-ducklake and passed to the export as a Parquet side input, so mssql stays
  lake-agnostic.
- **Mirrors:** use the same page mechanism.

**Snapshot guarantee:**
- Nothing is visible before the commit.
- A failed commit leaves only orphans, and the retry reuses the manifests.
- A commit that succeeded unseen is caught by `BackupSetGUID` in `imports`.

**Time travel:**
- `AT (VERSION => n)` is the database at that backup, as of CheckpointLSN, or LastLSN with redo.
- Backup time → snapshot: `max(snapshot_id)` from `ducklake_snapshots('lake')` with
  `backup_finish_date ≤ t`.
- Expired snapshots disappear from that list, so fall back to `imports` and its `snapshot_id`
  virtual column (`DL/../test/sql/virtualcolumns/ducklake_snapshot_id.test:45`).

## 10. Open items

- **E7:** the base tables behind `sys.partition_*`; SWITCH, SPLIT, MERGE and TRUNCATE PARTITIONS as
  seen in a diff.
- **Compaction:** whether `merge_adjacent_files` keeps each page's rows contiguous.
- **Commit cost:** measure the client-loop commit for 10k files and 1k tables.
- **Field ids:** write them and drop name maps; DuckLake's new-table ids are 1..n depth-first
  (`ducklake_field_data.cpp:93-121`).
