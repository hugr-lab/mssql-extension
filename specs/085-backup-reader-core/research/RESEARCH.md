# 001 — Reading SQL Server backups without SQL Server

Status: research, 2026-11-05 — the research record behind specs 085–090 (this repo) and 021–023 (mssql-ducklake). Built on three reports in this folder:
`research-web.md` (formats, prior art, licences), `research-experiment.md` (real backups from SQL
Server 2025, decoded by the scripts in `tools/`), `research-architecture.md` (how it fits mssql and
mssql-ducklake). Start here; follow the links for evidence. The specs built on it are 085–090 here
([series overview](../series.md)) and 021–023 in mssql-ducklake; the owner's decisions are in
[decisions.md](../decisions.md).

## 1. What we want

1. Read a backup FILE directly — `mssql_backup_scan('sales.bak', 'dbo', 'orders')` — and, for browsing,
   `ATTACH 'sales.bak' AS b (TYPE mssql_backup)`: a full backup becomes a read-only DuckDB
   catalog: schemas, tables (slow is fine), view definitions. No SQL Server instance anywhere.
2. Differential backups laid over their full backup; transaction-log backups later.
3. In mssql-ducklake, imports into a DuckLake catalog:
   - `ducklake_import_mssql_backup(<lake>, 'full.bak', <decryption params>)`
   - `ducklake_import_mssql_diff_backup(<lake>, 'diff.bak', ...)` — incremental
   - each imported FILE is ONE DuckLake snapshot, not one per table; views later.
4. As much of the type system as possible: LOBs, xml, geometry/geography, json, vector,
   sql_variant, sparse, ROW/PAGE compression, and columnstore.

## 2. Verdict

