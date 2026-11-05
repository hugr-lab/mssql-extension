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

1. **Write the files first, then commit once.** Parallel workers write the data Parquet. They can be
   threads, processes or machines, and they work outside any lake transaction. Then ONE transaction
   per backup file holds everything else: DDL, the deletes (DuckLake's `DELETE` scans), a SWITCH's
   lake-side copy, then file registration, `__mssql_sys`, the commit message and provenance. That
   transaction is the file's one snapshot; its length is the delete scans' and the copy's, not the
   export's.
2. **Register through DuckLake's own transaction, not by writing metadata rows.** mssql-ducklake
   compiles DuckLake in (its CLAUDE.md), so it can call `AppendFiles` without editing DuckLake.
   DuckLake keeps ids, row ids, stats and conflict checks, and mssql-ducklake's commit batch carries
   the result to SQL Server. v1 can be plain SQL (`ducklake_add_data_files` plus `DELETE`).
3. **Deletes are DuckLake's own `DELETE` by value.** Every row carries `_mssql_page`; per affected
   table, `DELETE … WHERE _mssql_page IN (<the table's delete set>)`, run before the file's new files
   are registered. DuckLake finds the positions and writes its own delete files. The table a page is
   deleted from is the one its allocation unit belonged to in the previous file, named by the lake's
   base IAM images (§5).
4. **Most catalog changes map to DuckLake ALTERs, with stable field ids.** Schema transfer,
   non-widening type changes and non-persisted computed columns need a rewrite inside the same
   commit.
5. **Partitioning:**
   - Files follow the lake's layout: its DuckLake partition values, sort order and file size (DQ-10);
     a SQL Server partition (rowset) is a work unit only.
   - A DuckLake transform is proposed at creation only where the boundaries align.
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
  - So an INSERT keeps neither physical order nor a page's rows together.
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
  3. deletes as in §5: DuckLake's own `DELETE`, in the same transaction, before step 2.

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

| | (A) `INSERT … SELECT FROM mssql_backup_scan`, one txn | **(B) workers write files, one commit** | (C) iceberg-style rows |
|---|---|---|---|
| Snapshots | 1 | **1** | 1 per source snapshot |
| Parallelism | within a table; big tables sequential (§3) | all units (partition × key or extent range) at once, any machine | n/a |
| Memory | threads × row group, plus sort spill | 1 row group per writer | small |
| Lake txn length | the WHOLE import (hours); a conflict at commit loses all | **the deletes, a SWITCH's copy and registration** | short |
| Failure | rollback deletes all files | per-unit retry; names `mssql-<BackupSetGUID>-<objid>-p<part>-u<unit>-<seq>.parquet`; orphans → `ducklake_delete_orphaned_files` | manual |
| Stats / ids | native | writer stats or footer; name map, or `FIELD_IDS` (`DD/extension/parquet/parquet_extension.cpp:149,210`) | hand-made |
| Order | unknown | **writer controls it**: physical order, a page's rows together (§5) | n/a |
| Commit path | client loop | client loop (server-side later) | bypasses DuckLake |

Inlining: `data_inlining_row_limit = 0` on every table the importer creates, as a table option, not
lake-wide (022 D6), so that the importer's native writes (phase A's provenance inserts, the `DELETE`s, a
SWITCH's `INSERT … SELECT`) land as files beside the data files, not as rows in the catalog database.

## 5. Deletes by `_mssql_page`

**The delete.** Every imported row carries `_mssql_page` and `_mssql_rid` (opaque BLOBs serialized by
the C++ code, DQ-02). Per affected lake table the import runs DuckLake's own
`DELETE FROM lake.s.t WHERE _mssql_page IN (SELECT page FROM <the delete set of t>)`. DuckLake's scan
finds the positions and its delete path writes the delete files (§3): cumulative against files of any
earlier import, a file dropped when every row goes. In the import's single transaction the DELETEs run
**before** the new files are registered (`ducklake_add_data_files` / `AppendFiles`), so a page's new
rows are never hit by the delete of that page. To verify: DELETE + `ducklake_add_data_files` in one
transaction give one snapshot.

**The delete set of a table**, from 089's `mssql_backup_changed_pages` (each page with its
`base_au_id` and that AU's owner in the base mirror, so the importer needs no IAM format knowledge):
- **A changed or freed page** is deleted from the table of the allocation unit that owned it in the
  previous file. The lake keeps the latest image of every allocation page (`__mssql_sys.alloc_pages`,
  GAM / SGAM / PFS / IAM; written for each file as 089's `base/allocation.parquet`, E2 R5; after a
  recovered file, the images of the recovered state, 090). Its IAM images, read along each AU's chain
  from its first IAM, give the AU that owned each extent, or each mixed-extent single-page slot, and
  the catalog mirror maps that AU to its rowset, partition and table. A page freed or reallocated to
  another AU is deleted from the table of its OLD AU only; a page that changed inside its AU, from
  that AU's table (E1 rule 5: pages move between tables).
- **A dropped table** is `DROP TABLE`.
- **A truncated table or partition** (AU replaced, deferred drop): the old AU's pages from the base IAM
  image (rowstore), or its rowgroup keys (columnstore).
- **A SWITCH** moves rows lake-side (§7.4).
- **Data pages only.** The set is 089's `changed`, `freed`, `dead_au`, `gone_au`, `dead_rowgroup` and
  090's `pending_applied` rows with `holds_rows`: pages of an `IN_ROW_DATA` AU of a heap or clustered
  index, delta-store pages and rowgroup keys. LOB and row-overflow pages reach their rows through the
  LOB map, and index and allocation pages hold no lake rows.

**Columnstore** (088 D3). A compressed row has no page: its `_mssql_page` is its rowgroup key,
`0x02 ‖ u64 rowset_id ‖ u32 rowgroup`, and its `_mssql_rid` adds `u32 tuple`; a delta-store row has
its delta page in the 0x01 form. The rowset id is in both because rowgroup ids are numbered per
partition and one lake table holds every partition. A removed, merged or rebuilt rowgroup is deleted
by `_mssql_page` like a page; a delete-bitmap entry for tuple t deletes `WHERE _mssql_page =
<rowgroup key> AND _mssql_rid IN (…)`, the rowgroup key pruning the files. The tuple mover shows up
as delta pages freed plus a new rowgroup.

**Physical order** (the writer contract of `mssql_backup_export`):
- one thread per file; a page's rows emitted together; a page splits only at a rotation or a DuckLake
  partition boundary;
- **clustered rowstore: leaf-chain order**, key order and page contiguity together. Split the work by
  the level-1 index pages, so each unit is a disjoint key range: tight key min/max, `SORTED BY (key)`;
- **heap: IAM order**, ascending page ids;
- **columnstore: rowgroup order.**

**The cost of a delete (expected, to verify).** A `DELETE` by `_mssql_page` should read only the
`_mssql_page` column of the files its min/max statistics cannot exclude. `_mssql_page` is
`0x01 ‖ u16 file ‖ u32 page`, big-endian, so within that shape byte order is page order; files
written in physical order hold contiguous pages, so a changed page should touch few files. A heap's
units are ascending page ranges, so its files cover disjoint ranges; a clustered index's leaf chain
follows the key, so a fragmented index gives files with overlapping page ranges, and a delete reads
more of them. A lake sort order (022 D5, DuckLake `SET SORTED BY`) scatters a page's rows over many
files and makes a delete scan more of them: a cost, never a wrong row. `skip_stats_columns` must not
include `_mssql_page`. Two checks, measured after N differentials and after a
`merge_adjacent_files`:
- DuckLake keeps min/max for a BLOB column (if not, the delete scans every file of the table; still
  correct);
- a `DELETE … WHERE _mssql_page IN (SELECT …)` prunes files at all (if not, the import runs one
  `DELETE` per contiguous page range with constant bounds, `_mssql_page BETWEEN x AND y` plus the
  `IN`).

**Compaction is DuckLake's.** Imported tables keep DuckLake's defaults. A compaction or a `CHECKPOINT`
that moves rows between files changes nothing for the importer, because nothing refers to file positions.
Compaction runs in a transaction of its own (`DL/storage/ducklake_transaction_state.cpp:421-423`), so a
concurrent one is another writer's snapshot, handled by DuckLake's own conflict rules.

## 6. Schema evolution, all inside the one commit

Field ids are DuckLake column ids: kept across RENAME and promotion, new on DROP + ADD. `__mssql_sys`
maps (object_id, colid) → (table_id, field id). Name-mapped files map a Parquet name to a field id,
so renames do not affect them.

| SQL Server | Lake | Rewrite |
|---|---|---|
| new / dropped table | CREATE (with PARTITIONED BY / SORTED BY) / DROP | no |
| rename (object_id stable) | `RENAME TABLE`, table_id kept (`ducklake_table_entry.cpp:1478`) | no |
| `ALTER SCHEMA … TRANSFER` | no SET SCHEMA in DuckDB (`DD/src/include/duckdb/parser/parsed_data/alter_table_info.hpp:72-92`): CREATE in the target, `INSERT … SELECT`, DROP | yes |
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
- One SQL Server partition gives one unit of writers, a work unit only: the files follow the lake's
  layout (DQ-10).
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
  by T, and only their pages can have rows in two files.

**Other boundary types:** no DuckLake spec.
- Files follow the lake's layout (DQ-10), and min/max on the column prunes.
- Optionally, identity on a service column `_mssql_partition_low` (the lower boundary) keeps
  compaction inside partitions and survives SPLIT/MERGE of other ranges. The partition number would
  shift.
- Avoid `bucket`: it scatters a page over n files.

**Clustered rowstore key** → `SORTED BY (key)`, only when every key column orders in DuckDB as on disk;
an ordered CCI's `ORDER (col)` proposes nothing (DQ-25: VertiPaq reorders a rowgroup's tuples, so the
export would sort them and spread them over files); segment min/max needs nothing.

