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
([below](#tables-and-views)). `mssql_backup_scan` reads a table's rows ([below](#rows)), and
`ATTACH … (TYPE mssql_backup)` makes the backup a read-only catalog ([below](#attaching-a-backup)).

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
- **A file, filegroup or partial backup**, and its differential (`BACKUP DATABASE … FILEGROUP = …`,
  `READ_WRITE_FILEGROUPS`), for reading: it leaves data files out, and those files' pages in its full
  are of another time (a read/write filegroup is consistent with the rest of the database only
  through the log). The error names what it carries and what it does not. `mssql_backup_info` reads
  it, with `is_present` false on the files it leaves out.
- **A file that is not a well-formed backup.** The headers the reader uses are checked (their
  checksums, lengths and offsets), and one that does not fit is an error naming the file, not a
  guess.

## Columns

`mssql_backup_info` returns one row: the backup set the files hold.

| column | type | meaning |
|---|---|---|
| `position` | `INTEGER` | the backup set's position on the media (RESTORE's `FILE = n`) |
| `backup_type` | `VARCHAR` | `full`, `differential` or `log`; `file` / `file differential` for a file, filegroup or partial backup (one that leaves data files out: HEADERONLY's types 4 / 6, and 7 / 8 not told apart from them) |
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
| `consistent`, `open_transactions`, `committed_after_checkpoint`, `rolled_back_after_checkpoint` | `BOOLEAN`, `INTEGER`, `INTEGER`, `INTEGER` | from the backup's log tail (see [A backup taken under writes](#a-backup-taken-under-writes)): no transaction open at the backup's end and none committed or rolled back after its checkpoint; the transactions open, and those committed / rolled back with a row change after the checkpoint (a rolled-back one makes `consistent` false: whether a page copy holds what it undid is the tables' `fuzzy_status`). NULL for a log backup, and with a WARNING when the tail cannot be read whole |
| `recovery_model` | `VARCHAR` | `FULL`, `BULK_LOGGED` or `SIMPLE` |
| `files` | `LIST(STRUCT(…))` | the database's files: `logical_name`, `physical_name`, `type` (`data` / `log`), `file_id`, `filegroup_id`, `filegroup_name`, `size_bytes`, `max_size_bytes`, `create_lsn`, `unique_id`, `backup_size_bytes` (NULL for now), `is_present` (FILELISTONLY's IsPresent: the file is in this backup) |

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
| `partition_count`, `row_count` | `INTEGER`, `BIGINT` | the row count the catalog keeps (`sys.partitions.rows`); NULL when a partition's count is negative (an upgraded database can hold one until `DBCC UPDATEUSAGE`). For a clustered columnstore it includes the delta stores the tuple mover has not dropped yet, as SQL Server 2022 counts them (2025 does not); exact once columnstores are read |
| `created`, `modified` | `TIMESTAMP` | `sys.tables.create_date` / `modify_date` |
| `decodable`, `reason` | `BOOLEAN`, `VARCHAR` | whether `mssql_backup_scan` reads the table, and why not (a clustered columnstore, for now; ROW and PAGE compression are read) |
| `fuzzy_status`, `open_transactions`, `stale_transactions` | `VARCHAR`, `INTEGER`, `INTEGER` | `consistent`, `stale`, `dirty` or `stale+dirty` (see [A backup taken under writes](#a-backup-taken-under-writes)); the transactions whose open changes the table's page copies hold, and those whose finished changes they lack |
| `columns` | `LIST(STRUCT(…))` | `column_id`, `name`, `sql_type` (as T-SQL declares it, e.g. `nvarchar(40)`), `type_schema` (the schema of the column's type: `sys` for a system type, the one an alias or CLR type was created in), `system_type_id`, `max_length`, `precision`, `scale`, `collation`, `code_page`, `is_nullable`, `is_identity`, `is_computed`, `is_sparse`, `duckdb_type` (the type the scan returns: the one `TYPE mssql` reads the column as, with `mssql_catalog_native_types` off -- a plain `VARCHAR` for a string --, or for now `BLOB`, its stored bytes, for `sql_variant`, `xml`, `geometry` / `geography`, `hierarchyid`, `vector` and `json`, which a later release converts), `decodable` (false only for a column the scan cannot read: a computed one not persisted, a sparse one), `reason` (why a column is not converted or not readable) |
| `allocation_units` | `LIST(STRUCT(…))` | the table's allocation units: `au_id`, `type` (`IN_ROW_DATA`, `LOB_DATA`, `ROW_OVERFLOW_DATA`), `rowset_id`, `index_id`, `partition_number`, `compression`, `filegroup_id`, `first_page`, `root_page`, `first_iam_page`, `live_pages` (NULL for now) |
| `partitioning` | `STRUCT(…)` | for a partitioned table (NULL on a filegroup): `function_name`, `scheme_name`, `column_name`, `column_id`, `parameter_type`, `range_right`, `boundaries` (a list of `VARIANT`s in the parameter's type, in boundary order; a NULL boundary is a NULL element), `destinations` (the filegroup of each partition), `next_used`, and `reason` when the reader cannot give it exactly (the boundaries are then NULL, never a partial list; the destinations and `next_used` are still given when only a boundary's value could not be read). The table's rows are read either way |
| `partitions` | `LIST(STRUCT(…))` | every partition: `partition_number`, `rows`, `compression`, `filegroup` |

`mssql_backup_schemas(path)` lists the database's schemas as `sys.schemas` does (the system schemas
and the roles' included): `schema_id`, `schema_name`, and the `table_count` and `view_count` of user
tables and views in each.

`mssql_backup_columns(path [, schema [, table]])` lists every column of the user tables, one row
each, with its state at a glance: `schema_name`, `table_name`, `column_id`, `column_name`,
`sql_type` (as declared: an alias type by its own name, e.g. AdventureWorks' `Name`), `type_schema` (its schema, `dbo` for that `Name`; `sys` for a system type), `system_type`
(the type underneath, declared: `nvarchar(50)`; NULL for a user-defined CLR type), `duckdb_type` (the
type the scan returns a column it reads as; the declared type's otherwise), `state` and `reason`.
The states:

| state | meaning |
|---|---|
| `converted` | read as `duckdb_type` |
| `stored_bytes` | read as its stored bytes (`BLOB`) until its type's conversion comes: `sql_variant`, `xml`, `geometry` / `geography`, `hierarchyid`, `vector`, `json` |
| `not_stored` | a computed column that is not persisted: it has no stored value |
| `storage_format` | a sparse column or a column set: its value is in the row's sparse vector, read in a later release |

A value stored off row -- `text` / `ntext` / `image`, a MAX type's long value, a value pushed off a
row past 8,060 bytes -- is read like one in row, so it has no state of its own. The `schema` and
`table` arguments are matched as stored (a name that is not there lists nothing). A column's state is its own: a table that `mssql_backup_tables` says is not read (a
columnstore, for now) keeps its columns' states and says so in each
`reason`, so a query for what will not be read asks for both:

```sql
SELECT table_name, column_name, state, reason
FROM mssql_backup_columns('/backups/sales_full.bak') WHERE state <> 'converted' OR reason IS NOT NULL;
```

A filter on `schema_name`, `table_name` or `view_name` -- `=` a name, `IN` a list, or an `OR` of
those -- makes the listings (and `mssql_backup_columns`) describe only the objects it names: on a
database of thousands of tables, `WHERE table_name = 'orders'` reads the system tables but builds
one table's columns, layout and partitioning. The rows are the same either way, but for one bound: a
call reads at most 1 GiB of views' definitions stored off row, so a narrowed call can give a
definition that the whole listing, past that bound, gives as NULL with its reason. `EXPLAIN` shows
what a call describes (`Describes`).

`mssql_backup_views` columns: `schema_name`, `view_name`, `object_id`, `definition` (the T-SQL text
as stored; NULL for a view created `WITH ENCRYPTION`, which the reader does not de-obfuscate),
`is_encrypted`, `created`, `modified`, `reason`: why a definition could not be read, NULL
otherwise; and `is_indexed`: true for an indexed view (one with a unique clustered index, which
stores its rows), with its `shape`, `compression`, `row_count`, and `decodable` / `scan_reason`
(whether `mssql_backup_scan` reads them, and why not) -- all NULL for a view that stores no rows
(`row_count` also when the count is unknown).

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

## Rows

`mssql_backup_scan(path, schema, table)` reads a user table's rows from a full backup, typed as
`TYPE mssql` reads them over a connection (`mssql_catalog_native_types` off: a plain `VARCHAR` for a
string). Every collation is converted, the double-byte code pages (Japanese, Chinese, Korean)
included. A type whose conversion comes in a later release -- `sql_variant`, `xml`, `geometry` /
`geography`, `hierarchyid`, `vector`, `json` -- is returned as its stored bytes, a `BLOB`; no column
is refused for its type. A column the scan cannot read -- a computed column that is not persisted (no
stored value) or a sparse column -- is left out of `SELECT *`; a query that names it is refused,
with the reason (`mssql_backup_tables.columns`).

Values stored off row are read like the others: `text` / `ntext` / `image`, a `varchar(max)` /
`nvarchar(max)` / `varbinary(max)` / `xml` value too long for its row, and a value a row past 8,060
bytes pushed off (row-overflow), up to 2^31 − 1 bytes a value. Each is assembled from the pieces
SQL Server stored it in and checked as it is: every piece holds at least the bytes the value's tree
says it does, lies in the partition's own LOB or row-overflow pages, and is used once. A value is
reserved before it is read, within half of `max_memory` (below); one of a mebibyte or more is copied
or decoded straight into its result.

An indexed view is read the same way: `mssql_backup_scan(path, schema, view)` returns the rows its
unique clustered index stores (`_mssql_rid` its clustered key), the rows `SELECT … WITH (NOEXPAND)`
returns on the server. A view without one stores no rows and is refused by name. Its columns are
the scan's (`DESCRIBE`); `mssql_backup_columns` lists the tables'.

The scan runs on DuckDB's threads (`threads`); `max_threads := n` uses fewer for one call (a value
above `threads` is clamped to it, with a warning in `duckdb_logs`). `max_memory := '2GB'` bounds
the memory one call reserves for decoding to half of it (default `memory_limit`, which it cannot
exceed): tasks wait for each other's memory, and a table whose rows cannot be decoded within it is
refused by name. Rows come in page order, which DuckDB keeps where insertion order is preserved.

```sql
SELECT * FROM mssql_backup_scan('/backups/sales_full.bak', 'sales', 'orders');

-- On two threads at most
SELECT count(*) FROM mssql_backup_scan('/backups/sales_full.bak', 'sales', 'orders', max_threads := 2);

-- Import into DuckDB
CREATE TABLE orders AS SELECT * FROM mssql_backup_scan('/backups/sales_full.bak', 'sales', 'orders');
```

Four virtual columns, which `SELECT *` leaves out:

| column | type | meaning |
|---|---|---|
| `_mssql_page` | `VARCHAR` | the row's page: `'01'`, then its file and page number in hex (14 characters) |
| `_mssql_rid` | `BLOB` | the row's identity: a heap row's original place (its `%%physloc%%`, after a byte `0x00`), a clustered row's key (after `0x01`), stable across index rebuilds |
| `_mssql_page_lsn` | `DECIMAL(25,0)` | the LSN of the page's last change, in `RESTORE HEADERONLY`'s numeric form |
| `_mssql_lob` | `BLOB` | the row's values stored off row and the pages that hold them -- every one of the row's, whichever columns are selected -- in a format of the reader's own (versioned by its first byte), for a later differential backup's import; NULL for a row with none (a `text` / `ntext` / `image` value updated to NULL keeps its place off row, so its row has a map). It maps a value's tree as the row points to it: a value read as NULL because a page of it became live while the backup ran (see [A backup taken under writes](#a-backup-taken-under-writes)) keeps its entry when only its data, not its tree, is on that page |

A ROW- or PAGE-compressed table, or partition, is read like an uncompressed one: the same values, the
same `_mssql_rid`; a PAGE page's values are rebuilt from its anchor record and dictionary, so a row reads
alike on any page, and its changes between two backups (`mssql_backup_table_changes`) are compared by
value, not by its compressed bytes -- a table rebuilt whole is no change. Refused by name, for now: a
table with a clustered columnstore index
(see `mssql_backup_tables.reason`); a column the scan cannot read, when it is named (above); a
value stored off row that cannot fit in half of `max_memory` even with nothing else of the scan
held, or in DuckDB's memory (named by its row's `_mssql_rid`, its page and slot, and its columns and
sizes), or whose stored pieces do not form its tree. A backup taken under writes is read as its data pages hold it, which may differ
from what `RESTORE` makes of it by replaying the log tail: each table says whether it does (next section).

The oracle lane restores every fixture backup of SQL Server 2017 to 2025 and compares each table's
rows with `mssql_backup_scan`'s, both ways, through `TYPE mssql`.

## A backup taken under writes

A backup of a database that was being written holds each page as it was when the backup copied it,
written by transactions finished or not, and the log from its checkpoint to its end (its *tail*). `RESTORE … WITH RECOVERY`
replays that log: it redoes what was committed after the pages were copied and undoes what was still
open. This reader does not replay it (yet). It reads the tail and says, table by table, where its rows
can differ from the restored database's:

- `dirty`: a transaction still open at the backup's end changed the table, and the copies of its
  pages hold that change (`RESTORE` undoes it);
- `stale`: a committed transaction changed the table after its pages were copied, or on a page the
  backup does not hold (`RESTORE` redoes it); or a rolled-back one changed a page before its copy and
  undid it after (`RESTORE` redoes the undo). A rolled-back transaction whose changes and undo are all
  before a page's copy, or all after it, leaves the copy as `RESTORE` has it;
- `stale+dirty`: both; `consistent`: neither.

```sql
SELECT table_name, fuzzy_status, open_transactions, stale_transactions
FROM mssql_backup_tables('/backups/busy.bak') WHERE fuzzy_status IS DISTINCT FROM 'consistent';
```

`mssql_backup_info` says it for the whole database: `consistent` when no transaction was open at the
end and none committed or rolled back after the checkpoint. A table the status does not name is read as the
restored database holds it in its rows' changes; one it names may differ in the rows those transactions
changed. (The query above keeps a NULL status: the tail could not be read, so nothing is known.)

The status counts the log's row changes -- inserts, deletes and updates of rows, a rollback's undo
included. What the log records otherwise is not counted yet: a bulk load logged as pages rather than
rows, a `TRUNCATE TABLE`, a partition switch and other DDL run while the backup ran leave the tables
they touched `consistent` (for now), though `RESTORE` would give them as those left them.

What a scan does with a table that is not consistent is `fuzzy :=` on `mssql_backup_scan`, the `fuzzy`
option of `ATTACH … (TYPE mssql_backup)` for its tables, or the `mssql_backup_fuzzy` setting:

| `fuzzy` | |
|---|---|
| `'warn'` (default) | read it; one WARNING in `duckdb_logs` naming the table and its status |
| `'error'` | refuse it by name -- what an importer that wants only exact rows asks for |
| `'ignore'` | read it, saying nothing |

The status is the one that held on every table of the measurements it was designed from (E3: a flag
from the log tail, not the pages' LSNs, which miss a page copied before it changed), and the oracle
lane checks it against `RESTORE` on every version: every table that differs from the restored one is
flagged, and no table equal to it is. When the tail cannot be read, the listings give NULL for the
status with a WARNING, and a scan with `fuzzy := 'error'` is refused.

A page the database took into use while the backup ran has no copy in the data the backup carries: its
content is only in the backup's log tail, which the reader does not replay yet. Its table is never
`consistent`. Its other pages are read as for any table that is not -- what was committed when the
backup began is in them -- and the page is passed over: with a WARNING naming it under `'warn'`,
silently under `'ignore'`; `'error'` refuses the table by name. A value stored off row (a `varchar(max)`
past the row, say) on such a page is read as NULL and its row read, with a WARNING of its own under
`'warn'`. `RESTORE` holds its rows and values, and so will the reader once it replays the tail.

### The log a backup carries

The tail's transactions and records can be read too, with no server, as `fn_dump_dblog` lists them --
a data backup's tail, or a log backup's whole log:

```sql
-- each transaction: its name, its state at the backup's end, the tables it changed and its effect on them
SELECT transaction_id, name, status, after_checkpoint, tables
FROM mssql_backup_transactions('/backups/busy.bak');

-- each record, its contents a VARIANT: an inserted or deleted row's columns, a modified row's bytes
SELECT lsn, operation, table_name, contents
FROM mssql_backup_transaction_records('/backups/busy.bak') WHERE transaction_status = 'open';
```

`mssql_backup_transactions` has one row per transaction: `transaction_id` (as `fn_dblog` prints it),
`name` (`user_transaction`, `UPDATE`, a name given to `BEGIN TRANSACTION`), `status` (`open`,
`committed`, `rolled back`, or `no begin`: in a data backup's tail the system's work, in a log backup also
a transaction begun in an earlier log), `begin_lsn`, `end_lsn`, `records`,
`row_operations`, `after_checkpoint` (a row changed after the backup's checkpoint), and `tables`, each
with its `schema_name`, `table_name`, `index_id`, `allocation_unit`, the `pages` it changed and its
`effect` (`dirty`, `stale`, or NULL when the page copies are what `RESTORE` gives).

`mssql_backup_transaction_records` has one row per record: `lsn`, `previous_lsn` (the transaction's
record before it), `operation` and `context` (`fn_dblog`'s names, `LOP_INSERT_ROWS`, `LCX_CLUSTERED`),
`transaction_id`, `transaction_status`, `page`, `slot`, `allocation_unit`, `schema_name`, `table_name`,
`index_id`, and `contents`:

| operation | `contents` |
|---|---|
| `LOP_INSERT_ROWS` | `{rows: [...]}`: the rows inserted (a page split moves many in one record), each a struct of the table's columns as `mssql_backup_scan` reads them |
| `LOP_DELETE_ROWS` | `{row: {...}}`: the row deleted, as it was (NULL when the record logs none) |
| `LOP_MODIFY_ROW` | `{offset, size, before, after}`: the bytes changed in the row |
| `LOP_MODIFY_COLUMNS` | `{ranges: [{offset, before, after}, ...]}` |
| `LOP_BEGIN_XACT` | `{name}` |
| others | `{elements: [...]}`: the record's contents as bytes |

A row is given as bytes, with `undecoded` saying why, when it is not a base table's row (an index's, a
LOB fragment), its values are stored off row (their pages are the backup's, not the record's moment),
or it is ROW- or PAGE-compressed (for now). The names of tables need the backup's catalog: a log backup's
records, and a differential's given without its full, name allocation units only. Both are on an
attached backup as `b.transactions()` and `b.transaction_records()`, with `version :=` / `as_of :=`.
The oracle lane compares every record with `fn_dump_dblog` over the same files on SQL Server 2017-2025.

## A differential over its full

A differential backup holds only the pages that changed since its full backup, so it is read over
that full: `diff :=` names it.

```sql
SELECT count(*) FROM mssql_backup_scan('/backups/sales_full.bak', 'dbo', 'orders',
                                       diff := '/backups/sales_diff_1500.bak');
SELECT table_name, row_count FROM mssql_backup_tables('/backups/sales_full.bak',
                                                      diff := '/backups/sales_diff_1500.bak');
ATTACH '/backups/sales_full.bak' AS b (TYPE mssql_backup, diff '/backups/sales_diff_1500.bak');
```

For a differential taken while the database had no writes at all, from its start to its end, the
result is what `RESTORE`
of the full `WITH NORECOVERY`, then of the differential `WITH RECOVERY`, gives: rows changed, added and
deleted since the full, tables created, dropped and truncated, columns added and dropped. Taken under
writes, it is the data pages as the differential stored them: `RESTORE … WITH RECOVERY` also replays the
backup's log tail and rolls back what was uncommitted, which this reader does not (yet), so the two may
differ there, as for a full taken under writes (above). A differential is cumulative since its
full, so a day of hourly differentials needs no chain: the state at 15:00 is the full plus the 15:00
differential alone. Every function takes `diff :=` but `mssql_backup_info`, which describes one backup.

It must be that full's differential: the full named is the one the differential names as its base,
and a copy-only full never is (with a full F, a copy-only C after it and a differential D, D goes over F:
`diff :=` beside C is refused, naming F's backup set). A differential over another backup set, of
another database, a full or a log given as `diff :=`, and a differential given as the path are refused,
each naming the files.

## Attaching a backup

A full backup can be attached as a read-only catalog, its schemas and user tables DuckDB entries:

```sql
ATTACH '/backups/sales_full.bak' AS bk (TYPE mssql_backup);
SHOW ALL TABLES;
SELECT count(*) FROM bk.dbo.orders;
SELECT o.id, c.name FROM bk.sales.orders o JOIN bk.sales.customers c ON c.id = o.customer_id;
```

A table reads as `mssql_backup_scan` reads it -- the same columns and types, the virtual
`_mssql_page`, `_mssql_rid`, `_mssql_page_lsn` and `_mssql_lob` included -- and `dbo` is the default
schema (`bk.orders`, `USE bk`). Views are not entries: their definitions are T-SQL, which DuckDB does
not run (`mssql_backup_views` lists them). Without `history`, a log or differential backup is refused
at `ATTACH` (a differential goes beside its full, `diff`).

It is always read-only, whatever the options say: `CREATE`, `INSERT`, `UPDATE`, `DELETE`, `ALTER`,
`DROP` and `COPY … FROM` are refused.

| ATTACH option | Description |
|---|---|
| `schema_filter` | A regex: only the schemas it matches (case-insensitive, a partial match: `^sales$` for one) |
| `table_filter` | A regex: only the tables and views it matches |
| `schema_exclude` | A regex: the schemas it matches are left out |
| `table_exclude` | A regex: the tables and views it matches are left out |
| `diff` | A differential backup to read over the attached full ([above](#a-differential-over-its-full)) |
| `history`, `snapshot_version`, `snapshot_time` | A history of backups ([below](#a-history-of-backups)) |
| `fuzzy` | What a scan of its tables does with one that is not consistent: `'warn'`, `'error'` or `'ignore'` ([above](#a-backup-taken-under-writes)); unset, the `mssql_backup_fuzzy` setting |

The filters work as `TYPE mssql`'s. An object is kept when it passes the filter -- it matches it, or
no filter is given -- and does not match the exclusion, so an exclusion alone keeps everything else:

```sql
ATTACH '/backups/sales_full.bak' AS sales (TYPE mssql_backup, schema_filter '^sales$',
                                           table_exclude '^(audit_log|staging_.*)$');
```

The `ATTACH` still reads the backup's headers, page index and system tables whole; what the filters
keep small is the catalog built from them -- the tables' and views' descriptions and the entries --,
which on a database of many objects is most of the `ATTACH`'s time and memory. Any other option is
refused by name.

The `ATTACH` opens the backup and builds its catalog once (`ATTACH OR REPLACE` does it again); a
table's pages are listed at its first scan and kept for the next.

Names are the backup's, quoted in DuckDB where they need it: `bk."my schema"."q]t'x"`, `"v'"`,
`"r.x"`. A case-sensitive database may hold columns whose names differ only in case (`Id`, `ID`,
`id`), which no DuckDB table can (its names are case-insensitive, quoted or not): they are read as
`Id`, `ID_1`, `id_2` -- the first suffix no column of the table is named, so a real column keeps its
own -- by the catalog and `mssql_backup_scan` alike. `mssql_backup_columns` gives that name as
`duckdb_name` beside the backup's `column_name`. Tables and schemas that differ only in case are
entries of their own, found by the exact name.

The file functions take the attached name in place of a path:

```sql
SELECT * FROM mssql_backup_scan('bk', 'dbo', 'orders', max_threads := 2);
SELECT table_name, row_count FROM mssql_backup_tables('bk');
```

Every statement opens the files again and checks each one against what the `ATTACH` opened: a file
whose size or modification time changed since is refused (`DETACH` it and `ATTACH` it again).

## A history of backups

A database backed up as a full a day and a differential an hour is a history of states, one an hour.
`history true` attaches every backup a glob or list names as one catalog, and DuckDB's `AT` reads any
state of it, as a DuckLake's snapshots are read:

```sql
ATTACH 's3://backups/sales/*.bak' AS b (TYPE mssql_backup, history true);
FROM b.snapshots();
SELECT count(*) FROM b.dbo.orders;                                         -- the latest state
SELECT count(*) FROM b.dbo.orders AT (VERSION => 42);
SELECT count(*) FROM b.dbo.orders AT (TIMESTAMP => TIMESTAMP '2026-11-17 15:30');
```

Each full backup is a version, and so is each differential over a full among the files, numbered from
1 in the order they finished (two in one second by their checkpoint LSN). `AT (VERSION => n)` reads
the n-th; `AT (TIMESTAMP => t)` the last one finished at or before `t`, in whole seconds, compared with
the backups' times as `RESTORE HEADERONLY` gives them: the server's local time, so a `TIMESTAMPTZ` is
taken in the session's time zone, and the hour a clock is set back in is ambiguous. Two versions can be
read in one query. A version this catalog does not have is an error, also when the catalog is only on
the `search_path`. A version's catalog is built at its first read and kept while the history has the
version (a refresh that no longer finds its files lets it go, once no transaction reading it is open);
while the opened-file cache keeps it, the full a day's differentials lie over is indexed once for all of
them. A name with no schema (`b.orders`) is looked up in the default version's default schema, `dbo`
unless the filters hid it, whatever `AT` names.

The file functions are the catalog's own too, as a DuckLake's: `b.snapshots()`, `b.refresh()`, and
`b.info()`, `b.schemas()`, `b.tables()`, `b.views()` (the views' T-SQL definitions) and `b.columns()`,
each taking `version :=` or `as_of :=` (`FROM b.tables(version := 3)`). They are
`mssql_backup_<name>('b', …)`, which reads the same. When the filters keep nothing they still answer,
with no rows.

`mssql_backup_snapshots('b')` -- or `b.snapshots()` -- lists the versions: `snapshot_id`,
`snapshot_time` (the finish), `backup_type`, `is_copy_only`, `backup_start_date`, `checkpoint_lsn`, the
backup set GUIDs, `recovery_fork_id`, `history_break` (another lineage than the version before -- another
recovery fork or another family GUID: the database restored or recreated between them), `is_default`, `database_name` and the files (`full_files`,
`diff_files`). It takes files too, with no `ATTACH`, reading each one's header only.

The `ATTACH` reads every file's header, one after another, and the default version's pages; nothing
else until a version is read. Log backups are left out with a WARNING in `duckdb_logs`, as are a
differential whose full is not among the files, a striped backup missing a file, a copy of a file
beside it (the same backup, read once), and a file, filegroup or partial backup (it is no state of the
whole database; the WARNING names its files). A backup left out because files of it are missing joins
the history at the refresh that finds them. Files of two databases (another name and family GUID), a file that is not a
backup, and one the reader refuses (encrypted, appended sets) refuse the whole `ATTACH`; a database
recreated under its name is one history, with `history_break` where it was. A file that changed since
the `ATTACH` is refused when its version is read, until a refresh reads it again. New backups are
taken in by `CALL b.refresh()` (`mssql_backup_refresh('b')`): the glob is expanded again and the headers
of the new or changed files read (a file whose server gives no ETag or modification time is read every
time), and the default version opened; a version already read keeps its catalog, a file moved or
rewritten makes its version a new one. A version named by number or time (or the default one, with no
`AT`) is resolved when a query is bound -- to its backups and their files -- and the query reads those to
its end, through the catalog or a file function on its name alike: a refresh meanwhile reaches the next
query, in a transaction or not. `snapshot_version` / `snapshot_time` choose
a version by its backups, which it stays across a refresh (if its files are gone, the latest is read,
with a WARNING). A version's number is its place among the files, so a backup added that
finished earlier than others (a full found for a differential left out, say) moves the later ones, and
an importer resuming by number would skip it; one resuming by the files it applied, or by time with a
look for versions that arrived late, does not. The headers are read 64 files at a time.

| ATTACH option | Description |
|---|---|
| `history` | Every backup the path names, as the catalog's versions |
| `snapshot_version` | The version read without `AT` (the latest by default) |
| `snapshot_time` | The version read without `AT`: the last one finished by this time |

Without `history` the backup attached is a history of its own: a full is version 1; a full with its
`diff` is version 1, the full, and version 2, with the differential.

The file functions take `version :=` or `as_of :=`, over an attached name or over files (which reads
every file's header again on every call, so a long history is better attached):

```sql
SELECT table_name, row_count FROM mssql_backup_tables('b', version := 3);
SELECT count(*) FROM mssql_backup_scan('/backups/sales/*.bak', 'dbo', 'orders',
                                       as_of := TIMESTAMP '2026-11-17 15:30');
```

## Changes between two states

`mssql_backup_schema_changes` gives what changed in the schema from one state of a history to another,
as an importer applies it before the rows -- compared by object id and column id, so a rename is a
rename:

```sql
FROM b.schema_changes(start_version := 41, end_version := 42);
FROM mssql_backup_schema_changes('b', start_version := TIMESTAMP '2026-11-17 14:00');
FROM mssql_backup_schema_changes('/backups/sales_full.bak', diff := '/backups/sales_diff.bak');
```

| `change_type` | When |
|---|---|
| `table_created`, `table_dropped`, `table_renamed` | as they say |
| `table_truncated` | `TRUNCATE` (the partition's pages all freed: its allocation starts anew, or not at all; a `DELETE` of every row or a shrink is no truncation), with the `partitions` concerned |
| `partition_switched_out` | a partition's rows moved to another table that the newer state still has (a sliding window's `SWITCH OUT`), by its old partition number |
| `table_reload` | the table is to be read whole: recreated under a new object id, a partition switched in from another table (its pages are not rewritten, so they cannot be told apart by their LSNs), or the database restored or recreated between the two states (`reason` says which) |
| `column_added`, `column_dropped`, `column_renamed`, `column_type_changed`, `column_nullability_changed` | with the old and new name, type and nullability |
| `view_created`, `view_dropped`, `view_renamed`, `view_definition_changed`, `view_indexed`, `view_unindexed` | with the old and new T-SQL, which the lake keeps though it cannot run it |

The rows come in the order an importer applies them: the tables' reloads and drops, then each kept
table's rename, truncation, switch and columns (renames, drops, additions, type and nullability changes),
then the tables created, then the views. `start_version` / `end_version` take a version's number or a
time; with neither, the default version against its full alone. One state gives no rows, not an error,
so an importer can ask on every backup. An index rebuild, a partition `MERGE` or `SPLIT` and a switch to
a table since dropped are no event: their pages are written anew, or their rows are gone, and the row
changes see them. Partitions are matched by their data (rowset), not their number, which a `MERGE` or
`SPLIT` moves. A table recreated under a new object id is a reload with no column events: compare its
columns to know whether the lake's table still fits. Not events: a column's collation, identity,
computed definition or sparseness alone.

### Row changes

`mssql_backup_table_changes` gives a table's rows that changed between the same two states, as
DuckLake's `table_changes` names them; `mssql_backup_table_insertions` / `_deletions` the rows an
importer inserts (inserts and the updates' new rows) and deletes (deletes and the updates' old rows):

```sql
FROM b.table_changes('dbo', 'orders', start_version := 41, end_version := 42);
FROM mssql_backup_table_deletions('b', 'dbo', 'orders', start_version := TIMESTAMP '2026-11-17 14:00');
```

Each row is `change_type` (`insert`, `delete`, `update_preimage`, `update_postimage`), then the
table's columns in the newer state's schema (a column the older state did not have is NULL in its rows,
one it dropped is gone), then `_mssql_rid`: the row's key, a clustered table's key or a heap row's place.
A heap row forwarded to another page keeps its `_mssql_rid` (its place before it moved); a heap row a
shrink or a rebuild moved has a new place, so it reads as a delete and an insert, as does a row deleted
and inserted again. A clustered table's rows keep theirs wherever they move.

Only what may have changed is read: the newer state's pages written since the older state's checkpoint
-- a differential's own pages, when it lies over the older state's full; every page's header otherwise
(two fulls, or a full after a differential) -- each with its older version; and the older state's pages
the table no longer has. On a page the two versions share, a record whose bytes lie unchanged at the
same place is the same row, unchanged, and is passed over; the others are paired by `_mssql_rid` and
compared as stored. A value stored off row changed in place rewrites only its LOB pages, not its row's
page (measured on SQL Server 2022): when a table's LOB pages changed, its rows' value trees are walked
(their leaves not read) and a row whose tree touches a changed page has its two versions' values
compared by their digests. A created table, or one to reload (see `schema_changes`), gives all its rows as
inserts. A column whose type changed takes the older rows' values through DuckDB's cast, not the
server's conversion; a value the cast refuses is NULL, with a `WARNING` in `duckdb_logs`.

The pages are read on DuckDB's threads, a unit of 64 at a time per task, as a scan reads them, and
each unit's changes are returned as it is done. A row that moved to another page (a page split, a
shrink, an index rebuild moves every row) cannot be paired on its page: it is kept by its place and
digests -- about a hundred bytes and its key, not the row -- in partitions by `_mssql_rid` that DuckDB
writes to its `temp_directory` past `memory_limit`, as its own hash join does, and paired one partition
at a time when every unit is done. A chunk's decoding is reserved against half of `memory_limit`, as
the scan's is.

The digests are XXH64 with the length by default: an accidental collision is out of reach, but bytes chosen to collide (by someone who writes to the source database and wants a change to
go unseen) are not. `SET mssql_backup_change_hash = 'sha256'` makes them SHA-256, a few per cent slower
on DuckDB's threads (about 15 % on one).

## Opening a backup once

A backup is opened once per DuckDB instance: what a call read to open it -- the headers, where
each page lies, which pages are live, the catalog's base tables -- is kept for the next call on the
same files, so a query per table of a large backup (an import, say) pays the open once. The files
themselves are opened again by every call, which checks that each one is the same: a file whose size
or modification time (an ETag on object storage) changed is opened anew, and so is a local file
replaced by another one (a new inode). What is not caught: a backup rewritten in place at the same size
whose modification time then reads as before -- set back with `touch -r`, or within a coarse clock (2 s on
FAT, 1 s over HTTP without an ETag). It is taken for the cached one, and its pages are looked for where
the old one had them. Turn the cache off for such files. What is kept
is evicted under memory pressure, like DuckDB's other caches, and another DuckDB instance in the
same process keeps its own. `SET mssql_backup_cache = false` opens the backup anew on every call.

## Prepared statements

`mssql_backup_info` reads the file when the statement binds, and a prepared statement keeps what
its `PREPARE` read (`mssql_backup_tables`, `mssql_backup_views`, `mssql_backup_catalog` and
`mssql_backup_scan` read it on every execution). `reread := true` reads the file again on every `EXECUTE`, for a file that is overwritten
between runs (a nightly backup to the same path):

```sql
PREPARE latest AS SELECT backup_finish_date FROM mssql_backup_info('/backups/nightly.bak', reread := true);
EXECUTE latest;
```
