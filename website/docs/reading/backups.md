---
title: Backup Files
sidebar_position: 4
---

# Backup Files (.bak)

The extension reads SQL Server backup files itself, with no SQL Server instance: no `RESTORE`, no
server to connect to. The file is read only through DuckDB's file system, so with the httpfs
extension (and its secrets) an `s3://` or `https://` path is read the way a local one is. A glob
needs a file system that can list files: a local path or `s3://`, not `https://`.

`mssql_backup_info` describes a backup: what `RESTORE HEADERONLY`, `RESTORE LABELONLY` and
`RESTORE FILELISTONLY` report. `mssql_backup_tables` and `mssql_backup_views` list what a full
backup's database holds: its tables, their columns and types, and its views
([below](#tables-and-views)). Reading the rows of a table comes in a later release.

```sql
SELECT backup_type, database_name, software_version, compatibility_level,
       collation_name, recovery_model, backup_size, compressed_size
FROM mssql_backup_info('/backups/sales_full.bak');
```

| backup_type | database_name | software_version | compatibility_level | collation_name | recovery_model | backup_size | compressed_size |
|---|---|---|---|---|---|---|---|
| full | sales | 17.0.4075 | 170 | SQL_Latin1_General_CP1_CI_AS | FULL | 4415488 | 589489 |

## What it reads

- **Full, differential and log backups** (`.bak`, `.trn`), copy-only or not.
- **Compressed backups**, `MS_XPRESS` (the default `WITH COMPRESSION`) and `ZSTD` (SQL Server 2025).
- **Striped backups** (`BACKUP … TO DISK = 'a.bak', DISK = 'b.bak'`): pass every file of the set,
  as a list or a glob, in any order.

```sql
-- A backup striped over two files
SELECT backup_type, family_count, compression_algorithm
FROM mssql_backup_info(['/backups/sales_1.bak', '/backups/sales_2.bak']);

-- The same with a glob
SELECT * FROM mssql_backup_info('/backups/sales_*.bak');

-- The database's files, one row each
SELECT f.logical_name, f.type, f.filegroup_name, f.size_bytes
FROM (SELECT unnest(files) AS f FROM mssql_backup_info('/backups/sales_full.bak'));
```

The reader is compared, field by field, with what `RESTORE HEADERONLY` / `LABELONLY` /
`FILELISTONLY` print: on synthetic backups made by SQL Server 2025, on every change, and, when the
project's oracle lane runs (it needs the repository's storage credentials, so not for a pull request
from a fork), on each version's backups made by SQL Server 2017, 2019, 2022 and 2025. Each version
makes what it can: ZSTD compression on 2025 only, `MS_XPRESS` named from 2022 and plain
`WITH COMPRESSION` (the same algorithm) before.

## What is refused

Each of these is refused by name, before any value is returned:

- **An incomplete striped set.** Given one file of a two-file set, the error names the missing
  family: `… is one family of a media set of 2: the families with FamilySequenceNumber 2 are
  missing (give every file of the set)`.
- **Files of different backups**, or a compressed file mixed with an uncompressed one.
- **An encrypted backup** (`WITH ENCRYPTION`). Decryption is planned for a later release.
- **A file holding more than one backup set** (appended with `NOINIT`), even with `position = 1`;
  and any `position` other than `1`.
- **A compressed format the reader does not know** (`QAT_DEFLATE`, or an encrypted `ZSTD`
  backup): refused naming the container's flag word.
- **A database file of a type other than data or log** in the file list (FILESTREAM, full-text):
  refused naming the file id.
- **A file that is not a well-formed backup.** The headers the reader uses are checked (their
  checksums, lengths and offsets), and one that does not fit is an error naming the file, not a
  guess.

## Columns

`mssql_backup_info` returns one row: the backup set the files hold.

| column | type | meaning |
|---|---|---|
| `position` | `INTEGER` | the backup set's position on the media (RESTORE's `FILE = n`) |
| `backup_type` | `VARCHAR` | `full`, `differential` or `log` |
| `is_copy_only` | `BOOLEAN` | `BACKUP … WITH COPY_ONLY` |
| `backup_set_guid` | `UUID` | the backup set's GUID |
| `first_lsn`, `last_lsn`, `checkpoint_lsn`, `database_backup_lsn` | `DECIMAL(25,0)` | as `RESTORE HEADERONLY` prints them |
| `differential_base_lsn`, `differential_base_guid` | `DECIMAL(25,0)`, `UUID` | a differential's base; NULL for any other backup |
| `database_name`, `server_name` | `VARCHAR` | |
| `database_version`, `compatibility_level`, `collation_name` | `INTEGER`, `INTEGER`, `VARCHAR` | the database's, when it was backed up |
| `family_guid`, `recovery_fork_id`, `first_recovery_fork_id`, `fork_point_lsn`, `binding_id` | `UUID`, `UUID`, `UUID`, `DECIMAL(25,0)`, `UUID` | the database's identity and recovery fork; `fork_point_lsn` is set only for a log backup that spans a fork point (then `first_recovery_fork_id` is the parent fork), as in `RESTORE HEADERONLY` |
| `backup_name`, `description`, `user_name`, `machine_name`, `software_version` | `VARCHAR` | `software_version` is the server's, e.g. `17.0.4075` (only `17.0` when the backup does not record the build) |
| `database_creation_date`, `backup_start_date`, `backup_finish_date` | `TIMESTAMP` | the server's local time, as `RESTORE HEADERONLY` prints them |
| `media_set_guid`, `family_count`, `family_sequence_number` | `UUID`, `INTEGER`, `INTEGER` | the media set; `family_sequence_number` only when one file is given |
| `compressed`, `compression_algorithm` | `BOOLEAN`, `VARCHAR` | `MS_XPRESS` or `ZSTD`; NULL for an uncompressed backup |
| `encrypted`, `encryptor_thumbprint` | `BOOLEAN`, `BLOB` | always false / NULL: an encrypted backup is refused |
| `has_backup_checksums` | `BOOLEAN` | `BACKUP … WITH CHECKSUM` |
| `backup_size`, `compressed_size` | `BIGINT` | `BackupSize` / `CompressedBackupSize`; equal for an uncompressed backup; `compressed_size` NULL when the backup does not record it |
| `page_count` | `BIGINT` | NULL for now |
| `log_tail_bytes` | `BIGINT` | the bytes of log the backup carries |
| `consistent`, `open_transactions`, `committed_after_checkpoint` | `BOOLEAN`, `INTEGER`, `INTEGER` | NULL for now |
| `recovery_model` | `VARCHAR` | `FULL`, `BULK_LOGGED` or `SIMPLE` |
| `files` | `LIST(STRUCT(…))` | the database's files: `logical_name`, `physical_name`, `type` (`data` / `log`), `file_id`, `filegroup_id`, `filegroup_name`, `size_bytes`, `max_size_bytes`, `create_lsn`, `unique_id`, `backup_size_bytes` (NULL for now) |