**Writers:** one writer per (partition × range) unit rotates at the target size and holds one open
file per partition value.

### 7.4 Maintenance between backups (confirm in E7)

**SWITCH in or out** swaps rowset ownership.
- Data pages are untouched (no DCM bit, header AU ids and IAMs unchanged); only catalog pages are in
  the diff.
- Rule: an AU whose owning (object, index) changed in `__mssql_sys` → MOVE its rows. They come from
  the lake, found without anything stored per row: 089's `au_moved` says which AU moved from which
  table to which, and the moved rows are those whose `_mssql_page` is one of that AU's pages in the
  base IAM image (columnstore: whose rowgroup key carries the moved rowset id).
- DuckLake cannot move files:
  - `ducklake_data_file.table_id` is fixed and there is no move change;
  - expiring the source's ended row schedules the PATH for deletion without reference counting
    (`ducklake_metadata_manager.cpp:5476-5530`), so a shared path would be deleted under the target.
- So a SWITCH is a lake-side copy (DQ-10), in the import's transaction, before the new files:
  `INSERT INTO target SELECT … FROM source WHERE _mssql_page IN (moved pages minus the changed pages,
  which the delete set handles)`, then `DELETE FROM source WHERE _mssql_page IN (moved pages)`.
- Not by partition boundary values: a range predicate evaluated in DuckDB would compare a string key by
  DuckDB's order, not the column's collation. The pages are exact for every key type.