**Feasible, and the core is already demonstrated** on our own samples (`samples/`, SQL Server 2025):
Python probes decoded MTF, pages, the system catalog, rows of every common type, LOBs (20 KB and
324 KB, byte-exact), row-overflow, sparse, sql_variant, vector, the uniquifier, forwarded and ghost
records, differential overlay, log records, and — partly — columnstore segments. Both compressed
formats (MS_XPRESS and 2025's ZSTD) decompressed completely with our own code.

No DuckDB extension reads .bak today: DuckBridge (community) makes a .bak by running `BACKUP
DATABASE` on a server and never parses one. Commercial readers exist (Redgate Data Compare reads
full + diff but not compressed/encrypted backups; ApexSQL reads compressed).

The hard, unsolved parts are the ones nobody has public answers for: ~~backup encryption, TDE~~
(**solved by E4**, 2026-11-05: both decrypt offline with the certificate's exported private key —
RSA-OAEP-SHA256-wrapped AES-256 key, AES-256-CBC per container block for encrypted backups and
per page body for TDE; see `experiments/E4-encryption.md`), **QAT_DEFLATE framing, the 2025 native
`json` binary, and log replay**. Columnstore is solvable but
costly. A realistic v1 (full + differential, uncompressed / MS_XPRESS / ZSTD, rowstore with ROW/PAGE
compression and LOBs, no log replay) is ~3–4 months of work (research estimate; the spec series costs
that reader at ~23 weeks for one developer: 085, 086 PR 1–2, 089 PR 1; §8).

## 3. Formats in one page (what the experiment established)

- **Container.** An uncompressed .bak is MTF: `TAPE SFMB SSET VOLB MSCI MSDA MSDA MSTL MSLS SFMB
  ESET…`. MSCI = file list (FILELISTONLY). MQDA (data) = 2 bytes + N × 8 KB pages, page-aligned; a
  second MSDA re-copies the allocation extents at the end; MSTL/MQTL = the log tail. Descriptor and
  stream checksums verify — validate them (a stray "VOLB" in data has looped other readers).
- **Striped and multi-file** (E2, R9): all data files share one MSDA per pass, in file id order; a
  striped set is FamilyCount complete MTF files with one MediaSetId, each extent in exactly one
  stripe — index all stripes into one page store. CHECKSUM / BLOCKSIZE / MAXTRANSFERSIZE never change
  page images (R10); walk the headers, never hard-code offsets. A page is live only if below the
  file's `Size`, GAM-allocated and PFS-allocated (R3) — full backups carry dead pages too.
- **Pages identify themselves** (`m_pageId`, `m_fileId` in the header), so the page store is an
  index by page id. Skip zero pages and the **type-101 pad units** (their bodies are stale buffer
  memory — they contained copies of real pages; reading them would leak/duplicate rows). Prefer the
  later copy where a page appears twice. Map pages to objects through **IAM chains**, not the
  header's object id (stale after shrink).
- **Compressed backups** start with `MSSQLBAK`: a 4 KB header, then blocks with a 28-byte header
  (compressed/stored flag, size in 512-byte units, payload length). Algorithm 3 = **MS_XPRESS =
  MS-XCA LZ77+Huffman** (documented codec; the match window spans blocks, so decode sequentially and
  checkpoint for random access). Algorithm 131 = **ZSTD**, plain frames (DuckDB bundles zstd). The
  decompressed stream is the same MTF as an uncompressed backup. QAT_DEFLATE: DEFLATE codec, framing
  unknown.
- **Catalog from the backup alone.** Boot page (1:9) → database name, version, collation, first
  `sysallocunits` page → `sysschobjs`, `syscolpars`, `sysrowsets`, `sysallocunits`, `sysrscols`,
  `sysidxstats`, `sysiscols`, `sysobjvalues` (view text in plain). Hard-code the layout of ~5 base
  tables; `syscolpars` then describes the rest generically. Collation id → code page needs a table
  (`tools/collations.psv`, 5540 rows); UTF-8 collations are recognisable by bit pattern.
- **Rows.** FixedVar records (status bits, fixed part, null bitmap, variable offsets); ROW
  compression's CD format and PAGE compression's anchor/dictionary (recognised, decode = work; MIT
  prior art in DbReader); sparse vector; LOB in-row roots and text trees; 24-byte overflow pointers;
  ghost records (status type 6, page ghost count, PFS bit) — skipped; forwarded stubs → moved
  record (with a back pointer).
- **Types vs the TDS wire.** Same bytes: integers, float, decimal, uniqueidentifier, date/time/
  datetime2/datetimeoffset, nvarchar (UTF-16LE), varchar (code page or UTF-8). Different: `datetime`
  and `smalldatetime` (time part first), `money` (plain int64), `bit` (packed). geometry/geography/
  hierarchyid: MS-SSCLRT (documented) — but we have no SSCLRT→WKB converter today (the server does
  `.STAsBinary()` for us). xml: MS-BINXML (documented). vector: header + float32. json (2025):
  undocumented binary.
- **Differential.** Exactly the extents whose DCM bit is set; overlay them on the full (diff pages
  win). Validate the chain: `DifferentialBaseGUID` = full's `BackupSetGUID`, base LSN, FamilyGUID.
  Diffs are cumulative since the full.
- **Log.** Log blocks in sector order in MQTL; 352 records decoded and matched against `fn_dblog`.
  INSERT/DELETE carry full row images; MODIFY_ROW/COLUMNS carry byte-range diffs, so row-level
  changes need page state, i.e. a redo engine. Sector parity bytes not yet handled.
- **Columnstore: FULLY DECODED (E5, 2026-11-05; `experiments/E5-columnstore.md`).** 21 test tables,
  3.74 M rows, 168 column checks — every table equals the server's SELECT, byte-exact. Covered: all
  five segment encodings; RLE + bit-packing at all 15 widths (1–10, 12, 16, 21, 32, 64); NULLs; primary
  and secondary dictionaries, plain and Huffman string dictionaries; strings by value (MS-XCA pages);
  COLUMNSTORE_ARCHIVE and off-row (max) values (an Xpress chunk container); every CCI type on 2025;
  COMPRESSED / OPEN / TOMBSTONE rowgroups; delete bitmaps (up to 120,300 entries); delta stores; NCCI
  delete buffers and base locators; partitioned and ordered CCI. Key facts: value = (data id +
  base_id) × magnitude (÷ for float); data ids start at 3 (2 = NULL, 3 = empty string); per-type
  integer layouts (e.g. datetime2 = days << 40 | 100 ns); rows assembled from `syscsrowgroups` +
  `sysrowsets.ownertype` (2 delete bitmap, 3 delta store, 4 delete buffer). Clean room: no decoder
  code opened; PROVENANCE §8. Not yet seen: CLOSED / INVISIBLE rowgroups, page-compressed
  delete-bitmap pages, non-string secondary dictionaries with entries; only 2025 tested (version
  drift is the main risk). C++ estimate ≈ 34 working days (5–8 weeks), risk now MEDIUM.
- **Columnstore (first look, superseded by E5).** Segment and dictionary blobs located via `syscscolsegments` /
  `syscsdictionaries` and extracted at exactly `on_disk_size`; a bit-packed segment decoded to the
  exact values; RLE and dictionaries visible. Undocumented beyond that; the only full decoder is
  GPL (Internals Viewer). Archive compression = Xpress Huffman.

## 4. Consistency

A full backup is fuzzy: pages are copied while the database changes; restore replays the log tail
(redo, then undo). Without replay, a busy database can show uncommitted rows, miss committed ones,
or see a torn page split (a row twice or not at all). Quiet databases (backups at night) showed no
user-row differences in DbReader's measurements.

**Measured (E3, 2026-11-05; `experiments/E3-fuzzy-backups.md`).** How SQL Server takes the
backup: pass 1 reads every allocated extent from disk (pages as of the checkpoint at backup start); a
second checkpoint follows — the header's CheckpointLSN; pass 2 re-copies the extents changed during
pass 1 (not DCM-driven, also under COPY_ONLY; 18–20k pages at moderate load, 73 % of the database
under heavy load); the log tail runs FirstLSN → LastLSN. So a no-replay reader (latest copy of each
page, PFS-checked) sees the database **as of CheckpointLSN, including changes uncommitted then**.
Measured on a 600 MB database under concurrent load: 0.2–0.4 % of rows differ in busy tables at
moderate load (1–2 % heavy), up to 12 % (20 %) in an append-only heap, 1.5–1.7 % (10.6 %) in a LOB
table. What is wrong: commits after CheckpointLSN are missing (a 0.6–3 s window); everything an
open transaction wrote before CheckpointLSN is visible; torn pages are rare (49 rows twice, 15 lost,
1 torn LOB in 7.8 M rows). Rolled-back transactions were never visible. A quiet backup decodes with
0 differences.

**Detecting it:** the page LSN > CheckpointLSN flag does NOT work (table recall 0.42: a page copied
before it changed keeps its old LSN). Exact at table level in every run (precision 1.00, recall
1.00): **(iii) open transactions in the log tail ∪ (iv) committed tail records newer than the page
copy**. Database level: no open transaction and no committed row operation after CheckpointLSN ⇒
the backup is consistent.

**v1 policy:** take the latest copy of each page (pass 2 over pass 1) and check PFS; always parse the
log tail (30–240 MB in E3); report per table `consistent` / `stale` / `dirty` / `stale+dirty` and a
database-level `consistent` flag; warn by default, `fuzzy := 'error'` refuses affected tables. The
log format is far enough along for this: sector parity solved (the first byte of sector k is saved
at `block[size-1-k]`; 4,283 of 4,283 records matched `fn_dblog`), opcode map verified, row-operation
offsets found.

**Redo** (time-boxed prototype, 7 operation types, open transactions skipped, no undo) removed 94–97 %
of the committed-side errors; the rest is mostly LOB trees. Completing it needs LOB-tree operations
and a logical undo of open transactions (the log carries the row images).

**Decision (owner, 2026-11-05): no dirt in the lake; uncommitted work is deferred to the next file,
and the next file must restore exactly. LOBs may be approximate at the full import.** Design:
1. Import = the pages' state + **redo** of committed tail records + **undo** of open transactions'
   changes from their logged row images (LOB trees forgiven).
2. The pages an open transaction touched are recorded as **pending**, with their page images from
   the file (a small set) and the transaction id.
3. Next file, per pending page P: P in the file → processed normally (exact), pending cleared; P not
   in the file and the transaction still open in the file's log tail → stays pending; P not in the
   file and the transaction not open → it COMMITTED (a rollback rewrites P, which would put P in the
   file) → apply P's stored image, pending cleared.
4. Healing of stale pages relies on the next differential carrying every page changed during the
   full backup — i.e. on when the DCM is reset (start vs end of the full).

**Verified (E6, 2026-11-05; `experiments/E6-exact-healing.md`), 3 cycles, 17 tables, ~240 MB, backups
under ~2,400–3,800 short transactions plus transactions open across files, truth = RESTORE:**
- **The DCM is cleared at the START of a non-copy-only full** (`Backup:InvalidateDiffMaps` writes
  `LOP_CLEAR_GAM_BITS` on the DCM 6–13 ms after BACKUP starts, before its first checkpoint;
  differentials and COPY_ONLY fulls clear nothing). Every page changed after its copy in the full was
  in the next quiet diff (5,205/5,205 and 4,739/4,739), identical to a COPY_ONLY full taken then.
- **Exactness:** a loaded full imports with 0 differences on all 17 tables, LOBs and locators included
  (8,457 differing keys without recovery, 7,516 of them dirty) — the LOB approximation was not needed.
  The next quiet diff: 0. A second full applied incrementally: 0, identical to a fresh import.
- **A loaded differential is never dirty but not exact on its own** (71 and 151 stale keys, some
  rows twice from torn page splits): its log tail changes pages the diff does not carry, and a lake
  of rows (not pages) has no base image to replay them on. The next file heals it (0). Alternative
  mode: skip such transactions whole — no duplicates or torn LOBs, more stale keys (383 / 518).
- **Necessity shown:** without the pending rule a diff loses 2,282 committed keys for good; dropping
  an unresolvable LOB's fragment map loses 3 LOBs for good.
- **Corrections to the rules:**
  - the pending inference must **check allocation first**: a pending page the new file's GAM/PFS show
    as free is discarded (rolled-back transactions freed whole extents — 97 / 115 / 96 pages whose
    images still held rolled-back rows). What absence proves is "unchanged since the image", not
    "committed" (one page belonged to a rolled-back transaction whose rows splits had moved off);
  - **order files by CheckpointLSN, not FirstLSN** (a transaction open across two files gives both the
    same FirstLSN; E1's check refused a valid diff);
  - **ignore tail records older than the previous file's LastLSN on pages the file lacks** (already in
    the lake; rebuilding such a page from scratch lost rows).
- **Redo / undo:** replay EVERY transaction in the tail (committed, aborted, open, compensation records
  included), then undo the open ones; 8 record types plus the PFS allocation byte; LOB fragments are
  ordinary records (no LOB-tree logic). Three fixes took E3's 94–97 % to exact (modify size from the
  record; rewrite a LOB fragment's length after a modify; redo only data/LOB contexts). Undo is
  physical on heap and LOB pages, by key on clustered pages (rows moved by splits). Estimate:
  ~3,000 lines of C++, **9–12 weeks** for rowstore with LOBs incl. pending / stale bookkeeping and an
  oracle harness; ADR/RCSI, ROW/PAGE compression, DDL during backup extra; columnstore open.
- **Decision (owner, 2026-11-05): variant 1.** No garbage in restored data, and the next full or
  incremental file must bring every change made during a backup (nothing lost). A loaded
  differential's tail transactions whose pages the file lacks are **skipped whole by default** (no
  duplicate rows, no torn LOBs — an older but clean version); the affected pages are tracked as
  **stale** per page and the next file heals them (E6: 0 differences after it). Keeping raw record
  bytes per row to make a loaded differential exact on its own is not done (it roughly doubles
  storage).