## Tables and views

`mssql_backup_tables(path)` lists the user tables of a full backup, one row each, read from the
database's own catalog inside the file; `mssql_backup_views(path)` its views with their T-SQL
definitions. A log backup holds no data pages and is refused; a differential is read over its full
backup, which comes in a later release, and is refused too.

```sql
SELECT schema_name, table_name, shape, row_count, decodable, reason
FROM mssql_backup_tables('/backups/sales_full.bak');

-- One table's columns: the SQL Server type and the DuckDB type the extension reads it as
SELECT c.name, c.sql_type, c.duckdb_type, c.collation, c.is_nullable
FROM (SELECT unnest(columns) AS c FROM mssql_backup_tables('/backups/sales_full.bak')
      WHERE schema_name = 'sales' AND table_name = 'orders');

SELECT schema_name, view_name, definition FROM mssql_backup_views('/backups/sales_full.bak');
```

`mssql_backup_tables` columns:

| column | type | meaning |
|---|---|---|
| `schema_name`, `table_name`, `object_id` | `VARCHAR`, `VARCHAR`, `INTEGER` | |
| `shape` | `VARCHAR` | `heap`, `clustered` or `columnstore` |
| `compression` | `VARCHAR` | `none`, `row`, `page`, `columnstore`, `columnstore_archive` (`level n` for a level the reader does not name); `mixed` when partitions differ |
| `partition_count`, `row_count` | `INTEGER`, `BIGINT` | the row count the catalog keeps (`sys.partitions.rows`); NULL when a partition's count is negative (an upgraded database can hold one until `DBCC UPDATEUSAGE`) |
| `created`, `modified` | `TIMESTAMP` | `sys.tables.create_date` / `modify_date` |
| `decodable`, `reason` | `BOOLEAN`, `VARCHAR` | whether a later release's scan reads the table, and why not (a clustered columnstore, ROW / PAGE compression, sparse columns, a partitioned table, for now) |
| `fuzzy_status`, `open_transactions`, `stale_transactions` | `VARCHAR`, `INTEGER`, `INTEGER` | NULL for now |
| `columns` | `LIST(STRUCT(…))` | `column_id`, `name`, `sql_type` (as T-SQL declares it, e.g. `nvarchar(40)`), `system_type_id`, `max_length`, `precision`, `scale`, `collation`, `code_page`, `is_nullable`, `is_identity`, `is_computed`, `is_sparse`, `duckdb_type` (the type `TYPE mssql` reads the column as, with `mssql_catalog_native_types` off: a plain `VARCHAR` for a string), `decodable`, `reason` |
| `allocation_units` | `LIST(STRUCT(…))` | the table's allocation units: `au_id`, `type` (`IN_ROW_DATA`, `LOB_DATA`, `ROW_OVERFLOW_DATA`), `rowset_id`, `index_id`, `partition_number`, `compression`, `filegroup_id`, `first_page`, `root_page`, `first_iam_page`, `live_pages` (NULL for now) |
| `partitioning` | `STRUCT(…)` | NULL for now |