**SPLIT / MERGE RANGE.** Rows that change partition are physically moved, fully logged, so their pages
are in the diff and the general delete-then-insert applies. New files take the new boundaries. An
empty range is catalog-only.

**TRUNCATE … WITH (PARTITIONS (n)).** The AUs are deallocated or deferred-dropped (E2); their pages'
rows are deleted by `_mssql_page`, the pages taken from the old AU's base IAM image (§5). DuckLake drops
a file whose rows all go.

## 8. Where things live, and worker connections

**mssql** (knows no lake):
- `mssql_backup_export(file, units :=, out :=, since_lsn :=, partition_by :=, order_by :=,
  target_file_size :=)` is a PARALLEL table function on DuckDB's scheduler.
  - Each thread writes its own files through the Parquet `CopyFunction` API, as DuckLake writes its
    delete files (`ducklake_delete.cpp:54-135`).
  - It returns one `WRITTEN_FILE_STATISTICS`-shaped row per file, plus (object, partition, unit).
  - It writes data files only. The changed and freed pages come from `mssql_backup_changed_pages`,
    the pending pages from `mssql_backup_pending_pages` (090), and the `base/` side inputs,
    `lob.parquet` included, are written by the importer from the lake (089 D10).
- `mssql_backup_export_plan(file)` lists the units, so other processes or machines can take subsets.

**mssql-ducklake** (orchestration):
- `PRAGMA ducklake_import_mssql_backup(…)` expands into one query in one transaction (`DD/src/planner/statement_preprocessor.cpp:56-66`;
  `DD/src/main/client_context_execution.cpp:48-50`). Its callback reads the header and the
  provenance through a side `Connection` and generates the DDL.
- `ducklake_mssql_backup_export(…)` runs the export on a side `Connection` (or the PRAGMA takes
  manifests from workers); `ducklake_mssql_backup_apply(…)` then registers into
  `DuckLakeTransaction::Get(context, catalog)` as add-files does (022 D1).

