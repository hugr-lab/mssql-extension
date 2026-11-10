---
title: Backup Files
sidebar_position: 4
---

# Backup Files (.bak)

The extension reads SQL Server backup files itself, with no SQL Server instance: no `RESTORE`, no
server to connect to. The file is read only through DuckDB's file system, so with the httpfs
extension (and its secrets) an `s3://` or `https://` path is read the way a local one is. A glob
needs a file system that can list files: a local path or `s3://`, not `https://`.

This release describes a backup: `mssql_backup_info` reports what `RESTORE HEADERONLY`,
`RESTORE LABELONLY` and `RESTORE FILELISTONLY` report. Reading the tables inside a backup comes in
later releases.

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

The reader is checked against backups made by SQL Server 2019, 2022 and 2025, against what
`RESTORE HEADERONLY` / `LABELONLY` / `FILELISTONLY` print for them, field by field. SQL Server 2017
joins the same test matrix.

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

## Prepared statements

The file is read when the statement binds, and a prepared statement keeps what its `PREPARE`
read. `reread := true` reads the file again on every `EXECUTE`, for a file that is overwritten
between runs (a nightly backup to the same path):

```sql
PREPARE latest AS SELECT backup_finish_date FROM mssql_backup_info('/backups/nightly.bak', reread := true);
EXECUTE latest;
```
