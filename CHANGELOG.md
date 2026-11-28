# Changelog

All notable changes to the DuckDB MSSQL Extension are documented here.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- **An encrypted backup (`BACKUP … WITH ENCRYPTION`) and a backup of a TDE database are read** (spec 087). The key material comes only from a new secret, `CREATE SECRET (TYPE mssql_backup_key, CERTIFICATE '…cer', PRIVATE_KEY '…pvk' | '…pfx' | '…pem', PASSWORD '…', SCOPE '…')`, checked when it is created (a wrong password fails there) and selected per file by the longest scope, then the certificate's thumbprint. AES-128 / 192 / 256 and TRIPLE_DES_3KEY, stored, MS_XPRESS or ZSTD; a TDE database's pages decrypted with its DEK, and its compressed backup (MS_XPRESS or ZSTD; `WITH ENCRYPTION` too under MS_XPRESS -- the ZSTD form of that, not yet seen, is refused by name) read through the DEK its container header keeps. A secret's scope is a prefix of the path, so end it with `/`. A backup whose key is wrapped with PKCS#1 v1.5 (`EncryptorType` `CERTIFICATE`, older versions) is refused by name: every version measured wraps with RSA-OAEP. A `DROP SECRET` stops the next statement's reads, of an attached backup too. Without a key `mssql_backup_info` shows what the file says in clear (the encryptor's and the DEK's certificate thumbprints, the compression) and NULL for the rest, with a WARNING; reading needs the key, refused by name before any row. New `mssql_backup_info` columns `key_algorithm`, `tde`, `tde_thumbprint`, and `files.tde_thumbprint` (FILELISTONLY's TDEThumbprint). A TDE database's log is encrypted and not read yet: its tables' `fuzzy_status` is `unknown`.
- **SQL Server 2025 `vector(n)` is read and written as `FLOAT[n]`** (spec 093 T3, D4; #363). The login asks for VECTORSUPPORT (`mssql_vector_support`, default true, read at ATTACH), so a float32 `vector(n)` arrives as its bytes and reads exactly; a `vector(n, float16)` -- which SQL Server 2025 sends as JSON text even with the feature -- and every vector on a session without it are parsed from the text: exact for float16, within one ulp for float32 (8 significant digits), which a `WARNING` says. A raw `mssql_scan` types a vector `FLOAT[n]`, or `VARCHAR` (the server's text) where it arrives as text. Written on every path: the statement path as `N'[...]'` (9 digits, exact), COPY / a large INSERT / CTAS / a staged UPDATE or MERGE as the value's bytes (float16 rounded to nearest even); a CTAS or COPY creates `vector(n)` from `FLOAT[n]`. A float16 `vector(n)` takes up to 3996 dimensions and a float32 one 1998, the same 8000 bytes (#644, measured). A NULL element, a NaN, a float16 value that rounds past 65504 (a magnitude of 65520 or more) and a `DOUBLE[n]` into a new table (the cast `::FLOAT[n]` named) are refused by name. Until now a table with a vector column could not be read through the catalog at all.

- **`mssql_datetime2_7_as`, and `datetime2_7_as` on the connection** (an ATTACH option, a secret parameter, a connection-string key, a URI parameter; spec 093 D8). A `datetime2(7)` column -- the default scale -- is read as `TIMESTAMP_NS` (every 100 ns tick, 1677-2262), and a value outside that range, such as a temporal table's `9999-12-31` `ValidTo`, is read as NULL. The scan now says so: one `WARNING` per column in `duckdb_logs`, as soon as it meets one. Set `mssql_datetime2_7_as = 'timestamp'` before ATTACH, or attach with `datetime2_7_as 'timestamp'`, to read such columns as `TIMESTAMP` over 0001-9999, the 100 ns digit truncated: they are then filtered as `time(7)` is (a pushed `= c` matches every stored value up to the next microsecond), are not used as a row key (a table with no other usable key -- a heap with such a column, say -- has its row-by-row UPDATE / DELETE, and a staged MERGE comparing the column, refused by name), and a value written through them drops the seventh digit too (a table created from one gets `datetime2(6)`). Read at ATTACH, for the catalog's life. Under the default, a row-by-row UPDATE / DELETE that would key a row by such a value is refused by name before anything is sent for it (#559): it updated nothing (a primary key or unique index holding the value) or, on a table with no key, another row. A table keyed by the column is checked where the statement takes each row's key, after DuckDB's own filters; a table with no key is checked as it is read, its filters being all on the server. Rows inside the range are keyed as before, and a statement pushed to the server whole is not affected.
- **A `VARIANT` is written into a `sql_variant` column** (spec 093 T2, D3): `INSERT`, `UPDATE`, `MERGE`, `COPY` and CTAS (which creates the column as `sql_variant`), each value under the narrowest SQL Server type that holds every value of its DuckDB type -- an `INTEGER` an `int`, a `TINYINT` a `smallint`, a `UBIGINT` a `decimal(38,0)`, a `TIMESTAMP_NS` a `datetime2(7)` (rounded to 100 ns), a `TIMESTAMPTZ` a `datetimeoffset` at `+00:00`, a `VARCHAR` an `nvarchar(n)` (or an ASCII `varchar(n)` past 4000 characters) --, on the statement path and on the bulk wire alike (the INSERT BULK `sql_variant` form, measured to be taken). An `INSERT ... SELECT` within one attached database is still copied by the server as it is (pushed whole). A value read through DuckDB and written back keeps its value and its `VARIANT` type, while its base type may change: a `numeric` returns as a `decimal`, `money` as `decimal(19,4)`, `smallmoney` as `decimal(10,4)`, a `time(0)`-`(5)` as `time(6)`, a `datetime2(1)` / `(2)` as `datetime2(3)` and `(4)` / `(5)` as `datetime2(6)`, a `datetime` / `smalldatetime` as `datetime2(6)`, a `datetime2(7)` / `datetimeoffset(7)` outside 1677-2262 at scale 6, a `datetimeoffset` at `+00:00` (scale 6 below 7), a `char` / `varchar` / `nchar` as `nvarchar`, a `binary` as `varbinary`. A string value is written under the database's default collation (on the bulk path, a collation this version does not know as `Latin1_General_100_BIN2_UTF8`, so such a value compares and sorts by it, not by the database's); a subnormal `DOUBLE` written by the statement path (a literal) is stored as 0, as a plain `float` column's literal already is, while the bulk paths (COPY, a large INSERT, CTAS, a staged UPDATE / DELETE / MERGE) keep its bits (a plain `float` column's literal also stores -0.0 as +0, as it did before; a `VARIANT`'s keeps the sign); a string written into a `sql_variant` column (`INSERT ... VALUES (1, 'x')`) is an `nvarchar`. A value `sql_variant` cannot hold (a list, a struct, an `INTERVAL`, a `TIMETZ`, `NaN` or ±infinity, a number past 38 digits, a string past 4000 UTF-16 units that is not ASCII, a `VARCHAR` whose bytes are not valid UTF-8, a blob past 8000 bytes, a date outside 0001-9999) is refused, naming the column and the value's type. Until now every write of a `VARIANT` was refused.
- **A `sql_variant` column is read as a `VARIANT`** (spec 093 T1, D3), through the catalog and a raw `mssql_scan` alike: each value keeps its base type -- an `int` an `INTEGER`, a `decimal(p,s)` a `DECIMAL(p,s)`, a `time(7)` / `datetime2(7)` with its 100 ns digit, a `datetimeoffset` its UTC instant, a `uniqueidentifier` a `UUID`, a `varchar` converted from its own collation, a `binary` a `BLOB` -- read off the wire with no conversion on the server. Breaking: the column was a `VARCHAR` of each value's text, which a plain cast cut short (a `datetime2` to the minute, a `float` to six digits; #354); `v::VARCHAR` gives a value's text back. A filter on it still runs in DuckDB (#514), now over the `VARIANT` (`v = 5` the int, `v = '5'` the string), and it is still never a row key. Writing a `VARIANT` into one is spec 093 T2 (above).
- **A `hierarchyid` column is decoded on the client** (spec 093 T4, D5). It was read through `CAST(… AS NVARCHAR(MAX))` on the server; it now comes as stored and the client writes its `ToString()` text -- the same `VARCHAR`, the same text (the integration test compares it with `ToString()` on every label range's ends, negative and fractional labels, depth 20, an 885-byte path and values `GetDescendant` made). A raw `mssql_scan` / `mssql_scan_params` reads it too, where it was refused at bind. A filter or `ORDER BY` on it still runs in DuckDB, over the text (which does not sort like the tree), and it is still never a row key; writes are a string literal the server parses, as before.
- **A raw `mssql_scan` reads a user CLR type as a `BLOB`** of its bytes (spec 093 T4), as `CAST(x AS varbinary(max))` gives them, where it was refused at bind. Through the catalog it is still its `ToString()` text. `geometry` / `geography` are still refused by a raw scan (spec 093 T5).
- **`mssql_backup_info`: a SQL Server backup file described with no SQL Server** (spec 085, the first part of the backup reader). `SELECT * FROM mssql_backup_info('/backups/db.bak')` returns one row with what `RESTORE HEADERONLY`, `LABELONLY` and `FILELISTONLY` report (backup type, LSNs, dates, database, server and version, compatibility level, collation, recovery fork, media set, compression, sizes, and the database's files as a list), read from the file alone and only through DuckDB's file system (so an `s3://` or `https://` path goes through httpfs). Full, differential and log backups; uncompressed, `MS_XPRESS` and `ZSTD`; striped sets, given as a list or a glob of every file (one missing is named). An encrypted backup, a file holding appended backup sets, files of different backups and a compressed format the reader does not know are refused by name. Compared field by field with the server's own `RESTORE` output on the committed synthetic samples, made by SQL Server 2025 (`test/data/backup/`, on every pull request), and, when the oracle lane runs (`.github/workflows/backup-oracle.yml`; it needs the repository's R2 credentials, so it does not run for a pull request from a fork), on each version's backups made by SQL Server 2017, 2019, 2022 and 2025, read from R2 through `s3://`: ZSTD on 2025 only, `MS_XPRESS` asked for by name from 2022 and as plain `WITH COMPRESSION` before. Reading a table's rows comes in a later release.
- **`mssql_backup_tables` and `mssql_backup_views`: a full backup's tables and views, with no SQL Server** (spec 085 PR 2b). Read from the database's own catalog inside the file: per table its schema, shape (heap / clustered / columnstore), compression, partitions, rows, dates, its columns with their SQL Server type, collation and code page and the DuckDB type `TYPE mssql` reads them as (native types off), and its allocation units; whether a later release's scan can read it and why not (a clustered columnstore, ROW / PAGE compression, for now). Per view its T-SQL definition (a long one read from its LOB pages; NULL for `WITH ENCRYPTION`). The catalog's own layout is checked against its self-description on every open, so a SQL Server version that moves a field is refused by name rather than misread. The oracle lane compares tables (id, shape, partitions, rows, dates), columns (name, type, length, precision, scale, collation, flags) and view definitions (length, SHA-256) with `sys.tables` / `sys.columns` / `sys.sql_modules` on backups made by SQL Server 2017-2025 (`test/backup/compare_catalog.py`).
- **A partitioned table's partitioning in `mssql_backup_tables`** (spec 085 PR 2c): its partition function, scheme, column, parameter type, side, boundaries (as `VARIANT`s of the parameter's DuckDB type: a `datetime2(7)` to the 100 ns, a `datetimeoffset` at its UTC instant, a NULL boundary as NULL), each partition's filegroup and the NEXT USED one, and a `partitions` list of every table's partitions with their rows, compression and filegroup. A boundary the reader cannot give exactly -- a `datetime2(7)` outside 1677-2262 such as a `9999-12-31` sentinel, a `time(7)` / `datetimeoffset(7)` with a 100 ns digit -- and a catalog that disagrees with itself (a boundary or destination missing or listed twice, a boundary of another type, collation or length than the parameter's, partitions that do not number 1..fanout) are named in `partitioning.reason`, never guessed: the boundaries are then not given (the destinations still are when only the boundaries could not be), and the table's rows are read all the same. A table with two live base rowsets for one partition (an index built or switched while the backup ran) is listed not decodable, by name. Compared in the oracle lane with `sys.partition_functions` / `partition_range_values` / `partition_schemes` / `destination_data_spaces` / `partitions`.
- **`mssql_backup_scan`: a table's rows from a full backup, with no SQL Server** (spec 085 PR 2d). `SELECT * FROM mssql_backup_scan('/backups/sales_full.bak', 'sales', 'orders')` reads a heap or a clustered table, partitioned or not, typed as `TYPE mssql` reads it, through the same decoding kernels; forwarded rows, ghosts, version tags (RCSI / snapshot), the uniquifier and columns added after rows were written are read as the server reads them. Virtual columns `_mssql_page`, `_mssql_rid` and `_mssql_page_lsn`. Columnstore comes in a later release and is refused by name (ROW and PAGE compression are read, below). Compared in the oracle lane with RESTORE + `TYPE mssql`, table by table, on SQL Server 2017-2025. Parallel on DuckDB's threads (PR 2e: units of 64 pages, the rows' page order kept where DuckDB keeps insertion order); `max_threads := n` uses fewer for one call, and `max_memory := '2GB'` bounds what it reserves for decoding (to half of it; tasks wait for each other's memory, and a table that cannot be decoded within it is refused by name). Measured on 16 threads, a 2.2 GB database (a heap of one row a page, 200k pages, and a 5M-row table) backed up plain, `MS_XPRESS` and `ZSTD`: 2.6-5.6x the single-threaded scan.
- **Values stored off row read from a backup** (spec 085 PR 3). `text` / `ntext` / `image`, a `varchar(max)` / `nvarchar(max)` / `varbinary(max)` / `xml` value too long for its row and a value a row past 8,060 bytes pushed off (row-overflow) are read by `mssql_backup_scan` like any other, up to 2^31 − 1 bytes a value, each assembled from the pieces SQL Server stored it in, every piece checked against the value's tree (a value shortened by `.WRITE` keeps stale bytes past its end, which are not read). A row's off-row values are reserved within half of `max_memory` before they are read, and one that cannot fit even with nothing else of the scan held is refused by name (its `_mssql_rid`, page and slot, columns and sizes); a value of a mebibyte or more is copied or decoded straight into its result. A virtual `_mssql_lob` maps every off-row value of a row to the pages that hold it, for a later differential backup's import (NULL for a row with none). Measured on 200,000 rows of 10 KB stored off row: 0.15 s on 8 threads, at the in-row rate per byte. Compared in the oracle lane with RESTORE on a fixture of every off-row form, and fuzzed (`fuzz_backup_lob`).
- **ROW-compressed tables are read from a backup** (spec 086 PR 1). `mssql_backup_scan` reads a table, or partition, stored `WITH (DATA_COMPRESSION = ROW)` -- every type the reader converts, values stored off row and row-overflow included, forwarded rows, the uniquifier, columns added after the rows -- with the same values and the same `_mssql_rid` as the uncompressed table. The record format and each type's compressed form were measured on SQL Server 2022's own pages (`specs/086-backup-row-formats-and-types/measured/E22-cd-records.md`), and the oracle lane compares a fixture of every type and record form with RESTORE on 2017-2025. PAGE compression is read too (PR 2, below).
- **PAGE-compressed tables are read from a backup** (spec 086 PR 2). `mssql_backup_scan`, the listings, an attached backup and `mssql_backup_table_changes` read a table, or partition, stored `WITH (DATA_COMPRESSION = PAGE)` -- every type, values stored off row and row-overflow, forwarded rows, ghosts, the uniquifier, columns added after the rows, keys of every kind, row versioning -- with the same values and the same `_mssql_rid` as the uncompressed table: each page's values rebuilt from its anchor record and its dictionary. The page's layout and how a value refers to them were measured on SQL Server 2025's own pages (`specs/086-backup-row-formats-and-types/measured/E23-page-compression.md`, 30,204 values checked against the server's), and the oracle lane compares a fixture of every type and record form, and a differential over it, with RESTORE on 2017-2025. A row's changes between two backups are compared by its values, so a table rebuilt whole (every page's anchor and dictionary made anew) is no change. Also read now, under ROW as under PAGE: a forwarded row deleted under row versioning (a record type E22 had not met), which made such a heap's scan fail by name.
- **Sparse columns are read from a backup** (spec 086 PR 3). `mssql_backup_scan`, an attached backup and the change listing read a column declared `SPARSE` -- every type one can have, a value stored off row or row-overflow inside the row's sparse vector, 1000 sparse columns, a heap's forwarded rows, a non-unique clustered index, an empty regular value (`''`) just before the vector, a partitioned table, row versioning, rows written before `ALTER COLUMN … ADD SPARSE` / `DROP SPARSE` -- as the server reads it, where it was left out of the scan (`mssql_backup_columns`' `storage_format`, now gone). The record's sparse vector was measured on SQL Server 2019's and 2025's own pages (`specs/086-backup-row-formats-and-types/measured/E24-sparse.md`), and the oracle lane compares a fixture of every case, and a differential over it, with RESTORE on 2017-2025. A column set (`COLUMN_SET FOR ALL_SPARSE_COLUMNS`) is not stored: like a computed column that is not persisted it is `not_stored`, left out of `SELECT *` and refused by name when named, its sparse columns read one by one. The change listing compares a sparse table's rows by their values, so `ALTER COLUMN … ADD SPARSE`, which rewrites every row, is no change. Also fixed (review of #626): under `ANSI_PADDING OFF`, a nullable `binary` / `char` value `0x` / `''` in a ROW- or PAGE-compressed table read as no bytes, where the server reads one zero byte / space (`DATALENGTH` 1), as the uncompressed table's already did.
- **A backup is opened once per DuckDB instance** (spec 085 PR 4a). What a backup function read to open a backup -- its headers, where each page lies, which pages are live, the catalog's base tables -- is kept, in the DuckDB instance's object cache, for the next call on the same files: an import that runs a query per table pays the open once (measured on the committed sample: the second open made 15 file reads where the first made 87). Every call still opens the files and checks them: a file whose size or modification time (ETag) changed is opened anew; one rewritten in place at the same size whose modification time then reads as before (set back with `touch -r`, or within FAT's 2 s) is taken for the cached one. Evicted under memory pressure; `SET mssql_backup_cache = false` opens anew on every call.
- **A backup attached as a read-only catalog** (spec 085 PR 4b). `ATTACH '/backups/sales_full.bak' AS bk (TYPE mssql_backup)` lists the backup's schemas and user tables as DuckDB entries (`SHOW ALL TABLES`, `duckdb_tables()`, `DESCRIBE bk.dbo.orders`), and `SELECT … FROM bk.dbo.orders` reads a table's rows through the same scan as `mssql_backup_scan`, its virtual columns (`_mssql_page`, `_mssql_rid`, `_mssql_page_lsn`, `_mssql_lob`) included; `dbo` is the default schema. Always read-only: every write is refused, whatever the ATTACH's options say. `schema_filter` / `table_filter` restrict it to the schemas and tables (and views) they match, regexes as `TYPE mssql` takes them, and the catalog is built for those only. The ATTACH opens the backup once (the opened-file cache's entry, held while attached, `mssql_backup_cache` on or off) and builds its catalog model; `ATTACH OR REPLACE` reads the file and the options again. A table's pages, listed from its IAM chain at its first scan, are kept with the opened backup, so a later scan of the same backup does not walk the chain again (through `mssql_backup_scan` by path too, while the opened-file cache keeps it). Names are read as the backup spells them -- spaces, quotes, `]`, dots, tables and schemas that differ only in case -- and are written quoted in DuckDB (`bk.dbo."q]t'x"`); columns of a case-sensitive database that differ only in case, which no DuckDB table can hold (DuckDB's names are case-insensitive, quoted or not), are read as `Id`, `ID_1`, `id_2` -- the first suffix no column of the table is named, so a real column keeps its own -- through the catalog and `mssql_backup_scan` alike, never refused; `mssql_backup_columns` and `mssql_backup_tables.columns` give that name as `duckdb_name` beside the backup's own. The file functions take the catalog's name in place of a path (`mssql_backup_scan('bk', 'dbo', 'orders')`, `mssql_backup_tables('bk')`), opening its files again for each statement: a file whose size or modification time changed since the ATTACH is refused by name (DETACH and ATTACH again). Views are not catalog entries (their T-SQL is not DuckDB SQL; `mssql_backup_views` lists them); a log or differential backup is refused at ATTACH.
- **`schema_exclude` / `table_exclude` on `ATTACH … (TYPE mssql_backup)`** (spec 085): regexes, as `schema_filter` / `table_filter` take them, for the schemas and tables (and views) to leave out; an object is kept when it matches the filter (or none is given) and not the exclusion, so a filter and the same pattern as an exclusion split one backup into two attachments. `TYPE mssql` keeps its filters alone, whose regexes take an exclusion as a negative lookahead.
- **A differential read over its full** (spec 085 PR 4c). `diff := '/backups/sales_diff.bak'` on `mssql_backup_scan`, the listings (`mssql_backup_tables`, `_views`, `_columns`, `_schemas`) and `mssql_backup_catalog`, and `diff` on `ATTACH … (TYPE mssql_backup)`, read the database as `RESTORE` of the full `WITH NORECOVERY` then the differential `WITH RECOVERY` gives it: the differential's pages win over the full's, which pages are live is decided by the overlaid allocation pages (a page freed since the full is gone, as after a shrink), and a table's pages come from its IAM chain read through the overlay. A differential is cumulative since its full, so the state at any differential is the full plus that one alone. The chain is checked first and refused by name: a differential whose base is another backup set, one of another database, a full or a log given as `diff`, and a differential given as the path. The overlay is cached as the full is, keyed by both backups' files, its base the full's cached entry.
- **A history of backups, and time travel over it** (spec 085 PR 4c-2). `ATTACH 's3://backups/sales/*.bak' AS b (TYPE mssql_backup, history true)` attaches every backup the glob names as one catalog's versions -- each full backup, and each differential over a full among the files, a state of the database at its finish, numbered in that order -- and DuckDB's `AT` reads any of them, as a DuckLake's snapshots: `SELECT … FROM b.dbo.orders AT (VERSION => 42)`, `AT (TIMESTAMP => TIMESTAMP '2026-11-17 15:30')` (the last version finished by then, in the backups' own local time), two versions in one query. Without `AT` the latest, or the one `snapshot_version` / `snapshot_time` names. `mssql_backup_snapshots('b')`, or `FROM b.snapshots()` as in DuckLake, lists the versions (finish, type, copy-only, LSN, GUIDs, files, and `history_break` where the database was restored or recreated between two); it takes files too, with no ATTACH. The ATTACH reads each file's header and the default version's pages only; a version's catalog is built at its first read and kept, and a day's differentials share their full's index. Log backups, a differential whose full is not among the files, a striped backup missing a file and a copy of a file are left out with a WARNING; files of two databases are refused, a database recreated under its name is a break in its history, and a file that changed since the ATTACH is refused when its version is read. A version read stays loaded while the history has it. A backup attached without `history` is a history of its own (a full with its `diff`: the full, then the two). The file functions take `version :=` or `as_of :=`, over an attached name or over files. The file functions are the catalog's own as well, as DuckLake's: `b.snapshots()`, `b.refresh()`, `b.info()`, `b.schemas()`, `b.tables()`, `b.views()` (the views' T-SQL), `b.columns()`, with `version :=` / `as_of :=`. `CALL b.refresh()` takes new backups in: the glob expanded again, only new or changed files' headers read, the versions renumbered, a version already read kept. A version named by number or time -- or the default, with no AT -- is resolved when a query is bound, to its backups and files, and the plan carries that key (serialized with it): a refresh before the plan runs, or before its last row, does not move it; a transaction keeps no versions, so its next statement reads a refresh made meanwhile.
- **`mssql_backup_schema_changes`: what changed in a database's schema between two backups** (spec 085 PR 4c-3). Tables created, dropped, renamed, truncated, a partition switched out, or to reload (recreated under a new object id, a partition switched in, the database restored between the two); columns added, dropped, renamed, retyped or of another nullability; views created, dropped, renamed, redefined (their T-SQL old and new), indexed or unindexed -- compared by object id and column id, so a rename is a rename, in the order an importer applies them before the rows. Between two versions of a history (`start_version` / `end_version`, a number or a time; `b.schema_changes()` on an attached one), or a full and the differential `diff :=` names. An index rebuild is no event. One state gives no rows. Checked against the oracle fixture's DROP, TRUNCATE, ADD / DROP COLUMN, CREATE and REBUILD.
- **Opening a differential no longer reads it through** (spec 085, DQ-41 revisited). A differential's pages were mapped by reading every page's header, so opening one read the whole file -- over object storage 8.3 s and 1.5 GB for a 1.5 GB differential. Its extents are those its DCM pages mark (allocated, or holding an allocation page, within the file's size; measured on 15 differentials), so a plain differential's map is now computed from its own allocation and DCM pages as a full's is, checked at sampled ranks and, for a backup taken under load, where the database changed while it ran (the extents it copied again, the allocation bits that moved): the same differential opens in 0.4 s reading 79 MB over S3 (local MinIO through httpfs). A compressed differential's map is computed the same way through its container (only the groups read are decoded): opening one of 2.2 GB took 1.5 s decoding it whole and takes 0.03 s. A striped differential's families are searched as a full's are, the DCM pages of its data files saying which extents they carry together (checked by the families' group count, sampled groups and the extents changed while the backup ran), so it is not read through either.
- **`mssql_backup_table_changes` reads each page once for the rows that moved, and runs under a small `memory_limit`** (spec 085). The rows left unpaired are kept by their `_mssql_rid`'s hash in 16 or more partitions and were returned a partition at a time: every page holding one was read again for each partition (in a compressed differential its block decoded again), and each row was a chunk of its own. Once paired they now go into partitions by their page's place -- DuckDB's partitioned collections, spilled to `temp_directory` past `memory_limit` -- returned one at a time sorted by place. And the tasks are no more than a quarter of `memory_limit` holds the chunks of: each keeps one of every partition, and under 256 MB on 16 threads the call was out of memory at once. Measured on an index rebuild of 10M rows: 4.0 -> 2.2 s, from a compressed differential 15.1 -> 2.9 s; 6.7 s under a 128 MB limit (1.7 GB spilled).
- **A file, filegroup or partial backup is no longer read as a state of the whole database** (spec 085). `BACKUP DATABASE … FILEGROUP = 'x' WITH DIFFERENTIAL` was taken for a database differential: in a history it became a version (the default one when it was the latest) whose other filegroups' tables were the full's, older than the version before it. It is now left out of a history with a WARNING naming the files it carries and leaves out, refused by name for reading, and `mssql_backup_info` reports it as `file` / `file differential`, with each file's `is_present` (FILELISTONLY's IsPresent).
- **`mssql_backup_table_changes` / `_insertions` / `_deletions`: a table's row changes between two backups** (spec 085 PR 4c-3). DuckLake's `table_changes` shape: `change_type` (`insert`, `delete`, `update_preimage`, `update_postimage`), the columns in the newer state's schema, `_mssql_rid`; `b.table_changes('dbo', 'orders', …)` on an attached history. Only the pages written since the older state are read -- a differential's own pages when it lies over the older state's full, else each page's LSN -- with their older versions, rows paired by `_mssql_rid` and compared as stored; rows that moved between pages unchanged are no change. A change to a value stored off row, which rewrites only LOB pages (measured), is found through the rows' value trees: a row whose tree has a node on a changed page that is not as it was, so the values beside a changed one on its pages are not read again. In a table with values stored off row, a row whose record was rewritten is compared by its values, column by column: the off-row pointers a shrink rewrites, a dropped column's bytes it leaves out of the records it moves, and a default a rebuild writes into them make no change. Parallel on DuckDB's threads, as the scan; rows that moved between pages (an index rebuild moves them all) are paired through DuckDB's partitioned collections, which spill to `temp_directory` past `memory_limit`. Measured on 10 million rows: 0.09-0.2 s for 10 thousand to 1.1 million changed rows (a scan of the full: 0.08 s), 4.1 s after an index rebuild. Checked against a whole comparison of the two states on the samples and, on SQL Server 2022 (the off-row fixture on 2019 and 2025 as well), on fixtures with updates that split pages, forwarded heap rows, truncates, a shrink, a rebuild, added and dropped columns, a sliding window, a partition switched in and off-row values changed every way.
- **A backup taken under writes says where its rows differ from a RESTORE, and its log can be read** (spec 085 PR 5). A backup holds each page as it copied it and the log from its checkpoint to its end; `RESTORE … WITH RECOVERY` replays that log, this reader does not (yet). It now reads the log tail -- its blocks and records, checked against `fn_dump_dblog` record by record on SQL Server 2017-2025 -- and `mssql_backup_tables` gives each table's `fuzzy_status`: `dirty` (a transaction open at the backup's end changed it, and the page copies hold that change), `stale` (a committed transaction changed it after its pages were copied, or a rolled-back one undid after the copy what the copy holds), `stale+dirty` or `consistent`, with the transactions involved; `mssql_backup_info` gives the database's `consistent`, `open_transactions`, `committed_after_checkpoint` and `rolled_back_after_checkpoint`. The oracle lane restores backups taken under three writing sessions and an open transaction and checks the status against the restored database, table by table: every table that differs is flagged, no equal one is. `mssql_backup_scan`, an attached backup's tables and the `mssql_backup_fuzzy` setting take `fuzzy := 'warn'` (default: read it, one WARNING in `duckdb_logs`), `'error'` (refused by name) or `'ignore'` -- a page the database took into use while the backup ran is passed over by the same rule, and a value stored off row on one is NULL, its row read; `ATTACH … (TYPE mssql_backup, fuzzy 'error')` too. `mssql_backup_transactions` lists the log's transactions (name, state, LSNs, the tables each changed and its effect on them) and `mssql_backup_transaction_records` its records as `fn_dump_dblog` does, with each record's contents as a `VARIANT`: an inserted or deleted row's columns decoded as the scan reads them, a modified row's bytes, a transaction's name -- over a data backup's tail or a log backup's whole log, with no SQL Server; `b.transactions()` and `b.transaction_records()` on an attached backup. A page the database took into use while the backup ran, whose content is only in the tail, is passed over by the scan with a WARNING naming it (`'warn'`; `'error'` refuses the table, as it did whatever the policy): the table's other pages, what was committed when the backup began, are read. A value stored off row on such a page (a `varchar(max)` past its row, say) is read as NULL and its row read, with a WARNING of its own under `'warn'` -- a NULL `RESTORE` would not give, until the tail is replayed. The status counts the log's row changes (a rollback's undo included); a bulk load logged as pages rather than rows, a `TRUNCATE`, a partition switch or other DDL run during the backup is not counted yet and leaves its table `consistent`. The log tail of a SQL Server 2017 backup is read too (its log blocks' header is 48 bytes, not 72). The change listing (`mssql_backup_table_changes` / `_insertions` / `_deletions`) and `mssql_backup_catalog` take the same policy (review of #591): each state's table, or the base table, is checked against its backup's tail -- one WARNING under `'warn'`, refused by name under `'error'` -- and they take `fuzzy :=`, then the ATTACH's option, then the setting. A page that became live while the backup ran is passed over by `mssql_backup_catalog` as by the scan; the change listing refuses it whatever `fuzzy :=` says, since a row moved onto such a page would read as deleted. `mssql_backup_transaction_records` projects its columns: the whole log is read ahead of the first row only when `transaction_status` is selected (a transaction's state is known at the log's end), and `contents` is decoded only when selected; reading a log's summary can be interrupted. The tail is read when a statement first needs it -- `mssql_backup_tables`' first row, `mssql_backup_info`'s D11 columns (projected), a scan, change listing or base table as it starts, not at bind: `DESCRIBE` and `EXPLAIN` read none, and a scan's WARNING is logged once per execution. Its summary counts against `memory_limit` as it is read, a tail that cannot be read is not read again while the backup stays open (cached or attached: the same reason is given), and the tables' status is worked out once per backup.
- **`mssql_backup_schemas`: a full backup's schemas** (spec 085 PR 2d), as `sys.schemas` lists them, with the user tables and views in each.
- **`mssql_backup_columns`: every column of a full backup's tables, with its state at a glance** (spec 085 PR 2e). `SELECT table_name, column_name, state, reason FROM mssql_backup_columns('/backups/sales_full.bak') WHERE state <> 'converted'` says which columns the scan converts, which it returns as their stored bytes for now, and which have no stored value or a sparse one; optionally one schema's or one table's. `sql_type` names an alias type as declared (AdventureWorks' `Name`), `type_schema` the schema it was created in (`dbo`; `sys` for a system type), `system_type` the type underneath (`nvarchar(50)`). `mssql_backup_tables.columns` carries `type_schema` too.
- **Indexed views read from a full backup** (spec 085 PR 2e). `mssql_backup_scan('/backups/sales_full.bak', 'sales', 'v_region_totals')` returns the rows a view's unique clustered index stores -- what `SELECT … WITH (NOEXPAND)` returns on the server -- and `mssql_backup_views` says which views are indexed, with their shape, compression and rows. A view without such an index stores no rows and is refused by name. Compared in the oracle lane with the server's view.
- **A name filter narrows what the backup's catalog functions describe** (spec 085 PR 2e). `WHERE table_name = 'orders'` (or `IN (…)`, or `schema_name` / `view_name`) on `mssql_backup_tables`, `_views`, `_schemas` and `_columns` builds only the tables and views it names; `mssql_backup_scan` describes only its table at bind. The rows are unchanged, but for one bound: a call reads at most 1 GiB of views' off-row definitions, so past it a narrowed call can give a definition the whole listing names as past the bound. `EXPLAIN` shows what a call describes.
- **No column of a backup is refused for its type** (spec 085 PR 2d). A type the reader converts takes the type `TYPE mssql` reads it as, and one whose conversion is still to come -- `sql_variant`, `xml`, `geometry` / `geography`, `hierarchyid`, a user CLR type, `vector`, `json`, a type the reader does not know -- is its stored bytes, a `BLOB`, for now, with `mssql_backup_tables.columns.reason` saying why. Every collation is converted, the double-byte code pages (932, 936, 949, 950) included, on the client. A column the scan cannot read yet -- a sparse one (spec 086) -- and a computed column that is not persisted (no stored value) say so in `columns`, are left out of `SELECT *`, and refuse a query that names them; the table's other columns are read.
- **Every table of a backup is read, the system and internal ones too** (spec 085 PR 2b; owner, 2026-11-22). The catalog's system base tables (`sysschobjs`, `syscolpars`, `sysobjvalues`, `sysowners` ...), the internal tables SQL Server keeps for a feature (Query Store's `plan_persist_*`, a queue's messages, change tracking's `syscommittab` ...) are described and read as the user tables are, by the one scan, whatever their shape or storage (a heap, a non-unique clustered key, a ROW- or PAGE-compressed one: 2017's `syscsrowgroups`): attached, they are entries of schema `sys` (`SELECT * FROM bk.sys.sysschobjs`), and over a path `mssql_backup_catalog(path, base_table)` is `mssql_backup_scan(path, 'sys', base_table)` -- the same types (a `char` trimmed, a `sql_variant` its stored bytes) and virtual columns (`_mssql_page`, `_mssql_rid`, `_mssql_page_lsn`, `_mssql_lob`), for a tool that mirrors the catalog (mssql-ducklake's import). `mssql_backup_tables` lists every table with its `type` (`U`, `S`, `IT`), `is_ms_shipped` and an internal table's `parent_object_id`; filter `type = 'U'` for the user tables. `mssql_backup_schema_changes` compares every table too, and its rows carry the object's `type` (`U`, `S`, `IT`, a view's `V`) and `is_ms_shipped`: across a history break (a database restored between two backups) every table reloads, the ~115 of schema `sys` with them, so an importer that mirrors the user's objects keeps `NOT is_ms_shipped`. A table is read whole -- `sysobjvalues` with module source, `sysowners` with contained users' password hashes: whoever holds the backup can read all of it, and protecting a backup is encryption's job (spec 085 § Security, revised 2026-11-21). The resource database's tables (`sys.trace_xe_*`), whose rows a database's backup does not hold, are not listed.
- **zstd** (1.5.7, from vcpkg, statically linked, BSD licence in `LICENSES/zstd-LICENSE-BSD.txt`) for the `ZSTD` blocks of compressed backups.
- **`duckdb_tables()` and `duckdb_columns()` filtered by `table_name` look the tables up instead of listing the catalog.** DuckDB now hands a `table_name` / `table_oid` filter to the catalog. Names a filter pins (`=`, `IN`, an `OR` of names; at most 16 lookups per scan, names times the schemas it asks, past which it lists) are looked up one by one, as a query's first use of a table is, so `SELECT … FROM duckdb_columns() WHERE schema_name = 'dbo' AND table_name = 't'` costs a round trip per table rather than a metadata load of every table, and does not wait for another session's uncommitted DDL elsewhere in the database. An oid filter is answered from the tables already loaded. `LIKE`, `lower()` and the rest still list. A `schema_name` filter already kept the other schemas out, but on the server any read of the catalog narrowed by schema still reads every schema's rows, so only a lookup by name avoids another session's locks.
- **`mssql_client_transcode` and the `client_transcode` ATTACH option** (spec 092, default on, read at ATTACH): code-page text is read as bytes and converted on the client, with tables generated from a SQL Server (spec 091). Measured on 1M rows against SQL Server 2025 under Rosetta: half the bytes on the wire, 26-57% less server CPU, wall time 0.43-0.74x.
- `make code-pages-check-wire`: every collation's wire key against a live server.

### Changed

- **A backup's `sql_variant` columns are read as `VARIANT`, as `TYPE mssql` reads them** (spec 093 T8b). They were their stored bytes (`BLOB`); now every base type keeps its own type in the `VARIANT` -- by T1's code, from the form a record stores a value in (measured on 2022 and 2025, `specs/093-type-kernels/measured/S1d.md`: bit's base type, a decimal mantissa of the fewest bytes that hold it, money's, datetime's and smalldatetime's parts in another order than over TDS, a string's collation as its CollationId) -- in every storage form (ROW / PAGE, a heap, a value pushed off row, a `sql_variant` clustered key), through an attached backup, the change listing and `mssql_backup_transaction_records`' `contents`. The system tables' `sql_variant` columns (`sysobjvalues.value` …) read the same. A char / varchar value under a collation this build has no code page for, or a UTF-8 one whose bytes are not UTF-8, is its bytes inside the `VARIANT` with one WARNING per column, as over a connection; a value whose stored bytes are no `sql_variant` (a damaged page) is NULL with one WARNING, its row read. Checked against a RESTORE on 2022 and 2025 (fixture `variant`, a full and a differential, which the oracle lane runs on every version).
- **A backup's `geometry`, `geography` and `hierarchyid` columns are read as `TYPE mssql` reads them** (spec 093 T8a). They were their stored bytes (`BLOB`); now `geometry` is `GEOMETRY`, `geography` `GEOMETRY('OGC:CRS84')`, `hierarchyid` its `ToString()` text, decoded by the same code as over a connection, in every storage form (ROW / PAGE, a heap, a value stored off row). A curve or FullGlobe is NULL with one WARNING per column, and so is a value whose stored bytes cannot be read as its type (a damaged page): its row is read (`mssql_backup_transaction_records` keeps such a row image as its bytes). `geometry_as` is an option of `ATTACH … (TYPE mssql_backup)` and a parameter of `mssql_backup_scan`, `mssql_backup_catalog` and the change listing: `'wkb'` or `'native'` reads the curves, as `TYPE mssql`'s option. A user CLR type stays its bytes. `mssql_backup_transaction_records`' `contents` holds a geometry / geography value as its ISO WKB (a `BLOB`, the curves included) and a hierarchyid as its text. Checked against a RESTORE on 2019, 2022 and 2025 (fixture `clr_types`, which the oracle lane runs on 2017 too).

- **Breaking: `geometry` / `geography` are decoded on the client, Z and M kept, and a `geography` column is `GEOMETRY('OGC:CRS84')`** (spec 093 T5, D6). The column is read as the bytes SQL Server stores and turned into ISO WKB by the extension, through an attached catalog and a raw `mssql_scan` alike; the server's `.STAsBinary()`, its CPU and its 2D-only output are gone. Breaking, for anyone who reads such columns: a value with Z or M now has them (`POINT ZM (1 2 3 4)` where it was `POINT (1 2)`); a `geography` column describes as `GEOMETRY('OGC:CRS84')` (WGS 84, built into DuckDB, written by Parquet as it is) where it was `GEOMETRY`; a `POINT EMPTY` is DuckDB's `POINT EMPTY` (also as a `MULTIPOINT`'s or a collection's member) where the server's WKB said `MULTIPOINT EMPTY` or left the member out; and a curve or `FullGlobe` (`CIRCULARSTRING`, `COMPOUNDCURVE`, `CURVEPOLYGON`, `FULLGLOBE`), which no DuckDB `GEOMETRY` holds, reads as NULL with one `WARNING` per column in `duckdb_logs`, where it read as bytes DuckDB's own functions then refused. The new ATTACH option **`geometry_as`** reads the catalog's spatial columns as a `BLOB` instead: `'wkb'` the ISO WKB, the curves included, `'native'` the stored bytes (`CAST(g AS varbinary(max))`), which an `INSERT` / `UPDATE` written row by row through the catalog refuses by name (the write path sends WKB; a DML pushed to the server whole copies them); `'geometry'` is the default. `IS NULL` / `IS NOT NULL` and `count(g)` of a spatial column go to the server, which answers by the real values: a `DELETE … WHERE g IS NULL` leaves a curve alone, and `count(g)` counts it. What DuckDB computes over such a column sees a curve as NULL, so an `UPDATE … SET g = <expression over g>` or an `INSERT … SELECT` between catalogs can write NULL over one; `geometry_as 'wkb'` keeps them. A raw `mssql_scan` / `mssql_scan_params`, which refused a bare spatial column, reads it as the catalog does. A comparison of a spatial column runs in DuckDB, never on the server. A stored value whose bytes cannot be read is NULL as well, with its own `WARNING` (the column and the first value's fault), in every mode that decodes: one value never fails the scan of its table. An `INSERT … RETURNING` that returns such a value says both `WARNING`s too, as does one that returns a `sql_variant` read as its bytes. Writes send what they sent (spec 093 T6), with one new refusal: a `geography` column being `GEOMETRY('OGC:CRS84')`, a value written into it is cast to that, and DuckDB refuses at bind a source labelled with another CRS (`GEOMETRY('OGC:CRS83')`, say: `Cannot cast GEOMETRY with CRS … to GEOMETRY with different CRS 'OGC:CRS84'`) where the plain `GEOMETRY` column took it relabelled. A source with no CRS or with `OGC:CRS84` is written as before; cast another to a plain `GEOMETRY` (`g::GEOMETRY`) to write its coordinates as they are. A `POINT EMPTY` read this way does not write back yet (the server refuses its NaN coordinates). A backup's are read the same way since spec 093 T8a (above).
- **A SQL Server 2025 `json` column is DuckDB's `JSON`** (spec 093 D7), where it was `VARCHAR`: `DESCRIBE` and `duckdb_columns()` say `JSON`, and a table created from it in DuckDB keeps the type, so DuckDB's JSON functions take it as it is. The values are the same text; a raw `mssql_scan` still reads `VARCHAR`, the type the server describes. A pushed query over such a column binds with the json extension not loaded, as an unpushed one does.
- **Breaking: `mssql_exec` runs once per row of the query that calls it.** A call with constant arguments ran once per chunk and reported its count for every row, so `SELECT mssql_exec('db', 'INSERT …') FROM t` inserted one row however many `t` has; it now inserts one per row of `t`, as `mssql_exec_params` always did. Check queries that call it over a table to run a statement once: call it over no table (`SELECT mssql_exec(…)`) instead. A context name given per row was read from the first row, and a NULL one ran against a catalog named `NULL`; now each row's is used, and NULL answers NULL. A call whose result nothing reads still does not run: DuckDB drops the column (documented).
- **DuckDB pin: the head of the 2.0 branch** (`v2.0-cyanoptera`, 880 backported fixes past v2.0.0, among them the catalog filter pushdown above). The CLI now reports a failing statement and exits 1 (at v2.0.0 it printed nothing and exited 0): a script that runs the CLI and reads its exit code now sees the failure, as the TPC-H harness does, which reports such a session as failed.
- **A raw `mssql_scan` of code-page text returns UTF-8, not the stored bytes**, on every code page (#224). A query that recovered the bytes (`::BLOB`, `encode()`) gets other bytes: `client_transcode false`, or `CAST(col AS varbinary(n))` in the T-SQL, gives them.
- **The types of a raw scan change:** a code-page `char` / `varchar` reports `MSSQL_VARCHAR(n, collation)`, as a catalog read does. So a CTAS from it into DuckLake is refused (`native_types false`, or a cast to `VARCHAR`), and one into SQL Server creates the column in that collation.
- **A CTAS or a creating COPY into Fabric takes what the warehouse can hold:** an `nvarchar(n)` or a code page's `varchar(n)`, from a SQL Server source or a cast, becomes `varchar(3n)` in the warehouse's collation (`MAX` past 8000), where it was refused. `CREATE TABLE` still refuses them by name.
- **A catalog read of a single-byte code page travels as bytes**, so the T-SQL in `EXPLAIN` shows the bare column, and more statements push: a computed value over such a column (`COALESCE`, `LAG`), derived-table keys, subqueries and set operations. A pushed `DISTINCT`, set operation or derived key compares the column itself, not an `NVARCHAR` cast: on a `SQL_` collation those are different rules (`'ss'` and `'ß'` are two values as varchar, one as nvarchar). `client_transcode false` restores today's reads. A column whose collation was changed, since the catalog loaded, to one newer than the build's code-page tables is refused by name rather than returned undecoded: run `mssql_invalidate_cache()`.
- `mssql_warn_non_utf8_collation` now fires only when bytes really do arrive unconverted: `client_transcode` off, or a collation newer than the tables.
- **The macOS release binaries need macOS 15 or later** (v0.3.0's needed 14). GitHub retired its macOS 14 runner images on 2026-11-02, so the macOS builds moved to macos-15; the binaries are built for macOS 15.0 (`MACOSX_DEPLOYMENT_TARGET`), and the release fails a macOS build that requires anything else (`scripts/ci/check_macos_minos.sh`). `INSTALL mssql FROM community` is unaffected.
- **The release binaries load into a stock DuckDB when the DuckDB pin is a release tag.** DuckDB 2.0 labels every build `-dev` unless told otherwise, the release tag's commit included. So the v0.3.0 binaries attached to the GitHub release say `2ec7e8401c` and load only into that build: a stock DuckDB 2.0.0 refuses them ("built specifically for DuckDB version '2ec7e8401c'"). With the pin on a tag, the build is labelled with the tag: CI, the release workflow and `make` set `OVERRIDE_GIT_DESCRIBE` (`scripts/ci/duckdb_release_tag.sh`). The binaries then load into that DuckDB, unsigned (`-unsigned`), and `INSTALL azure` works in a local or CI build, since DuckDB publishes its extensions for a release only under the tag. The release smoke test fails a release pin whose CLI or extension does not carry the tag (`scripts/ci/check_duckdb_label.sh`). `INSTALL mssql FROM community` is unchanged.

### Fixed

- **A `schema_filter` / `table_filter` regex no longer hides schemas or tables it matches** (#663). The regex is turned into a predicate ANDed into the catalog's metadata queries, and three forms made that predicate narrower than the regex: an empty alternative (`a|b|`, `^(a|b||c)$`), and an inner `^` / `$` (`^(a$|b)$`, `^(a|^b)$`, `^^foo$`). They are now converted to a predicate that keeps every name the regex can match, or not converted at all (the regex is applied on the client either way).
- **A filter comparing two string columns of different collations runs in DuckDB** (#635, found by #633's property tests). The catalog scan sent it to SQL Server, which answers error 468 ("Cannot resolve the collation conflict"). So a `SELECT`, `UPDATE` or `DELETE` filtered by, say, `v < u` failed on default settings. Now, wherever two string columns of two known collations meet in a comparison, `IN` or `BETWEEN` (read through the no-op cast over a column of another length), the comparison is not sent, as the remote-pushdown writer already did not send it. A `CASE` counts by its `THEN` / `ELSE` branches: one whose branches are columns of two collations is not sent (the server's 457), nor is one compared with a column of another collation. Columns of one collation, a constant, or a `CASE` over them are pushed as before. A table with no usable key, whose UPDATE / DELETE needs every filter on the server, now refuses such a statement by name where it failed with 468.
- **A MERGE that assigns a boolean expression to a `bit` column runs** (#636, found by #633's property tests). The staged MERGE wrote the condition where T-SQL needs a value, so the server refused it (102 / 156) and nothing was written, though the same value in an `UPDATE` worked. Examples are `UPDATE SET b = (t.n < 0)`, `SET b = (t.n IS NULL)` and `SET b = (t.b AND s.c)`. A condition at the top of the assigned value -- a comparison, `IS NULL`, `NOT`, `AND` / `OR` -- is now written as the bit DuckDB gives it, `CASE WHEN c THEN CAST(1 AS bit) WHEN (NOT c) THEN CAST(0 AS bit) END`, so an UNKNOWN stays NULL (`ExpressionVocabulary::AsValue`, `FilterEncoder::EncodeValue`). A condition nested below it (`SET b = COALESCE(t.n < 0, false)`) is still refused by name, nothing sent; an `INSERT … VALUES` of a condition reads the source alone and was computed by DuckDB already.
- **A result whose metadata declares what no server sends is refused by the column's name.** A `time` / `datetime2` / `datetimeoffset` column declaring a fractional-second scale past 7 was refused as `Column '' declares …`: the message was built before the column's name had been read. A `decimal` / `numeric` column declaring a precision outside 1..38, or a scale past its precision, was not refused at all: a precision past 38 surfaced as a DuckDB internal error (`Decimal has a width of 39 which is bigger than the maximum supported width of 38`). Both, and a vector column's element type and length, are now checked once the name is read, and the error says `Column 'amount' declares precision 39 and scale 0; … The TDS stream is malformed.` Only a server (or a proxy) that breaks the protocol sends such metadata.
- **A filter on a `uniqueidentifier` or `binary` / `varbinary` column returns the rows DuckDB's comparison picks** (#639). The filter was pushed to SQL Server, which compares these types differently.
  - **`uniqueidentifier`:** the server orders it by its own byte groups (the last six bytes first). A pushed range on one returned other rows: `g > k` gave 923 rows where DuckDB gives 999, and a `BETWEEN` gave 802 where DuckDB gives 501.
  - **`binary` / `varbinary`:** the server pads the shorter value with zero bytes, so `0x01 = 0x0100`. On a table with no unique key, `DELETE … WHERE vb = '\x01'::BLOB` also removed the `0x0100` row, and an `UPDATE` changed it.
  - **What is pushed now:**
    - a uniqueidentifier's `=`, `<>` and `IN` stay pushed, and its ranges run in DuckDB;
    - a binary's comparisons are all still pushed, but in DuckDB's terms: `a = b` is sent as `a = b AND DATALENGTH(a) = DATALENGTH(b)` (an index seek remains), and a range lets the lengths decide a tie;
    - so a `rowversion` range (`rv > @watermark`) is still pushed.
  - **Where:**
    - the scan's filters;
    - `rowid` on a binary key;
    - the staged `MERGE`'s conditions. A uniqueidentifier range in one is now refused by name;
    - an `UPDATE` / `DELETE` on a table with no unique key, which needs every filter on the server. One filtered by a uniqueidentifier range or `BETWEEN` is now refused by name, where it ran on the server's rows;
    - the staged UPDATE / DELETE on a table with no unique key. Its join pins a binary or string key column's length too: the byte match of a string, through `varbinary`, padded `'a'` to tie `'a' + CHAR(0)` as well.
  - **Unchanged:**
    - the remote-pushdown writer already refused both;
    - a unique key cannot hold `0x01` beside `0x0100` (the server's error 2627), so the keyed DML paths were not affected.
- **A backup made by SQL Server 2017 under writes reads a table whose extents were taken into use while it ran** (spec 085). SQL Server 2017 copies the data once, sizing the copy when it starts, and closes it with pad units; an extent allocated after that is not copied, its pages only in the backup's log tail. `mssql_backup_scan` failed on such a table, `the copy of page … is a pad unit` (or `the backup carries no copy of it`). The page is now read as one that became live while the backup ran, as a zero copy already was: when the log tail formats it, its rows are not read and a `WARNING` names it (refused under `fuzzy := 'error'`), and the table's fuzzy status already says it is stale (or dirty, for a transaction still open). A backup with a second data pass (2019 and later) copies such a page there, so one it lacks is still refused. Found by the oracle lane on a 2017 backup under load.
- **`mssql_backup_transaction_records` gives the page of a `LOP_MIGRATE_LOCKS` record** (spec 085), as `fn_dump_dblog` does: its fixed part has the page and no slot, and the page was read only beside a slot. Its `slot` is NULL.
- **A nullable `char` / `binary` declared under `ANSI_PADDING OFF` holding `''` / `0x` is read from a backup as the server reads it**: one space / one zero byte (`DATALENGTH` 1), where the reader trimmed it to nothing (spec 086 PR 3, measured on SQL Server 2025; a `char` is read trimmed in DuckDB either way, so it shows as `''`, while a `binary` reads `\x00`).
- **A catalog scan no longer returns rows its query's filter excludes when DuckDB pushes filters into it twice** (#623). The WHERE clause a scan stores from DuckDB's complex filter pushdown was replaced by a second call on the same scan, and the first call's conditions had already left the plan. A materialized CTE does this: `WITH c AS MATERIALIZED (SELECT * FROM db.dbo.t WHERE year(d) = 2024) SELECT * FROM c WHERE x = 1` read every year's rows of `x = 1` (600 where 73 are), read by one consumer or two. Each call's conditions are now ANDed onto the stored ones, their parameters numbered after them.
- **A TLS handshake is over by its timeout, whatever the server sends.** The handshake's packets were read with blocking reads behind one timed wait for a packet's first byte, so a server that sent part of a packet and then nothing -- before its certificate is checked -- kept ATTACH, or a pool refill, waiting for as long as it held the connection open, whatever `mssql_connection_timeout` said. Every read now waits within the timeout, and the handshake as a whole has one deadline: a server that sent a little just inside each read's timeout was never late either (a byte every 200 ms under a 300 ms timeout held the login for as long as it had bytes). It fails with `TLS handshake failed: Timeout after … ms`.
- **A signal during a wait is not a timeout.** A signal delivered to a thread waiting on a connection (`SIGINT` under a handler, `SIGCHLD`, a profiler's) ended the wait at once: a read gave up before its time, with TLS or without (a query failed with `Connection closed while waiting for COLMETADATA` or as timed out), a dial moved to the next address as `Connection timed out`, a TLS handshake failed, and a send cut short ended a healthy connection. The wait now goes on for what is left of its time. A wait that fails for a reason of its own (`poll` refusing) ends the connection, where it read as a timeout.
- **A dial that fails locally after waiting keeps the reason.** When a connection attempt failed with no route or no address of that family through the wait (not at once), the error was `Socket error during poll`: neither that reason nor the one an earlier address of the name gave, which it is meant to keep (review of #610).
- **Windows: a failed send or read names the Winsock error.** Outside the dial, socket errors were read from `errno`, which Winsock does not set: the message was whatever an earlier call left (`Send failed: No error`), and a stale `EINTR` / `EAGAIN` there would have retried a failed handshake read or send forever. They are read from `WSAGetLastError()` now, worded as the dial's are.
- **`TrustServerCertificate` takes yes or no, and nothing else.** In a connection string or URI, every value but `no` / `false` / `0` was read as yes: `TrustServerCertificate=off`, an empty value or a typo turned certificate verification off, and the connection accepted any certificate without saying so. It now takes `yes` / `no`, `true` / `false`, `1` / `0` (any case) and refuses anything else at ATTACH (without quoting the value, which can hold what follows it when a `;` is missing). `Encrypt` is unchanged: an unknown word keeps the session encrypted.
- **A port with trailing characters is refused.** `Server=host,1433x` (or `mssql://host:1433x/db`) connected to port 1433; it is now `Invalid port in Server parameter`, as a port that is no number already was. Neither that refusal nor `Port must be between 1 and 65535` quotes what was read as the port any more: in a URI whose password holds an unencoded `?`, that text is the start of the password. So is a URI's bracketed IPv6 literal with no port (`mssql://[::1]/db`), which was split at its last colon and dialled host `[:` on port 1. (A bracketed IPv6 literal in a URI is not supported: with a port it parses, but the brackets stay in the host name.)
- **A backup's bulk load or `TRUNCATE` after a page's copy is no longer read as consistent** (spec 085 D11, review of #591). Checked against a RESTORE on a backup whose changes are made at known points of its copy (`test/data/backup/tail_window`). An `INSERT … WITH (TABLOCK)` into a heap logs its pages as images, and a small table's `TRUNCATE` logs only its extents freed: such a table read `consistent` with its rows not the RESTORE's. Both count now: an open bulk load's table reads `dirty`, a committed one's or a truncated table's `stale`, and so does one whose bulk load or `TRUNCATE` was rolled back after the copy (the RESTORE undoes what the copy holds; review of #619); a table truncated by a transaction still open reads `dirty`. An index rebuilt after the copy reads `stale` too, though its rows are equal. Still read `consistent` where a RESTORE differs: a large table's `TRUNCATE` (deferred), a partition switched while the backup ran. A log whose blocks do not follow one another, or that begins a transaction id twice, is not read: the listings give the status NULL with a WARNING, the log functions an error.
- **An unreachable server is told to check the network, not its name** (review of #610). An error naming no route to the host, a host that is down or an unreachable network (`No route to host`, `Host is down`, `Network is unreachable`) read as a name that does not resolve -- the DNS case matched the bare word `host` -- and now reads `Cannot reach host:port - check network connectivity and firewall settings`. On a name of several addresses, an address that fails locally (no route to the network, no address of that family: an IPv6 address on a host without IPv6) no longer replaces what an address actually tried reported, a refusal or a timeout. On Windows a dial's socket error is named as on POSIX (`Connection refused`, `Connection timed out`, `No route to host`, ...) instead of `Unknown error`, so those reach the same advice.
- **A refused connection is reported as refused.** A port nothing listens on -- SQL Server stopped, the wrong port -- failed at once but was worded `Connection timed out`, and the ATTACH error advised checking network connectivity and firewalls. The socket's own error is now the reason: `Connection refused to host:port - check if SQL Server is running and accepting connections`. A dial that really times out reads as before. (On Windows the refusal is named too: see the entry above.)
- **The last message of a TLS 1.3 handshake inside PRELOGIN packets is sent.** The handshake's bytes are kept and sent when the next read asks; TLS 1.3 ends on the client's own message, with no read after it, so it stayed unsent while the connection was reported encrypted. SQL Server negotiates TLS 1.2 in this handshake (TLS 1.3 under TDS 8, which does not wrap it), so no known server was affected; found by the new socket test.
- **`ALTER TABLE … ALTER COLUMN` keeps what it does not name** (#577). SQL Server's `ALTER COLUMN` declares the column whole, and the extension filled in what the statement did not say from the wrong place: `ALTER COLUMN c TYPE BIGINT` on a `NOT NULL` column sent `bigint NULL` and dropped the constraint, and `SET NOT NULL` / `DROP NOT NULL` re-declared the column from its DuckDB type -- a `money` column became `decimal(19,4)`, a `datetime` `datetime2`, a `varchar(30) COLLATE x` `varchar(max)` in the database's collation. The column's declaration is now read from the server at the ALTER (not the catalog's cache, which `mssql_exec` or another client may have outdated): a type change keeps its nullability, and a character column changed to a character type that names no collation keeps its own; a nullability change re-declares it as the server declares it -- its type, length, precision and scale or fractional-second scale, collation, an alias type by its name, a CLR type by its schema-qualified name, a typed `xml` with its schema collection, `SPARSE` (kept through a type change too). Not carried: a `USING` expression (ignored, as before), and `MASKED` / `ANSI_PADDING OFF` attributes. A known limit: the declaration is read, then the ALTER sent, as two statements, so a change another session makes to the same column between the two is re-declared away.
- **A table whose columns differ only in case is read** (#572). A case-sensitive database (`COLLATE Latin1_General_100_CS_AS`) may hold columns `Id` and `ID` in one table; DuckDB's names are case-insensitive, quoted or not, so the table failed with `Column with name "ID" already exists!` wherever it was named. Such a column now reads under the first `_N` suffix no column of the table is named -- `Id`, `ID_1`, `id_2`; every real name is reserved first, so a real column keeps its own -- and every statement the extension sends names the server's column: the scan, filter / ORDER BY / TOP pushdown, a statement sent whole (its result columns aliased to the DuckDB names), INSERT / UPDATE / DELETE / MERGE, COPY into the table (a source column loads the column of its DuckDB name), ALTER TABLE (a DuckDB name maps to the server's column); the hidden key columns of a table with no key and a composite rowid's fields are named as the columns read (`rowid.ID_1` is the column `"ID_1"`). `mssql_scan`'s result columns take the same rule, so an exact repeat (`SELECT a.id, b.id`), which DuckDB refused, now reads as `id`, `id_1`; an unnamed column stays DuckDB's to name. Because every real name is reserved, a column the server adds later can move a suffix (a new real `ID_2` makes the old `ID_2` `ID_3` once the catalog reloads).
- **A backup's nullable `char` / `binary` declared under `ANSI_PADDING OFF` reads as the server returns it** (spec 086 PR 1, E22): without its trailing zero bytes (a `binary(8)` holding `0x0100` read `0x01`, as `DATALENGTH` says), where the reader returned the stored, padded 8 bytes. A `char` read trimmed already. A clustered key of such a column (a non-unique or non-primary-key index) gives `_mssql_rid` its trimmed bytes accordingly.
- **A backup's table with a user CLR type of `Format.Native` is read** (spec 093). The server stores such a type at a fixed offset and its catalog describes it there as `binary` of the type's size; the reader refused the table ("stored as xtype 173 …, which its declaration does not say"), and a nullable one lost its trailing zero bytes, taken for `ANSI_PADDING OFF` padding. It now reads as its stored bytes, like every CLR column until T8, uncompressed, ROW- and PAGE-compressed and in a heap (fixture `test/data/backup/clr`).
- **A backup's table whose CLR column (`hierarchyid`, `geometry`, …) the catalog describes as `varbinary` is read** (spec 086 PR 1): `SELECT INTO` makes such a column, and the table was refused as "stored as xtype 165, which its declaration does not say"; the column's bytes are the type's, read as before.
- **A table changed on the server after the catalog loaded it fails once and reads again** (#570). The catalog keeps a table's columns until it is invalidated (`mssql_catalog_cache_ttl` 0, the default), so after another client dropped or renamed a column, or the table, every read of it sent the old column list and failed with the server's `Invalid column name` (207) / `Invalid object name` (208), until `mssql_invalidate_cache()`. Such a read now drops the table's cached entry, as `mssql_invalidate_cache(db, schema, table)` does, and says so: the error names the table and asks to run the statement again, which then binds against the server's columns. A statement the remote-pushdown rewriter sent whole (an aggregate, a join, an ORDER BY …) still fails until the catalog is invalidated: it can read several tables, and the server's error does not say which.
- **A filter on a SQL Server 2025 `json` column runs in DuckDB** (spec 093, J1). The server compares a `json` value by nothing but `IS NULL`, so a pushed `WHERE doc = '…'` (or `IN`, `<`, …) failed with error 402; it is now evaluated in DuckDB over the text read, and `IS NULL` is still pushed. For the same reason, on the row-by-row path (an UPDATE / DELETE the server is not sent whole), a table with no usable key and a `json` column refuses UPDATE / DELETE by name, as it does for an `xml` column, and so does a staged MERGE whose ON compares a `json` column; both used to reach the server with a comparison of the `json` column, which it refuses (13636).
- **A login Azure SQL refuses for a while is tried again** (#481). A serverless database resuming answers 40613 until it is up, and the ATTACH, or the statement whose pool refill met it, failed with a message saying to retry. The login is tried again on 40613, 40197, 40143, 40501, 49918 and 42109 (a Synapse / Fabric pool warming up; not 42108, a paused one) -- the server's error number decides, never the text, and never 18456 or a network failure -- with a pause of 1 s doubling to 10 s, within `mssql_connection_timeout` (the ATTACH's validation timeout while it runs). Each attempt is an INFO line in `duckdb_logs`, and Ctrl-C ends the wait wherever the ATTACH or a statement asks for a connection. An extra parallel bulk-load writer, the planner's row-count refresh and a failed CTAS's cleanup make one attempt, as before; the connections `min_connections` opens retry with no Ctrl-C. Out of time, the error says how many times it tried.
- **`mssql_scan` of a statement that returns no result set is refused by name** (#525). `PRINT`, `SET`, a procedure that only prints bound with no columns, and DuckDB answered with an internal error (`Table function must return at least one column`). It now says the statement returns no result set and points at `mssql_exec`; a batch run at bind to learn its shape gives its connection back first. `mssql_scan_params` too.
- **A filter on a `sql_variant` column lost rows** (#514). It was sent against the raw column, which SQL Server compares by the value's base type: `WHERE v = '5'` found an `nvarchar` `'5'` and not an `int` 5, though DuckDB reads both as `5`. A filter on a column read through a cast (`sql_variant`, `hierarchyid`, a CLR type) now runs in DuckDB, over what DuckDB read.
- **A CTAS from a code-page column stored mangled text.** Over BCP, `CREATE TABLE ... AS SELECT` from a column under a code-page collation declared the rows UTF-8 and the server stored those bytes in the column's page: `ж` read back as `Р¶`, silently. Such a column now travels as nvarchar, and so does a plain one under an `mssql_utf8_collation` naming a code page. Tables already created this way hold the wrong bytes.
- **A parameter from an annotated string column lost its non-ASCII characters.** It was declared `varchar(n)`, which takes the database's code page, not the column's: `ж` became `?`. Such a value goes as nvarchar.
- **A statement whose branches return different string lengths** (`IF` arms, a procedure's) failed with "result shape changed between bind and execution". A stream no longer than the described length now fits.

- **A metadata load no longer deadlocks with another session's DDL.** A load (a table's first use, `SHOW TABLES`, `duckdb_tables()`) runs in the open transaction, and when it waited on a row another session's uncommitted DDL held, the two could deadlock, with the load as the victim: SQL Server rolled back the whole transaction, and the error said `The server failed to resume the transaction`. It could also make the other session's `DROP` the victim. A load now never queues for a lock (`LOCK_TIMEOUT 0`): it reads again until `mssql_metadata_timeout`. If that passes, the error names the cause and the statement has failed, so DuckDB asks for a `ROLLBACK` as usual. Two transactions that each hold uncommitted DDL and each list the catalog now both wait out that timeout and both fail, where a deadlock used to roll one of them back within seconds and let the other go on. A CTAS, DDL or a first read of a table needs no listing and does not wait for another transaction's uncommitted DDL. No permission is needed. Measured over 3 interleaved runs of the test suite on 12 workers: 63 deadlocks before, 0 after. Not on Fabric Warehouse or Synapse.

## [0.3.0] - 2026-10-20

The first release for **DuckDB 2.0**. DuckDB 1.5.x stays on the 0.2.x line
(v0.2.5).

### Highlights

- **Whole queries run on SQL Server** (spec 079, `mssql_remote_pushdown`, on
  by default). A SELECT the extension can write in T-SQL — joins of one
  database's tables, aggregates, GROUP BY / HAVING, DISTINCT, window functions,
  QUALIFY, ORDER BY / LIMIT, subqueries, CTEs, set operations — goes to the
  server as one statement, and `EXPLAIN` shows it.
- **UPDATE, DELETE, INSERT … SELECT and MERGE run on SQL Server as one
  statement** (spec 080), when they can be written in T-SQL: no row crosses the
  wire and no key is needed.
- **MERGE INTO from any source** (spec 080 PR 4b): a DuckDB table, a file, a
  VALUES list or another database is staged into a session `#src` and merged
  by one server-side MERGE. A MERGE into an MSSQL table always runs on the
  server, atomically.
- **UPDATE and DELETE need no primary key and can return rows** (spec 077,
  spec 080): a usable unique index serves as the row key, a table with none is
  keyed by every column, and `UPDATE / DELETE … RETURNING` work. Past
  `mssql_dml_stage_threshold` rows they go through a session `#stage` and one
  JOIN statement instead of VALUES batches.
- **INSERT loads through BCP** (spec 062): an INSERT of 1M rows went from
  74.7 s to 0.51 s, with CHECK constraints and triggers still applied.
- **An autocommit UPDATE / DELETE / MERGE commits and rolls back with DuckDB**
  (#451): an error later in the statement, Ctrl-C or a dropped result undoes
  the write.
- **Parameterized statements** (specs 075, 076, 083): pushed filter constants
  travel as typed RPC parameters, one server plan per statement shape;
  `mssql_scan_params` / `mssql_exec_params` take parameters from DuckDB, and
  `prepared := true` compiles once.
- **`mssql_scan` describes instead of executing at bind** (spec 075):
  `DESCRIBE` / `EXPLAIN` no longer run the query.
- **A faster, transaction-aware catalog** (specs 076, 084, #380, #383): a
  table's first touch is one round trip, metadata inside a transaction is the
  transaction's own, and new ATTACH options (`preload`, `min_connections`,
  `default_schema`, `transaction_isolation`, `native_types`,
  `remote_pushdown`) reach DuckLake's `METADATA_PARAMETERS`.
- **TLS certificates are verified by default** (spec 074), with
  `HostNameInCertificate` for a name that differs from the address dialled.
- **Microsoft Fabric Warehouse**: UPDATE, DELETE and MERGE.
- **Correctness**: comparisons with `datetime` ticks (#426), `char(n)` padding
  (#405), dates SQL Server cannot hold (#447), a DELETE that removed the wrong
  rows (#337), the pool overshooting its limit (#461) — the full list is below.

### Upgrade notes

- **DuckDB 2.0 is required.** On DuckDB 1.5.x install v0.2.5.
- **The server certificate is verified by default** (spec 074). A server on a
  self-signed certificate — the docker image, many on-premises instances — now
  fails the handshake with `certificate verification failed for <host>`. Add
  `TrustServerCertificate=yes` to the connection string,
  `trustservercertificate=true` to the URI, or `trust_server_certificate true`
  to the secret, or install the certificate's CA.
- **An autocommit UPDATE / DELETE / MERGE joins DuckDB's statement
  transaction** (#451): it commits when DuckDB commits the statement, and it
  holds its connection until the query ends.
- **`enable_external_access = false` is honoured** (#452): ATTACH and the
  authentication test functions are refused.
- **Remote pushdown is on by default** (spec 079). In a pushed statement,
  GROUP BY, DISTINCT, joins and set operations compare strings under the
  column's SQL Server collation — a `_CI_` collation groups `'a'` with `'A'`.
  `SET mssql_remote_pushdown = false`, or `remote_pushdown false` on the
  ATTACH, keeps every statement on the scan path.
- **A MERGE into an MSSQL table runs on the server or is refused by name**
  (spec 080 D4). DuckDB's own client-side MERGE no longer runs for it, so these
  are refused: more WHEN clauses than T-SQL takes, `ERROR`, an INSERT naming
  the identity column, `RETURNING`, and any MERGE with DuckDB's optimizer
  turned off.
- **An INSERT of more than `mssql_insert_bcp_threshold` (1000) rows uses
  INSERT BULK** (spec 062). On a heap it takes a TABLOCK (`mssql_copy_tablock
  auto`), so readers under READ COMMITTED wait for the load.
  `mssql_insert_use_bcp = false` keeps INSERT statements.
- **Removed**: `mssql_open()`, `mssql_close()`, `mssql_ping()` and
  `mssql_close_all()`; ATTACH a catalog instead.
- **Deprecated, no effect**: `mssql_dml_use_prepared` and
  `mssql_insert_use_returning_output`. A `SET` of either still parses; both
  go in a later release.

### Added

- **MERGE INTO from any source runs on SQL Server as one statement** (spec 080
  PR 4b). A MERGE whose source is not in the same database — a DuckDB table, a
  VALUES list, a file, another database — or any MERGE with
  `mssql_dml_pushdown = false` is staged: DuckDB computes the source and every
  expression over the source alone (any DuckDB function), the rows go into a
  session `#src_<uuid>` by INSERT BULK on the statement's connection, and ONE
  `MERGE [s].[t] AS [t] USING [#src] AS [s] ON … WHEN …;` does the match, the
  conditions and the writes on the server, then `#src` is dropped. EXPLAIN shows
  it as `Mssql Staged Merge` with the T-SQL.
  - One statement, one server transaction: a failing row rolls back every
    other, and in autocommit it commits with DuckDB's statement (#451).
  - No key is needed: a table with no usable key, which DuckDB's own MERGE
    refused, is merged by the server.
  - What touches the target — the ON condition, each WHEN condition, a SET /
    INSERT value that reads the target — is written in T-SQL with the scan's
    filter rules and the column collations (D4). A source value compared
    directly with a target column is staged in that column's own type and
    collation, so a `datetime` read back matches its row (#358, #426); strings
    and binaries are widened to MAX so no value is cut into a false match, and
    a code-page `char` / `varchar` is compared in Unicode (`'Łukasz'` is not
    `'Lukasz'`). Any other source value is staged in its CTAS type.
  - As the server compares: a source value between two of a temporal column's
    steps (a `datetime` tick, a `smalldatetime` minute, the digits of a
    `datetime2(n)` / `time(n)` / `datetimeoffset(n)` below 7) is rounded onto
    the column's precision first, as the column would store it.
  - A column ON compares with the source must survive the round trip through
    DuckDB: `time(7)` / `datetimeoffset(7)`, `xml`, spatial and CLR columns
    are refused there, since an unmatched row would make a WHEN NOT MATCHED
    INSERT duplicate it.
  - Refused by name, nothing sent: a shape T-SQL does not have (see PR 4a's
    list), `ERROR`, `RETURNING`, an INSERT naming the identity column (a MERGE
    runs without IDENTITY_INSERT), a subquery, an expression over the target
    T-SQL cannot write, a value DuckDB computes per row (`random()`, a
    parameter) in a WHEN NOT MATCHED BY SOURCE clause, Azure Synapse, and any
    MERGE with DuckDB's optimizer off (`enable_optimizer = false`), where the
    extension cannot rewrite it.
  - **Changed** from DuckDB's own MERGE, which ran each action through the row
    paths and is gone for MSSQL tables: more than two WHEN MATCHED clauses and
    an identity column in the INSERT are now refused; `INSERT DEFAULT VALUES`
    is sent as such; `UPDATE` with no SET list is written as a SET of every
    column; `SET x = DEFAULT` writes NULL, as before; a MERGE whose every
    action is `DO NOTHING` stays a no-op (count 0, nothing sent).

- **MERGE INTO within one database runs on SQL Server as one statement**
  (spec 080 PR 4a). The remote-pushdown rewriter hands over a MERGE whose
  source is in the same catalog whole: `MERGE [s].[t] AS [r1] USING … ON … WHEN
  …;` through `mssql_dml` (`kind := 'merge'`). The server matches the rows,
  and no key is needed.
  - The source can be a table, `USING (columns)`, or a query over the
    database's tables.
  - The WHEN clauses must fit T-SQL's shape: two WHEN MATCHED (UPDATE and
    DELETE, the first conditional), one WHEN NOT MATCHED INSERT, two WHEN
    NOT MATCHED BY SOURCE (UPDATE and DELETE, the first conditional). A last
    DO NOTHING is dropped.
  - A MERGE whose source is elsewhere runs through `#src` (PR 4b, below);
    one T-SQL has no form for is refused by name.

- **INSERT … SELECT within one database runs on SQL Server as one statement**
  (spec 080 PR 3d), through the same `mssql_dml` vehicle as a pushed UPDATE /
  DELETE. No row crosses the wire, and a value is copied by the server as it
  is.
  - A number, `bit` or `date` must have the column's type or be an integer
    widened into a wider one. Anything else must be a column declared alike.
  - Such a copy is exact, which the row path is not for every type: it reads
    the value through DuckDB. It drops a `datetimeoffset`'s offset and a
    `time(7)`'s 100 ns digit, turns a `datetime2(7)` past 2262 into NULL,
    gives a geometry SRID 0, and loses a `sql_variant`'s base type. So with
    `mssql_dml_pushdown` on or off such a copy stores different values. The
    pushed one is the faithful one. A pushed `UPDATE … SET a = b` (PR 3c) is
    the same.
  - A column the statement does not name gets the server's DEFAULT, as on the
    row path. Naming the identity column brackets the statement with `SET
    IDENTITY_INSERT`.
  - These stay on the row paths: VALUES, a DuckDB or other-database source,
    RETURNING, `ON CONFLICT`, `BY NAME`, a string constant.
  - `mssql_dml_pushdown = false` keeps them all there.
  - The SET rule of PR 3c now also takes an integer widened into a wider
    column (`SET big = small`).
  - A pushed UPDATE / DELETE / INSERT … SELECT has no deadline. The server
    answers nothing until the statement is done, and `mssql_query_timeout`
    (30 s) used to roll back a large copy that the row path finished.
    Cancelling the query (Ctrl-C) cancels the statement (ATTENTION).
  - Every connection now sets TCP keepalive (probes after 30 s idle, every
    10 s, 6 unanswered), so a dead peer ends such a wait with an error in
    about 90 s. It also keeps an idle pooled connection alive through a NAT
    or load-balancer idle timeout.

- **UPDATE and DELETE run on SQL Server as one statement** (spec 080 PR 3c),
  when their WHERE and SET can be written in T-SQL. No rows cross the wire and
  no key is needed: a table the row path refuses (no usable key, an `xml`
  column, a `TIME(7)` key) is written by the server, and a SET of a key column
  goes. `UPDATE … FROM` goes when the WHERE equates the source's key with the
  target's columns; `DELETE … USING` tables of the same database goes.
  Stays on the row paths: RETURNING, a function with no T-SQL form, a SET value
  DuckDB would convert (it rounds `SET int_col = 1.5`; the server truncates), a
  view, a statement inside a WITH, and on Fabric any FROM / USING / subquery.
  The statement's type is SELECT to a client (`SELECT * FROM mssql_dml(…)`)
  and its errors are the server's. `SET mssql_dml_pushdown = false` (new,
  default true) keeps every UPDATE / DELETE on the row paths. Checked by an
  agreement suite that runs each shape through a pushing alias and a
  `remote_pushdown false` one and compares rows and counts.

- **`mssql_dml()`, the vehicle of a pushed UPDATE / DELETE** (spec 080 PR
  3b), internal: the remote-pushdown rewriter is to send such a statement to
  SQL Server whole through `SELECT * FROM mssql_dml(...)` (PR 3c). It runs the
  statement once, when the query executes (never at `EXPLAIN` / `PREPARE`, once
  per `EXECUTE`), consumed exactly as the native DML it replaces (since DuckDB
  #26880 a submitted write, native or this, runs and waits at its count until
  its consumer settles it; checked by `make test-dml-vehicle`) --
  atomically in autocommit and on the transaction's connection inside one, and
  under DuckDB's read-only and one-writable-database rules. Its `Count` is the
  server's: measured, an AFTER trigger's counts arrive before the statement's.
  `MSSQL_COUNTERS=1` now also prints which way each UPDATE / DELETE went
  (`dml: pushed / staged / values_join`).

- **UPDATE and DELETE on Fabric Warehouse** (spec 080). Fabric has no
  `UPDATE / DELETE … FROM … JOIN`, which the VALUES-join statements are, so a
  keyed UPDATE / DELETE there failed on the server. Every UPDATE / DELETE on
  Fabric now goes through `#stage` (filled by Fabric's BCP API, in preview,
  which COPY and CTAS already use) and selects the stage's rows through
  `WHERE EXISTS` / a correlated subquery; a table with no key works the same
  way. RETURNING and a MERGE with an UPDATE / DELETE action stay refused by
  name there. Tested against SQL Server emulating Fabric
  (`mssql_test_dml_platform`, test only); `fabric-probe/` p19 / p20 are to
  confirm the forms on a live warehouse.

- **UPDATE / DELETE … RETURNING** (spec 080 PR 2b). The rows go through the
  same `#stage` as a large UPDATE / DELETE, whatever their number, and the
  JOIN statement carries `OUTPUT inserted.* / deleted.* INTO #out`, which is
  read back as the scan reads the table: UPDATE returns the rows as written
  (computed and rowversion columns included), DELETE as they were, with the
  `rowid`. `OUTPUT … INTO` works beside an enabled trigger, where a bare
  `OUTPUT` is refused. Refused by name on Fabric and Synapse (no stage yet),
  and where a key or SET column cannot be bulk-loaded (geometry).

- **UPDATE and DELETE on a table with no key** (spec 080, #140). A table with
  no primary key and no usable unique index is keyed by all of its columns: the
  selected rows go into a session `#stage` by bulk load and one
  `UPDATE … JOIN #stage` / `DELETE … JOIN #stage` finds them again by value
  (NULL equal to NULL, strings byte for byte). Rows identical in every column
  move together, and the count includes them. Refused by name — never a
  statement that changes nothing — when the server would not select the same
  rows as DuckDB (part of the WHERE runs in DuckDB, a volatile function,
  `UPDATE … FROM` / `DELETE … USING`, `MERGE INTO`), or when a column cannot
  be a key (`xml`, spatial, `text`, `sql_variant`, `time(7)`, …).

- **`mssql_dml_stage_threshold`** (default 1000, spec 080): an UPDATE / DELETE
  on a keyed table with more rows than this goes through the same `#stage`,
  as one statement, instead of `VALUES` statements of `mssql_dml_batch_size`
  rows. 0 stages every statement.

- **`native_types` ATTACH option**: `mssql_catalog_native_types` for one
  catalog, over the global setting. `ATTACH '…' AS meta (TYPE mssql,
  native_types false)` reports that catalog's bounded string columns as plain
  `VARCHAR`, in its tables and its `mssql_scan` results, while every other
  attached catalog keeps `MSSQL_VARCHAR(n)` / `MSSQL_NVARCHAR(n)`. Takes a
  string too (`'false'`, `'no'`), so DuckLake's `METADATA_PARAMETERS` can set it
  for a metadata catalog. Unset, the setting decides as before.

- **`mssql_scan_unsafe` / `mssql_scan_params_unsafe`** (spec 081): `mssql_scan`
  / `mssql_scan_params` with the result's shape given as
  `columns := {'name': 'TYPE', ...}` instead of described. The bind asks the
  server nothing, so the call binds inside a transaction, on a pool of one
  connection, and for a batch the server cannot describe (a `#temp` read) like
  any other statement; the rows are checked against the shape when the
  statement runs (column count, the kind of each column as the catalog reads
  it, no conversion; a string's length and collation are not checked), and a
  mismatch fails the statement naming it. A type no SQL
  Server column is read as is refused at bind. `EXPLAIN` shows
  `Shape: given (columns :=)`.

- **Remote pushdown, first shapes (spec 079 PR B), behind
  `mssql_remote_pushdown`** (read at ATTACH; on by default since PR E2, see
  Changed). An attached catalog answers DuckDB's remote-pushdown
  rewriter, which then sends a whole single-table `SELECT` to SQL Server as
  one statement: its columns, a `WHERE` of column-vs-constant comparisons,
  `IS [NOT] NULL` and `AND` / `OR` / `NOT`, `ORDER BY` on columns, and
  `LIMIT` / `OFFSET` as `TOP` / `OFFSET … FETCH`. The statement runs through
  `mssql_scan_params` (or `mssql_scan` when it has no constants), so `EXPLAIN`
  shows the T-SQL it sends. A node is taken only when it sends the server more
  than the catalog scan would: in this PR an `ORDER BY` or a `LIMIT`. A node
  without either stays with the catalog scan, which pushes projections and
  filters itself and still takes a filter from an enclosing query. Anything
  outside that list is left to the scan path, never guessed. Not pushed in
  this PR: `$n` parameters, and an unqualified name while the session's
  search path puts another schema of the catalog first (`USE db.sales`).
  While the setting is on, the catalog's schema `main` answers as its default
  schema for every lookup (`db.main.t`, `CREATE TABLE db.main.x`), DuckDB
  skips its catalog-or-schema ambiguity check for the catalog, and the
  rewriter runs on every statement of the instance.
- **Remote pushdown: the expression vocabulary (spec 079 PR C).** A pushed
  single-table SELECT now carries computed columns and real WHERE clauses:
  arithmetic over same-type integer / decimal operands, division (floating,
  as DuckDB's `/`), widening casts, CASE, COALESCE, IN, BETWEEN, LIKE / NOT
  LIKE, IS NULL of any value, a bit column as a condition, and a column
  compared with a column of the same type and collation. The scan path's
  filter pushdown and the rewriter now render through one vocabulary
  (`pushdown/mssql_expression_vocabulary`), so a construct both paths take
  answers the same whichever takes it. What is not pushed, each with a measured reason:
  mixed-type arithmetic (the promotions differ), decimal arithmetic whose
  result passes 38 digits (the server rounds), a division inside a condition
  (a zero divisor is `inf` in DuckDB and NULL on the server, which would
  change the rows; for a selected value the NULL is the documented
  divergence), ILIKE / GLOB / LIKE … ESCAPE, a code-page `varchar` inside a
  computed value (it would arrive as its code-page bytes), a division under
  COALESCE, a string constant in a CASE / COALESCE branch wider than its
  column. A computed column's type is the server's, as for any `mssql_scan`.
- **Remote pushdown: joins, aggregates, DISTINCT (spec 079 PR D).** A pushed
  SELECT now carries joins of one catalog's tables (INNER / LEFT / RIGHT /
  FULL / CROSS with ON or USING; SEMI / ANTI as `EXISTS` / `NOT EXISTS`),
  `count` / `sum` / `avg` / `min` / `max` / `stddev` / `variance`, GROUP BY on
  columns (by name, position or select alias), HAVING, `SELECT DISTINCT` and
  ORDER BY an aggregate (`… ORDER BY count(*) DESC LIMIT 10`). Aggregates have
  DuckDB's types: an integer `sum` is HUGEINT (sent as `decimal(38,0)` and
  cast back). A floating-point aggregate (`sum` / `avg` of a float, any `avg`,
  `stddev` / `variance`) may differ from DuckDB's in its last bits, as
  DuckDB's own parallel float sum does; it is not pushed where that would
  choose rows (HAVING, ORDER BY, DISTINCT). Under D4 a string group or
  DISTINCT follows the column's collation (`a` and `A` are one value on a
  `_CI` collation); `min` / `max` of a string is not pushed. `GROUP BY ()` is
  sent without a GROUP BY clause (over no rows the server would return no row
  where DuckDB returns one). Not pushed: FILTER, `string_agg`, ROLLUP /
  CUBE / GROUPING SETS / GROUP BY ALL, expression keys, NATURAL / ASOF /
  POSITIONAL joins, a join with a subquery or a local table, a SEMI / ANTI
  join under a later RIGHT / FULL join, and a `CROSS JOIN` (or comma join)
  with nothing else to gain. `stddev` / `variance` can differ by far more
  than the last bits on large, close values (the server computes them in one
  pass). A `float` column is compared and divided when pushed, not added,
  subtracted or multiplied (an overflow is `inf` in DuckDB and an error on
  the server).
  A computed result column now has DuckDB's type or is not pushed: a decimal
  beside a constant (`v + 700`) stays with DuckDB as a result column. A
  comparison of an integer `x + c` / `x - c` / `x * c` / `-x` with a constant
  is not pushed, as DuckDB moves the constant across and never computes a
  value the server could overflow on. A repeated ORDER BY key no longer fails on the
  server (error 169, since PR B).
- **Remote pushdown: whole statements and their parts (spec 079 PR E1).**
  - A statement over one catalog now goes to the server whole when it is
    built from subqueries in FROM, subqueries in expressions (`IN`,
    `EXISTS`, a scalar subquery, correlated or not), set operations inside a
    subquery (`UNION [ALL]`, `EXCEPT`, `INTERSECT`) and `WITH` clauses (each
    CTE inlined where it is referenced).
  - A statement that cannot go whole (an `EXCLUDE`, a window, a local
    table) has each part that can go replaced by its own `mssql_scan`, and
    DuckDB runs the rest. A set operation at a statement's top always runs
    this way: DuckDB combines the children.
  - A node that would send the server nothing the catalog scan does not is
    left to the scan, which still takes filters from above it.
  - A join whose gain is uncertain (many-to-many, or `CROSS` with no key
    equality) is pushed only while every table it joins has fewer rows than
    the new setting `mssql_pushdown_join_rows_threshold` (default
    1000000, `0` turns the check off), by the cached row counts. It is not
    checked inside a transaction or on a pool of one connection.
  - `UNION` / `EXCEPT` / `INTERSECT` over strings compare under the
    column's collation when pushed, as `DISTINCT` does (D4).
  - A CTE whose body has a `LIMIT` is not inlined where the server could
    evaluate it twice. A CTE body inlined twice is read twice by the server.
  - A scalar subquery is pushed only when it returns one row (a key lookup,
    an aggregate, a `LIMIT 1`): the server evaluates it lazily and would
    return rows where DuckDB raises "More than one row returned".
  - A join inside a subquery or a CTE is gain-checked with the statement
    around it; a subquery's `GROUP BY` columns count as its key.
  - Under `USE db.sales`, `db.t` is read as DuckDB reads it (`sales.t`); it
    was pushed reading `dbo.t`.
  - A set operation's child that DuckDB sends on its own (the set operation
    also reads a local table) is pushed like a statement: a division by zero
    in it is NULL, not `inf`, before DuckDB deduplicates or filters.
  - Not pushed: `EXCEPT ALL` / `INTERSECT ALL`, `UNION BY NAME`, recursive
    CTEs, CTE column aliases, `> ANY` / `ALL`, and any scalar subquery under
    `scalar_subquery_error_on_multiple_rows = false`.
  - In a statement that also reads a local table, DuckDB's rewriter does not
    yet hand a FROM subquery or a CTE body over on its own
    ([duckdb/duckdb#26280](https://github.com/duckdb/duckdb/issues/26280)),
    so their aggregate runs in DuckDB.
- **Remote pushdown: windows, QUALIFY, FILTER, string_agg; the describe
  result shapes cached (spec 079 PR E2).**
  - Window functions go to the server: `row_number`, `rank`, `dense_rank`,
    `ntile`, `lag` / `lead` (an offset, a default), `first_value` /
    `last_value`, and aggregates `OVER (…)` with `PARTITION BY`, `ORDER BY`
    and `ROWS` frames. An integer `sum` over a window goes through
    `decimal(38,0)` (the server's `SUM(int)` overflows) and `avg` in an exact
    form (the server's `AVG(int)` truncates).
  - `QUALIFY` goes too: `QUALIFY rn = 1` filters the window's result in an
    outer query, the window evaluated once.
  - `agg(x) FILTER (WHERE c)` as `agg(CASE WHEN c THEN x END)`;
    `string_agg` / `group_concat` / `listagg` as `STRING_AGG(… nvarchar(max) …)
    WITHIN GROUP (ORDER BY …)`.
  - Not pushed: `IGNORE NULLS` (SQL Server 2022 on), `percent_rank`,
    `cume_dist`, `nth_value`, `RANGE` frames with offsets, `GROUPS`,
    `EXCLUDE`, `count(DISTINCT …) OVER`, `string_agg(DISTINCT …)`.
  - The result shape of a pushed statement is cached per statement form
    (constants are parameters), so planning it again costs no round trip and
    no connection, in a transaction too while it has changed no schema; the
    cache ends with the catalog's metadata. A table changed by another client
    fails the next pushed statement over it once, saying to run it again. Measured on a
    local server: planning 1.9 ms → 0.68 ms, a point read 4.6 ms → 2.7 ms;
    on a 1M-row table an aggregate, a TOP N, a QUALIFY and a join ran 4-15x
    faster pushed.
  - `mssql_pushdown_min_rows` (default 0): a statement whose tables hold
    fewer rows together than this is left to the plain scans.
  - The ATTACH option `remote_pushdown true/false` gives one database its own
    answer, over the setting — what DuckLake's `METADATA_PARAMETERS` can pass.
- **`column_types := [...]` on `mssql_scan` / `mssql_scan_params`**: the type
  each result column is read as, `''` for the described one, checked against
  the server's describe. The rewriter uses it so a pushed `SELECT` has the
  catalog's column types: `datetime2(7)` as `TIMESTAMP_NS` with its 100 ns
  ticks, a `varchar` under a code-page collation as its declared
  `MSSQL_VARCHAR(n)`, `geometry` as `GEOMETRY`.
- **`EXPLAIN` shows the statement of an `mssql_scan` / `mssql_scan_params`.**
- **ORDER BY … LIMIT on a nullable key is pushed** under
  `mssql_order_pushdown`. SQL Server sorts NULL lowest and has no
  `NULLS FIRST` / `LAST`, so a nullable key asking for another placement
  stopped the pushdown before. That included DuckDB's default, `NULLS LAST` on
  an ascending key. Under a LIMIT a leading `CASE WHEN key IS NULL` key now
  gets DuckDB's placement and the server returns only the N rows. A plain
  ORDER BY still sorts in DuckDB: that leading key defeats any index, so the
  server would sort the whole table where DuckDB does now.
- **`mssql_scan` reports `MSSQL_VARCHAR(n)` / `MSSQL_NVARCHAR(n)`**, as the
  catalog has since spec 060, under `mssql_catalog_native_types`, so
  `CREATE TABLE … AS SELECT * FROM mssql_scan(…)` keeps the source's lengths
  (and a UTF-8 `varchar`'s collation) instead of making `nvarchar(max)`.
  `typeof()` of such a column changes from `VARCHAR`; setting the option to
  `false` restores it. With `mssql_utf8_support = false` the column is still
  reported as the declared `MSSQL_VARCHAR(n)`, although it travels as
  nvarchar. A `char` / `varchar` under a code-page collation stays
  plain `VARCHAR`: a raw scan hands its bytes over untranscoded, and an
  annotation naming the code page would invite a re-encode. `prepared := true`
  reports the same types, asking `sp_describe_first_result_set` for the
  collations `sp_prepare` names only by id.
- **The pool opens its connections at ATTACH, in parallel; `preload` loads the
  catalog at ATTACH** ([#324](https://github.com/hugr-lab/mssql-extension/issues/324)).
  - **`mssql_min_connections` did not open anything.** It only kept idle
    connections from being closed. The pool now opens that many connections
    at ATTACH, with their logins running concurrently.
  - **ATTACH validates through the pool.** The credentials check is the
    pool's first login, and the connection stays in the pool instead of being
    closed and logged in again: a plain ATTACH is one login, not two. Pool
    refills now report the same classified reasons the ATTACH did (server error
    number and state: a paused serverless database, the 18456 login state).
    Every ATTACH validation failure is now an `Invalid Input Error` reading
    "MSSQL connection validation failed: …" -- a refused dial or an unknown
    host used to be an `IO Error`, and an Azure AD failure read "MSSQL Azure AD
    connection validation failed: …" (now "…: Azure AD authentication
    failed: …").
  - **Measured on a local server:**

    | ATTACH | before | after |
    |---|---|---|
    | plain | 0.40 s | 0.20 s |
    | `min_connections 4` | 0.54 s | 0.39 s |
    | `min_connections 8` | 0.63 s | 0.54 s |

    Opened one after another, four connections would cost about a second.
  - **Nothing opened under `lazy_validation`.** The ATTACH options `preload` and
    `min_connections` are refused beside it, and `preload` inside a transaction
    that has used an MSSQL catalog is refused — all before the ATTACH dials. A
    `min_connections` shortfall is logged as a WARNING and does not fail the
    ATTACH.
  - **New ATTACH options:**
    - `min_connections` is the ATTACH form of the setting.
    - `preload true` runs `mssql_preload_catalog()` as part of the ATTACH.

    Both exist because DuckLake's `METADATA_PARAMETERS` can forward ATTACH
    options but neither a setting nor a function call. Both accept the string
    form it sends. `preload` is refused next to `lazy_validation true` or
    `catalog false`.

- **`transaction_isolation`: the isolation level of explicit transactions**
  ([#331](https://github.com/hugr-lab/mssql-extension/issues/331)). The
  scans of one DuckDB transaction are separate statements on its pinned
  connection. Under the server's default READ COMMITTED, a row that another
  session commits between two scans is seen by one scan and not the other: 3
  rows then 4 in the same transaction. Under SNAPSHOT both reads say 3.
  - **Values.** `default` (unset) sends nothing, as before. The five T-SQL
    levels are sent as `SET TRANSACTION ISOLATION LEVEL …` before
    `BEGIN TRANSACTION`. `auto` means SNAPSHOT where the database has
    `ALLOW_SNAPSHOT_ISOLATION ON`; that state is probed at ATTACH in the same
    query as the collation, and only when `snapshot` or `auto` asks for it.
  - **Checked at ATTACH.** An explicit `snapshot` on a database with snapshot
    isolation OFF is refused there. Fabric Warehouse and Synapse get nothing.
  - **Found along the way.** SQL Server does not reset the isolation level with
    `RESET_CONNECTION`, so a level set in a transaction would follow the pooled
    connection into every later autocommit statement. The extension now puts
    READ COMMITTED back after COMMIT or ROLLBACK, including after the server
    aborted the transaction on an update conflict (3960). A `SET TRANSACTION
    ISOLATION LEVEL` sent through `mssql_exec()` has always leaked this way.
- **`default_schema`: the schema unqualified names resolve against**
  ([#322](https://github.com/hugr-lab/mssql-extension/issues/322)). It was a
  hard-coded `dbo`. It is now an option on every configuration path: the ATTACH
  option `default_schema` (which wins), `DefaultSchema=` in a connection
  string, `default_schema=` in a URI, and `default_schema` in a secret. It
  governs `db.table`, `USE db`, CTAS into `db.table`, and COPY into `db.table`
  or `mssql://db/table`. Unset is `dbo`, as before. A default that
  `schema_filter` hides is refused at ATTACH.
- **Every mssql function is documented in `duckdb_functions()`**
  ([#371](https://github.com/hugr-lab/mssql-extension/issues/371)): a
  description, runnable examples and a category on each of the 23 overloads,
  and real parameter names on the scalar functions — `mssql_exec(context,
  sql)` instead of `mssql_exec(col0, col1)`. `duckdb_functions()` is all a
  client connected to the database can read about an extension, so this is
  what an agent working through SQL sees. The scalar names are also callable,
  `mssql_exec(context := 'db', sql := '…')`, and binder errors list them.
  Table functions keep `col0`, `col1`, … for their positional parameters,
  because DuckDB's `duckdb_functions()` names those by position whatever the
  registration says; their named parameters (`prepared`) show by name.
  `mssql_preload_catalog`'s optional schema is now a second parameter instead
  of a vararg, so it is named too, and a third argument, which used to be
  accepted and ignored, is refused.

### Changed

- **The documentation lives at https://hugr-lab.com/docs/mssql-extension/** (#495,
  #499). https://hugr-lab.github.io/mssql-extension/ keeps serving the same
  pages and names hugr-lab.com as canonical.
- **Breaking: an autocommit UPDATE / DELETE / MERGE commits and rolls back
  with DuckDB** (#451). Such a statement now runs on its catalog as a
  transaction of one statement, as postgres_scanner and mysql_scanner run every
  DuckDB transaction: one pinned connection, `COMMIT` / `ROLLBACK` when DuckDB
  commits or rolls back the statement.
  - An error later in the same statement now undoes the write, as do Ctrl-C and
    a result dropped before its query ended. The write used to be committed at
    its operator's end. Example: `WITH u AS (UPDATE … RETURNING id) SELECT
    error('boom') FROM u`.
  - Since DuckDB ef5b57f94ad, DuckDB aborts such a dropped result and rolls
    back a DuckDB table. An MSSQL table now behaves the same.
  - The statement reads the scans of that catalog as in a transaction:
    materialized, on its connection. It holds that connection until DuckDB
    ends the query.
  - A MERGE's actions now share one connection and commit together.
  - `mssql_insert_use_bcp = false` makes an INSERT join the same way, and
    `mssql_ctas_use_bcp = false` makes a CTAS run whole inside the DuckDB
    transaction, also inside `BEGIN … COMMIT`. A CTAS then runs its checks,
    `CREATE` and rows (as INSERT statements) on one connection, and a
    `ROLLBACK` drops the table.
  - With both settings `true` (the default), an INSERT and a CTAS commit as
    they load, with their parallel writers, as before.
  - COPY TO always commits on its own. `mssql_exec()` does too, unless it is
    called inside a statement that has joined its catalog.

- **Breaking: `enable_external_access = false` is honoured** (#452). A new
  `ATTACH … (TYPE mssql)` is refused by name, `lazy_validation` included, and
  so are `mssql_azure_auth_test`, `mssql_kerberos_auth_test(_secret)` and
  `mssql_winsspi_auth_test(_spn)`. Until now all of them reached SQL Server,
  Azure AD or a KDC with the setting off. A session that disables external
  access and then attaches must attach first. An MSSQL database attached before
  external access was disabled keeps working: scans, `mssql_exec`, DML, CTAS
  and new pool connections. DuckDB keeps its own attached databases the same
  way. COPY TO into it is refused by DuckDB's own check on the COPY target (a
  path); `INSERT … SELECT` takes the same bulk wire.

- **One function reads every DML statement's response** (spec 080
  acceptance 7). Five paths had their own copy of the token loop: the INSERT,
  UPDATE and DELETE batches, INSERT … RETURNING's parser, and a pushed DML
  (through `MSSQLSimpleQuery`). They now share `mssql::ExecuteDmlBatch`. What
  changes for a user:
  - `mssql_query_timeout` bounds each INSERT / UPDATE / DELETE statement the
    extension sends row by row. Until now those batches waited a fixed 30 s,
    whatever the setting said, and `0` now means no deadline for them too.
    - On a timeout the statement is cancelled (ATTENTION) and fails like any
      failed statement.
    - The error names the setting, and for an INSERT also the statement and
      its rows.
    - A pushed DML still has no deadline.
  - Ctrl-C cancels such a statement within a second. Before, a batch could
    not be interrupted while it waited for the server.
  - A read that runs out of time is reported as a timeout, not as `Failed to
    receive TDS packet`. A read cut short by a signal is retried, not taken
    for a timeout.
  - When the server sends several errors, the first is reported, because it
    is the cause. The batches used to report the last.
  - The INFO messages of a row-by-row INSERT, UPDATE or DELETE and of a
    pushed DML now reach DuckDB's log, as `mssql_exec`'s already do. These
    are PRINT, or RAISERROR of severity 10 or less, from a trigger for
    instance. The staged and bulk paths still do not log them.
  - A pushed DML's TDS desync says the server executed the statement.
    `mssql_test_fail_parse_after_tokens` now reaches that path, so it can be
    tested.

- **UPDATE and DELETE through the catalog are refused on Azure Synapse**
  (spec 080): its primary keys and unique constraints are `NOT ENFORCED`, so a
  key could match rows the statement did not select. `mssql_exec()` still
  sends what you write. Synapse is recognised by its host name, or by
  `SERVERPROPERTY('EngineEdition') = 6` behind any other name.

- **A `DATETIME` key is a rowid key again** (spec 080, #358). On a table whose
  primary key is `DATETIME` beside a usable unique index, `rowid` is now the
  primary key, as for any other usable primary key.

- **`mssql_dml_use_prepared` is deprecated** (spec 080): it was read by
  nothing. It stays registered, as a documented no-op, for one more release,
  so a `SET` of it in a `.duckdbrc` does not throw; it is removed in the
  release after.

- **`mssql_insert_use_returning_output` is deprecated** (review of #471), for
  the same reason: nothing read it, and an `INSERT … RETURNING` always uses
  OUTPUT. It stays a documented no-op for one more release.

- **Metadata queries compute a table's row count once, not once per column**
  (spec 084). Every query that returned columns carried
  `OBJECTPROPERTYEX(..., 'Cardinality')` in the same SELECT list, and the
  server evaluated it on every column row: about 110 logical reads per call on a
  catalog with keys and constraints. The loads now read the object row and the
  columns as separate result sets of one batch. A whole-catalog load without
  filters takes its row counts from one pass over `sys.dm_db_partition_stats`;
  without the `VIEW DATABASE STATE` permission, on Fabric / Synapse and inside a
  transaction, it falls back to per-object calls with the same counts. Measured
  on 200k tables in 100 schemas:

  | operation | before | after |
  |---|---:|---:|
  | `mssql_preload_catalog` of the whole catalog | 166 s | 24-29 s |
  | preload of one schema of 2,000 tables | 1.88 s | 0.53 s |
  | a table's first touch, server CPU | 1.86 ms | 0.39 ms |
  | a missing name's "did you mean" names query | 680-700 ms | 297 ms |

- **The bulk loads and `mssql_refresh_cache` carry every table's rowid key**
  (spec 084). After a preload or a listing, each transaction used to discover
  the key of every table it scanned on its pinned connection, one extra round
  trip per table per transaction. The key now comes in the same batch as the
  columns, everywhere. `mssql_refresh_cache` reloads the catalog in one batch
  instead of one columns query per table.
  - A key that cannot be read now fails the metadata load and is retried on the
    next access. It is no longer cached as a refusal naming
    `mssql_invalidate_cache`, and the "could not be read" refusal is gone.
  - A key changed behind the catalog's back (an index added by another client
    or through `mssql_exec`) is seen after an invalidation or the TTL, like a
    column. It used to be read at the table's first scan.
- **CTAS's table and schema existence checks are parameterized** (spec 084):
  one cached plan for every name instead of an ad-hoc plan per name, and
  non-ASCII names are checked correctly. A check that fails now reports its
  error instead of reading as "does not exist".

- **Parameters go as an RPC call, not a batch** (spec 083). Every
  `sp_executesql` the extension sends -- a pushed filter's constants,
  `mssql_scan_params`, `mssql_scan_params_unsafe`, `mssql_exec_params`, the
  per-table metadata queries -- is now a TDS RPC request (packet type 3) with
  the values as typed parameters, and a `prepared := true` execution an
  `sp_execute` RPC call. The batch form, `DECLARE @p0 ... = ...; EXEC
  sp_executesql ...`, made the server parse and compile the outer batch on
  every call, because its text changes with every value: measured locally, a
  scan with a pushed filter cost the server 0.26-0.29 ms of CPU that way and
  0.06-0.07 ms over RPC -- what a literal costs, while keeping one plan per
  filter shape. A declaration with no RPC encoding (a CLR type such as
  `geometry`, an alias type, `sql_variant`), or a value its declared type
  cannot take, keeps the batch form for the whole call, decided before
  anything is sent. `sp_prepare` stays a batch: it runs once per bind.
- **A scan allocates its string staging on demand** (spec 083 D4). Each
  string column's payload buffer used to be sized and zero-filled for a full
  chunk when the stream opened -- up to 2 MB for a bounded `nvarchar(n)`, 64 KB
  for a MAX column -- even when the result was empty. It now starts at 4 KB on
  the first value and doubles, stopping at the column's provable worst case,
  which used to be reserved up front.

### Fixed

- **The write path could desync the session, truncate silently or overflow its
  own buffer on an over-large or empty-encoding value** (#489, #496). Both were
  found by static analysis and an encode-side fuzz harness.
  - A MAX-typed value past SQL Server's 2^31-1-byte limit is refused, with its
    size named. Before, on the columnar BCP path that COPY / CTAS / INSERT
    take, a value over 2 GiB had its 32-bit length wrap. An ASCII value then
    went in silently truncated, a non-ASCII one overflowed the client's chunk
    buffer, and an over-long value into a bounded column skipped its clamp.
  - An `nvarchar(max)` value whose UTF-8 encodes to nothing (a lone invalid
    byte, reachable from a raw scan of a code-page varchar, #224) was framed in
    16 bytes where the value ends at 12. The four stray bytes desynced the
    session.
- **A malformed server response could read past a buffer or force a large
  allocation** (#485, #486).
  - A temporal value shorter than its type is refused on the string fallback
    too (#89's path read past it).
  - A PLP value's declared length no longer sizes an allocation before its bytes
    arrive: one 8-byte response used to reserve 2 GiB.
- **`mssql_azure_auth_test` showed the token's last three characters, and a
  token of up to 16 characters in full** (#492). It now shows a JWT's 10-character
  prefix and its length, `eyJ0eXAiOi... [1642 chars]`, and an opaque token's
  length alone.
- **`INSERT … RETURNING` failed on a table with an enabled trigger** (#471),
  including through a view over one, with SQL Server error 334: "cannot have
  any enabled triggers if the statement contains an OUTPUT clause without
  INTO clause". The statement path sends a bare `OUTPUT INSERTED.…`.
  - It still does: on a table with no trigger that is the cheaper form
    (measured on 10000 rows: 0.09 s, against 1.2 s with a session table per
    batch).
  - When SQL Server refuses it with 334, a compile error that ran nothing,
    the batch is rebuilt and sent again: it creates `#mssql_returning_<uuid>`,
    fills it with `OUTPUT … INTO`, reads it and drops it, in one request. The
    rest of the statement's batches go the same way.
  - The table is typed by the same expressions as UPDATE / DELETE …
    RETURNING's. `rowversion` keeps its 8 bytes. A `char(n)` is held as a
    trimmed NVARCHAR, so wide `char` columns stay under the 8060-byte row.
    The IDENTITY property is not copied.
  - A result set the trigger returns of its own is not taken for RETURNING
    rows.
  - RETURNING shows each row as the INSERT wrote it, before an AFTER trigger
    changed it.
  - Fabric and Synapse keep the bare OUTPUT, so the 334 still reaches the
    user there.

- **A filter on a `datetime`, `time(7)` or `datetimeoffset(7)` column missed
  rows read from it** (#426). A
  `datetime` stores 1/300 s ticks; the scan reads each to the microsecond,
  truncated (`.003` is `.00333…` on the server and `.003333` in DuckDB). The
  pushed filter compared the column with its constant as `datetime2`,
  exactly.
  - `WHERE dt = <a value read from dt>` matched only on a whole 10 ms: 200 of
    603 values found their own row. `>` and `<=` disagreed with DuckDB on
    such a value in the same way.
  - A constant compared with the column is now the tick that decides the
    comparison, sent as `datetime`. For `=`, `<>` and `IN` it is the tick
    read as the constant. For `>` and `<=` it is the tick read at or below
    it, and for `>=` and `<` the one at or above it. Such a comparison gives
    DuckDB's answer over the values it reads, parameterised or as a literal.
  - Where no tick decides it (`=` between two reads, `<` past the last
    `datetime`), the constant goes as a `datetime2` value that gives the same
    answer at every database compatibility level. Below 130 the server
    converts a `datetime` to `datetime2` at the millisecond, so `dt =
    '….003'` matched the `.003` row there, where DuckDB reads `.003333`.
  - A `rowid` of a `datetime` primary key used to round a constant between
    two reads onto a tick and match that row; it now matches nothing.
  - A `datetime` column compared with anything but constants and other
    `datetime` columns -- a `datetime2(6)` column of the same table, a `CASE`
    over it, a constant with it in its `IN` list -- is no longer pushed:
    `dt = dt2` found nothing where DuckDB, reading both, finds the row.
  - `time(7)` and `datetimeoffset(7)` -- the default scales -- are read as
    microseconds with the 100 ns digit dropped (`.1234569` reads as
    `.123456`), so `t = TIME '….123456'` found only the row whose seventh
    digit is 0. The read equals `c` for every stored value in `[c, c + 1 µs)`:
    `=` is now that range, `<>` its complement, `>` starts at `c + 1 µs`,
    `<=` ends before it, and `>=` / `<` are unchanged. At the last
    microsecond (`23:59:59.999999`, `9999-12-31 23:59:59.999999`) `c + 1 µs`
    is never sent, where the server would raise an overflow. Such a column
    against anything but constants -- even another `time(7)`, whose values
    100 ns apart DuckDB reads as equal -- is no longer pushed.
  - The read itself is unchanged. DuckDB 2.0's `TIME_NS` would keep the digit,
    but it does not yet compare with `TIME` or take an interval, `strftime`
    or `date_trunc`, so reading the default `time` as `TIME_NS` would break
    ordinary queries.

- **A DELETE could remove the wrong rows, without an error, when its WHERE
  held a correlated subquery with a range** (#337). DuckLake's snapshot
  expiry is one: `DELETE … WHERE end_snapshot IS NOT NULL AND NOT EXISTS
  (SELECT … WHERE snapshot_id >= begin_snapshot AND snapshot_id <
  end_snapshot)`.
  - The row-by-row path read the rowid from the last column of its input. A
    DELETE, unlike an UPDATE, has no projection of its own, and DuckDB plans
    such a subquery as a mark join whose column follows the rowid. That
    BOOLEAN was taken as the key.
  - On a single-column key it was sent as the key value: every row the
    `NOT EXISTS` kept has mark false, i.e. `0`. The DELETE removed the row
    keyed `0` (or nothing) instead of its own rows, and reported that count
    (measured: 1 row, the wrong one, instead of 2). On a composite key it
    failed with `Expected STRUCT rowid for composite PK, got BOOLEAN`.
  - It now takes the column DuckDB's row-id expression names, as DuckDB's own
    DELETE does and as the staged path already did.
  - Affected: a DELETE on the row-by-row path. That is one not pushed whole
    to the server (`mssql_dml_pushdown = false`, an ATTACH with
    `remote_pushdown false` such as DuckLake's metadata catalog, or a
    statement the rewriter declines), of up to `mssql_dml_stage_threshold`
    rows, keyed by the primary key or a unique index.
  - On v0.2.5 the same statement failed intermittently with `Invalid
    unicode` instead. That was the materialized scan's unbound rowid members,
    fixed by #453. The new test fails on either cause.

- **A `char(n)` column kept its padding on two read paths, and joins and
  `GROUP BY` disagreed with SQL Server** (#405). SQL Server compares strings
  ignoring trailing spaces, so `'AB  '` (char(4)) equals `'AB    '` (char(6));
  DuckDB compares bytes, and the codec trims only a fixed-length wire value.
  - A code-page `char(n)` (the installation default
    `SQL_Latin1_General_CP1_CI_AS` included) was read through
    `CAST(… AS NVARCHAR(n))`, and a UTF-8 one on a connection without
    UTF8SUPPORT arrived as the server's NVARCHAR. Both kept the padding:
    a `char(4)` joined with a `char(6)` found 0 rows where the server finds 2,
    and a `char(5000)` came back 5000 characters long.
  - Such a column is now read as `CAST(… AS NCHAR(n))`, or past 4000 as
    `RTRIM(CAST(… AS NVARCHAR(MAX)))`. A UTF-8 `char(n)` on a connection
    granted UTF8SUPPORT is still read as itself, as UTF-8 bytes. The catalog
    scan, a statement pushed whole and the `RETURNING` / `OUTPUT` lists read it
    the same way. A lazily validated ATTACH learns whether UTF8SUPPORT was
    granted from its first connection.
  - A statement pushed whole leaves a value computed from a `char(n)` or
    `nchar(n)` -- `COALESCE`, `CASE`, `LAG` / `LEAD` / `FIRST_VALUE` /
    `LAST_VALUE` -- to DuckDB: on the server it is `varchar` / `nvarchar` and
    kept the blanks (`COALESCE(code, 'x')` was `'AB    '`).

- **A materialized scan crashed on a composite rowid key it did not project,
  and ignored the filters DuckDB applies itself** (found by #451). A scan is
  materialized inside an explicit transaction, on a pool of one connection,
  and now in an autocommit DML statement.
  - The rowid STRUCT's key fields were written only into the streaming
    chunk. The drain appended garbage, and an `UPDATE` in a transaction on
    such a table crashed (SIGBUS).
  - The filters the encoder does not push, which run in DuckDB, ran only on
    the streaming path. A materialized scan returned the rows they reject, so
    an `UPDATE … WHERE lower(name) = 'x'` in a transaction changed rows it
    should not have.
- **After `SET mssql_catalog_native_types` a cached table, its scan and its
  INSERT disagreed on column types** (#415).
  - A table entry fixes its types when it is built (`MSSQL_NVARCHAR(n)` with
    native types on). The scan's types and an INSERT's column types read the
    setting again on every statement, so after a `SET` they disagreed with the
    entry the statement was bound against: plain `VARCHAR` beside its
    `MSSQL_NVARCHAR(n)`, or the reverse.
  - This is the extension-type disagreement behind #369, which fails an
    assertion in a debug build.
  - Both now take the entry's own types. A setting change applies through
    `mssql_invalidate_cache()`, as documented.

- **A `COMMIT` the server refused counted as done, and a failed `COMMIT` kept
  its connection out of the pool for good.** The transaction manager now reads
  the answer to `COMMIT` / `ROLLBACK` and closes a connection whose state it
  cannot confirm.
  - A failed `COMMIT` says whether nothing was written (the server refused it)
    or whether the outcome is unknown (no answer).
  - The `COMMIT` round trip no longer holds the lock every statement on the
    catalog takes to start its transaction.

- **The pool opened more connections than `mssql_connection_limit`** (#461).
  A login runs without the pool's lock, and the count of connections grew
  only after it, so every caller arriving during a login dialled too:
  measured, 21 connections on a limit of 8 and 79 on 64.
  - A login now holds its slot from before it starts. It gives the slot back
    if it fails or anything throws, and wakes a caller that was waiting on
    it.
  - An extra bulk-load writer (`TryAcquire`) no longer dials over a slot
    another login holds.
  - At `mssql_connection_limit = 1`, two DuckDB connections starting
    statements at once could both log in.
  - `mssql_pool_stats.total_connections` now counts logins in flight. An
    exhaustion timeout says how many there were (`…, 2 logging in`).
  - A statement that needs more connections at once than the limit now waits
    for one to be released (`mssql_acquire_timeout`) instead of opening more.

- **A pushed DML whose connection died spun at 100% CPU until Ctrl-C** (#456,
  a regression from #450). A pushed UPDATE / DELETE / INSERT … SELECT waits
  with no deadline, in 1-second read slices, and keeps going while the socket
  says it is connected.
  - Under TLS (the default) a closed peer came back as a timeout, and a TLS
    read error left the socket "connected". A server restart, a `KILL`, a
    failover, a network cut or a keepalive expiry during such a statement left
    it reading again at once, forever.
  - It was worse than a stale code: recv() returning 0 at end of stream does
    not set errno, so the EAGAIN of the previous timed-out slice made the EOF
    read as one more timeout.
  - Every TLS read error and a closed peer now mark the connection dead, and
    so does a TDS frame shorter than its header. The statement fails in about
    a second and names the cause.
  - A cancelled stream draining a dead connection stops at once instead of
    retrying for its 5 s.

- **An INSERT that cannot use the bulk path counted its last batch as 0
  rows.** This is an INSERT naming the identity column, with a geometry, CLR
  or `sql_variant` column, into a view, or under `mssql_insert_use_bcp =
  false`. One that fitted a single batch reported `Count` 0; a larger one was
  short by its last batch. The rows were inserted; only the count was wrong.
  Found by the agreement suite of spec 080 PR 3d.

- **A date / time SQL Server cannot hold was sent as another value** (#447).
  The wire's three-byte day count kept the low bytes of what it was given:
  - DuckDB's `DATE 'infinity'` became 1969-12-31 and `-infinity` 1970-01-02;
  - a year near 50000 became a year near 4066;
  - a datetime2(7) column's (TIMESTAMP_NS) `±infinity` became
    2262-04-11 23:47:16.8547758;
  - `TIME '24:00:00'` became 23:59:59.999999.

  It happened wherever such a value went to the server:
  - a pushed filter: `DELETE … WHERE d = 'infinity'` deleted the 1969-12-31
    rows (measured);
  - a parameter;
  - a statement literal (an INSERT / UPDATE under the bulk threshold);
  - a bulk load (INSERT, COPY, CTAS, the `#stage` of a large UPDATE / DELETE).

  Now:
  - A filter with such a constant runs in DuckDB, `rowid =` included:
    `d = 'infinity'` matches nothing, `d < 'infinity'` everything. A keyless
    UPDATE / DELETE with one is refused by name: part of its WHERE runs in
    DuckDB.
  - A load, a parameter or a literal carrying one fails, naming the value
    and where it was going: `(column d)`, `(parameter @d)`. The statement
    literals, the parameters and the `#stage` (which named its own `n0`) got
    the name in #460.
  - A timestamp that rounds past 9999-12-31 at the column's scale fails too:
    `datetime2(4)` ← `23:59:59.99999` would carry into a day that does not
    exist.

- **UPDATE / DELETE through a `DATETIME` key** (#358). A key value with a
  1/300 s fraction (`.003`, `.007`, …) matched no literal, so the statement
  reported success and changed nothing; since spec 077 such a key was refused.
  The key is now compared in its own type —
  `CAST(CAST(<literal> AS datetime2(7)) AS datetime)`, or a `datetime` stage
  column — and lands on the stored tick: 603 of 603 values measured either
  way, against 200 of 603 before. A `WHERE rowid = …` on such a table compares the same
  way. (A plain `WHERE dt = …` filter on a `datetime` column compares in the
  column's type as well since #426, above.)

- **The planner's row estimate follows a DELETE** (spec 080). After an
  autocommit DELETE through the catalog the table's estimate drops by the rows
  deleted, instead of keeping the count it was loaded with until
  `mssql_invalidate_cache()`.

- **UPDATE and DELETE on a pool of one connection in autocommit waited out
  `mssql_acquire_timeout`, and so did a MERGE** (spec 080 PR 1): the scan
  feeding them held the only connection. UPDATE / DELETE / MERGE now count as
  sinks of their catalog, so that scan is materialised first, in autocommit on
  a pool of one as in a transaction. In a transaction they no longer defer
  every batch to the end of the statement with all its rows buffered: batches
  go out as the rows arrive.

- **A MERGE in a transaction whose actions included an INSERT failed with
  "connection not in Idle state"** when its source was a table of the same
  catalog (spec 080 PR 1). Its actions are fed from several threads onto the
  one pinned connection. Each action now sends its rows at its own Finalize
  (an INSERT action as statements, never a bulk stream held open across the
  sink), and each batch on the pinned connection holds the transaction's
  materialize lock. A MERGE's actions are still separate statements to the
  server (spec 080 PR 4 makes them one): in autocommit they are not atomic
  together, and on a pool of one connection they take turns at it, bounded by
  `mssql_acquire_timeout`.

- **A MERGE's `WHEN NOT MATCHED THEN INSERT (cols)` wrote NULL into the
  columns it did not name**, where the server had a DEFAULT, and refused a
  table with an IDENTITY column (spec 080 PR 1, review of #423). DuckDB binds
  the action full width, with a copy of each column's bound default where the
  INSERT named nothing; those columns now stay out of the INSERT, so the server
  supplies their identity, DEFAULT or computed value, as for a plain INSERT.

- **`INSERT … DEFAULT VALUES` sent an empty column list** and failed with
  server error 102 (as did a MERGE action's `INSERT DEFAULT VALUES`). It is
  refused by name until the statement builder has a `DEFAULT VALUES` form.

- **UPDATE / DELETE … RETURNING ended in an InternalException** ("Vector::Reference
  used on vector of different type"), which invalidates the database. They now
  return their rows (see Added).

- **Catalog DDL in a transaction on a pool of one connection failed after
  `mssql_acquire_timeout`** (#419): `CREATE` / `DROP` / `ALTER TABLE` and
  `CREATE` / `DROP SCHEMA` asked the pool for a second connection while the
  transaction held the only one. On a pool of one they now run on the
  transaction's own connection, as CTAS does since #380, and roll back with it.

- **The row count a table's storage info asks the server for always failed.**
  It read `sys.dm_db_partition_stats` through `p.rows`, but that view's column
  is `row_count`, so every call failed with error 207 from spec 008 on. The
  empty answer was taken as 0 and cached. That path is reached when DuckDB asks
  a table for its storage info in autocommit before any listing, for example
  when binding `INSERT ... ON CONFLICT`. The 0 then reached the planner for a
  table the catalog had loaded while it was empty: its scan planned with no
  estimate. The count now comes from `OBJECTPROPERTYEX(..., 'Cardinality')`,
  as the catalog's own metadata queries read it: 3 logical reads, against a
  scan of `sysrowsets` (spec 071). A failed count is no longer cached as 0.

- **In-transaction scans of different DuckDB connections ran one at a time**
  (#409): the lock that keeps a transaction's scans and sinks from using its
  pinned connection at once was one per attached catalog, so every
  transaction in the process waited for every other one's batch and drain,
  while SQL Server sat idle (measured by the mssql-ducklake side: 70 reads/s at
  16 readers). It is now the transaction's own; the catalog's is kept for
  autocommit on a pool of one connection, which every statement shares.

- **A pushed `LIMIT 0` cost a describe and an execution** (spec 081): DuckDB
  answers it with an empty result and asks the server nothing, so the rewriter
  now leaves it alone. DuckLake's attach probes every inlined-data table in one
  `UNION ALL` of `LIMIT 0` branches, and a thousand of them made each attach
  1.6-1.8x slower with pushdown on.

- **A query naming a missing table loaded every column of the catalog, and
  reloaded it every time when the database had a schema without tables**
  (#412). DuckDB walks every schema for its "did you mean" hint; that walk now
  reads table names only -- from the cache, or one names-only query for all
  schemas -- instead of building every table's entry with its columns. And a
  schema with no table or view (a new one, one holding only procedures) never
  counted as loaded, so each full listing of it ran the whole-catalog metadata
  query again; it now loads once, like any other. Measured by the
  mssql-ducklake side on a 4,724-table catalog database: 13-34 s a
  missing-table query, every time.

- **`mssql_invalidate_cache(ctx, schema)` did nothing for a schema created
  after the catalog listed its schemas**: the schema stayed "does not exist"
  until a whole-catalog invalidate. Naming a schema the cache has not listed
  now re-reads the schema list, and the re-read keeps what is cached for the
  schemas it already knew instead of starting from an empty cache.

- **A CTE over an attached table, used more than once under a `LIMIT`, failed
  with `Table Function with name mssql_catalog_scan does not exist`.**
  DuckDB inlines such a CTE by copying its plan, and the copy looks the scan
  function up by name; the catalog scan was never registered. It is now
  (`mssql_catalog_scan`, internal: called by name it refuses), and the copy
  rebuilds the scan from the attached table, pushed filters included.

- **Spec 079 PR C follow-up review (roborev 1821): the `%` gate reached only the
  planner.** It is a case where the two pushdown walkers could still answer
  differently.
  - The `error_on_division_by_zero` gate was read in `BuildEncodeContext` (the
    planner's dry run) but **not** in `FilterEncoder::Encode`'s execution-time
    path or the optimizer's client-side-filter probe, which built their own
    `ExpressionEncodeContext` and left the flag at its default. A `%` filter
    reaching those without a matching dry run was still pushed and still failed
    with error 8134. `Encode` now takes the policy as a parameter **with no
    default**, so a new call site has to answer; all four paths (dry run,
    execution encode, optimizer probe, remote-pushdown writer) read it through
    one shared `LoadErrorOnDivisionByZero`.
  - Tests: the float case in `filter_arithmetic_types.test` could not detect a
    regression (1e200 + 1e200 and 2.0 + 2.0 are both representable, so the rows
    were the same either way) — a `1e308` row now separates them, since a pushed
    sum would raise on the server while DuckDB gives `inf` and returns the row.
    The `%` gate gained its first end-to-end case. `test_sql_writer.cpp` also
    pins the UNPARAMETERISED form of narrow-integer arithmetic, which nothing
    covered: an unparameterised literal already carries the column's type
    (`[tiny] + CAST(1 AS tinyint)`) through the cast `BindConstant` wraps it in,
    so the server overflows at 255 + 1 exactly as DuckDB overflows `UINT8`.

- **Spec 079 PR C review (roborev 1819): three divergences between the two
  pushdown paths.** All three are cases where the server and DuckDB would answer
  differently for a construct one path sent and the other did not — the
  invariant PR C's own `DATAMODEL.md` states as "a construct both paths take
  answers the same whichever takes it".
  - A **widening cast to `UTINYINT`** rendered as T-SQL `bigint`. `UTINYINT` is
    the only integer rank-1 type (SQL Server's `tinyint` is 0-255), so
    `CAST(tiny AS UTINYINT)` over a `tinyint` column passed the widening gate
    and then fell through the arm list to `bigint`, while the writer went on
    treating the operand as `UTINYINT`. `CAST(tiny AS UTINYINT) + 1` at
    `tiny = 255` therefore computed 256 on the server where DuckDB raises
    "Overflow in addition of UINT8". Now renders `tinyint`.
  - **`+` and `-` on `float` / `real` are no longer pushed**, as the product
    already was not. SQL Server's `float` cannot represent infinity, so an
    overflow is error 8115 — for every row `WHERE` passes, `TOP`'s discards
    included — where DuckDB returns `inf`; and because an encoded predicate is
    erased from the DuckDB plan, the statement fails rather than falling back to
    the client-side filter net. A product reaches the window near 1e154 and a
    sum near 1.8e308, but the divergence is the same one.
  - **`%` under `SET error_on_division_by_zero = false`** was refused by the
    remote-pushdown writer and still pushed by the scan path's filter encoder,
    so `WHERE i % j = 0` with a zero divisor failed with error 8134 through the
    scan where DuckDB alone answers `NULL` and simply omits the row. The gate
    moved into `ExpressionVocabulary::FunctionFor`, which both walkers call, so
    one answer serves both; `ExpressionEncodeContext` carries the session's
    setting to the encoder's walk.

- **LIKE pushdown is documented as the server's, and its case-insensitive
  form is gone (#392).** A simple `LIKE` is pushed as T-SQL `LIKE` and
  evaluated under the column's collation, exactly as `=` is (spec 079 D4): on
  a case-insensitive collation `LIKE '%abc%'` also returns `'ABC'`. The
  `LOWER(x) LIKE LOWER(p)` form for `iprefix` / `isuffix` / `icontains` was
  unreachable on the 2.0 pin, where `ILIKE` stays DuckDB's, and would have lost
  rows had it been reached: the server's `LOWER(N'ẞ')` stays `ẞ`, DuckDB's
  `lower` gives `ß`. It is removed so it cannot come back.

- **ORDER BY pushdown returned SQL Server's order for string keys**
  ([#362](https://github.com/hugr-lab/mssql-extension/issues/362)). With
  `mssql_order_pushdown` on, a pushed ORDER BY removes DuckDB's own sort, and
  nothing checked the key's collation. So `ORDER BY name` on a `varchar` under
  the installation default `SQL_Latin1_General_CP1_CI_AS` came back in
  linguistic order, with case and accents interleaved, instead of DuckDB's.
  - **Rule:** a key is now pushed only when the server sorts it as DuckDB
    does: numeric, `bit` and date/time types (not `datetime2(7)`, whose
    out-of-range values DuckDB reads as NULL). A `varchar` / `char` under a
    UTF-8 collation is pushed only under a LIMIT, as its bytes
    (`CAST(col AS varbinary(n))`, bounded `varchar` only — `char(n)` is read
    trimmed): as text the server pads with spaces, so `ab` + TAB would sort
    before `ab`. Not `nvarchar`, even under `_BIN2`: its
    UTF-16 order puts a character above the BMP before U+E000–U+FFFF, DuckDB
    after. Never a string-valued function (`upper(name)`) or a date part of a
    `datetimeoffset`.
  - **Never pushed:** `uniqueidentifier` (SQL Server compares its last six
    bytes first), `binary` / `varbinary` (compared zero-padded, so `0x01` =
    `0x0100`), `json`, `sql_variant`.
- **`upper()` / `lower()` in a filter could lose rows.** They were pushed to
  SQL Server, whose case mapping is not DuckDB's: `WHERE upper(name) =
  'STRAẞE'` found nothing, because the server's `UPPER('ß')` is `ß`. They are
  now applied by DuckDB, as `length()` already was; `trim`, `ltrim` and
  `rtrim` with them. A date part of a `datetimeoffset` is no longer pushed
  either: the server takes it in the value's own offset.
- **ORDER BY … LIMIT under-returned when a filter ran client-side.** With
  `mssql_order_pushdown` on, the TOP N went to the server while a filter the
  extension cannot translate (a `rowid` field of a composite key) was applied
  to what came back, so the server's N rows were filtered down, not refilled:
  `WHERE rowid.a >= 2 ORDER BY k LIMIT 2` returned 0 rows of 2. TOP N is now
  pushed only when every filter is on the server.
- **RENAME of a table or column with a dot in its name** failed: `sp_rename`
  parses the old name as a multi-part name and it was sent unbracketed.
- **Names containing `]` broke the bulk load and DELETE.** Several places put
  brackets around a table, schema or column name without doubling `]`: the
  `INSERT BULK` of COPY / INSERT / CTAS, a rowid DELETE, and the target-shape
  probe (whose `OBJECT_ID('…')` literal also broke on a `'`). Every identifier
  now goes through one quoter, which replaced eight copies of it.
- **Transactions fill the shared metadata cache again**
  ([#383](https://github.com/hugr-lab/mssql-extension/issues/383)). Since #380
  a transaction loads a missing table's metadata into a cache of its own, so a
  workload that runs everything in transactions (DuckLake) loaded every table
  once per transaction. At COMMIT or ROLLBACK what the transaction loaded and
  did not change is now published into the shared cache from memory, with no
  round trip: 100 transactions reading the same two tables load each table's
  metadata once. Nothing is published after DDL (the transaction's own through
  the catalog included, or `mssql_exec` DDL), under READ UNCOMMITTED, or when
  the shared cache was invalidated while the transaction ran. Inside a
  transaction the shared metadata cache is read too.
- **Reading the schema list no longer makes a transaction count as having used
  the catalog.** `MSSQLCatalog::SchemaListCache` asked the transaction for its
  metadata layer unconditionally, and that call CREATES this catalog's
  transaction — so any schema lookup DuckDB makes against an attached MSSQL
  catalog (search-path resolution, `duckdb_schemas()`, a "did you mean" scan)
  flipped `HasUsedAnyMSSQLCatalogInTransaction` even when it answered from the
  shared list and took no connection. `mssql_refresh_cache()` and
  `mssql_preload_catalog()` were then refused inside a transaction that had
  touched no server — the case
  [#380](https://github.com/hugr-lab/mssql-extension/issues/380)'s refusal was
  narrowed to allow. An already-loaded shared list is now read without creating
  anything; every other path creates the transaction as before, so a load still
  lands in its own cache.
- **A table created inside a transaction can be read in it; a pool of one
  connection works in a transaction**
  ([#380](https://github.com/hugr-lab/mssql-extension/issues/380)).
  - **The hang.** `BEGIN; COPY … TO 'mssql://db/dbo/new' …; SELECT … FROM
    db.dbo.new` waited out `mssql_metadata_timeout` (5 minutes by default). So
    did the same after `mssql_exec` DDL. The metadata load ran on a pool
    connection, which waits on the schema lock of the transaction's
    uncommitted CREATE. It has done so since spec 076.
  - **Pool of one.** With `mssql_connection_limit = 1`, the first table a
    transaction touched failed with `pool timed out (1 active of 1)`: its
    metadata wanted a second connection.
  - **The fix.** Inside a transaction every metadata load goes on the pinned
    connection, into a cache that belongs to the transaction. The catalog's
    shared cache is read for names the transaction did not change and is
    never written from inside a transaction. Loading into it was measured to
    leave a phantom table after ROLLBACK and to show another connection the
    uncommitted table. At COMMIT or ROLLBACK the shared cache forgets what the
    transaction changed.
  - **Refused inside a transaction that uses MSSQL.**
    `mssql_refresh_cache()` and `mssql_preload_catalog()` are bulk loads into
    the shared cache. They are refused once the DuckDB transaction has used
    any attached MSSQL catalog. Any, not just this one, because two ATTACHes
    of one database are independent catalogs (#389). A DuckDB transaction that
    touched no MSSQL catalog is not refused. `mssql_invalidate_cache()` is
    always allowed.
  - **A pool of one connection.**
    - *CTAS in a transaction* runs whole on the pinned connection: checks,
      CREATE, and rows as INSERT statements. ROLLBACK undoes it. It used to
      fail with `Failed to acquire connection to check table existence`.
    - *In autocommit*, a scan of the catalog the statement writes into is
      materialised first, whether it is a catalog scan or a raw `mssql_scan`.
      A COPY's or CTAS's bulk load then takes the one connection on its first
      chunk. COPY, CTAS and INSERT … SELECT from the same catalog used to wait
      out the acquire timeout.
  - **Extra parallel writers never wait for a connection.** For a COPY, CTAS
    or INSERT via BCP, an extra writer takes an idle connection or opens one
    under the limit. Otherwise it shares the main writer, asks again on a
    later chunk, and is not reported as a pool timeout. A connection the pool
    could not create, such as a login the server refuses, stops the asking. It used to wait
    `mssql_acquire_timeout` for a connection the statement itself held: a
    300k-row CTAS in a transaction on a pool of two took 30 s and now takes
    0.84 s. There are never more writers than `mssql_connection_limit`.
  - **Found along the way.** A CTAS on the INSERT path (`mssql_ctas_use_bcp =
    false`) reported its row count without the last batch: 1000 for 1500
    rows.
- **`mssql_preload_catalog` loads the schema list, marks only what it loaded,
  and reports it** ([#376](https://github.com/hugr-lab/mssql-extension/issues/376),
  [#375](https://github.com/hugr-lab/mssql-extension/issues/375)). Neither path
  loaded the schema list:
  - **Per-schema preload hid the other schemas.**
    `mssql_preload_catalog('db', 'dbo')` on a fresh ATTACH marked the list
    loaded while it held only `dbo`, so every other schema was gone for the
    session: `schema "test" does not exist`. It also marked every schema's table
    list and every table's columns loaded, so a schema looked up before the
    preload stayed at the one table that had been looked up.
  - **Whole-catalog preload was thrown away.** `mssql_preload_catalog('db')`
    left the list unloaded, so the first catalog access afterwards cleared the
    cache and ran the whole-catalog query again.

  Both paths now load the schema list first, which is one light query and free
  when it is already loaded, and mark only what they loaded. The schema name
  matches ignoring case when exactly one schema does, as it did when the name
  went to the server. A schema the catalog does not show, because it is absent
  or hidden by `schema_filter`, is refused by name instead of being reported as
  empty, and it gets no phantom entry. The summary counts what the call loaded,
  and the numbers are the same on every call. Before, a second preload said
  `Preloaded schema 'dbo': 0 tables, 358 columns`.
- **Boolean ATTACH options accept a string value**
  ([#325](https://github.com/hugr-lab/mssql-extension/issues/325)).
  `lazy_validation`, `catalog` and `order_pushdown` were read through an
  integer cast, so `lazy_validation 'true'` failed with `Could not convert
  string 'true' to INT8` — and a string is the only thing DuckLake's
  `METADATA_PARAMETERS` can send, since it is a `MAP(VARCHAR, VARCHAR)` whose
  values reach the inner `ATTACH` quoted. None of the boolean options were
  reachable for a `ducklake:mssql:` catalog. A string now goes through DuckDB's
  own boolean cast, the one `SET` uses (`true`/`false`, `t`/`f`, `yes`/`no`,
  `y`/`n`, `1`/`0`, any case), and anything else is refused with the option
  named.
- **A debug build no longer asserts on `SELECT k, rowid` over a string key
  column** ([#369](https://github.com/hugr-lab/mssql-extension/issues/369)).
  With `mssql_catalog_native_types` on (the default) the projected column is
  `MSSQL_VARCHAR(n)` / `MSSQL_NVARCHAR(n)` while the rowid — the scalar one and
  every child of a composite key's STRUCT — is typed from the column's plain
  type; copying the projected vector into the rowid slot was a typed copy
  across the two, which a debug build refuses (`source_p.GetType() ==
  target.GetType()`) and a release build performed on the same bytes. The
  copy now goes through a reinterpreting view of the source in the rowid's
  type. The rowid suite runs against the debug build in CI from here on, so
  the class stays caught.
- **An `UPDATE`/`DELETE` through a `varchar` key under a `SQL_` collation no
  longer changes rows it was not given.** Every rowid value is sent as an
  `N'…'` literal, so the key join compared a `char`/`varchar` key column under
  its collation's Unicode rules, where `'Straße'` equals `'Strasse'` and `'Æ'`
  equals `'AE'` — while the unique index, under the non-Unicode SQL sort
  order, holds them as distinct keys. Measured on
  `SQL_Latin1_General_CP1_CI_AS`, the installation default: deleting the
  `'Straße'` row deleted `'Strasse'` too, and updating `'Æ'` updated `'AE'`,
  both without an error. The join now converts the sent value back to the
  column's own type and collation, so the comparison is the one the index
  made. Present for a `varchar` primary key before spec 077; spec 077 would
  have extended it to every such unique index. Windows and UTF-8 collations
  apply the same rules to `varchar` and `nvarchar` and are unchanged.

- **An `INSERT` that supplies identity values works** (spec 077 W2). Naming the
  identity column — explicitly, or positionally with a value for every column —
  used to fail with the server's error 544; the statement's connection is now
  bracketed with `SET IDENTITY_INSERT … ON` before its first batch and `OFF`
  before its `COMMIT`, and the values land verbatim. `OFF` is guaranteed on
  every way out — after a failing batch, after `ROLLBACK` on a transaction's
  pinned connection, and from the destructor on an unwind, bounded by a
  timeout since that path carries no query timeout — and a connection on which
  it could not be confirmed is discarded rather than returned to the pool. A
  transaction's pinned connection cannot be discarded: there an unconfirmed
  `OFF` lasts until the transaction ends, whose session reset clears it under
  the default `mssql_reset_connection = true` and, by that setting's meaning,
  not under `false`.
  Measured both ways: with the `OFF` deliberately leaked, the next ordinary
  `INSERT` on that session fails with the server's 545. The server's refusals
  of the `ON` are explained rather than relayed: 1088 says the statement needs
  ALTER on the table (the server's own text claims the table may not exist),
  8106 that the catalog's `is_identity` is stale and `mssql_invalidate_cache()`
  is the fix, 8107 that another table is already `ON` for this session and
  where that could have come from. A NULL in a named identity column is refused
  before anything is sent — DuckDB hands `DEFAULT` and `NULL` to the extension
  identically — with the way out named, which also covers a batch mixing rows
  that supply the value with rows that do not. **This stays the statement
  path:** tens of rows, not millions; loading many rows with their identity
  values is `COPY`, which keeps a source column named like the identity column
  and lets the server assign when it is omitted.

- **`rowid`, and with it `UPDATE`/`DELETE`, no longer require a primary key**
  (spec 077 W1). A table with no primary key but a usable unique index — one
  that is not filtered, not disabled, and whose key columns are all NOT NULL —
  gets its rowid from that index; a `BIGINT IDENTITY … UNIQUE` is the common
  shape. Among several, the order is documented and unit-tested: a
  single-column identity key first, then the fewest key columns, then the
  narrowest, then the lowest `index_id`. A usable primary key still wins.

  **A primary key that cannot address a row now falls through instead of being
  taken.** A `DATETIME` key produced an `UPDATE` that reported success and
  changed nothing ([#358](https://github.com/hugr-lab/mssql-extension/issues/358):
  a `datetime` value with a 1/300-second fraction — `.003`, `.007`, most of
  them — equals no `datetime2` literal at any precision; one on a whole 10 ms
  did match, and is refused with the rest), a
  `TIME(7)` or `DATETIMEOFFSET(7)` key does the same the other way round (the
  read path keeps microseconds, so a key whose 100 ns digit is set comes back
  truncated and its literal never matches — measured: three such rows, one
  updated), and a `SQL_VARIANT` key does the same through its lossy read
  ([#354](https://github.com/hugr-lab/mssql-extension/issues/354)).
  `SMALLDATETIME` and `DATETIME2(7)` keys match their literals and stay usable. Such a
  table now uses another unique index if it has one, and otherwise refuses by
  name — a behaviour change, and a deliberate one, since what it replaces is a
  statement that did nothing and said so to nobody. Every refusal names what
  was looked for and why each candidate was rejected: the index, the column,
  and the reason. The discovery query itself lost its `sys.types` join, which
  dropped every CLR UDT key column and could return a primary key with a column
  missing — and its `sys.key_constraints` join with it. Returning every unique
  index therefore costs no more than returning the one primary key did, and on
  a small catalog less: measured server-side, 2000 executions a run, four
  interleaved runs, 151 → 38 µs on the test database (a primary key and two
  ordinary indexes; 267 → 92 µs with eight unique indexes). On a catalog of
  3000 tables and 9200 indexes the joins matter less and the extra rows a
  little more: 58 → 49 µs against the query as it shipped, and 6 µs (14%)
  more than a primary-key-only query without the joins would cost. Tens of
  microseconds either way, inside a batch that already pays a round trip.

- **`NOT IN` on a string column reaches the server**
  ([#366](https://github.com/hugr-lab/mssql-extension/issues/366)). DuckDB
  binds `v NOT IN (...)` as `NOT (v IN (...))`, and the filter encoder had no
  case for `IN` as an operator expression (only for the table filter the
  combiner builds from a bare-column `IN`), so `NOT IN` — and `IN` over an
  expression such as `n + 1 IN (2, 3)` — ran client-side after a full
  transfer, and under DuckDB's byte equality: on a `_CI` collation
  `v NOT IN ('ab', 'x')` kept `ab␣` and `AB` where `v <> 'ab'` beside it did
  not. Both now render as `[v] [NOT] IN (@p1, @p2)` with the list declared
  from the operand's column (spec 076), the server's answer like every other
  string predicate (spec 079 D4), and a seek where the column is indexed.
  `NOT IN` lists, and `IN` over an expression, longer than 256 items stay
  client-side, as they did before this change (each item is one of the
  statement's 2000 parameters; the cap is per predicate); a bare-column `IN`
  is a table filter and is not capped, as it never was.
- **A non-ASCII constant no longer costs the index seek on a `SQL_` collation**
  ([#361](https://github.com/hugr-lab/mssql-extension/issues/361)). Spec 076
  declared any non-ASCII constant of a pushed filter as an `nvarchar`
  parameter whatever the column's collation — right for a UTF-8 column on a
  Latin-1 database (#321: a `varchar` parameter takes the *database's* code
  page), and an Index Scan with `CONVERT_IMPLICIT` on the column for the
  installation default, `SQL_Latin1_General_CP1_CI_AS`, where `'ñu'`,
  `'Müller'`, `'café'` read the whole index (Windows collations seek through
  the convert and were unaffected). The declaration is now `varchar` when every
  character of the constant is representable in **both** the column's and the
  database's code page, and `nvarchar` otherwise, exactly as before. The pages
  are the server's own answer — `COLLATIONPROPERTY(collation, 'CodePage')` now
  rides with the column metadata and the database collation — and
  representability is answered from tables for the single-byte pages 874 and
  1250–1258; a double-byte or unknown page keeps the safe form. `text` columns
  follow the same rule (their `varchar(max)` parameter turned `'ы%'` into
  `'?%'` and `LIKE` matched rows it should not). **One visible consequence on
  a `SQL_` collation:** the comparison now runs under the collation's own
  (non-Unicode) sort rules instead of the Windows Unicode rules an `nvarchar`
  parameter forced — `'ß' = 'ss'` was true and is now false on
  `SQL_Latin1_General_CP1_CI_AS`, which is what a T-SQL `varchar` literal
  answers there.
- **`COPY` no longer drops a CLR UDT column of an existing target in silence**
  ([#353](https://github.com/hugr-lab/mssql-extension/issues/353)). The
  target-metadata query joined `sys.types` on `system_type_id`, which no
  `sys.types` row satisfies for a `geometry`, `geography` or `hierarchyid`
  column, so the column never reached the resolver: a source column feeding one
  was ignored and its values lost, and a NOT NULL target failed with "Cannot
  insert the value NULL into column …" about a value the user had supplied. The
  join is gone from all eight metadata queries — the name comes from
  `ISNULL(TYPE_NAME(c.system_type_id), TYPE_NAME(c.user_type_id))`, which is
  correct for UDTs and **8.7× cheaper on the query itself** (1316 µs → 152 µs
  per execution on a 101-column table, 2000 executions a run, four interleaved
  runs). The join is expensive not by itself — without the `ORDER BY` it costs
  2.7× — but because it defeats the index order on `sys.columns` and makes the
  server sort for the `ORDER BY c.column_id` every one of these queries
  carries. A source that feeds such a column is now
  refused by name, **at init, before a row is encoded** — so a COPY that is
  going to fail this way writes nothing rather than committing the batches that
  happened to precede the first value. A source that omits such a column still
  loads and the server fills it, exactly as before, and so does one that gives
  it a constant `NULL AS g`: that is how one says "leave this column alone", it
  worked before #353 (by accident — the join hid the column), and it is safe
  because DuckDB types a bare NULL as SQLNULL, which has no other value it
  could hold. Such a column is dropped from the load rather than declared;
  declaring it would make the server refuse the whole `INSERT BULK`, since the
  type mapping gives it the VARCHAR fallback. The two non-UDT types the change
  also refuses,
  `sql_variant` and `rowversion`, lose nothing: measured against the server,
  both already failed mid-stream — `Operand type clash: nvarchar(max) is
  incompatible with sql_variant` and error 273 respectively — so the refusal
  only moves the error earlier and names the column.
- **A transaction's first statement could fail with `Cannot execute: connection
  not in Idle state`** ([#356](https://github.com/hugr-lab/mssql-extension/issues/356)).
  The connection a transaction pins was published to other threads **before**
  `BEGIN TRANSACTION` had finished on it, and DuckDB initialises a plan's source
  and its sink on different threads — so the second one could execute on a
  connection that was still mid-BEGIN. Intermittent, about 4% for a statement
  that both reads from and writes to the same catalog inside a transaction, and
  it also leaked the connection out of the pool when it struck. Acquiring,
  beginning and publishing are now one critical section per transaction, so a
  second thread waits and then finds a connection that is pinned, begun and
  idle. Measured: 250 runs of the case that used to fail, in two independent
  batches, with no failures.

### Added

- **`INSERT … RETURNING` works against a table holding a column the wire cannot
  decode raw** — a spatial UDT, `sql_variant`, `hierarchyid`. It used to fail
  with `COLMETADATA parse error: Unsupported SQL Server type: UDT` **even when
  the RETURNING list did not name that column**, because the generated `OUTPUT`
  clause carries every column of the table: DuckDB's RETURNING projection sits
  above the insert and expects the table's full width. The OUTPUT list now uses
  the same expressions the read path does, `.STAsBinary()` for the spatial
  types and a CAST to NVARCHAR(MAX) for the rest, so those columns come back in
  the same shape a catalog scan gives them.

- **A `GEOMETRY` value can be written into a `geometry` / `geography` column**
  (#296). It used to go as a bare `0x…` literal, which SQL Server reads as its
  own Spatial Type Binary Format rather than as the OGC WKB a DuckDB GEOMETRY
  carries, and rejects: `24210: Geometry type with an unexpected version of 0
  received`. INSERT and UPDATE now wrap it as
  `geometry::STGeomFromWKB(0x…, srid)`, the server-side reader for that form.
  The **SRID is an assumption**, because `.STAsBinary()` does not carry one and
  a value read from SQL Server has already lost it: a `geometry` target gets 0
  (planar, undefined) and a `geography` target gets 4326 / WGS 84, since
  geography refuses 0 outright. Set another one server-side after the load. An
  INSERT naming a spatial column stays on the statement path whatever
  `mssql_insert_bcp_threshold` says, as it always has — the bulk wire would
  declare the column nvarchar and send the WKB as text.

- **`ROWVERSION` columns are readable, and the native `JSON` type of SQL
  Server 2025 is read uncast** ([#296](https://github.com/hugr-lab/mssql-extension/issues/296)).
  Neither type name was in the catalog's table, so both took the unknown-type
  route, `CAST(col AS NVARCHAR(MAX))`. For `rowversion` that is not merely
  wasteful, the server **refuses** it — `[529] Explicit conversion from data
  type timestamp to nvarchar(max) is not allowed` — so a table carrying such a
  column could not be read at all. It is `binary(8)` on the wire and now reads
  as `BLOB` with no conversion; the name to look for in `sys.types` is
  `timestamp`, which has nothing to do with time. Do not write it: SQL Server
  refuses an explicit value with error 273, so leave it out of the INSERT
  column list. The 2025 `JSON` type arrives as `varchar(max)` under a UTF-8
  collation and now reads as `VARCHAR` directly, where the CAST used to convert
  every value server-side for nothing. `COPY` into an existing table with a
  `JSON` column is accepted too: the compatibility table listed `xml` for a
  VARCHAR source but not `json`, so a COPY was refused at bind while an INSERT
  into the same column worked.

- **INSERT loads through BCP** (spec 062). An INSERT with more rows than
  `mssql_insert_bcp_threshold` (default 1000), no `RETURNING` and no
  explicitly named identity column goes through `INSERT BULK` — the wire
  COPY and CTAS use — on the transaction's pinned connection or on a pool
  connection inside a server transaction of its own, with parallel writers
  where their loads cannot block each other: a target with no nonclustered
  index on it, either a heap under TABLOCK or a clustered columnstore
  without it. SQL Server gives concurrent bulk loaders compatible BU locks
  only on a bare heap — add a nonclustered index and the same hint takes a
  Sch-M lock, and on a columnstore the extra writer fails its load outright. Measured: 1M
  rows × 3 columns into an existing table in 1.8 s on one writer and 0.5 s
  on four, where the
  statement path took 74 s. The rows are staged until the threshold decides
  the path, so the decision is exact; `RETURNING`, an identity column and
  `mssql_insert_use_bcp = false` keep the statement path. The bulk wire
  carries `CHECK_CONSTRAINTS, FIRE_TRIGGERS, KEEP_NULLS` so the INSERT still
  checks constraints, fires triggers and keeps its NULLs — a bulk load
  ignores all three by default, and COPY still does. A failed load names
  the batch and says `rolled back`. The batch size, TABLOCK policy and
  writer count are the `mssql_copy_*` settings. Reviewed by
  [@oluies](https://github.com/oluies), who found the writer rule blind to a
  nonclustered index and pushed the fix (#349, merged into #348) — a heap
  carrying one takes Sch-M rather than BU, and the extra writers stall 30 s
  behind it.

- **The catalog knows which columns are IDENTITY** (spec 062 W4, the
  metadata half of #327). `sys.columns.is_identity` rides in the four
  column-metadata queries and on `MSSQLColumnInfo`; the INSERT planner reads
  it instead of hard-coding false. It is what routes an INSERT that names an
  identity column onto the statement path. The other half of #327 — omitting
  the column from a column-list-less INSERT — is not reachable from an
  extension: DuckDB's binder compares the value count against the columns the
  catalog reports, before any extension code runs. See spec 077.

- **Pushed filters are parameterised** (spec 076). The constants of a pushed
  filter travel as `sp_executesql` parameters declared from the column they
  are compared with, so the statement text is one fixed string per filter
  shape and SQL Server keeps one plan for it — 40 scans with 40 distinct
  two-predicate filters left 40 ad-hoc plans (2.3 MB) before and leave 1
  (57 KB) now. Declared from the column, not the constant: varchar stays
  varchar and an index on it stays seekable, the width is never narrower
  than the constant, a non-ASCII constant goes as nvarchar whatever the
  column's collation (a varchar variable takes the database's code page). `IN` lists and `IS NULL` stay literal.
  `mssql_scan_parameterize_filters = false` restores literal SQL, the
  escape hatch for parameter sniffing.
- **A fresh table's first touch is one round trip** (spec 076 W2). The
  catalog loaded a table's metadata and columns in one query and its primary
  key in a second, on a second connection; the discovery statement now rides
  in the same `sp_executesql` batch as the table's metadata (a second result
  set off the same `@s` / `@t`) and the entry is created with its key already
  known. Against a remote server that is the difference between two login
  round trips and one before the first row.
- **ATTACH initialised the catalog twice** (spec 076 W2). The storage
  extension's attach callback called `MSSQLCatalog::Initialize` and DuckDB's
  `AttachedDatabase::Initialize` called it again right after: the second
  call built a second connection pool (the first, with its freshly logged-in
  connection, was thrown away), logged in again and asked the database
  collation again — two of the three logins an ATTACH cost (#324), and the
  "collation query twice" the spec measured. `Initialize` is now a no-op
  once the pool exists.

- **`mssql_scan_params` and `mssql_exec_params`** (spec 075): the raw-SQL
  functions with parameters. `mssql_scan_params(ctx, statement, {'id': 42,
  'since': TIMESTAMP '2024-01-02'})` sends `@id` and `@since` through
  `sp_executesql`, declared from the DuckDB types (`VARCHAR` →
  `nvarchar(4000)`, or `nvarchar(max)` past 4000 bytes; `MSSQL_VARCHAR(20)` →
  `varchar(20)`; `TIMESTAMP` → `datetime2(6)`; `DECIMAL(10,2)` →
  `decimal(10,2)`; the full table is in `docs/query-execution.md`) or from
  the caller's own list as a fourth argument (`'@ts datetime, @c
  varchar(8)'`). The server keys its plan on the statement text and the
  declarations, so every call — from any session — reuses one compiled plan
  instead of compiling per distinct value; a statement `mssql_exec_params`
  runs for a thousand rows compiles once. A bare `NULL`, a LIST or STRUCT
  value and a key that is not a T-SQL identifier are refused at bind with
  the fix named.
- **`mssql_scan(..., prepared := true)`** (spec 075): compile once via
  `sp_prepare` — the shape comes from the prepare's answer and execution is
  `sp_execute` by handle, on the session that holds it. The default path
  describes at bind and compiles again at execution; the server's ad hoc
  cache makes that second compile cheap, but a statement the plan cache
  will not keep (`optimize for ad hoc workloads`, a large batch) pays it in
  full. A statement the server prepares without a shape — a batch of more
  than one statement — degrades to the default path.

### Changed

- **A pushed statement whose every column the extension types is planned
  without a round trip** (spec 081): the remote-pushdown rewriter sends it
  through `mssql_scan_params_unsafe` with the catalog's types, so its first
  run takes one connection instead of two and its bind needs no connection
  (77% of the pushdown suite's statements). A statement with a column the
  server types (`g + 1`) or one cast back after the read (an integer `sum`) is
  described as before, through
  the shape cache. Over a table changed outside the catalog such a statement
  now fails at execution rather than at bind, naming the statement and
  `mssql_invalidate_cache()`.

- **A user's `mssql_scan` / `mssql_scan_params` of a text already described
  binds from the describe cache** (#410): no `sp_describe_first_result_set`,
  no connection at bind. The describe was most of a short query's cost on the
  0.3.0 line -- a TOP 1 took 3.5 ms against 0.65 ms on v0.2.5, and 1.1 ms
  with the shape given. The first run of a text still describes. A shape
  changed behind the catalog (`mssql_exec` DDL, another client) fails that
  statement once, saying to run it again; `mssql_invalidate_cache()` drops
  the shapes.

- **`mssql_remote_pushdown` is on by default** (spec 079 PR E2). A SELECT
  over one attached database with a join, an aggregate, `DISTINCT`, `ORDER
  BY` or `LIMIT` runs on SQL Server as one statement. **String
  `GROUP BY`, `DISTINCT`, `count(DISTINCT …)`, join keys, `PARTITION BY` and
  a nested `UNION`'s deduplication now follow the column's collation**, as `WHERE`
  already did: on a case-insensitive collation (`SQL_Latin1_General_CP1_CI_AS`
  is SQL Server's installation default) `'a'` and `'A'` are one group —
  `count(DISTINCT legacy)` over `'a', 'A', 'b', 'B'` is 2 where it was 4.
  String orders stay DuckDB's. `SET mssql_remote_pushdown = false;` before
  `ATTACH` (or the ATTACH option `remote_pushdown false`) restores the
  previous behaviour. While on, the database's schema
  `main` answers as its default schema, for DDL too (`DROP TABLE
  db.main.x` drops `dbo.x`), and DuckDB skips its catalog-or-schema ambiguity
  check for it.
- **An error names its cause.** When the server answers a metadata query, an
  `mssql_exec` or a result-shape describe with several errors, the first one
  is reported (it was the last), as a result stream already did: a pushed
  statement over a dropped table says `Invalid object name 'dbo.t'` instead
  of `sp_describe_first_result_set`'s "see previous error".
- **No INSERT statement carries more than 1000 constants** (spec 062 W1b).
  SQL Server auto-parameterises a multi-row `VALUES` INSERT — one cached
  plan per (table, column list, row count), compiled once — only up to 1000
  constants; past that every statement compiles its own ad-hoc plan and
  leaves it in the cache. The default of 1000 rows per statement put every
  table with two or more columns past the line: a 1M-row `INSERT … SELECT`
  into a 3-column table cost 74 s, 70 µs a row, all of it compile. Rows per
  statement are now `min(mssql_insert_batch_size, 1000 / columns)` — 333 for
  three columns, at 14 µs a row (measured 5.5× at the boundary). This is the
  statement path only; the bulk path is spec 062's main work.

- **One bulk-load session type for every writer** (spec 062 W0). COPY's and
  CTAS's shared writer — the one on the operator's own connection — ran their
  own copies of the INSERT BULK / COLMETADATA / flush-and-reopen / DONE
  sequence; both now run it through `BulkLoadSession`, which the parallel
  writers already used, via a second entry point that adopts a connection the
  operator holds (the transaction's pinned one, or a pool connection). No
  wire change. One release path changed: a COPY or CTAS returning its pool
  connection after a successful load now sets the reset flag from
  `mssql_reset_connection`, as every other release path did — before, the
  success path returned it without. And a CTAS whose SELECT yields no rows
  no longer opens a bulk stream at all (it used to send `INSERT BULK` and a
  zero-row `DONE`); the table is created the same.

- **`mssql_scan` no longer executes its query at bind** (spec 075, #336).
  Bind asks `sp_describe_first_result_set` for the result's shape; the query
  runs when the scan initialises. A `DESCRIBE` or `EXPLAIN` of a batch with
  an `INSERT` ahead of its `SELECT` therefore inserts nothing, and a query
  with a side effect runs exactly once. A statement the server cannot
  describe — a batch that reads a `#temp` table it creates, a procedure — is
  still run at bind, as before. Should the shape at execution differ from
  the one bound, the scan fails loudly rather than serving rows of another
  shape.
- **The per-table metadata queries are parameterised** (spec 075, #334).
  The table list, a table's columns, its primary key and its row-count
  estimate used to carry the schema and table names inside the query text,
  so the server compiled a plan on every first touch of a table — measured
  at ~30 ms per table before, 0–1 ms after, because the text is now one
  fixed string per shape with `@s` / `@t` as `sp_executesql` parameters.
- **A DML batch whose response cannot be parsed is an error, not a silent
  success** (#344). In autocommit UPDATE and DELETE run in batches of
  `mssql_dml_batch_size` and INSERT in statements of `mssql_insert_batch_size`,
  each committed on its own; a desync in batch K therefore leaves batches 1..K
  applied and K+1.. never sent, and the error says exactly that. Chosen over
  the previous behaviour, which applied everything and reported success with
  the server's own errors after the desync dropped, and as an interim: spec
  062 and the UPDATE/DELETE rework move DML to one server-side statement per
  DuckDB statement, and the batch boundary goes with them.

- **Breaking: the server certificate is verified by default** (spec 074).
  `Encrypt`, `TrustServerCertificate` and the new `HostNameInCertificate` now
  mean what they mean in the Microsoft drivers (ODBC 18, SqlClient 4.0, JDBC
  10.2, go-mssqldb): `Encrypt` (default `true`) says whether the session is
  encrypted; `TrustServerCertificate` (default `false`) whether the server's
  certificate chain is checked against the platform trust store (Windows
  ROOT + CA, macOS keychain, OpenSSL's default paths; `SSL_CERT_FILE` /
  `SSL_CERT_DIR` honoured) and its subject against the host connected to;
  `HostNameInCertificate` names the subject when it differs from the address
  dialled (an IP, an SSH tunnel, an alias), and after a login-time routing
  hop the routed host is checked unless it was given. `TrustServerCertificate`
  used to be an alias of `Encrypt`, and giving both with different values was
  an ATTACH error; `Encrypt=true;TrustServerCertificate=false` is now simply
  the default. **A connection to a server on a self-signed certificate** --
  the docker image, an on-prem instance with none installed -- **now fails**
  with `certificate verification failed for <host>: self-signed certificate`
  and needs `TrustServerCertificate=yes`, exactly as `sqlcmd -C` does. The
  message names that, and `HostNameInCertificate=<name>` for the case where
  the certificate is valid but issued for another name. URI spellings
  `trustservercertificate` / `hostnameincertificate`; secret fields
  `trust_server_certificate` / `host_name_in_certificate`.

### Removed

- **`mssql_open()` / `mssql_close()` / `mssql_ping()` / `mssql_close_all()`**,
  the standalone diagnostic-handle API, and the `MSSQLConnectionHandleManager`
  singleton behind it. Marked `[DEPRECATED]` in spec 047 (v0.2.1) for removal
  at the next major boundary, which the first release on the duckdb 2.0 line
  is; it was the last extension-internal process-wide state. `ATTACH` plus
  `mssql_scan` / `mssql_exec` / `mssql_pool_stats` cover every use it had,
  inside the catalog lifecycle and the per-catalog pool.

### Fixed

- **`INSERT … RETURNING` returned only the last statement's rows** once the
  insert spanned several statements: the executor kept the last result
  chunk "for simplicity", so a RETURNING insert of more than one
  statement's worth of rows (1000 before, `1000 / columns` after the cap
  above) silently lost the earlier rows from its RETURNING output while
  inserting all of them. Every statement's rows come back now
  (`insert_returning_batches.test`).

- **A failed INSERT, UPDATE or DELETE leaves the table as it was** (spec 062
  W1c, closes #344). The three statement executors took a pool connection
  per batch in autocommit, and each batch committed on its own, so a failure
  in batch K left batches 1..K-1 applied with no way back; the interim of
  #344 could only say so in the message. A statement now runs on ONE
  connection — the pinned one inside a DuckDB transaction — and in
  autocommit brackets its batches in a server transaction of its own:
  `BEGIN TRANSACTION` before the first, `COMMIT` after the last, `ROLLBACK`
  on any failure. The message says what happened to the rows: `rolled back`
  in autocommit, or that they sit in the open transaction until its
  `ROLLBACK`.

- **Two reads of one catalog inside a transaction** (spec 075, #329). Two
  `mssql_scan` calls in one statement, or an `mssql_scan` beside a catalog
  scan, collided on the transaction's pinned connection (`Cannot execute:
  connection not in Idle state`) because the raw scan held it from bind to
  the last row. Inside a transaction every scan of a catalog now drains at
  initialisation under the catalog's materialise lock, in whatever order
  DuckDB initialises them.
- **A sink reading from the catalog it writes to, inside a transaction**
  (spec 075, #239). `COPY (SELECT ... FROM srv.dbo.t) TO 'mssql://srv/...'`
  and `INSERT INTO srv.dbo.t2 SELECT ... FROM srv.dbo.t` past one batch
  failed with `Connection is busy executing another query`: the bulk load or
  the second INSERT batch went down the pinned connection the source scan
  was still streaming from. The planner now materialises the scans of any
  catalog the plan also sinks into, and the bulk load opens its stream
  (`INSERT BULK` + COLMETADATA) on the first chunk rather than when the sink
  is created -- DuckDB creates the sink before it initialises the source, so
  an open stream at that point would have collided with the source's drain.
  Documented as a limitation until now.
- **A TDS desync in a DML response is reported, names what the server did,
  and never hangs** (#323 follow-up, #344; the four response loops of the
  INSERT batch, `INSERT ... RETURNING`, UPDATE and DELETE). A parser that
  desynced mid-response was fed the whole remaining response for nothing and
  then reported success — every token after the desync dropped, including a
  SQL Server error following a procedure call. `INSERT ... RETURNING` did
  worse: its loop had no end-of-message exit, so it blocked for its full 30 s
  and then blamed the socket, leaving the connection `Executing`. Each loop
  now stops feeding a parser in Error, drains to end-of-message, and fails
  with the parse error; the message says the server executed the batch (a
  desync is a client-side framing failure, noticed after the batch ran) and
  how many rows the batches before it applied. A parse error no longer
  renders as `[0]`, which read as a server error code. Test lever:
  `mssql_test_fail_parse_after_tokens`. Found on the way: an INSERT error
  named the statement after the failing one and an empty row range
  (`statement 1 (rows 1000-999)`), because the batch builder had already
  moved past the batch by the time it ran; the batch's own index and range
  are reported now.
- **`mssql_pool_stats.last_create_error` carries its age.** A pool at its
  limit recovers by reuse and never reaches the creation-success path that
  clears the recorded error, so the column could show a reason the pool had
  outlived for hours — or, had a reuse cleared it, blank out a live failure
  (an expired Azure AD token beside warm connections). The new
  `last_create_error_age_ms` says which; the creation backoff of #302 is
  untouched by reuse.
- **CTAS names why it could not get a connection.** Its four `Acquire` calls
  (existence checks, the bulk load, the cleanup DROP) threw without the
  reason the pool now records; an expired token read as "Failed to acquire
  connection to check table existence" and nothing more.

- **An attached Azure AD catalog can open new connections after its token has
  expired** ([#302](https://github.com/hugr-lab/mssql-extension/issues/302),
  spec 073). The pool factory captured the FEDAUTH token bytes at `ATTACH` and
  presented them for every connection it ever created; an Azure AD token lives
  **60 minutes**, so a pool refill hours later — a second scan in a join, an
  `UPDATE` after the idle connection was reaped — was dropped by the gateway
  and reported as `Failed to acquire connection from pool (timeout)` after the
  full `mssql_acquire_timeout` (measured: 600 s, then `DETACH`/`ATTACH` and it
  worked). The token is now resolved when a *connection* is created: the
  factory holds the secret's name and the `DatabaseInstance`, `TokenCache`
  answers while the token is good and the secret is re-read and a new token
  minted past the refresh margin. A fixed `access_token` secret, and an
  interactive credential chain that cannot mint silently from a worker thread,
  fail **by name** — *"expired at …; DETACH and ATTACH …"* — before any socket
  is opened, because Azure SQL will not say "expired" itself: it hangs up.

- **A connection the pool cannot create no longer looks like a full pool.**
  `Acquire` treated a factory failure as exhaustion and waited for a `Release`
  that, with nothing active, could not come — then said "(timeout)" and threw
  the reason away. Now: nothing active → fail at once, every time; others
  active → keep waiting for a release, but retry creation on a backoff
  (250 ms doubling to 4 s), not on every wakeup; and the message carries the
  reason of **this** call's attempt — `could not create a connection: Login
  failed for user 'sa'.` — while a timeout on a pool that has since recovered
  is reported as the timeout it is. A wrong password used to take
  `mssql_acquire_timeout` seconds to say nothing.

- **`mssql_connection_timeout` now bounds a pool refill.** Every factory passed
  no timeout to `Connect` (compiled-in 30 s), and every login-phase read —
  PRELOGIN, TLS, LOGIN7 and FEDAUTH responses, on all three auth paths — had
  `DEFAULT_CONNECTION_TIMEOUT` spelled out. The setting governed `ATTACH`-time
  validation and nothing after it. This is the reporter's "31 seconds": one
  30 s login read on a connection the gateway had dropped.

- **Stored-procedure calls desynced the TDS parser**
  ([#323](https://github.com/hugr-lab/mssql-extension/issues/323), spec 072).
  RETURNSTATUS (0x79) is a fixed five-byte token with no length field, and the
  parser skipped it as if it carried one: a procedure returning 0 left two bytes
  behind, one returning *n* ate *n* bytes of the DONEPROC that follows. Through
  `mssql_scan` that was `TDS parse error: Unknown token type: 0x0` and a
  discarded connection. Through `mssql_exec` it was **worse and silent**: the
  batch reported success with every token after the procedure dropped —
  including a SQL Server ERROR raised after `EXEC`, so `EXEC p; RAISERROR(…)`
  returned 0 with no error. Three more in the same group, found by the survey:
  TABNAME was registered as 0x04 (the wire says 0xA4, so any `FOR BROWSE`
  result failed); RETURNVALUE has no length field either and now fails by name
  instead of mis-skipping (it is unreachable — sent only for RPC, which the
  extension does not issue); and the unknown-token message printed decimal
  after `0x`, so 0xA4 read as `0x164`. `mssql_exec` now reports a parser error
  as an error rather than as end-of-batch, after draining to EOM so the
  connection stays reusable.

- **A metadata load that fails mid-query no longer mutates the cache**
  ([#317](https://github.com/hugr-lab/mssql-extension/issues/317), reported by
  [@oluies](https://github.com/oluies)). `LoadAllSchemasMetadata` cleared each
  schema's table map from inside the row callback and only re-published
  `tables_load_state` after the query returned, so a non-retryable throw —
  metadata timeout, reset connection, killed session — left every schema the
  callback had touched **emptied while still carrying its previous load
  state**. The reset lambda covered the 1205 retry path only. It is the shape
  the #178 review fixed for `Refresh()`, and spec 071 widened the blast radius
  from one schema to the whole catalog by replacing the per-schema loop with a
  single query. Nothing is written into the cache now until the query has
  returned.

  The same defect was in the sibling loader — `BulkLoadAll`'s per-schema branch,
  where `mssql_preload_catalog(ctx, 'schema')` routes — and **there it was
  observable** (found by @oluies reviewing the first fix). That loop cleared the
  columns of a table already cached and already marked `LOADED`, so a throw
  before publication left it claiming to be complete with only the columns that
  had arrived: a two-column table came back from `SELECT *` with one column, for
  the rest of the session. Both loaders are staged now.

### Added

- **`mssql_test_fail_metadata_after_rows`** (test-only, default 0 = off): make
  a metadata query throw after N rows. Every cause of a mid-query metadata
  failure in the wild arrives from outside and cannot be asked for from SQL, so
  without it the invariant above is untestable — which is how it came to be
  broken twice. Off costs nothing: the row callback is passed through unwrapped.

- **`MSSQL_VARCHAR('MAX', 'collation')` — a per-column MAX target**
  ([#321](https://github.com/hugr-lab/mssql-extension/issues/321)). The
  annotation was capped at 8000/4000, so the MAX form could only be reached by
  leaving a column *unannotated* — where which of `varchar(max)` /
  `nvarchar(max)` it becomes, and with which collation, is decided by two
  GLOBAL settings. Library code creating tables through CTAS or
  `COPY … (CREATE_TABLE true)` cannot set those: it would change the type of
  every other unannotated string column in the caller's session. `'MAX'`, `0`
  and `-1` are now all accepted in the length position and mean the same thing
  (`0` matches `mssql_default_string_length`, `-1` matches
  `sys.columns.max_length`). A bare `MAX` keyword cannot be supported — DuckDB's
  parser rejects a non-constant type modifier before the extension is consulted
  — note that this is also the form the type *prints* as (`MSSQL_VARCHAR(MAX)`
  in `DESCRIBE` / `duckdb_columns()`), so pasting it back into a cast needs the
  quotes added.

- **A non-UTF-8 text column now says so, once per query**
  ([#224](https://github.com/hugr-lab/mssql-extension/issues/224)). A
  `CHAR`/`VARCHAR`/`TEXT` column whose collation is not a UTF-8 one is handed
  back as code-page bytes in a DuckDB `VARCHAR`, which is UTF-8 by contract.
  That stays **documented rather than enforced** — validating costs the scan's
  hot path in order to make a hand-written `mssql_scan()` fail loudly, and it
  bills hardest the UTF-8 configuration #225 optimised for. Instead the check
  runs over COLMETADATA — a handful of columns, never a per-row cost — as soon
  as the stream has its column types *and* again when it is drained, and reaches
  `duckdb_logs` at `WARNING` naming the column, its LCID and its SortId. Both
  calls matter: warning only at the drain says nothing for any query that stops
  early, which is the `LIMIT` that a code-page column is most likely to be met
  by first. The predicate is the TDS fUTF8 flag, verified against a live SQL
  Server rather than read off MS-TDS. `mssql_warn_non_utf8_collation` (default
  `true`) turns it off: the trigger is the majority configuration, since
  `SQL_Latin1_General_CP1_CI_AS` is the installation default, so anyone who has
  read the warning once and decided their data is fine needs a way to stop
  hearing it.

### Fixed

- **Forward-port of the v0.2.5 fixes to the duckdb 2.0 line** (#314). The
  released v0.2.5 sits on the `duckdb-v1.5.5` maintenance branch; these are the
  same two fixes against duckdb `main`.
  - **An MSSQL catalog's default schema is `dbo`** (#129). `Catalog::GetDefaultSchema()`
    answered `main` from the base class, which no SQL Server database has, so
    anything resolving an unqualified name through the catalog default failed
    with `Schema 'main' not found` — ducklake hits it on ATTACH, before any
    table exists. On the 2.0 API the signature is `optional<Identifier>`, where
    `nullopt` means "this catalog has no default, do not try"; we return
    `Identifier("dbo")`.
  - **Two scans of one catalog inside a transaction** (#239). A transaction pins
    one connection and routes every read of that catalog to it, but a scan holds
    that connection from `InitGlobal` until its last row, and DuckDB does not
    promise to drain one source before starting the next. Such plans are now
    materialized into a buffer-managed collection as each scan starts, freeing
    the connection immediately. Autocommit is untouched.

- **`mssql_scan()` inside a transaction took the pinned connection and kept it**
  (#316). The #239 gate runs on the optimized plan and sees only
  `mssql_catalog_scan`. `mssql_scan()` is unreachable from there by construction:
  it executes its query during **binding** — that is how it reads the result's
  column types — so a second scan of the catalog failed inside its own Bind,
  before any `InitGlobal` and long before the gate. Reported shapes were
  `Connection closed unexpectedly` (catalog scan + `mssql_scan`) and
  `Cannot execute: connection not in Idle state` (two `mssql_scan`). It now
  drains at Bind whenever the session is not in autocommit — the trigger is "in
  a transaction", not "more than one scan", because Bind cannot see the rest of
  the plan. Autocommit still streams.

- **The materialized scan could not spill** (#318, thanks @oluies). It was built
  from `Allocator::Get(context)`, and that overload is duckdb's
  `IN_MEMORY_ALLOCATOR` one: unaccounted against the buffer manager and unable to
  spill, so a large materialized scan grew in process memory until it OOMed while
  the docs promised the opposite. Built from the `ClientContext` overload, which
  defaults to `BUFFER_MANAGER_ALLOCATOR`.

- **SQL Server INFO messages were collected and thrown away.**
  `SurfaceWarnings` walked them and did nothing, on the grounds that "DuckDB
  doesn't have a built-in warning API" — true when it was written, not true
  now. `mssql_exec()` was worse: its token loop read
  `case Info: // Ignore informational messages`, so running a procedure that
  `PRINT`s dropped every line. `PRINT` output, `RAISERROR` at severity 10 or
  below and procedure progress notices now reach `duckdb_logs` on both paths,
  at `INFO` (`SET logging_level = 'INFO'` to see them), carrying the message
  number and severity. They are logged **before** a failing batch raises — the
  notices of a batch that failed are the ones worth reading — including when a
  scan fails mid-stream.

- **The 5th collation byte was parsed and discarded** (`offset += 5; // we only
  store 4`), thanks [@oluies](https://github.com/oluies) —
  [#305](https://github.com/hugr-lab/mssql-extension/pull/305). It is the
  SortId, and for the `SQL_*` collations it is the *only* thing that names the
  code page: `SQL_Latin1_General_CP1_CI_AS` (CP1252) and
  `SQL_Latin1_General_CP1251_CI_AS` (CP1251) both report LCID `0x0409` and
  differ only as SortId 52 vs 106.

## [0.2.4] - 2026-08-17

### Fixed

- **Azure Synapse Serverless login** (#254; closes
  [discussion #88](https://github.com/hugr-lab/mssql-extension/discussions/88),
  [#164](https://github.com/hugr-lab/mssql-extension/issues/164)).
  `ParseLoginResponse` skipped DONE/DONEPROC/DONEINPROC as 8 bytes after
  the token byte — the pre-7.2 layout with a 4-byte row count. We negotiate
  TDS 7.4, where `DoneRowCount` is 8 bytes ([MS-TDS] 2.2.7.9), so every
  DONE token desynced the parse by 4 bytes. Harmless against regular SQL
  Server, which sends LOGINACK first and DONE last; fatal against Synapse
  Serverless, which runs internal procs during login and fronts the
  LOGINACK with a run of DONEINPROC tokens — the parser never reached the
  LOGINACK, and every auth path failed with `No LOGINACK token in
  response`. `ParseDoneForAttentionAck` had the same 8-byte skip; fixed
  alike. The unit-test DONE helper itself emitted the pre-7.2 layout —
  which is why the fixtures never caught this — and now emits 7.2+; two
  regressions replay the Synapse token pattern, and #255 adds a fuzz corpus
  over login-response token streams plus tests for the quiet casualties of
  the same skip. Confirmed against a reporter's `MSSQL_DEBUG=2` hex dump of
  the exact stream.

- **Login errors are translated from the server's error number, not our own
  wrapper text** (#262, via #263). Every login-time server error used to
  surface as "check username and password", because the translator
  pattern-matched the wrapper message it had itself produced.
  Classification now reads the server's error number: 40613/40197/40501/
  49918 are named as retryable (Azure serverless resume, reconfiguration,
  throttling), 18456 keeps and explains its State byte, and the server's
  own message text is always appended.

- **Kerberos SPN derivation from an IP literal** (#259, via #263). An SPN
  built from an IP can never match an Active Directory registration. IP
  literals are now reverse-resolved to the host's name (definitive answers
  only, memoized); hostnames pass through unresolved. The derivation is
  shared with the `mssql_kerberos_auth_test` diagnostics, so they cannot
  report an SPN the connection path would not request.

- **Windows build**: `winsock2.h` is included before anything that reaches
  `windows.h` in the auth strategy factory (d5408e5).

- **Login-time routing is now followed on every authentication path**
  (spec 068, #258). A server can answer LOGIN7 with a ROUTING ENVCHANGE (type 20)
  meaning "log in over there instead" — Azure SQL Managed Instance, Azure SQL
  under the **Redirect** connection policy (the default for clients connecting
  from inside Azure), Fabric/Synapse gateways, and on-prem AlwaysOn read-only
  routing all do it. Only the Azure AD path honoured it; SQL authentication and
  integrated (Kerberos/SSPI) authentication did not.
  - **SQL auth**: a routed login used to fail as `Authentication failed` when
    the redirect arrived without a LOGINACK, and — worse, because it was silent
    — appear to *succeed* when a LOGINACK came with it, leaving the session
    bound to a gateway that had just told the client to leave. Both now follow
    the hop.
  - **Azure AD**: a redirect with no LOGINACK (legal per [MS-TDS]) died as
    `Azure AD authentication failed` because the success check ran before the
    routing fields were read. Behaviour against gateways that send LOGINACK is
    unchanged.
  - **Integrated auth**: hops now acquire a service ticket for the **routed**
    host's SPN instead of replaying the gateway's, which could not validate. An
    explicit `service_principal_name=` is still used verbatim on every hop. See
    `Kerberos.md`.
  - The hop budget (5) and the routed-target parsing (`host\instance:port`,
    no SQL Browser lookup on a hop) are unchanged; exceeding the budget now
    names the last routed target in the error.

  **Behaviour change**: SQL authentication against an endpoint that answers with
  ROUTING *and* a LOGINACK now redirects instead of transacting against the
  gateway. That is the correct behaviour per [MS-TDS] — a routed session is not
  usable — but it is a visible change for anyone who was unknowingly relying on
  the old one.

  **Internal API change**: `TdsConnection::AuthenticateIntegrated` takes an
  `AuthenticatorFactory` (`(host, port) -> IAuthenticator`) instead of a
  pre-built authenticator. Affects embedders calling the TDS layer directly.

### Added

- **`azure_tenant_id` on ATTACH and MSSQL secrets now works** (#264). The
  parameter has been registered on the MSSQL secret since spec 032 and was
  read by nothing: `credential_chain`/`interactive` authentication always
  fell back to the `/common/` endpoint, with no way to pick a tenant in a
  single-tenant org (`TENANT_ID` on the *azure* secret is not an
  alternative — duckdb-azure rejects it on `provider='credential_chain'`).
  The value now flows into token acquisition (ATTACH option
  `azure_tenant_id` / `azure_tenant` overrides the secret's value) and into
  the token-cache key, so two tenants never share a token.
- **A runnable Azure test lane** (#264, #265). `require azure` was
  unsatisfiable in every environment the suite ran in, so the whole
  `[azure]` group silently skipped. `make azure-test` now self-provisions
  the azure extension into a local extension repository and runs the group
  against live Azure SQL (serverless warm-up, assertion floor); CI gains a
  manually dispatched job (`run_azure_tests`) that mints the
  service-principal token in-job and unsets absent secrets, so files skip
  instead of failing on empty values. The two tests the skip had hidden —
  never-executed WIP syntax — are fixed.
- **Windows SSPI CI coverage** (#260): a context/token test runs on every
  PR in an ungated job, needing no domain and no SQL Server. **Kerberos
  routing-hop e2e** (#261): `test/kerberos` gained a routing gateway, so CI
  shows a real MIT KDC issuing a second service ticket for the routed host.
- Multi-column read coverage the one-column spec-058 cases could not reach
  (#251).

### CI / Build

- The DuckDB-compat matrix could pass without comparing and broke its own
  `--json` output (#250); the LOB detector is excluded from the TruffleHog
  secret scan (#252); the Security Scan can be dispatched manually (#253).
- Linux CI fetches the simdutf amalgamation directly instead of a distro
  package; the unit-test link is wrapped in `--start-group` so GNU ld's
  single-pass archive scan cannot drop the extension loader (via #264).

> The three sections below were backfilled on 2026-08-17: v0.2.1–v0.2.3
> shipped without CHANGELOG sections. The spec 052 and credential-wipe
> entries under 0.2.1 were written at the time but sat under [Unreleased].

## [0.2.3] - 2026-08-07

The performance release. Full v0.2.2 → v0.2.3 report:
`test/bench/bench_results_v023_report.md` (44 columns × 38M rows,
interleaved same-session A/B).

### Changed

- **Write path rebuilt** — columnar encode, parallel writers, TABLOCK and
  flush policy sized to columnstore rowgroups (specs 057/063/064; #234,
  #240, #241). Like-for-like 1.34×, out of the box ≈6× (parallel writers),
  sized strings ≈9.7×.
- **Read path staged** — column-major staging + batch decode (spec 055,
  #213: read −14…−47%), framing (spec 058, #244), materialization quick
  wins (spec 054, #209), uniform column chunks published as CONSTANT
  vectors (spec 056, #221). Reads 1.22–1.40× wall vs 0.2.2.
- **UTF8SUPPORT advertised in LOGIN7** (#225, #227) — UTF-8-collated
  columns arrive as UTF-8 instead of being transcoded to UTF-16.
- Default TDS packet size 4096 → 16384 (`mssql_tds_packet_size`, spec 055).
- **Target column types and table shape for CTAS/COPY** (spec 060, #230):
  `MSSQL_VARCHAR(n)` / `MSSQL_NVARCHAR(n)` annotations, `table_kind`
  (heap/columnstore), UTF-8 collation for created VARCHAR columns.

### Fixed

- Partitioned tables were unreadable, and their row estimate came from one
  arbitrary partition (spec 049, #85, #223).
- DATETIME day count widened before epoch conversion (#222).
- Catalog-scan bind data is serialized, so common-subplan optimization
  keeps subplans distinct (#211).
- 0.2.2 segfaulted on dictionary vectors from any real columnar source —
  found while sizing this release's report, pinned by
  `test/sql/copy/vector_encodings_bcp.test` (#245).

### Added

- Versioned documentation site (Docusaurus, #243).
- Staged-read-path test pack — and the four defects it found (spec 059,
  #220).
- Local SQL Server version × charset compat matrix (#232).

### CI / Build

- POSIX vcpkg clones pinned to vcpkg.json's builtin-baseline (#249).

## [0.2.2] - 2026-07-28

### Changed

- DuckDB bumped to v1.5.5 (#207); OpenSSL 3.4.1 → 3.5.4 LTS.

### Fixed

- The named-instance resolver (spec 045) is actually wired into ATTACH —
  `Server=host\instance` resolves through the SQL Server Browser (#205,
  #206).
- Reentrant `gmtime_r`/`gmtime_s` in Azure token timestamp formatting
  (#194).
- COPY connection leak (#191, #193).

### CI / Build

- The SQLLogicTest suite actually runs in CI (#192); vcpkg binary caching
  repaired after GitHub removed `x-gha` (#198); CodeQL filters vendored
  dependencies (#195).

## [0.2.1] - 2026-06-07

### Changed

- DuckDB submodule pinned to v1.5.3 (#148); spec 051 API-compat shim
  (`mssql_compat.hpp`) so one source tree compiles against both the pinned
  SHA and rolling DuckDB main (#124).
- `DATAMODEL.md` — layered architecture reference with diagrams (#130).

### Fixed

- **Integrated-auth LOGIN7 is fragmented across TDS packets** (#138) — an
  AD-sized Kerberos PAC overflowed a single packet and broke login. Adds
  the test-only `mssql_login7_max_packet` setting so the multi-packet path
  is exercisable without a real AD.
- `mssql_query_timeout` is honored in `mssql_exec()`; long-running
  statements are no longer dropped (#90, #145).
- Clean error for DELETE on tables without a primary key (#141).
- **dbt segfault with `threads >= 2`** (spec 052, closes
  [#126](https://github.com/hugr-lab/mssql-extension/issues/126)).
  Catalog entries (`MSSQLTableEntry`, `MSSQLSchemaEntry`) switched from
  `unique_ptr` to `shared_ptr` ownership; concurrent first-load of the
  same table is coordinated via per-table singleflight so only one
  thread issues the SQL Server round trip (waiters re-check the cache).
  Lifetime extension across `Invalidate()` / `OnDetach` is done via
  per-`ClientContext` bind-time anchors registered as
  `ClientContextState`: every `MSSQLSchemaEntry::LookupEntry` /
  `MSSQLCatalog::LookupSchema` / catalog `Scan` callback path stashes
  the `shared_ptr` into the per-context `MSSQLBindAnchors` holder, which
  DuckDB releases via the `QueryEnd` hook. In-flight binders therefore
  hold every entry they touched alive for the full bind+execute span;
  release happens naturally at end of query, with no catalog-lifetime
  accumulation. Audit of `MSSQLMetadataCache::GetTableMetadata` confirms
  its only caller (`MSSQLTableSet::LoadSingleEntry`) copies fields
  immediately; contract pinned in the header.
  `MSSQLStatisticsProvider` returns by value, no pointer-handout surface.
- **`MSSQLTableEntry::EnsurePKLoaded` double-free under thread stress**
  (spec 052). Two threads both saw the load flag false, both fetched, and
  both move-assigned to `pk_info_` — the second move freed the loser's
  previous `vector<PKColumnInfo>` while the first thread still held it.
  Caught by AddressSanitizer during the spec 052 invalidation-race soak.
  Fixed with a `pk_load_mutex_` + `std::atomic<bool> pk_loaded_`
  acquire/release publication so the lock-free fast path stays correct
  under the C++ memory model.
- **`MSSQLCatalog::RefreshCache` connection leak on TDS hiccup** (spec
  052). Under thread stress (~318 invalidations / 30 s), SQL Server
  occasionally returns a transient TDS error mid-Refresh. The exception
  propagated past `connection_pool_->Release`, leaving one connection
  checked out and tripping `~ConnectionPool`'s quiescence-contract
  assert at catalog teardown. Wrapped the `Refresh` call in try/release.
- **`MSSQLTableEntry::EnsurePKLoaded` / `GetStorageInfo` connection
  leak on TDS hiccup** (spec 052). Both functions intentionally swallow
  exceptions to fall back to `pk_info_.exists = false` / cached
  `approx_row_count_`, but the outer `catch (...)` never released the
  pool-owned connection acquired inside the try. Every SELECT bind
  calls `EnsurePKLoaded`, so under scenario 5/8 stress a single TDS
  hiccup inside `PrimaryKeyInfo::Discover` stranded one connection in
  `active_connections_` for the lifetime of the pool. Nested an inner
  try/release/throw around the SQL Server I/O so the connection is
  returned BEFORE the outer fallback catch runs.
- **`ConnectionPool::Shutdown` cleanup-thread strand on quiescence
  violation** (spec 052). `D_ASSERT` in DuckDB-debug builds throws
  `InternalException` rather than calling `abort()`. The throw fired
  while `cleanup_thread_` was still joinable; `~ConnectionPool noexcept`
  caught the exception, but `~std::thread` on the joinable thread then
  called `std::terminate()`. Reordered: signal + join the cleanup thread
  and close pooled connections FIRST, then emit the warning and assert
  at the end. The warning is the operator-visibility signal; the
  trailing assert preserves the debug invariant without stranding
  resources on its unwind path.

### Added

- `test/cpp/test_concurrent_reads.cpp` scenarios 5-8 (spec 052
  US2/US3 + concurrent-write acceptance): 30 s soak runs that exercise
  every spec 052 lifetime guarantee under AddressSanitizer/UBSan.
  Scenario 5 — 4 readers + invalidator at 50 ms cadence
  (~2500 reads × ~300 invalidations). Scenario 6 — scenario 5 plus a
  `duckdb_schemas()` / `duckdb_tables()` walker exercising the bulk-scan
  anchor path (~1200 reads × ~260 invalidations × ~200 schema walks).
  Scenario 7 — 4 writers + 1 reader on one shared table with disjoint PK
  ranges (~550 INSERT/UPDATE/DELETE cycles + ~500 reads). Scenario 8 —
  4 pure-write threads (~2700 INSERTs / 30 s).
- `.github/workflows/concurrency-tests.yml` job that rebuilds the
  extension with ASan/UBSan and runs `test-concurrent-reads` on every
  PR that touches the catalog or singleflight surfaces. Includes an
  8 GB swap on `/mnt` to absorb the link-time RAM peak, a
  `TestDB`-creation step for scenarios 4-8, and `LD_PRELOAD`-ed
  `libasan`/`libubsan` on the test binary run only (so the preload
  doesn't leak into vcpkg's compiler probe).
### Security

- **Wipe bearer credentials on destruction.** `MSSQLConnectionInfo` gains a
  user-declared destructor that `OPENSSL_cleanse`s `password` and
  `access_token`; `~MSSQLCatalog` wipes the cached `fedauth_token_utf16le_`
  byte vector. `OPENSSL_cleanse` defeats dead-store elimination, so secrets
  do not linger in heap-recycled memory after the owning
  `shared_ptr<MSSQLConnectionInfo>` or `MSSQLCatalog` is destroyed.
  Rule-of-five compliance: copy/move ctors and assigns on
  `MSSQLConnectionInfo` are explicitly `= default`-ed so that user-declaring
  the destructor doesn't silently disable move generation.

## [0.2.0] - 2026-05-20

Major release: integrated authentication (Kerberos + Windows SSPI),
process-wide singleton cleanup, security hardening, and a deep codec
refactor. Closes [#82](https://github.com/hugr-lab/mssql-extension/issues/82)
(custom Application Name) and [#96](https://github.com/hugr-lab/mssql-extension/issues/96)
(ATTACH/DETACH-in-Python-loop crash class).

### Added

- **Custom Application Name in connection string** (spec 047 FR-014,
  closes [#82](https://github.com/hugr-lab/mssql-extension/issues/82)).
  ADO.NET keys `Application Name` / `ApplicationName` / `App Name` /
  `application_name` (case-insensitive), URI query parameter
  `applicationname`, and MSSQL secret fields `application_name`
  (canonical) / `applicationname` (fallback) propagate to LOGIN7
  `program_name` — visible as `APP_NAME()` /
  `sys.dm_exec_sessions.program_name`. Empty falls back to the
  extension default (`"DuckDB MSSQL Extension"`); values exceeding
  128 UTF-16 code units are clamped client-side so what the user sees
  in `APP_NAME()` equals what we sent.
- **`lazy_validation` ATTACH option** + **`mssql_attach_validation_timeout`
  setting** (spec 047 FR-011). Eager ATTACH validation is on by default
  (wrong password / unreachable host fail ATTACH instead of being
  deferred to the first query); opt out per ATTACH with
  `lazy_validation true` to preserve the pre-047 lazy behaviour
  (useful for container / orchestrator startup where the SQL Server
  may not yet be reachable). The setting bounds the eager round-trip;
  `0` (default) inherits `mssql_connection_timeout`.
- **`mssql_close_all()`** scalar function (spec 047 FR-013). Closes every
  diagnostic-API connection opened via `mssql_open()` in one call; returns
  the count of handles closed. Idempotent — a second call after a full
  close returns 0. Recommended shutdown hook for hosts that use the
  diagnostic API but do not track individual handles. Marked `[DEPRECATED]`
  from registration: lives in the same group as `mssql_open` / `mssql_close`
  / `mssql_ping` and will be removed alongside them in a future major
  release once the catalog-bound API covers all diagnostic needs (FR-010).

### Changed

- **LOGIN7 default `program_name` unified to `"DuckDB MSSQL Extension"`**
  (spec 047 FR-014 side-effect). Pre-047 SQL auth sent `"DuckDB"` and
  integrated auth sent `"DuckDB MSSQL Extension"`. The single resolution
  point (`ResolveAppName` helper, called by every auth path) now sends
  `"DuckDB MSSQL Extension"` uniformly when no `application_name` is
  supplied. Observable change for SQL-auth users who previously saw
  `APP_NAME() = 'DuckDB'`; they will now see `'DuckDB MSSQL Extension'`
  unless they explicitly set `Application Name=DuckDB`.
- **`mssql_open` / `mssql_close` / `mssql_ping` are now documented as
  `[DEPRECATED]`** (spec 047 FR-010). They remain functional and are kept
  for backward compatibility. Prefer ATTACH + the catalog-bound functions
  (`mssql_scan`, `mssql_exec`, `mssql_pool_stats`) which integrate with
  DuckDB's catalog lifecycle and the per-catalog pool ownership introduced
  in spec 047. The handle manager singleton these three functions share is
  the last extension-internal process-wide state and will be removed
  together with the functions themselves.

### Security

- **Azure TokenCache cross-instance aliasing** fixed (spec 047 FR-012).
  Pre-047, two DuckDB instances in the same process that each defined a
  secret with the same name (e.g. `mssql_secret`) shared a single
  TokenCache row keyed by `secret_name` alone — instance B could silently
  authenticate with instance A's already-acquired token even when the two
  secrets resolved to different Azure principals. The cache key is now
  namespaced by `(DatabaseInstance address, cache_key)`; tokens from
  different instances are independent. The `OnDetach` invalidation path
  is scoped to the calling instance's namespace so a sibling instance
  sharing the secret name keeps its token.

- **ATTACH credentials are now validated eagerly** by default (spec 047
  FR-011). Wrong passwords / unreachable hosts surface as ATTACH errors
  instead of being deferred to the first query. Error messages never
  contain the password (audited via sentinel substring assertion in
  `test/sql/attach/attach_validates_credentials.test`). Opt out with
  `lazy_validation true` for container/orchestration scenarios; ceiling
  controlled by the new `mssql_attach_validation_timeout` setting.

### Internal

- **Process-wide singletons removed** (spec 047, closes [issue #96](https://github.com/hugr-lab/mssql-extension/issues/96)):
  `MssqlPoolManager`, `MSSQLContextManager`, and `MSSQLResultStreamRegistry`
  are gone. Connection pools are now owned per-`MSSQLCatalog` via
  `unique_ptr`; result streams live on the catalog as
  `RegisterStream` / `RetrieveStream` methods. Two attached MSSQL
  databases under different ATTACH aliases no longer alias to the same
  pool, and ATTACH/DETACH cycles in a Python-style loop (the issue #96
  repro) succeed indefinitely.

- **Windows SSPI** integrated authentication (spec 042 Phase 4). `WinSspiAuthenticator`
  via `secur32.dll`'s Negotiate package. Uses the current Windows logon session — no
  `kinit` needed. Same connection-string surface as POSIX (`Trusted_Connection=yes` /
  `authenticator=winsspi`). Mirrors the structure of `Krb5Authenticator`; shares the
  `IAuthenticator` interface and the SPNEGO continuation loop in
  `TdsConnection::AuthenticateIntegrated`. Linked against `secur32.lib` from the
  Windows SDK — no third-party dependency.

- **Integrated Authentication (Kerberos)** for POSIX hosts (spec 042, phases 1-3).
  Adds the `IAuthenticator` multi-round interface, parser support for the
  `microsoft/go-mssqldb` connection-string surface, LOGIN7 `fIntSecurity`
  wiring, `0xED` SSPI continuation tokens, and a POSIX Kerberos backend via
  system GSSAPI. Self-contained `test/kerberos/` docker-compose stack (KDC +
  SQL Server + test-client) — no real Active Directory required.
  - New connection-string keys (verbatim from `go-mssqldb`): `authenticator`,
    `krb5-configfile`, `krb5-keytabfile`, `krb5-credcachefile`, `krb5-realm`,
    `service_principal_name`.
  - Aliases: `Trusted_Connection=yes`, `Integrated Security=SSPI/true` —
    resolve to `krb5` on POSIX, `winsspi` on Windows.
  - Three credential modes on POSIX (Linux only for keytab + raw):
    credential cache (default, uses `kinit` ticket), keytab, raw credentials
    (secret-only).
  - macOS supports credential-cache mode (uses `GSS.framework`); keytab and
    raw modes are rejected at construction time with a clear error pointing
    at the Linux container path.
  - Verbatim GSSAPI status text in errors plus actionable hints (no
    ccache → run kinit; clock skew → ntp/chrony; SPN not registered →
    setspn -L; etc.) per spec 042 R8.
  - New end-user documentation: `Kerberos.md` (mirrors `AZURE.md`).
  - Windows SSPI (`winsspi` authenticator) is Phase 4 — pending. WSL2 Ubuntu
    is the supported testing path on Windows in the meantime.

### Fixed

- Linux build with Kerberos enabled now links `libkrb5` explicitly. Previous
  builds failed at the link step with `undefined reference to symbol
  'krb5_free_error_message'` on distros where `krb5-gssapi.pc` doesn't
  transitively pull in `libkrb5` (Ubuntu 24.04 is the documented case).
  Affects spec 042 raw-credentials mode users on Linux only — macOS uses
  `GSS.framework` which bundles all symbols. Configure-time warnings now
  cite both Debian (`libkrb5-dev`) and RHEL (`krb5-devel`) package names.

### Security

- Hardened FEDAUTH JWT debug logging: `tds_connection.cpp` previously
  hex-dumped the first 20 bytes of the access token at debug level 2.
  Replaced with size-only logging plus `(contents redacted)`.
- Raw-credentials Kerberos mode is SECRET-ONLY by design — cleartext
  `Password` is rejected in any connection string when integrated auth is
  selected. Defends against cleartext passwords in connection-string logs.
- Per-connection krb5 overrides (`krb5-configfile`, `krb5-credcachefile`)
  apply through `gss_acquire_cred_from` `cred_store` elements per instance,
  not via process-global `setenv()`. Thread-safe vs concurrent `getenv` on
  pool worker threads.
- Raw-mode `MEMORY:` ccache is destroyed after `gss_acquire_cred_from`
  copies credentials internally, so cleartext credentials don't linger in
  MIT's process-global ccache registry.

### Changed

- README's stale "Windows Authentication: Only SQL Server authentication is
  supported" limitation removed. Windows SSPI is now scoped as "Phase 4
  pending" with WSL2 documented as the interim testing path.
- README's Secret Fields and Key Aliases tables expanded with Kerberos rows.
- `docs/architecture.md` Authentication Strategy Pattern section updated to
  document the new `IAuthenticator` layered interface and the
  `IntegratedAuthStrategy` adapter.
- `docs/TESTING.md` gained a Kerberos Tests section covering the docker-compose
  stack and WSL2 testing.

### Internal

- New `src/include/tds/auth/iauthenticator.hpp` — three-method multi-round
  interface (`InitialBytes` / `NextBytes` / `Free`), modeled on
  `microsoft/go-mssqldb`'s `integratedauth.IntegratedAuthenticator`. No
  DuckDB headers — the TDS auth layer is reusable outside DuckDB.
- New `src/tds/auth/krb5_authenticator.{hpp,cpp}` — POSIX GSSAPI
  implementation. SPNEGO mechanism. Inline GSS OID literals to work around
  macOS GSS.framework not exporting the well-known OID symbols.
- New `src/include/tds/auth/integrated_auth_strategy.hpp` — adapter wrapping
  `IAuthenticator` in the existing `AuthenticationStrategy` interface.
- `src/tds/tds_protocol.cpp` gains `BuildLogin7WithSSPI` (sets
  `OptionFlags2.fIntSecurity`, writes SPNEGO blob into LOGIN7's SSPI field;
  `cbSSPILong` fallback for blobs > 65 535 bytes) and `BuildSSPIMessage`
  (continuation packet, type `0x11`).
- `src/tds/tds_token_parser.cpp` recognizes `TokenType::SSPI = 0xED`.
- `src/tds/tds_connection.cpp` gains `AuthenticateIntegrated()` — drives the
  full SPNEGO continuation loop on `0xED` tokens, with an 8-round cap to
  detect cross-realm misconfiguration.
- `src/connection/mssql_pool_manager.cpp` gains
  `GetOrCreatePoolWithIntegratedAuth` — each pool connection builds a fresh
  `Krb5Authenticator` so kinit-refreshed tickets are picked up on the next
  fill. Logs verbatim GSSAPI errors to stderr on pool refill failures.
- `CMakeLists.txt` adds `ENABLE_KRB5` option (default ON on POSIX),
  pkg-config GSSAPI discovery on Linux, `find_library(GSS_FRAMEWORK GSS)` on
  macOS, `secur32` linkage hook for Windows (Phase 4).

- **Type codec consolidation** (spec 045). Per-type encoding/decoding/literal/DDL
  logic consolidated into 9 family modules under `src/codec/`:
  boolean/integer/float/decimal/money/string/binary/datetime/uuid. Each
  `<family>_codec.cpp` owns `EncodeToBcp` / `DecodeFromTds` /
  `FormatSqlLiteral` / `FormatDdlTypeName` for its types. Dispatch via
  `FamilyFromLogicalType` switch in `literal_format.cpp` + `type_family.cpp`.
  5 LogicalType-side dispatch sites collapsed; net −762 LOC across dispatch
  sites (3243→2481, −23.5%). Bonus: TIMESTAMP_MS/NS/S/TZ now round-trip
  losslessly through SQL Server DATETIME2(3/7/0/7) with full
  type-transparency. Closes [issue #91](https://github.com/hugr-lab/mssql-extension/issues/91)
  (BCP nvarchar character-vs-byte length) and [#89](https://github.com/hugr-lab/mssql-extension/issues/89)
  (VIEW catalog-vs-runtime type divergence). No new vcpkg deps. Per-row bench
  (1M rows): within 5% gate vs spec-044 baseline.

- **Named instance resolver** (spec 045 phases 0-2). SQL Server Browser
  (UDP 1434) discovery for named instances. Mock-browser test stack under
  `test/named-instance/`.

- **UTF-16 codec consolidation** (spec 044). Finishes the simdutf migration
  started in spec 043 — every legacy `Utf16LE*` call site moves to the
  simdutf-backed wrapper. simdutf becomes the production UTF-16 codec; the
  legacy hand-rolled converter survives only as a private invalid-input
  fallback. Includes microbenchmark (`make bench-utf16`) and an end-to-end
  before/after benchmark (`test/bench/bench_codec_e2e.sh`).

- **LOGIN7 non-ASCII fix + simdutf foundation** (spec 043). Non-ASCII bytes
  in LOGIN7 username/password/database fields no longer get corrupted by the
  hand-rolled UTF-16 converter. Adds simdutf as a vcpkg dependency
  (statically linked, MIT). Foundation for spec 044's full migration.

### CI / Build

- **Tier-1 lint and security checks** added: CodeQL (C++), gitleaks,
  shellcheck, hadolint, yamllint, codespell. Dependabot updates enabled.
  PR prompt-injection scanner for review descriptions.
- **CodeQL speedup** (3-part): target restriction + vcpkg cache + submodule
  trim. Cuts CodeQL job runtime substantially on PR triggers.
- **Kerberos integration job** in CI: spins up the
  `test/kerberos/docker-compose.yml` KDC + SQL Server + test-client stack
  for every PR touching the integrated-auth path.
- **Drive-by fix**: 9 codec headers (spec 045) had `class ColumnMetadata` /
  `class BCPColumnMetadata` forward declarations while the real
  definitions are `struct`. MSVC mangles `class` and `struct`
  differently (clang/gcc don't), producing 16 unresolved-external LNK2019
  errors at link time. Latent regression — last successful MSVC build on
  main was 2026-05-15, BEFORE spec 045 merged. Fixed all 9 forward
  declarations.

## [0.1.18] - 2026-02-24

### Added

- XML data type support (spec 041). XML columns read as VARCHAR; BCP write
  path; clear errors for INSERT-with-RETURNING / UPDATE on XML columns.

### Fixed

- UDT type alias crash in catalog metadata queries (issue #81).

## Earlier history

Earlier versions are tracked in git history under `specs/NNN-*/` directories.
Notable recent specs:

- **041-xml-type-support** — XML column read/write (TDS type 0xF1).
- **040-fix-datetimeoffset-nbc** — DATETIMEOFFSET in NBC row reader.
- **039-order-pushdown** — ORDER BY pushdown to SQL Server (experimental).
- **037-replace-libcurl-httplib** — Replaced libcurl with bundled cpp-httplib
  for Azure OAuth2.
- **036-azure-token-docs** — Azure AD documentation expansion.
- **034-duckdb-v15-upgrade** — DuckDB v1.5 upgrade.
- **033-fix-catalog-scan** — Catalog metadata cache fix.
- **032-fedauth-token-provider** — Manual access token support for Azure AD.
- **031-connection-fedauth-refactor** — Auth strategy pattern introduction.
- **027-ctas-bcp-integration** — CTAS via BCP protocol.
- **024-mssql-copy-bcp** — COPY TO via BCP.
- **020-multi-statement-scan** — Multi-statement support in `mssql_scan`.
- **001-azure-token-infrastructure** — Initial Azure AD support.

See `specs/` for the full feature design history.

[Unreleased]: https://github.com/hugr-lab/mssql-extension/compare/v0.2.4...HEAD
[0.2.4]: https://github.com/hugr-lab/mssql-extension/compare/v0.2.3...v0.2.4
[0.2.3]: https://github.com/hugr-lab/mssql-extension/compare/v0.2.2...v0.2.3
[0.2.2]: https://github.com/hugr-lab/mssql-extension/compare/v0.2.1...v0.2.2
[0.2.1]: https://github.com/hugr-lab/mssql-extension/compare/v0.2.0...v0.2.1
[0.2.0]: https://github.com/hugr-lab/mssql-extension/compare/v0.1.18...v0.2.0
[0.1.18]: https://github.com/hugr-lab/mssql-extension/releases/tag/v0.1.18