**Worker connections are legal.** Precedents: `IB/iceberg_to_ducklake.cpp:941-967`,
`DL/storage/ducklake_checkpoint.cpp:15`, `DL/storage/ducklake_transaction.cpp:858`,
`DD/src/function/table/system/create_external_resource.cpp:177`.
- The rule: a side connection writes files only. Its lake writes would be another snapshot.
- Temp-table writes are exempt from the one-database rule (`DD/src/transaction/meta_transaction.cpp:259-284`).

## 9. Pipelines

```sql
-- inlining is off per table: every CREATE TABLE the importer runs carries data_inlining_row_limit = 0
-- (022 D6); the lake-wide default stays the user's

-- FULL: one transaction, one snapshot
PRAGMA ducklake_import_mssql_backup('lake', 's3://bak/sales_full.bak',
       partitioning := MAP {'dbo.orders': 'epoch_month(order_date)'});
-- the callback plans at S0 (what ducklake_mssql_backup_plan shows) and writes plan.json; expands to:
CREATE TEMP TABLE __mssql_backup_manifest_<run> AS     -- the export, outside any lake transaction
       SELECT * FROM ducklake_mssql_backup_export('lake', 's3://bak/sales_full.bak', plan := …);
SELECT * FROM ducklake_mssql_backup_validate('lake', 's3://bak/sales_full.bak', plan := …);
--   the transaction's first statement: re-validates the plan at S1 before any DDL
CREATE SCHEMA lake.__mssql_sys;
CREATE SCHEMA lake.__mssql_backup;
CREATE SCHEMA lake.dbo;
CREATE TABLE lake.dbo.orders (id BIGINT, order_date TIMESTAMP, …, _mssql_rid BLOB, _mssql_page BLOB)
       PARTITIONED BY (epoch_month(order_date)) SORTED BY (id)
       WITH (data_inlining_row_limit = 0, skip_stats_columns = ['_mssql_rid']);
--   … every table, the __mssql_sys mirrors, alloc_pages, pending_pages, stale_pages, the registry
SELECT * FROM ducklake_mssql_backup_apply('lake', 's3://bak/sales_full.bak',
       manifest := '__mssql_backup_manifest_<run>', plan := …);
--   AppendFiles: data, mirrors, allocation images, pending pages, the provenance rows; the commit
--   message with extra_info {"backup_set_guid": …, "type": "full", "first_lsn": …, …}

-- DIFFERENTIAL: the callback checks the chain (DifferentialBaseGUID, W; E1 rule 1), diffs the
-- catalog pages against __mssql_sys (→ §6 DDL) and writes each table's delete set (§5)
PRAGMA ducklake_import_mssql_diff_backup('lake', 's3://bak/sales_diff_2026-11-05.bak');
-- expands to:
--   CREATE TEMP TABLE … AS SELECT * FROM ducklake_mssql_backup_export(…);  -- changed pages, W
--   SELECT * FROM ducklake_mssql_backup_validate(…);  -- first in the transaction: S1 against S0
--   DDL;
--   DELETE FROM lake.s.t WHERE _mssql_page IN (<t's delete set>);  -- per table (§5)
--   SWITCH moves (§7.4);
--   SELECT * FROM ducklake_mssql_backup_apply(…);  -- registers the files; the mirrors, allocation
--                                                  -- images and pending pages; provenance, message
```

**Side state:**
- **Pending pages:** `__mssql_sys.pending_pages`, 090's `mssql_backup_pending_pages` rows (`file_id`,
  `page_id`, `page`, `xids`, `lsn`, `image`) plus `import_seq`, the import that produced them; resolved
  by the reader at the next file (090, RESEARCH §4's rules); the table is small.
- **LOB maps:** live next to the values (`_mssql_lob`). The old nodes a diff lacks are read from
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
- **Deletes:** DuckLake keeps min/max for a BLOB column (`_mssql_page`), and a DELETE whose page set
  arrives as a subquery prunes files by it (else one DELETE per contiguous page range, with constant
  bounds `_mssql_page BETWEEN x AND y` plus the `IN`); measured after N differentials and after a
  `merge_adjacent_files`; DELETE + `ducklake_add_data_files` in one transaction give one snapshot.
- **Commit cost:** measure the client-loop commit for 10k files and 1k tables.
- **Field ids:** write them and drop name maps; DuckLake's new-table ids are 1..n depth-first
  (`ducklake_field_data.cpp:93-121`).