`mssql_backup_views` columns: `schema_name`, `view_name`, `object_id`, `definition` (the T-SQL text
as stored; NULL for a view created `WITH ENCRYPTION`, which the reader does not de-obfuscate),
`is_encrypted`, `created`, `modified`, and `reason`: why a definition could not be read, NULL
otherwise.

The oracle lane compares both with the server's own catalog of the same database (`sys.tables`,
`sys.columns`, `sys.sql_modules`): per table its id, shape, partitions, rows and dates, per column
its name, type, length, precision, scale, collation and flags, per view its definition (length and
SHA-256), on backups made by SQL Server 2017 to 2025.

### The catalog's base tables

`mssql_backup_catalog(path, base_table)` returns a system base table of the database as stored,
for a tool that mirrors the catalog: its rows, its columns typed by its own description (a
`sql_variant` column as its raw bytes), and two virtual columns that `SELECT *` leaves out:
`_mssql_page`, the row's page (`'01'`, then the file and page number in hex), and `_mssql_rid`, the
row's clustered key. With no `base_table` it lists the tables it serves: `sysallocunits`,
`sysrowsets`, `sysschobjs`, `syscolpars`, `sysrscols`, `sysseobjvalues`, `sysclsobjs`,
`sysobjvalues`, `sysidxstats`, `sysiscols`, `systypedsubobjs`, `syssingleobjrefs` and
`sysscalartypes`. Any other is refused by name. `sysobjvalues` gives only the definitions of views
(not encrypted ones), defaults and the computed columns of tables, and partition boundaries -- never
the source of a procedure, function or trigger -- and `sysseobjvalues` only the defaults of columns
added after rows were written; the listing's `row_count` is NULL for these two.

```sql
SELECT * FROM mssql_backup_catalog('/backups/sales_full.bak');
SELECT _mssql_page, id, name, type FROM mssql_backup_catalog('/backups/sales_full.bak', 'sysschobjs');
```

## Prepared statements

`mssql_backup_info` reads the file when the statement binds, and a prepared statement keeps what
its `PREPARE` read (`mssql_backup_tables`, `mssql_backup_views` and `mssql_backup_catalog` read it on
every execution). `reread := true` reads the file again on every `EXECUTE`, for a file that is overwritten
between runs (a nightly backup to the same path):

```sql
PREPARE latest AS SELECT backup_finish_date FROM mssql_backup_info('/backups/nightly.bak', reread := true);
EXECUTE latest;
```