**For the lake model (§6.2):** a no-replay import reflects CheckpointLSN, not LastLSN, so the
watermark must not be LastLSN — E1's W = min(FirstLSN, CheckpointLSN) is the right one. Pages stale
or dirty at import are re-processed by the next file (their LSN moves past W when they change, and a
rolled-back transaction's compensation rewrites its pages), so the lake heals along the chain.

## 5. Row identity without primary keys (owner's question)

Many tables have no PK, so incremental import cannot rely on keys. SQL Server already has a
physical row locator, confirmed on the samples:

| table shape | locator | stable across |
|---|---|---|
| heap | RID = file:page:slot of the ORIGINAL location (a forwarded record carries a back pointer to it; `%%physloc%%` agrees) | updates, growth, forwarding. Not a rebuild or shrink |
| clustered rowstore, unique or not, PK or not | clustering key + **uniquifier** (hidden column, rscolid 0; 0 for the first row of a key, then 1, 2…) | updates of non-key columns, page splits, index rebuilds |
| clustered columnstore | (rowgroup, tuple) | only within a rowgroup — the tuple mover renumbers; handle per rowgroup |

The reader exposes it as **virtual columns** of the scan (as mssql exposes `rowid` today):
`mssql_rid` (BLOB or STRUCT) and `mssql_page` (file:page where the row lives now). The import
materialises both into the lake table as `_mssql_rid`, `_mssql_page`.

## 6. Architecture

### 6.1 The reader — inside the mssql extension (owner, 2026-11-05)

**Read-only, always** (owner, 2026-11-05): the catalog is attached READ_ONLY whatever the ATTACH
options say, so DuckDB's own read-only check refuses CREATE / ALTER / DROP / INSERT / UPDATE /
DELETE / MERGE / COPY into it, and the catalog implements no write path at all (no DML planning,
no DDL, no transactions beyond a trivial read-only one). A backup is never modified; writing goes to
the lake (mssql-ducklake) or a regular `TYPE mssql` catalog.

A **new catalog type in the mssql extension**, `ATTACH 'x.bak' AS b (TYPE mssql_backup)` (class
`MSSQLBackupCatalog`; working names), next to `TYPE mssql`, plus table functions for reading full and
differential backups that mssql-ducklake builds its imports on — the same way it builds on
`mssql_exec` / `mssql_scan` today.

Why inside rather than a separate extension: the decoders, the SQL Server → DuckDB type map, the
collation / code-page tables and the spatial handling are used directly from the same binary — no
source sharing, no drift between two type mappings; one install; mssql-ducklake depends on one
extension. Accepted costs: the mssql binary grows by the reader (the XPRESS Huffman decoder is
small; zstd is already bundled with DuckDB), and the reader follows mssql's release cadence. The
catalog must stay clear of everything the live catalog does through a connection: the ~10 sites
that test `GetCatalogType()=="mssql"` keep doing so, and `mssql_backup` is a different type with its
own trivial transaction manager, no pool, no remote pushdown, no MSSQLOptimizer rewrites.

Code layout (proposal): `src/backup/` — container (MTF, MSSQLBAK, codecs), page store, catalog reader
(base tables), row engine, on-disk type decoders; `src/catalog/backup/` — `MSSQLBackupCatalog`, schema and
table entries, the scan; namespace `duckdb::mssql::backup` (no prefix inside it, per the naming rule).

Layers (bottom up):

1. **ByteSource — DuckDB's file system only** (owner, 2026-11-05): files are opened through
   `FileSystem::GetFileSystem(context).OpenFile(path, READ, opener)` with the ClientContext as the
   file opener, so `s3://`, `gs://`, `r2://`, `https://` go through **httpfs** (and `az://` /
   `abfss://` through the azure extension) and **DuckDB secrets apply** — the same `CREATE SECRET
   (TYPE s3 …)` a user already has for Parquet. No SDK, no curl, no own credential handling; the
   extension autoloads httpfs for a remote scheme like any reader. Access pattern: uncompressed
   and ZSTD backups are read with ranged reads (httpfs range requests); MS_XPRESS needs one
   sequential pass (its match window spans blocks) that records decompression checkpoints in the
   opened-file cache, after which reads are ranged too. Decryption keys for an ENCRYPTED backup are
   a separate secret type of ours (e.g. `TYPE mssql_backup_key` holding the certificate's private key), never a storage secret. It is
   **scoped by path** like any DuckDB secret (`CREATE SECRET (TYPE mssql_backup_key, …, SCOPE
   's3://backups/sales/')`). **Selection (owner, 2026-11-05): scope first, thumbprint second.**
   1. Candidates = the `mssql_backup_key` secrets whose SCOPE matches the backup's path (a secret
      without SCOPE matches every path, as DuckDB scopes do); the longest matching scope wins.
   2. Only when several candidates share that longest scope does the thumbprint decide: the one
      whose certificate thumbprint (SHA-1 of its DER) equals the `EncryptorThumbprint` stored in
      plaintext in the backup header (or the boot page's DEK record, for TDE).
   3. The chosen secret's thumbprint must equal the backup's; otherwise fail by name: the path,
      the backup's thumbprint, how many secrets matched the path. Never fall through to a
      secret outside the matching scopes.
   DuckDB's `LookupSecret` returns only the single longest match, so step 2 iterates the type's
   secrets and applies the scope match itself.
2. **Container** — MTF walker with checksum validation; `MSSQLBAK` decompressor (XPRESS Huffman,
   ZSTD; checkpoints for random access); later decryption.
3. **PageStore** — page id → bytes, built in one pass when a file is first opened (then cached);
   diff overlay; skips pads and
   unallocated pages (GAM/PFS).
4. **Catalog reader** — boot page → base tables → schemas, tables, columns, partitions, allocation
   units, indexes, view text; collation → code page.
5. **Row engine** — B-tree / IAM traversal, FixedVar, CD rows, page dictionaries, sparse, LOB /
   overflow (lazy), ghost skip, forwarding.
6. **Type decoders** — the existing codec where bytes match TDS; own decoders for datetime/money/bit,
   SSCLRT → WKB, BINXML → text, vector → FLOAT[n], sql_variant → VARIANT.
7. **DuckDB surface — file functions first (owner, 2026-11-05).** Import and update work on the
   FILE, with no ATTACH; ATTACH is a convenience for browsing a backup, built on the same machinery.
   - `mssql_backup_info('file')` — HEADERONLY / FILELISTONLY equivalents: backup type, GUIDs
     (`BackupSetGUID`, `DifferentialBaseGUID`, family), LSNs, dates, database, compression, fuzzy
     flags.
   - `mssql_backup_tables('file')` — schemas, tables, columns (SQL Server and DuckDB types), allocation
     units, row counts; `mssql_backup_views('file')` — view definitions as T-SQL text;
     `mssql_backup_catalog('file', base_table)` — the raw base tables (sysschobjs, syscolpars, …),
     what an importer mirrors into its `__mssql_sys`.
   - `mssql_backup_scan('file', 'schema', 'table' [, since_lsn := …])` — the rows of one table, with the
     virtual columns `_mssql_rid` and `_mssql_page`; projection and filter pushdown (DuckDB 2.0: the
     scan applies every filter it accepts; filter columns decoded first), parallel over IAM extents.
     With `since_lsn`, only the rows on pages changed after that LSN — the incremental view of a
     file. The bind reads the table's columns from the file's catalog.
   - `mssql_backup_changed_pages('file' [, schema, table], since_lsn := …)` — page id, allocation unit,
     table, page LSN, allocated or freed: what changed after `since_lsn`.
   - **Opened-file cache.** An import calls these functions once per table on the same file, and a
     cold open costs a pass over the whole file (decompression, the page index, the catalog) — from S3
     too. The opened backup (page index, decompression checkpoints, decoded catalog) is kept in
     DuckDB's per-instance object cache, keyed by path plus size, last-modified / ETag and
     `BackupSetGUID`, so the second call on a file is instant; evicted under memory pressure, never
     trusted across a changed file.
   - Browsing: `ATTACH 'full.bak' AS b (TYPE mssql_backup [, diff := ['d1.bak', …]])` — the same opened
     backup as a read-only catalog: `SELECT … FROM b.dbo.orders`.
   - Decryption parameters only through a SECRET (generated SQL and logs would carry anything
     passed inline).

### 6.2 The import (mssql-ducklake)

- **One snapshot per file.** DuckLake makes one snapshot per committed transaction (verified:
  `ducklake_transaction.cpp`; `ducklake_snapshots.test` shows CREATE SCHEMA + CREATE TABLE + INSERT
  in one transaction = one snapshot). A table function cannot run statements, so the import is a
  **PRAGMA that expands into SQL**, like DuckDB's own `copy_database`; an expanded PRAGMA runs as one
  query in one transaction: COPY SCHEMA → COPY DATA (one UNION ALL plan over all tables) →
  `ducklake_set_commit_message(..., extra_info := json)` → a provenance row. Usage:
  `PRAGMA ducklake_import_mssql_backup('lake', 'full.bak')` — no ATTACH: the generated SQL calls
  mssql's file functions (§6.1) — `mssql_backup_info`, `mssql_backup_tables`, `mssql_backup_scan`,
  `mssql_backup_changed_pages` — so the import logic lives in mssql-ducklake and the format logic in
  mssql, the split mssql-ducklake already has with `mssql_exec` / `mssql_scan`. The PRAGMA resolves
  `since_lsn` from the lake's provenance table when it expands. Sketch of a diff import:
  ```sql
  PRAGMA ducklake_import_mssql_diff_backup('lake', 's3://bak/sales_diff_2026-11-05.bak');
  -- expands, per changed table, into one transaction (one snapshot):
  --   DELETE FROM lake.s.t WHERE _mssql_page IN
  --       (SELECT page FROM mssql_backup_changed_pages('s3://…', 's', 't', since_lsn := '<last LSN>'));
  --   INSERT INTO lake.s.t
  --       SELECT *, _mssql_rid, _mssql_page
  --       FROM mssql_backup_scan('s3://…', 's', 't', since_lsn := '<last LSN>');   -- changed pages only
  --   + catalog DDL, provenance, commit message
  ```
- **Owner's direction for the import (2026-11-05):**
  - **Parquet first, not MERGE/INSERT.** The INSERT/MERGE path pays DuckLake's metadata updates and
    snapshot-id allocation statement by statement. Instead, parallel workers write Parquet files —
    data AND delete files — and ONE metadata transaction registers everything: atomic, fast, one
    snapshot per backup file. Model: duckdb-iceberg's Iceberg → DuckLake function
    (`src/function/ducklake/*`), which writes DuckLake metadata for existing files. Research:
    `research-ducklake-import.md` (in progress).
  - **Parallel across tables** (and within a table, over extents).
  - **Positional deletes from a page map**, with no data scan: the reader emits rows page by page, so
    a page's rows are contiguous in the Parquet it writes; the lake keeps page → (data file, first
    row, row count), and "delete the rows of page P" becomes positional delete ranges written as
    DuckLake delete files directly. Physical read order gives key order and page contiguity at once
    (clustered index in leaf-chain order; heap in IAM order; columnstore by rowgroup).
  - **Schema evolution** must be handled inside the per-file commit: tables added, dropped, renamed,
    transferred between schemas; columns added, dropped, renamed, retyped — mapped to DuckLake schema
    evolution with stable field ids, or to a table rewrite where DuckLake cannot express the change.
  - **Partitioning** is a first-class import setting: per-table partition specs as an import
    parameter for the full import, and derived defaults from SQL Server's own partitioning (partition
    function boundaries → identity / year / month / day transforms), the clustered index key (sort
    order) and columnstore (ordered CCI, rowgroup min/max). A SQL Server partition is its own rowset
    with its own allocation units, so one SQL Server partition maps naturally to one DuckLake
    partition and one parallel writer. Partition maintenance between backups — `SWITCH` in/out (a
    rowset changes owner, no page rewrite), `SPLIT` / `MERGE RANGE`, `TRUNCATE … WITH (PARTITIONS)` —
    must be recognised in the catalog diff and applied in the same commit (a SWITCH possibly as a
    metadata-only move of data files, if DuckLake allows it). *(Refined by the owner's principle
    below: SQL Server partitions are work units only, and a SWITCH is a lake-side copy.)*
- **The import pipeline (research-ducklake-import.md, 2026-11-05) — recommended:**
  - **Write first, commit once.** Parallel workers (threads, other processes, other machines — no lake
    transaction) write the data files plus a per-file record of which rows each page occupies; then
    ONE short transaction does DDL, file registration, positional deletes, the `__mssql_sys` mirrors,
    the commit message and provenance — the file's one snapshot. Not one long INSERT transaction: the
    lake transaction would last the whole import, a rollback deletes every file written so far, and
    DuckLake's parallel copy appends row groups to shared files, so a page's rows have no known place.
  - **Register through DuckLake's own transaction, not its metadata rows.** mssql-ducklake compiles
    DuckLake in, so it can call `AppendFiles` / `AddDeletes` / `DropFile` / the delete-file writer
    directly; DuckLake still assigns ids and row ids, merges stats and checks conflicts, and
    mssql-ducklake's commit batch carries it to SQL Server. v1 can be plain SQL
    (`ducklake_add_data_files` + a native DELETE). Writing metadata rows directly like
    iceberg_to_ducklake is rejected: that function takes format 0.4 and an empty catalog only, leaves
    `row_id_start` NULL, skips conflict checks, and would mean hand-written T-SQL per DuckLake bump.
  - **`ducklake_add_data_files`:** maps columns by name, reads stats from footers, appends to the
    caller's transaction (several tables + DDL = one snapshot), cannot register delete files, is not
    rolled back on failure (good for retries) and owned by DuckLake after commit (compaction / cleanup
    apply; no "external" flag).
  - **Deletes from the page map** (`__mssql_sys.page_map`: page → file, first position, count): a
    diff's changed / freed / dead pages become position sets per file, merged with existing deletes
    and written by DuckLake's delete writer; a file with every row gone is dropped. Cost ∝ changed
    rows. The native `DELETE … WHERE _mssql_page IN (…)` is the oracle and the v1 fallback. A
    columnstore delete bitmap maps straight to positions. **Catch: compaction** rewrites files and
    moves positions (`auto_compact` defaults to true): the importer turns it off and owns compaction,
    rebuilding the page map entries it moves.
  - **Commit cost:** imports take mssql-ducklake's client commit loop (the server-side apply covers
    add-files-only commits); ~80k metadata rows for 2,000 files × 40 columns — seconds, not measured.
  - **Schema evolution:** without rewrite, field ids kept — new / dropped tables, table rename, ADD
    COLUMN (NOT NULL DEFAULT k: old files read k as the initial default), DROP COLUMN (after resetting
    a partition / sort spec on it), column rename, int→bigint and decimal-precision widening, varchar
    widening (no-op: lake VARCHAR has no length), DROP NOT NULL. Rewrite inside the same commit —
    `ALTER SCHEMA … TRANSFER` (DuckDB has no SET SCHEMA), scale changes and other non-widening type
    changes, PK / clustered-index / partitioning rebuilds (SQL Server rewrites every page anyway);
    non-persisted computed columns are kept as text in the mirror (DuckLake refuses them).
  - **Partitioning:** DuckLake has identity, bucket, year/month/day/hour and (format 1.1)
    epoch_year/month/day/hour — no truncate; `SET PARTITIONED BY` applies to new files only. ~~Files are
    always split per SQL Server partition;~~ a DuckLake transform is declared only when the RANGE
    boundaries align (finest transform with one value per partition: monthly → epoch_month,
    quarterly → epoch_year), else no spec and min/max stats prune. Clustered key / ordered CCI
    `ORDER (col)` → sort order, not partition. Avoid `bucket` (scatters a page's rows). Per-table
    specs as an import parameter override the derived default. (Files follow DuckLake's layout: see
    the owner's principle below.)
  - **Partition maintenance:** SPLIT / MERGE / TRUNCATE PARTITIONS move or free pages — handled by the
    general delete-then-insert by page. SWITCH cannot be a metadata-only file move (DuckLake has no
    move, and cleanup deletes files by path without reference counts): ~~object-store copy +
    re-registration, or~~ INSERT … SELECT from the lake (a lake-side copy plus positional deletes:
    the owner's choice, below).
- **Principle (owner, 2026-11-05): DuckLake's rules govern placement; SQL Server's do not.** Once data
  has left SQL Server, its physical layout is DuckLake's business. SQL Server's partitioning and
  clustered key only PROPOSE the initial spec / sort order when a table is created. Partition values
  are computed with DuckLake's own `DuckLakePartitionUtils::ApplyPartitionTransform` bound in the
  importing session, like any DuckLake writer (the session TimeZone for TIMESTAMPTZ, DuckLake's
  TIMESTAMP_NS rounding) — no forced UTC. Files are laid out by DuckLake partition values and
  settings, not per SQL Server partition (SQL Server partitions may still be work units for parallel
  writers). SWITCH is a lake-side copy of the rows (resolved by E7's AU / rowset rule) plus positional
  deletes — not a file move; the page map still records the AU / rowset per page, because that rule
  needs it. Sort order and file sizes follow DuckLake's settings; SQL Server-derived defaults (the
  clustered key as the sort order) are proposals at creation only. This closes E7's datetimeoffset
  question ([decisions.md](../decisions.md) DQ-10). Statements below that say otherwise are superseded by this one.
- **Partitioning ownership (owner, 2026-11-05).** The importer sets a table's partitioning and sort
  order ONLY when it creates the lake table — at the first full import, or when a table first
  appears in a later file — mirroring SQL Server (its partition scheme → partitions as above; the
  clustered key / ordered CCI → sort order), unless the import call names a spec for that table.
  Incremental imports NEVER change partitioning or sort order, even when SQL Server repartitions
  (the rewritten rows land under the lake's current spec). **Manual changes made with DuckLake take
  priority and are never reverted**: the importer records in `__mssql_backup.tables` what it set at
  creation and does not touch the spec afterwards; a table dropped and re-created in SQL Server under
  the same name keeps the lake table's current spec. A page split across partition files simply has
  several page-map entries.
- **SQL Server partitioning in a backup — VERIFIED (E7, `experiments/E7-partitioning.md`).**
  - Where it lives: function = `sysclsobjs` class 30 (`intprop` = fanout, `status & 1` = RIGHT);
    parameter type = `systypedsubobjs` class 30; boundaries = `sysobjvalues` valclass 30 as
    sql_variant (positional ids — SPLIT / MERGE renumber); scheme → function = `syssingleobjrefs`
    class 31; scheme → filegroups = class 30 (destination fanout+1 = NEXT USED); index → scheme =
    class 7 (no row = PRIMARY; `sysidxstats.dataspace` is always 0); partition column =
    `sysiscols.tinyprop2`; partitions = `sysrowsets.numpart` (+ compression, `rcrows` exact);
    `sysallocunits.fgid` = filegroup. Decoder matched the catalog views in 16 states, 0 mismatches.
  - Transform rule (finest of identity / epoch_day / epoch_month / epoch_year with one value per inner
    partition) verified on every row. **DuckDB 2.0 caveats:** TIMESTAMP_NS is rounded to µs before
    `date_diff` / `month()` / `CAST AS DATE`, so a `datetime2(7)` value in a period's last 500 ns gets
    the next period — compute each row's value with DuckLake's own expression when splitting files;
    TIMESTAMPTZ transforms follow the session `TimeZone` (same instant = month 672 in UTC, 673 in
    Europe/Berlin) — ~~owner decision needed: derive nothing for datetimeoffset unless writers run in
    UTC?~~ decided: behave like a DuckLake writer, values in the importing session's TimeZone (above).
    (At this DuckLake pin only `bucket` values prune files, so no wrong results today.)
  - Maintenance in a differential: **SWITCH is catalog-only** (the two rowsets exchange
    `sysrowsets.idmajor` / `numpart`; no data, LOB or index page of the partition is in the diff);
    NEXT USED and empty SPLIT / MERGE are catalog-only; populated SPLIT (RIGHT) moves rows to a new
    rowset (pages in the diff); populated MERGE moves the right rowset's rows left and frees its AUs;
    TRUNCATE PARTITIONS frees pages (small: AU kept; big: AU type 0 / owner 0, new empty AU) with no
    pages in the diff; REBUILD PARTITION gives new rowsets / in-row AUs, LOB AU kept and re-owned.
  - **Rule for SWITCH (E7):** after updating the mirror, resolve every lake row's recorded AU (for
    columnstore, its rowset) through `sysallocunits.ownerid` → `sysrowsets`; if the (object, index)
    changed, move the row; if only `numpart` changed, do nothing. With it all 15 differentials replay
    exactly (plain SWITCH otherwise leaves rows in the old table). So the **page map records the AU /
    rowset, not the table**. ~~In DuckLake a SWITCH moves whole per-rowset files (copy, re-register
    under the target with the same delete positions, drop the sources).~~ Per the owner's decision
    above, the moved rows are copied lake-side into the target and deleted by position in the source.
    The dead-AU clause is needed too (a deferred TRUNCATE otherwise leaves 9,000 rows). A spec set at
    creation stays valid after SPLIT / MERGE, since each row's value is computed from the row itself.
- **Snapshot retention (owner, 2026-11-05).** Every import snapshot is tagged (snapshot `extra_info`
  and `__mssql_backup.imports`): backup type, `BackupSetGUID`, base full GUID, backup finish date,
  LSNs. A **generation** = the snapshots from one applied full backup up to the next applied full.
  Once a newer full is applied, the older generations are only time-travel history — the lake's
  current state plus the new chain is all future imports need — so they become expirable.
  - Policy: keep the last **m** generations (full backups) and, within them, the last **n** import
    snapshots (`keep_fulls := m, keep_snapshots := n`; either may be unlimited).
  - `ducklake_mssql_backup_expire(lake, keep_fulls := m, keep_snapshots := n, dry_run := true)`
    computes the snapshot ids outside the policy from the tags, calls DuckLake's
    `ducklake_expire_snapshots(lake, versions := […])`, then `ducklake_cleanup_old_files` (files
    referenced only by expired snapshots, after a grace period). It can also run automatically after
    each successful import when the policy is stored in `__mssql_backup.config`.
  - Only import snapshots are counted and expired; snapshots made by users or by compaction are left
    alone unless an option includes them. DuckLake never expires the current snapshot.
  - Expiring also frees the files our own compaction replaced (page-map rebuild, §6.2 deletes).
- **Provenance.** `BackupSetGUID`, FirstLSN/LastLSN, `DifferentialBaseGUID/LSN`, database name,
  file names — in the snapshot's `extra_info` and in a versioned side table
  `__mssql_backup.imports` (snapshots can expire). Each import validates the chain against it.
- **The operating model (owner, 2026-11-05): the lake IS the base state.** A full backup is
  imported once; every later file — differential, a periodic new full, later a log backup — is
  applied on top of the lake WITHOUT reading the full backup or any intermediate file. Each file is
  one snapshot, so DuckLake time travel (`AT (VERSION => n)`) is the history of the database at
  backup times; `extra_info` carries `BackupFinishDate` and the LSNs, and a helper maps a backup
  time to its snapshot.
- **Why a differential needs nothing but itself and the lake:**
  1. Diffs are cumulative since their base full: every page changed since the base is in the file,
     so intermediate diffs are never needed (diff 5 can follow diff 2).
  2. A page's header LSN says whether it changed since the last import: pages with LSN ≤ the LSN
     already applied are skipped; the rest are the changed set.
  3. Rows *before* come from the lake: `SELECT _mssql_rid FROM lake.t WHERE _mssql_page = P`
     (§5's locators); rows *after* are P's new image.
  4. **Freed space is NOT in the DCM** (E2, R1–R5): a diff copies the extents with DCM = 1 AND still
     GAM-allocated; DROP / TRUNCATE / deferred drop free extents without setting a DCM bit, and a freed
     extent is never copied. Only GAM, SGAM, DCM, BCM and the first PFS page are always in a diff; later
     PFS pages and IAM pages only when they changed. So **the lake keeps a page map** (file, page →
     allocation unit, LSN) for every page holding rows **plus the latest image of every allocation
     page** (file header, GAM/SGAM/PFS/DCM, IAMs — a few pages per 64 MB), and per file it deletes the
     rows of every page that is no longer live (R3: below the file's `Size`, extent GAM-allocated, PFS
     byte 0x40), that now names another allocation unit, or whose allocation unit is gone or dead
     (type 0 / owner 0 = deferred drop). The page header's allocation unit id is correct on every
     allocated page, after SHRINKFILE and REBUILD too (10,473 pages, 0 mismatches) — gated by R3 it is
     the practical page → table map, since the IAMs of unchanged allocation units are absent.
  5. The schema comes from the lake too: the SQL Server base tables (sysschobjs, syscolpars,
     sysrowsets, sysallocunits, sysrscols, sysidxstats, sysiscols, sysobjvalues) are imported into
     a hidden schema (`__mssql_sys`) with their own locators and updated by the same mechanism. A
     diff therefore knows every table's physical layout, maps a changed page to its table through
     the page header's allocation unit id, and turns catalog changes (new / dropped tables,
     ALTER ADD/DROP COLUMN — metadata-only in SQL Server, rows keep the old column count) into
     DuckLake DDL in the same transaction.
- **Applying a file**, in one transaction (one snapshot):
  1. Validate the chain: the file's `DifferentialBaseGUID` (for a diff) equals the base full the lake
     records; its LSNs are later than the last applied file's; out-of-order files are refused.
  2. Update `__mssql_sys` from the changed catalog pages; apply DDL.
  3. Per table: changed pages → delete rows that were on changed pages and are on no changed page
     now; delete + insert the rows of the new images (DuckLake MERGE allows one UPDATE/DELETE action,
     so DELETE + INSERT); rows on freed pages are deleted. Page splits and moves need nothing
     special: both pages changed, the row is found on its new page and `_mssql_page` follows it.
  4. Provenance row and commit message.
- **What still needs older bytes, and the answer:**
  - LOB / row-overflow values: copied from the lake ONLY when the in-row root is byte-identical (its
    update counter unchanged); otherwise rebuilt from the file's nodes plus the lake's fragment map
    (E1 rule 7, E2 R8). Reusing the lake's value by `_mssql_rid` alone is wrong after a `.WRITE`.
  - LOB values (E2, R8): an in-row LOB root carries an update counter, so a byte-identical root = an
    unchanged value, copied from the lake. A whole-value UPDATE rewrites the existing fragments in place
    (same pages, newer LSN), all in the diff. A `.WRITE` partial update leaves most fragments out of
    the diff (2 of 749 rewritten for a 6 MB value) — **applied exactly when the lake keeps a per-value
    fragment map** (`_mssql_lob_map`: fragment → page, offset, length) next to the value: all six E2
    cases rebuilt byte-exact. Without the map, refuse by name.
  - REBUILD (heap or clustered): a new rowset and new IN_ROW / ROW_OVERFLOW allocation units — the
    table is replaced from the diff — but **the LOB_DATA allocation unit is kept and its pages are not
    rewritten**, so LOB values come from the lake matched by in-row root bytes (identical after the
    rebuild), not by RID. Heap RIDs all change on REBUILD and on SHRINK (rows moved one by one,
    forwarding collapsed); a clustered index moves whole pages and keeps its slots. TRUNCATE: a small
    one keeps its allocation unit id (its pages fail R3), a big (deferred) one gets a new allocation
    unit under the same rowset. ALTER COLUMN int → bigint is size-of-data (whole table in the diff);
    ADD COLUMN (… NOT NULL DEFAULT) and varchar widening are metadata-only — a missing column's value
    is the `sysseobjvalues` default (so the mirror includes `sysseobjvalues`); DROP COLUMN keeps the
    bytes (skip `sysrscols` status 0x02).
  - Columnstore: new rowgroups are new segment LOBs (in the file); deletes are in the delete bitmap
    (in the file, as rowstore pages); the tuple mover compressing a delta rowgroup shows up as the
    delta's pages freed (rows deleted) plus a new rowgroup (rows inserted).
- **A periodic new full is applied incrementally too:** only its pages with LSN above the last
  applied one are processed, and rows on pages it no longer allocates are deleted — a small
  snapshot, after which it is the base for the diffs that follow. Rule: a full that SQL Server made
  a new differential base (not COPY_ONLY) must be applied before its diffs, or the changes between
  the last import and that full would be missing; the `DifferentialBaseGUID` check enforces it.
- **Prototype: DONE (E1, 2026-11-05) — the model holds.** 18 applied files and 3 fresh imports over
  5 chains (diffs, a diff skipping its predecessor, two fulls applied incrementally, `.WRITE` partial
  LOB updates up to 630 KB, ghost records, a no-change diff) — every state equals the server row for
  row (LOBs by SHA-256); lakes that reached one backup by different routes are byte-identical
  (`__mssql_sys` and locators included); heap `_mssql_rid` = `%%physloc%%` incl. forwarded rows;
  uniquifiers = DBCC PAGE. 0.26–0.5 s per applied file in Python. Only 3–68 % of a diff's pages
  actually changed (it carries whole extents). Report: `experiments/E1-lake-as-base.md`; replay with
  `experiments/E1/run_all.sh map` (23 s, no server). **The rules it established:**
  1. Chain check from the file alone: `BackupSetGUID` (SSET+100); FirstLSN / LastLSN / CheckpointLSN /
     DatabaseBackupLSN and DifferentialBaseLSN / GUID in the trailer SCIN record; SSET attributes 0x4
     full, 0x6 copy-only full, 0x8 differential — all equal RESTORE HEADERONLY on 7 files. Wrong base
     and out-of-order files are refused (forcing D3 onto a lake that skipped F1 corrupted 3 tables).
  2. Changed pages: page LSN > W, W = min(FirstLSN, CheckpointLSN) of the last applied file (not its
     LastLSN). Re-processing a page is idempotent.
  3. Freed pages come only from the PFS (allocated bit 0x40), never from the DCM (with E2's R1–R5); an
     absent PFS page means nothing in its interval was freed since the base.
  4. Rows only from PFS-allocated data pages whose header names a table's in-row allocation unit; skip
     ghost and stub record types (2, 5, 6, 7) and freed slots.
  5. **Delete by page in EVERY table, then insert** — pages move between tables (a page went from a
     truncated table to another; deleting only from the new owner left a row behind). One
     transaction per file.
  6. Catalog first: update `__mssql_sys` (it needs `sysseobjvalues` — ADD COLUMN defaults keyed by
     hbcolid — and `sysclsobjs` — schema names), diff it against the registry by object id and column
     id; key order is `ordkey` (+36).
  7. **LOBs: store the in-row pointer and a fragment map** (per node: RID, offset, length, depth) with
     the value; rebuild a LOB not wholly in the file from the file's nodes plus the lake's old bytes
     looked up by node RID (exact, because a page absent from a diff is unchanged since the base);
     refuse by name if a node is in neither. "Reuse the lake's value by `_mssql_rid`" is WRONG after a
     `.WRITE` (10 of 21 steps failed with it): every `.WRITE` changes the root's update counter, but
     only part of the value is in the file. This replaces the `base := 'full.bak'` fallback.
  8. Store LOB bytes, not decoded text: the rebuild re-encodes old values.
  Also found: the forwarded back pointer is the LAST variable column of a forwarded record (trailing
  NULL variable columns are omitted) — a bug in `tools/pages.py`, fixed in `experiments/E1/pages_ext.py`.
  Not covered by E1: multiple data files (E2 did), partitioning, columnstore (E5), PAGE compression,
  compressed backups (proven separately), row-overflow `varchar(n)`, real DuckLake (a plain DuckDB file).
- **Lake specifics:** DuckLake rejects alias and collated string types, so the reader reports plain
  VARCHAR; with a SQL Server metadata catalog, set data inlining to 0 for imports (inlined inserts
  run after DDL, table by table); VARIANT is not inlined.
- **Views.** DuckLake stores views as DuckDB SQL only. Keep T-SQL definitions as text in
  `__mssql_backup.views`; translate a simple SELECT subset into DuckDB views later (opt-in).

## 7. Prior art and licences

| project | licence | use |
|---|---|---|
| BusinessCentral.DbReader (C#) — full .bak, ROW/PAGE compression, LOB, overflow; every fact sourced in PROVENANCE.md | MIT | adapt with attribution |
| pyxca (C), rust-lzxpress — Xpress codecs | MIT | adapt with attribution |
| Internals Viewer (C#) — compressed backups, columnstore | GPL-3.0 | facts only, clean room |
| OrcaMDF (C#), mdf-rs/mtf-rs (Rust), SQLServerForensics (Go) | GPL / AGPL | facts only |
| baktoany, MTF_Reader, mssql-backup-checker | no licence | read-only reference |

We stay MIT: clean-room implementation from documented specs (MTF, MS-XCA, MS-SSCLRT, MS-BINXML),
our own measurements, and MIT sources.

## 8. Phases

The research-time phases, with the draft spec that now carries each (2026-11-05). The specs re-cost them
with the hardening, fuzz harnesses, CI oracle lanes and docs of §9; the per-spec breakdown and the
differences are in the [series overview](../series.md) (phase map) and mssql-ducklake's [import series](https://github.com/hugr-lab/mssql-ducklake/blob/main/specs/021-import-full-backup/series.md).

| phase | scope | research estimate | risk | spec (draft) and its estimate |
|---|---|---|---|---|
| 0 | MTF + MSSQLBAK (XPRESS, ZSTD) + PageStore + catalog + FixedVar rows + LOB/overflow + common types; file functions (`mssql_backup_info`, `mssql_backup_tables`, `mssql_backup_scan`) with the opened-file cache, ATTACH for browsing; fuzzy flags | 6–8 wk | low–med | mssql 085, 12.5–14 wk (also takes phase 1's overlay, locators, parallel scan, and E7's partition model) |
| 1 | ROW/PAGE compression, sparse, sql_variant, xml, spatial (SSCLRT→WKB), vector; differential overlay; virtual row locators; parallel scan | 5–6 wk | med | mssql 086, 7–8 wk + the `json` spike E9 |
| 1b | encryption and TDE with a `mssql_backup_key` secret (certificate + PVK/PFX + password), selected by scope first, then by the thumbprint stored in plaintext in the backup / boot page (owner, §6.1) — format established by E4 | 1–2 wk | low–med | mssql 087, 3–3.5 wk + the TDE log-tail spike E10 |
| 1c | tail recovery (owner: no dirt in the lake): redo of every tail transaction + undo of open ones, pending pages with allocation check, stale-page tracking, CheckpointLSN ordering — established by E3 + E6 | 9–12 wk | medium | mssql 090, 10–12 wk (+ guard extensions) |
| 2 | mssql-ducklake: full import (one snapshot), provenance, diff import by locators, table-replace fallbacks, views as text | 3–4 wk | med | reader side: mssql 089, 8 wk; importer: mssql-ducklake 021–023, 20–26 wk |
| 3 | columnstore (required): segments, dictionaries, RLE/bit-packing, delta stores, delete bitmaps, archive — format established by E5 | 5–8 wk | medium | mssql 088, 6–9 wk + E11 |
| 4 | log backups: redo of row operations → consistent full backups and CDC-style incremental import; sector parity | 3–6 mo | high | not in the series |
| 5 | QAT_DEFLATE, 2025 `json` — research first | open | high | `json`: 086's E9; QAT_DEFLATE refused by name |

Totals of the drafts: mssql 085–090 ≈ 255 working days (≈ 51 weeks for one developer, ≈ 34 with two);
mssql-ducklake 021–023 ≈ 20–26 weeks, gated on the reader. The order of 088 against 090 and the other
open questions are in [decisions.md](../decisions.md).

## 9. Hardening and tests

- A .bak is untrusted input: every length, offset and pointer is bounded; the parsers get the same
  fuzzing treatment as the TDS parser (cflite corpora per layer: container, page, record, LOB).
- Oracle tests: generate samples on SQL Server 2016 / 2019 / 2022 / 2025 in docker (all types,
  compression on/off, ROW/PAGE, LOB, sparse, columnstore, diff chains, workloads during backup);
  RESTORE + SELECT is the expected answer; byte-compare.
- Measure the fuzzy-backup error: run a write workload during BACKUP and compare reader vs restore;
  check the LSN flag catches every differing table.

## 10. Decisions for the owner

1. ~~Separate extension or inside mssql?~~ Decided (2026-11-05): inside mssql, a new catalog type
   (`mssql_backup`, working name) plus reading functions that mssql-ducklake builds on.
2. ~~Locator columns or a side table?~~ Decided (2026-11-05): service columns with a prefix
   (`_mssql_rid`, `_mssql_page`) in the lake tables. Their physical types are DQ-02.
3. Fuzzy policy default: settled for the lake by the no-dirt decision (§4): the importer refuses. The
   reader's default for browsing is DQ-13.
4. Columnstore: **required** (owner, 2026-11-05), decoded by E5; its order relative to tail recovery is
   DQ-03.
5. Minimum SQL Server version for the reader: DQ-04 (the drafts propose 2017).
6. ~~WASM?~~ Moot with the reader inside mssql (the extension needs sockets/TLS and is not built
   for WASM).
7. ~~Partition layout and datetimeoffset values?~~ Decided (2026-11-05): follow DuckLake's rules (§6.2),
   DQ-10.

Every other open question of the two spec series, deduplicated, ordered by impact and marked where it
blocks the start of implementation, is in [decisions.md](../decisions.md).

## 11. Unknowns to resolve by experiment

Page placement under load and with multiple data files; the compressed-container header fields;
the type-101 pad units; striped / appended backup sets; CHECKSUM, encryption and TDE backups;
non-default BLOCKSIZE / MAXTRANSFERSIZE; FILESTREAM and in-memory OLTP; catalog drift across
versions; log sector parity; the json and xml on-disk forms on controlled values.

For the lake-as-base model (§6.2) specifically: that the page header's allocation unit id is
reliable for every ALLOCATED page (including after a shrink — the reports say it goes stale only on
freed pages); that freeing pages and whole extents always sets the DCM bit, so a diff carries the
evidence of a delete; ALTER TABLE ADD/DROP COLUMN as seen on pages and in the base tables; a
`.WRITE` partial LOB update as seen in a diff; and the end-to-end prototype (import full, apply diff
alone, compare with RESTORE).

## 12. Experiment plan (started 2026-11-05)

Each experiment has its own database on the test server (`bk_e1` …), its own folder
`experiments/E<n>/` and a report `experiments/E<n>-<topic>.md`. Server access only through
`tools/sql.sh` / `tools/sqlh.sh`; files out with `docker cp` only.

| id | question | done when |
|---|---|---|
| E1 | **Lake as base**: does a differential apply correctly with only the lake + the diff file? Python prototype: import a full into a local DuckDB "lake" with `_mssql_rid` / `_mssql_page` and a mirror of the base tables; workloads (insert/update/delete, page splits, forwarded growth, whole-LOB update, uniquifier duplicates, ADD/DROP COLUMN, new / dropped / truncated table); diffs D1, D2 (cumulative), D2 applied straight after the full (skipping D1); a new non-COPY_ONLY full applied incrementally, then a diff on it | every table equals the server's state at each backup, row for row |
| E2 | **Allocation edges**: DCM evidence of freed pages and extents; page-header allocation unit id after SHRINKFILE; heap REBUILD (new allocation unit? RIDs); ADD/DROP/ALTER COLUMN on pages and in sysrscols; `.WRITE` partial LOB update in a diff; a second data file; striped backup (two DISK targets); CHECKSUM, non-default BLOCKSIZE / MAXTRANSFERSIZE | each behaviour observed and stated, with the rule a reader needs |
| E3 | **Fuzzy full backups**: writes running during BACKUP; the reader's output (no log replay) vs RESTORE + SELECT of the same file; does the "page LSN > CheckpointLSN" flag catch every differing table? | measured error and flag recall |
| E4 | **Encryption first look**: BACKUP WITH ENCRYPTION (AES_256, server certificate) and a TDE database; the certificate exported WITH PRIVATE KEY (PVK and PFX); where the thumbprint and the wrapped key sit; time-boxed attempt to unwrap the key and decrypt blocks / pages | layout known, decrypted or the blocker named |
| E5 | **Columnstore decoding** (required, owner 2026-11-05): every segment encoding (value / dictionary, base_id and magnitude, RLE + bit-packing, string dictionaries primary / secondary, NULLs), per data type; delta stores; delete bitmap / deleted buffer; COLUMNSTORE_ARCHIVE; nonclustered columnstore; ordered CCI (2022+). Clean room: own experiments, papers and blogs; every fact's source recorded (as DbReader's PROVENANCE.md) | every column of the test tables decoded from the backup equals the server's SELECT |
| E6 | **Exact healing under load** (owner's requirement): a full taken under load (committed work after CheckpointLSN, an open transaction that later commits without touching its pages again, one that later rolls back, a LOB update during the backup) → import with redo + undo + pending pages → a later differential (quiet, then under load) → is every table EXACT vs the server? Is the DCM reset at the start or the end of a full (are pages changed during the full in the next diff)? | every table equals the server after the next file; the DCM timing stated |
| E7 ✓ | **SQL Server partitioning in a backup** (done): which base tables hold partition functions, schemes and boundary values (expected sysclsobjs, sysobjvalues, syssingleobjrefs / sysmultiobjrefs, sysidxstats.dataspace, sysiscols; only `sysrowsets.numpart` confirmed); what SWITCH (in/out), SPLIT / MERGE RANGE and TRUNCATE … PARTITIONS leave in a differential (SWITCH: catalog pages only?) | partition specs derived from a backup alone; each maintenance operation's diff evidence stated |
| E8 | ~~Import commit cost~~ — not run (owner, 2026-11-05): the architectural estimate from the code is enough (research-ducklake-import.md) | — |
| E9 (planned) | **The 2025 `json` binary**: a controlled corpus (every JSON type, nesting to the limit, Unicode and surrogate pairs, escapes, numbers at every edge, off-row documents) decoded clean-room and compared with `CAST(j AS nvarchar(max))`; timebox 1–2 wk (spec 086) | every value equals the server's text, or the refusal stays with the reason recorded |
| E10 (planned) | **The log tail of a TDE backup**: is MQTL encrypted, where is each VLF's DEK record, what is the IV; decrypt the tail of a quiet and of a loaded TDE backup; timebox 3–5 d (specs 087, 090) | the tail decrypted and parsed, or the blocker named; until then a TDE backup's status is `unknown` and the importer refuses it |
| E11 (planned) | **Columnstore in differentials**: inserts, deletes, updates, the tuple mover, REORGANIZE, REBUILD, TRUNCATE, a partitioned CCI's SPLIT / MERGE (E7 covered only its SWITCH), and the tuple mover running during a full backup; timebox 3 d (specs 088, 089) | each operation's diff evidence stated, as E7 did for partitions |
| later | version matrix (2016/2017/2019/2022 containers) for catalog and columnstore drift and container differences; log-backup phase | needs a decision on priorities and on running more SQL Server containers |
