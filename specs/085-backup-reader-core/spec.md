# Spec 085: reading a backup file — the core

Status: **draft**, 2026-11-05. Not started. Part of the backup-reader series (see `../README.md`).
Design and every owner decision: `design/001-mssql-backup-reader/RESEARCH.md` (cited as R§n).
Evidence: `research-experiment.md` (X§n), `research-web.md` (W§n), `research-architecture.md`
(A§n), experiments E1–E7 (`experiments/E<n>-*.md`, cited as E<n>§m). The owner's decisions:
[decisions.md](decisions.md) (cited as DQ-nn; each one decided or accepted). DuckLake source is cited
as `DL/` = mssql-ducklake's `ducklake/src/` at `dcf71031`, DuckDB source as `DD/` = this repository's
`duckdb/` submodule at `d54cbbe3df1` (the pin).

Markers used below: **(2025)** = verified on SQL Server 2025 RTM-CU8 (17.0.4075.5, Linux) only, the one
build every experiment ran on; **(unverified)** = inferred, not observed. E7 (partitioning) finished
after the first draft; its results are integrated, and no point waits for it any more.

## Problem

A SQL Server backup file is today readable only by RESTORE on a SQL Server. The research (R§2–§3, X)
showed that the file alone is enough: the container, the pages, the system catalog and the rows of
every common type decode byte-exact with no server. This spec builds that core inside the mssql
extension:

- the container: MTF, the `MSSQLBAK` compressed container with MS_XPRESS and ZSTD, striped media
  sets, several data files, pads skipped;
- the page store with the liveness rule;
- the catalog reader;
- the row engine for uncompressed rowstore: FixedVar records, LOBs and row-overflow, forwarded
  records, ghosts, the uniquifier;
- the common types;
- the file functions with the opened-file cache, and a read-only ATTACH for browsing;
- I/O through DuckDB's file system, so httpfs and azure apply;
- `mssql_backup_info` with the header fields E1 located;
- the fuzzy status of a backup from its log tail (E3);
- a differential laid over its full backup, for browsing.

It returns the database **as of the backup's CheckpointLSN**, dirty reads included (E3§2), and says so
per table. Making a busy full backup exact (redo and undo of its own log tail) is spec 090.

## Scope

In: everything above.

Out, each refused by name until its spec lands:

| feature | where | refusal scope |
|---|---|---|
| ROW / PAGE compressed rowsets, sparse columns | 086 | the table |
| `sql_variant`, `xml`, `geometry`, `geography`, `hierarchyid`, other CLR types, `vector`, `json` (2025), DBCS code pages (932/936/949/950) | 086 | the column, when projected |
| encrypted backups, TDE databases | 087 | the file |
| clustered / nonclustered columnstore | 088 | the table (an NCCI is ignored: the scan reads the base rowset) |
| `since_lsn`, changed / freed pages, the export, lake-mode state | 089 | — |
| redo / undo of the log tail | 090 | — |
| QAT_DEFLATE (Windows, framing unknown, W§3) | none | the file |
| log backups (`.trn`) | phase 4 of R§8, not in this series | `mssql_backup_info` only; a scan is refused |
| FILESTREAM, memory-optimized tables (unverified: never produced in an experiment) | none | the table |

## Decisions

- **D1 — a separate catalog type inside mssql (owner, R§6.1).** `ATTACH 'x.bak' AS b (TYPE
  mssql_backup)` registers a second `StorageExtension` next to `TYPE mssql`
  (`src/mssql_storage.cpp:1232,1650`, A§1). The catalog is **always read-only** (owner, R§6.1): the
  attach forces `AccessMode::READ_ONLY`, so DuckDB's own check refuses every write, and the catalog
  implements no write path at all. It has its own trivial transaction manager, no pool, no remote
  pushdown, and no `MSSQLOptimizer` rewrites. Reason: the decoders, the type map and the collation
  tables come from the same binary with no drift; the ~10 sites that test
  `GetCatalogType() == "mssql"` (A§1) keep their meaning because `mssql_backup` is a different type.

- **D2 — file functions first, ATTACH built on them (owner, R§6.1 item 7).** Every function takes the
  backup's path; nothing needs an ATTACH. The catalog and the functions share one `OpenedBackup`
  object (D9). Reason: the importer (mssql-ducklake) generates SQL over files, as it does over
  `mssql_scan` today, and an ATTACH inside its PRAGMA expansion would add DETACH problems
  (A§2: a DETACH inside an expanded PRAGMA is refused).

- **D3 — I/O only through DuckDB's file system (owner, R§6.1 layer 1).** Files open through
  `CachingFileSystem::Get(context).OpenFile(path, FILE_FLAGS_READ)` (D3a), which wraps the
  client-context file system that carries the opener
  (`DD/src/include/duckdb/storage/external_file_cache/caching_file_system.hpp:152-159`), so `s3://`,
  `gs://`, `r2://`, `https://` (httpfs) and `az://`,
  `abfss://` (azure) work with the user's DuckDB secrets, and DuckDB autoloads the extension for the
  scheme (`virtual_file_system.cpp:428`). Reads are positional, so threads share one
  `shared_ptr<CachingFileHandle>`. No SDK, no own HTTP, no own credential handling.

- **D3a — reads are ranges, coalesced, fetched asynchronously and in parallel, as the Parquet reader
  does (owner, 2026-11-09; DQ-38).** On httpfs a positional read is an HTTP range request, so the
  layer above the parsers works in ranges, never in a stream:
  - **Ranges, coalesced.** A consumer asks for the byte ranges it needs (a unit's pages, a block, a
    header); adjacent and nearly adjacent ranges are merged into one request, as Parquet merges its
    read heads within a 16 KiB gap (`DD/extension/parquet/include/thrift_tools.hpp:61-79`,
    `AddReadHead` `:107-130`), and the merged range is split again by the reader at a size that suits
    the pins it can afford: one `CachingFileHandle::Read` pins its whole range. Below that, the
    caching handle cuts a remote read into cache blocks of at most
    `external_file_cache_remote_max_block_size` (2 MiB by default) and fetches them in parallel on the
    ASYNC pool (`DD/src/storage/external_file_cache/caching_file_system.cpp:378-418`). A local file
    reads the same way.
  - **Asynchronous.** The fetches run as `AsyncTask`s on DuckDB's I/O pool:
    `AsyncResult::FromTasks(…, TaskSchedulerType::ASYNC)` (`DD/src/include/duckdb/parallel/async_result.hpp`,
    the pool `async_threads` sizes), returned as a `BLOCKED` result from the table function, so the
    decode threads do not wait on the network. Any plain table function may do it: the table scan
    runs in TASK_EXECUTOR mode by default (`DD/src/parallel/async_result.cpp:284-294`) and schedules
    the tasks, or runs them with `ExecuteTasksSynchronously(context)` when it cannot block
    (`DD/src/execution/operator/scan/physical_table_scan.cpp:171-206`;
    `TableFunctionInput::HandleBlocked` runs them side by side, `DD/src/function/table_function.cpp:214-231`).
    Its limits: a `BLOCKED` result comes with an empty chunk (`physical_table_scan.cpp:150-155`), only a
    `function` may block (not an `in_out_function`), and the multi-file wrapper forces SYNCHRONOUS
    (`DD/src/common/multi_file/table_function_multi_file.cpp:376`), so 085's functions do not use it.
  - **Cached.** Reads go through DuckDB's `CachingFileSystem` / `ExternalFileCache`
    (`DD/src/include/duckdb/storage/external_file_cache/`), whose buffers are the buffer manager's,
    counted against `memory_limit` (D17). A **remote** file is cached when `enable_external_file_cache`
    is on (the default) and the file has validation metadata (ETag or last-modified); its blocks are
    evictable once unpinned, so an importer calling the functions once per table on one file, beside
    D9's opened-file cache, usually fetches a range once (an evicted block is fetched again). A
    **local** file is not cached by default (`cache_local_files` is off,
    `DD/src/storage/external_file_cache/external_file_cache.cpp:103-110`): its reads are counted
    buffers freed on release.
  - **The open pass in parallel.** Only the MTF headers are walked one after another (a few small
    reads). An uncompressed MSDA is one run of 8 KB units, each naming its own page (D4), so once its
    stream header gives its extent the page index is built from many ranges fetched at once. A
    compressed chain (`MSSQLBAK`) is a run of headers each giving the next one's offset: when the seek
    index after the chain (§ Rules, MSSQLBAK) lists the blocks' offsets, it is read first and the
    chain is fetched in parallel too (task 4 finds out); otherwise the chain is read ahead in large
    ranges and its blocks are decompressed on every thread (D10).
  - **LOB trees** are fetched one level at a time: a node's children are known only once it is read,
    so the ranges of one level are coalesced and fetched together.
  - The parsers stay DuckDB-free (D15): they take spans the I/O layer has fetched.

- **D4 — the page store is an index by page id, gated by liveness.** A page names itself
  (`m_pageId` @32, `m_fileId` @36, X§3), so the store is `(file_id, page_id) → location of its latest
  copy`. The later MSDA pass wins over the earlier (X§3, E3§2: reading only pass 1 doubled the error).
  A page's rows are read, and its header allocation-unit id is believed, **only if the page is live**
  (E2 R3): below its file's `Size`, its extent GAM-allocated, its PFS byte with 0x40. Full backups
  carry dead pages too (E2§1.3: 42 `BULK_OP_PREALLOC` pages in a full; 34 and 53 freed data pages with
  live-looking records after a shrink). The page → allocation unit map is the **page header**, not the
  IAM chains: the header AU was right on 10,473 of 10,473 allocated pages, after SHRINKFILE and REBUILD
  too (E2§2, R4), and stale only on pages the liveness rule already drops. This corrects R§3's "map
  through IAM chains". IAM pages remain an optional cross-check (`verify := true`).

- **D4a — what an opened backup keeps, and the budget: at most 0.4 MB of resident memory per GB of
  backup, 4 GB for 10 TB (owner, 2026-11-09).** The open pass may be slow (a backup is opened once per
  instance, D9); what must be fast is a bind and a read. So the opened backup keeps three tiers:
  - **Resident while the backup is open** (pinned; what every bind and every page read needs):
    the headers (families, sets, data files, `mssql_backup_info`'s fields); per pass a bitmap of the
    extents it carries with a rank directory, so a page's place in the stream is the rank of its
    extent among the pass's extents, found in O(1) (≈ 21 MB per pass and 2.6 MB of ranks for 10 TB);
    for compressed media one checkpoint per 64 blocks (physical offset, logical offset), the block
    itself found by walking the ≤ 63 headers inside the range the read fetches anyway (≈ 40 MB for
    10 TB, against ≈ 2.6 GB for an entry per block); the catalog model (schemas, tables, columns,
    types, a table's AUs, its partitioning: grows with objects, not data); the per-table fuzzy status;
    the directory of the AU index below. Typically 0.1–0.2 GB for 10 TB.
  - **Buffer-managed, spilled under pressure** (counted against `memory_limit`, D17): the AU index,
    `AU → runs (first extent, count)` of live pages, built at open by DuckDB's sort over the pass's
    `(AU, extent)` records and cut into segments; a scan pins its table's segments only. Liveness is
    applied once, at open: GAM, the PFS allocated bit (1 bit a page, ≈ 168 MB for 10 TB, read from the
    allocation pages while the pass runs) and `Size` decide which pages enter a run, a partly
    allocated or mixed extent (pages of several AUs, PFS 0x20) is listed by page; GAM and PFS are not
    kept after it. Typically tens of MB; up to ≈ 2 GB when tables interleave extent by extent. Also
    the raw base tables when `mssql_backup_catalog` asks for them.
  - **Not kept**: page bytes (fetched as ranges, D3a; a remote file's in `ExternalFileCache`), decoded
    pages, LOB values (the scan's working memory, bounded by `max_memory`), log records (only D11's
    flags stay), the open pass's intermediate records (freed once the index is built).
  - **The stream order is checked, not assumed.** A pass's extents must come in ascending (file, page)
    order for the rank to give their place; the open pass checks it, and a pass or a stretch of one
    that breaks it is recorded as explicit runs `(file, first extent, count, offset)`: a loss of
    compactness, never of correctness.
  - **A page's type, level and `m_lsn` are not indexed**: the scan reads them from the page it reads
    anyway (and skips the index pages there); D11's check (iv) reads the few pages the log tail names.
    Spec 089's changed set (`m_lsn > since_lsn` on every live page, 089 D3) is computed by 089's own
    pass over the page headers when `since_lsn` is given, not stored in this index.
  - **E12 (planned, before task 7)**: whether a full backup's first pass carries exactly the
    GAM-allocated extents, in order. If it does, the open pass reads the allocation pages and the
    MTF headers instead of every page (the open of a 10 TB backup without reading 10 TB); if not, the
    pass reads every page header, as above.
  - **A saved index is a later option**, not part of 085: written to a path the user names
    (`temp_directory` does not outlive the process), keyed by `BackupSetGUID`, the families' sizes and
    version tags.

- **D5 — the catalog from the backup alone, data-driven per version.** The boot page gives the
  database name, version, collation and the first `sysallocunits` page (X§4). Five base tables have
  hard-coded layouts (`sysallocunits`, `sysrowsets`, `sysschobjs`, `syscolpars`, `sysrscols`); every
  other base table is decoded generically through its own `syscolpars` / `sysrscols` rows (X§4: the
  22 columns of `syscscolsegments` came out that way; E7: every partitioning table came out that way
  too, 0 mismatches against the catalog views in 16 database states). The five layouts are a table
  keyed by `dbi_version`, not code branches; only 998 (2025) is verified, and the other versions' rows
  are filled by the CI version matrix (D14).

- **D6 — one type map, shared with `TYPE mssql`.** A column's DuckDB type is
  `MSSQLColumnInfo::MapSQLServerTypeToDuckDB` (`src/catalog/mssql_column_info.cpp:199`), with native
  types off: strings are plain `VARCHAR`. Reasons: a restored database attached as `TYPE mssql` and the
  backup then report the same types, which is what the oracle tests compare (§ Testing); DuckLake
  rejects alias and collated string types (A§1, `ducklake_types.cpp:158-163`). The reader needs a
  `system_type_id → name` table for it. The deliberate exceptions, where a backup can return what the
  wire cannot, are spec 086's (`sql_variant` → `VARIANT`, `vector` → `FLOAT[n]`); 085 has none.

- **D7 — values go through the codec layer's batch kernels.** The row engine copies each value's
  bytes into the codec's column-major staging (`codec/staging/column_staging.hpp`) and finalises one
  column at a time with the family's `DecodeChunkFromStaging` where the on-disk bytes equal TDS's
  (integers, float, decimal, uniqueidentifier, date, time, datetime2, datetimeoffset, nvarchar, UTF-8
  varchar; W§7, A§1). Own kernels, in the same shape, for the bytes that differ: `datetime` and
  `smalldatetime` (time first on disk), `money` / `smallmoney` (a plain little-endian integer) and `bit`
  (packed). No per-value type dispatch (the repository's rule: one batch call per column). The per-row
  work is the record walk, which is inherent.

  The code-page transcode of a `char` / `varchar` column under a non-UTF-8 collation is a vectorized
  kernel, one pass, one call per column per chunk, never per value: ASCII stored a block at a time,
  the rest by arithmetic on the page's run or by table, with kernels specialised to the frequent pages
  (spec 091 D5 / D5a, PR #527). The tables come from SQL Server itself: spec 091's generator reads
  every byte, and every DBCS lead / trail pair, in a scratch database whose default collation has the
  page (a `COLLATE` would convert the characters instead), and checks a second collation per page;
  an invalid sequence comes out as the server makes it. Not iconv, ICU or Python's codecs: they
  differ from the server in places, and are platform-dependent or heavy. The same
  generator writes the collation → code page table (`tools/code_pages/data/collations.psv`, § Rules,
  catalog). Spec 091 (SQL Server code pages, generated tables and vectorized kernels) builds it all,
  as the module `src/codec/code_page/`; 085 and 086 reuse it. 085 maps the on-disk CollationId onto
  091's table and reuses its single-byte tables (task 13).

- **D8 — service columns are virtual columns with a `_mssql_` prefix (owner, R§5, R§10.2).** The
  scan and the catalog's table entries expose:

  | column | type | meaning |
  |---|---|---|
  | `_mssql_rid` | `BLOB` | stable row identity. Heap: `0x00 ‖ u32 page ‖ u16 file ‖ u16 slot` of the row's **original** location (the forwarded record's back pointer when it moved); after the shape byte, equal to `%%physloc%%` (E1§3). Clustered rowstore: `0x01`, then per key column in `ordkey` order `u16 length ‖` its uncompressed storage bytes (`0xFFFF` for NULL), then `u32` uniquifier for a non-unique index (0 when absent). Columnstore: spec 088 |
  | `_mssql_page` | `VARCHAR` | the page the row lives on now (E1§2), as the uppercase hex of a shape-byte encoding, fixed width per shape: a page is `01` + 4 hex digits of file id + 8 of page id, 14 characters (file 1, page 0x1A2 = `010001000001A2`). A compressed columnstore row has no page: its rowgroup key is `02` + 16 hex digits of rowset id + 8 of rowgroup, 26 characters (spec 088 D3); a delta-store row has its delta page in the `01` form. Hex digit order is byte order, so within a shape string order is page order |
  | `_mssql_page_lsn` | `DECIMAL(25,0)` | that page's `m_lsn`, in `RESTORE HEADERONLY`'s numeric form; a scan column only (below) |

  The clustered encoding uses the uncompressed bytes so that a rebuild with another `DATA_COMPRESSION`
  keeps every identity (E1§3: key + uniquifier survived REBUILD for 3,188 of 3,188 rows). `_mssql_lob`
  comes with PR 3 in its plain form (DQ-05, task 20), so a phase-A lake has its LOB maps from the first
  import; 089 uses them for differentials. Every lake service column is an opaque value serialized and
  read only by this code, each starting with a shape (DQ-02): only the importer reads them, and a lake
  materialises them, so their format is versioned rather than typed. `_mssql_rid` and `_mssql_lob` are
  `BLOB`s. `_mssql_page` is hex `VARCHAR` so that DuckLake's file statistics prune its deletes (owner,
  2026-11-06): DuckLake builds no file filter for a `BLOB` column
  (`DL/storage/ducklake_metadata_manager.cpp:1764-1765`), and a 26-character value keeps its min/max
  under mssql-ducklake's `stats_length` (1024 by default, its spec 018). `_mssql_page_lsn` is a scan
  column only: no lake stores a page LSN, because which pages changed is decided by the file (the DCM and
  `m_lsn > since_lsn`, spec 089 D3; R§6.2 point 4). The lake deletes by `_mssql_page` with DuckLake's own
  `DELETE` (R§6.2).

- **D9 — the opened-file cache (owner, R§6.1).** An import calls the functions once per table on the
  same file, and a cold open costs a pass over the whole file. The opened backup is kept in DuckDB's
  per-instance `ObjectCache` (`storage/object_cache.hpp`), keyed by every family file's canonical path,
  size and version tag (`GetVersionTag`, the ETag, else `GetLastModifiedTime`), and re-checked against
  `BackupSetGUID` after opening. The entry reports `GetEstimatedCacheMemory`, so it is evictable under
  memory pressure; an attached catalog holds a `shared_ptr` and pins it. A file whose size or version
  tag changed is reopened, never trusted.

- **D10 — MS_XPRESS blocks decode one at a time.** R§3 and W§3 (from Internals Viewer) say the match
  window spans blocks, which would force a sequential pass. Our own decoder
  (`tools/xpress_huff.py`) starts every block with an empty history and raises on a match before the
  block's start; it decoded 181 of 181 blocks of the MS_XPRESS sample (X§5) and every block of E4's
  encrypted-and-compressed sample **(2025)**. So the container keeps a block index, a page read
  decompresses one block (≤ 64 KB), and the open pass decompresses blocks on all threads. A match
  before the block start is a named error, not a silent fallback; if an older version shows one, a
  history-carrying sequential decoder is added for that version. ZSTD blocks are self-contained
  frames (W§3, X§5).

- **D11 — the fuzzy status comes from the log tail (E3§4, §7).** The flag that worked at table level
  in every run (precision 1.00, recall 1.00 over 54 table-runs) is (v) = (iii) ∪ (iv):
  - (iii) a transaction open at `LastLSN` has a row operation on one of the table's allocation units;
  - (iv) a committed record in the tail is newer than the copy of its page, or its page is not in the
    backup.

  Not "page LSN > CheckpointLSN": its recall was 0.42, because a page copied before it changed keeps
  its old LSN. Status per table: `consistent`, `stale`, `dirty`, `stale+dirty`; database level
  `consistent` iff no open transaction and no committed row operation after `CheckpointLSN` (E3§4).
  The `fuzzy` parameter: `'warn'` (default; rows returned, one `WARNING` per affected table in
  `duckdb_logs`), `'error'` (affected tables refused by name), `'ignore'`. R§10.3 is settled for the
  lake by the owner's no-dirt decision (R§4): the importer always passes `'error'`. For browsing the
  default is `'warn'`, with 090's `recovery := true` (DQ-13, accepted). The log-tail reader is built
  here and extended by 090.

- **D12 — a differential overlay for browsing.** `diff :=` names one differential backup (a path, a
  glob or a list of its stripes). The chain is checked first: the diff's `DifferentialBaseGUID`
  equals the full's `BackupSetGUID` and both carry the same `dbi_familyGUID` (E1 rule 1). Pages of
  the diff win; liveness is then recomputed from the overlaid allocation pages (E2 `apply_diff`: a
  naive overlay doubled every row after a shrink). Several differentials are refused: they are
  cumulative (W§10), only the latest matters. A differential given as the path, with no full, is refused
  naming both ways out (`diff :=` on its full, or spec 089's `base :=`): it lacks every catalog page
  that did not change since its base (089 D5). Without spec 090 the overlay reflects the diff's
  `CheckpointLSN`; the fuzzy status is the diff's.

- **D13 — refuse by name at the narrowest scope, never fill with NULL.** An undecodable column is
  refused when projected (`SELECT` of the other columns works); an undecodable table when scanned; an
  undecodable file at open. The message names the object, the feature and the SQL Server type.

- **D14 — versions.** `dbi_version` 998 (2025) is verified. 869 / 904 / 957 (2017 / 2019 / 2022,
  expected values, unverified) are accepted once the CI version matrix passes on their images;
  any other value is refused by name unless `unsupported_version := true`. The minimum supported
  version is 2017, the oldest Linux image the CI can run (DQ-04, accepted; R§10.5).

- **D15 — parsers are DuckDB-free.** Everything under `src/backup/` below the function layer takes
  byte spans and returns plain structs, with no DuckDB headers, so its fuzz harnesses run in the
  per-PR ClusterFuzzLite job (`fuzz/README.md`: the libduckdb-linked harnesses cannot). The DuckDB
  types appear only in the value kernels, the functions and the catalog.

- **D16 — names.** `backup` everywhere, never `bak`, and the folders `src/backup/` and
  `src/catalog/backup/` (owner, 2026-11-05, given with this series; R§6.1 had listed the layout as a
  proposal). Internals in `duckdb::mssql::backup` with no prefix; catalog classes
  in `duckdb` with the `MSSQL` prefix (CLAUDE.md's namespace rule). Source files
  `backup_<component>.cpp`, the peer of `tds_<component>.cpp` (DQ-37, accepted; CLAUDE.md's file rule,
  which names only `mssql_` and `tds_`, is extended to name it).

- **D17 — resources: no OOM, every core, DuckDB's limits (owner, 2026-11-06).** Do not load
  everything into memory, use the machine to the full for parallelism, never fail with OOM, respect
  DuckDB's memory and thread limits, and offer limits on the import itself. 086–090 cite this rule, and
  so do mssql-ducklake's 022 D12 and its 023 / 024; § Resources lists each component with its home or
  its bound.
  - **DuckDB's limits are the ceiling.** `memory_limit` (an alias of DuckDB's `max_memory` setting,
    `DD/src/main/config.cpp:275`) and `threads` are GLOBAL settings, for the whole instance
    (`DD/src/common/settings.json:1201-1210`, `:1534-1543`). Spilling goes to `temp_directory`, up to
    `max_temp_directory_size`, global too (`:1517-1523`, `:1221-1227`). The `max_memory` parameter
    below is per call, unlike DuckDB's `max_memory` setting.
  - **Whatever grows with the input lives in DuckDB's buffer manager**, so it is counted against
    `memory_limit` and spills: buffer-managed blocks, a `ColumnDataCollection` built with the
    `ClientContext` and its default `BUFFER_MANAGER_ALLOCATOR`
    (`DD/src/include/duckdb/common/types/column/column_data_collection.hpp:37-41`), DuckDB's sort.
    The `Allocator&` constructors build an in-memory collection that cannot spill (`:29-32`); none is
    used to accumulate. The rest is bounded by a stated budget, and that working
    memory (a chunk's staging, a LOB value being assembled, a writer's row group) is reserved before it
    is allocated, with `BufferManager::ReserveMemory`
    (`DD/src/include/duckdb/storage/buffer_manager.hpp:100-103`). So it is counted too, under the tag
    `EXTENSION` of `duckdb_memory()`.
  - **One rule for a failed reservation.** `ReserveMemory` evicts blocks or throws
    `OutOfMemoryException` through `EvictBlocksOrThrow`
    (`DD/src/storage/standard_buffer_manager.cpp:775-783`, `:127-138`). Every reservation, the
    reader's and mssql-ducklake's importer's (022 D12), follows one rule: one that does not fit, in its
    budget or in DuckDB's, first waits for other units to release memory; one that still cannot fit
    when its unit is alone is refused by name, naming the object, the size, `max_memory` and
    `temp_directory`. With no `temp_directory` nothing spills (`InMemoryWarning`, `:765-773`), so the
    limits are hard. An in-memory DuckDB has one, `.tmp`, unless `temp_directory` is set to `''`
    (`DD/src/main/config.cpp:651-659`).
  - **Parallelism is `threads`, throttled by memory, never the other way round.** The table functions
    report `MaxThreads()` from their units, and DuckDB runs at most that many tasks of a pipeline on its
    own threads (`DD/src/parallel/pipeline.cpp:127-166`).
  - **Backpressure.** When the budget is reached, a unit waits, or a writer flushes and closes a file.
    Nothing allocates past the budget, and no stage decodes everything before anything is written.
  - **Out of temp space.** DuckDB throws from `TemporaryFileManager::IncreaseSizeOnDisk`
    (`DD/src/storage/temporary_file_manager.cpp:647-659`); the reader refuses the file by name, naming
    `max_temp_directory_size`. Nothing it allocates escapes DuckDB's accounting, so nothing is killed
    for memory.
  - **Limits on one call.** `memory_limit` and `threads` are instance-wide, so a narrower limit for one
    import cannot be a `SET` without changing every other session. The reader's own components enforce
    it: `max_memory := <bytes or '2GB'>` and `max_threads := <n>` on `mssql_backup_scan`, which decodes
    in parallel, and on `mssql_backup_export` (089); 090 adds them to every function that recovers
    (090 D9), and mssql-ducklake's export and commit take the same names (022 D12). The default is
    DuckDB's own limit; a value above it is clamped to it, with a WARNING. They bound the reader's own
    allocations, by the split below (the decode staging, LOB values, 090's recovery structures, 089's
    writers: F comes from `max_memory` / 4), and its concurrency (`MaxThreads()` ≤ `max_threads`).
  - **The split of `max_memory`.** A call's `max_memory` is its whole budget. Half of it is left to
    DuckDB's operators around the reader, its sorts and hash tables: mssql-ducklake gives its side
    connections `operator_memory_limit = max_memory / 2` (022 D12). The reader's and the importer's
    own reservations (decode staging, LOB values, writers, recovery structures) stay under the other
    half, and the export's writers take at most half of that: F comes from `max_memory` / 4 (089).
  - **What they cannot bound**: the DuckDB statements around the reader (an importer's `DELETE` scans,
    a SWITCH's copy, a sort) obey the instance's limits. DuckDB has one per-connection memory knob at
    the pin, `operator_memory_limit` (scope local, `DD/src/common/settings.json:1260-1266`): it caps a
    connection's sorts and hash tables at the smaller of it and the instance's limit
    (`DD/src/main/client_data.cpp:126-133`; read by `TemporaryMemoryManager`,
    `DD/src/storage/temporary_memory_manager.cpp:106`). The importer sets it to `max_memory` / 2 on its
    own side connections (the split above, 022 D12); the reader never sets a caller's. There is no
    per-connection thread knob: `threads`, `external_threads`, `async_threads` and `pin_threads`
    (which pins threads to cores) are all global (`settings.json:1534-1543`, `:958-967`, `:174-180`,
    `:1315-1321`).

## Design

### SQL surface

```sql
-- the backup sets on a media set: HEADERONLY + LABELONLY, with FILELISTONLY as a nested list
SELECT * FROM mssql_backup_info('s3://bak/sales_full.bak');

-- tables, columns (SQL Server and DuckDB types), allocation units, row counts, fuzzy status
SELECT * FROM mssql_backup_tables('s3://bak/sales_full.bak');
SELECT * FROM mssql_backup_views('s3://bak/sales_full.bak');            -- T-SQL text

-- a base table as stored (the importer's mirror; allow-listed, see Security)
SELECT * FROM mssql_backup_catalog('s3://bak/sales_full.bak', 'syscolpars');
SELECT * FROM mssql_backup_catalog('s3://bak/sales_full.bak');   -- the allow-listed base tables

-- one table's rows
SELECT id, amount, _mssql_rid, _mssql_page
FROM mssql_backup_scan('s3://bak/sales_full.bak', 'dbo', 'orders');

-- a differential over its full, for browsing
SELECT * FROM mssql_backup_scan('full.bak', 'dbo', 'orders', diff := 'diff_2026-11-05.bak');

-- browsing as a catalog (read-only whatever the options say)
ATTACH 's3://bak/sales_full.bak' AS b (TYPE mssql_backup, diff := 's3://bak/sales_diff.bak');
SELECT count(*) FROM b.dbo.orders;
```

Common named parameters of every function, and ATTACH options of the same names:

| parameter | default | meaning |
|---|---|---|
| (path) | — | one file, a glob, or a `LIST(VARCHAR)`: the families (stripes) of **one** media set. Every family is required: an incomplete set is refused naming the missing `FamilySequenceNumber`s (E2 R9) |
| `position` | 1 | the backup set on the media, as RESTORE's `FILE = n` (appended sets; unverified: never produced) |
| `diff` | none | a differential to overlay (D12) |
| `fuzzy` | setting `mssql_backup_fuzzy` = `'warn'` | D11 |
| `unsupported_version` | false | D14 |
| `verify` | false | also cross-check every table's pages against its IAM chain and the positional rule (§ Rules, page index) |

`mssql_backup_info(path)` returns one row per backup set:

| column | type | source |
|---|---|---|
| `position` | `INTEGER` | SSET data set number |
| `backup_type` | `VARCHAR` | `'full'` / `'differential'` / `'log'`: SSET attribute 0x8 differential (E1§3); a full and a log backup both carry 0x4 (task 3), and a log backup is the one with no MSDA block (X§3) |
| `is_copy_only` | `BOOLEAN` | SSET attribute 0x2 (E2: 0x6 = copy-only full, over 37 files) |
| `backup_set_guid` | `UUID` | SSET +100 (E1§3) |
| `first_lsn`, `last_lsn`, `checkpoint_lsn`, `database_backup_lsn` | `DECIMAL(25,0)` | trailer SCIN (MSLS / MQCI) +32 / +42 / +52 / +62, 10 bytes each (E1§3) |
| `differential_base_lsn`, `differential_base_guid` | `DECIMAL(25,0)`, `UUID` | a differential's base: trailer SCIN +260 / +200 (task 3; E1's "+830 / +842" were offsets in the MQCI stream, which move with the SCIN's length, 41 of 41 differentials); NULL for any other backup, as HEADERONLY (a full establishes the base its own `checkpoint_lsn` and `backup_set_guid` name) |
| `database_name`, `database_version`, `compatibility_level`, `collation`, `family_guid` | | boot page: `dbi_dbname` +52, `dbi_version` +4, `dbi_cmptlevel` +314, `dbi_collation` +392 (name from the collation table), `dbi_familyGUID` +408 (record offsets, X§4) |
| `recovery_fork_id`, `first_recovery_fork_id`, `fork_point_lsn`, `binding_id` | `UUID`, `UUID`, `DECIMAL(25,0)`, `UUID` | located (task 3, § Rules, header fields): trailer SCIN +156 `RecoveryForkID`, +124 `BindingID`; +172 (10-byte LSN) and +184 (GUID) the fork point and the parent fork, set once the database was restored with recovery; HEADERONLY's `FirstRecoveryForkID` / `ForkPointLSN` are derived from them. The chain check compares the file's fork with the lake's and fails closed: until the fork id is read every apply by LSN, a differential or a full applied incrementally, is refused (089 D7, 023 C1) |
| `backup_name`, `description`, `user_name`, `machine_name`, `software_version` | `VARCHAR` | SSET strings, VOLB machine name, SSET software major / minor (X§3) |
| `database_creation_date`, `backup_start_date`, `backup_finish_date` | `TIMESTAMP` | located (task 3): trailer SCIN +72 / +77 / +82, MTF dates (5 bytes, big-endian bit fields), as HEADERONLY prints them (server local time) |
| `media_set_guid`, `family_count`, `family_sequence_number` | | TAPE's RAID stream +0 / +20 / +24 (E2 layouts) |
| `compressed`, `compression_algorithm` | | `MSSQLBAK` header word @0x0C (§ Rules, container) |
| `encrypted`, `encryptor_thumbprint` | | `MSSQLBAK` @0x0C bit 0x4, thumbprint @0x18 (E4§1); decryption is spec 087 |
| `has_backup_checksums` | `BOOLEAN` | MQDA / MQTL media-format attribute 0x100 and the `CSUM` streams (E2§7) |
| `backup_size`, `compressed_size` | `BIGINT` | `BackupSize` = the sum over the families of (the MSLS block's offset − the soft-filemark size, TAPE's `soft_filemark_block_size` × 512): task 3, on every truth, a 64 KB `BLOCKSIZE` and a striped set included **(2025)**; the files' size |
| `page_count`, `log_tail_bytes` | `BIGINT` | live pages; MQTL bytes |
| `consistent`, `open_transactions`, `committed_after_checkpoint` | | D11, database level (data backups only) |
| `recovery_model` | `VARCHAR` | located (task 3): trailer SCIN +244, `0x08` SIMPLE, else `0x04` BULK_LOGGED, else FULL (sysdatabases' "trunc. log on chkpt" and "select into/bulkcopy" bits) |
| `files` | `LIST(STRUCT(logical_name, physical_name, type, file_id, filegroup_id, filegroup_name, size_bytes, max_size_bytes, create_lsn, unique_id, backup_size_bytes))` | the trailer MQCI's SFIN / SFGI records (§ Rules, header fields); `backup_size_bytes` from the page index (task 7), NULL before it |

`mssql_backup_tables(path)` returns one row per user table: `schema_name`, `table_name`, `object_id`,
`shape` (`heap` / `clustered` / `columnstore`), `compression` (from `sysrowsets.cmprlevel`; `mixed`
when partitions differ, E7: `cmprlevel` is per partition), `partition_count`, `row_count` (sum of
`sysrowsets.rcrows`, exact in every rowset of E7's 16 states), `created`, `modified`, `decodable`,
`reason`, `fuzzy_status`, `open_transactions`, `stale_transactions`, and three nested values:

- `columns LIST(STRUCT(column_id, name, sql_type, system_type_id, max_length, precision, scale,
  collation, code_page, is_nullable, is_identity, is_computed, is_sparse, duckdb_type, decodable,
  reason))`;
- `allocation_units LIST(STRUCT(au_id, type, rowset_id, index_id, partition_number, compression,
  filegroup_id, first_page, root_page, first_iam_page, live_pages))`;
- `partitioning STRUCT(function_name, scheme_name, column_name, column_id, parameter_type,
  range_right BOOLEAN, boundaries LIST(VARIANT), destinations LIST(VARCHAR), next_used VARCHAR)`, NULL
  for a table on a filegroup (E7§2, § Rules, catalog). Boundaries are listed in their current order;
  their ids are positional and renumbered by SPLIT / MERGE, so a consumer compares value lists, never
  ids (E7§6). The base index's data space only: a non-aligned nonclustered index is not reported (the
  scan never reads it). Deriving a DuckLake spec from it is mssql-ducklake's (022 D5).

`mssql_backup_views(path)`: `schema_name`, `view_name`, `object_id`, `definition` (`VARCHAR`, the
T-SQL text from `sysobjvalues`, X§4), `is_encrypted`, `created`, `modified`. A `WITH ENCRYPTION` view
reports `definition` NULL and `is_encrypted` true; the reader does not de-obfuscate.

`mssql_backup_catalog(path, base_table)`: the base table's rows with its own columns as typed by its
`syscolpars` rows, plus the virtual `_mssql_page` / `_mssql_rid` (for a base table, `_mssql_rid` is
its clustering key by `ordkey`; checked against E1's keys: `sysallocunits(auid)`,
`sysrowsets(rowsetid)`, `sysschobjs(id)`, `syscolpars(id, number, colid)`, `sysrscols(rsid,
rscolid)`, `sysseobjvalues(valclass, id, subid, valnum)`, `sysclsobjs(class, id)`). Without
`base_table` it lists the allow-listed base tables (`base_table`, `object_id`, `row_count`,
`columns`), which is the set an importer mirrors (022 D3); the list is the reader's, per version.

`mssql_backup_scan(path, schema, table)`: the table's columns plus the D8 virtual columns.
Projection pushdown; LOBs read only when their column is projected. **No filter pushdown in this
spec**: DuckDB applies filters above the scan, which avoids the 2.0 rule that a scan must apply every
filter it accepts (CLAUDE.md) until filter pushdown with late materialisation is measured (task 25).
`cardinality` from `rcrows`. Parallel over units of live pages (§ Scan). Order: page order within a
unit; no key order (spec 089's export gives one). `max_memory` and `max_threads` (D17; default
DuckDB's `memory_limit` and `threads`, clamped to them with a WARNING) bound the call's own memory
and its units in flight (§ Resources).

The ATTACH catalog: one schema entry per `sysclsobjs` class-50 row, one table entry per user table
(`sysschobjs.type = 'U '`); views are **not** catalog entries (T-SQL is not DuckDB SQL;
`mssql_backup_views` lists them, R§6.2 "Views"). `GetVirtualColumns()` returns D8's columns;
`GetScanFunction` binds `mssql_backup_scan` on the catalog's `OpenedBackup`.

Settings: `mssql_backup_fuzzy` (`'warn'`), `mssql_backup_cache` (`true`: use the opened-file cache).

### Layers and C++ layout

```text
src/backup/                         namespace duckdb::mssql::backup (no prefix); DuckDB-free unless noted
  backup_bytes.cpp                  ByteReader: bounded little-endian reads over a span (every parser uses it)
  backup_source.cpp                 ByteSource: FileSystem handles of every family, positional reads   [DuckDB]
  backup_mtf.cpp                    MtfWalker: DBLK and stream headers, checksums, SQL Server DBLKs
  backup_set.cpp                    BackupSetInfo: TAPE / SSET / VOLB / MSCI (SCIN, SFGI, SFIN) / MSLS fields
  backup_container.cpp              Container: MSSQLBAK header, block chain, block index; logical → physical
  backup_xpress.cpp                 XpressHuffmanDecoder ([MS-XCA] 2.2), bounded by the declared output size
  backup_zstd.cpp                   ZSTD block decode
  backup_page_index.cpp             PageIndex: every unit of every MSDA pass of every family; latest copy
  backup_allocation.cpp             AllocationMap: file header Size, GAM/SGAM/DCM/BCM, PFS, IAM; IsLive (R3)
  backup_page_store.cpp             PageStore: page bytes by (file, page); differential overlay
  backup_catalog_reader.cpp         boot page, the five bootstrap layouts, generic base-table decoder
  backup_catalog_model.cpp          CatalogModel: schemas, objects, rowsets, AUs, columns, TableLayout
  backup_collation.cpp              on-disk collation id → spec 091's collation table (code page, name)
  backup_record.cpp                 RecordView: FixedVar split, record types, forwarding, uniquifier
  backup_lob.cpp                    LobReader: in-row roots, LOB trees, row-overflow, text pointers
  backup_value_kernels.cpp          on-disk bytes → codec staging → DuckDB vectors                    [DuckDB]
  backup_log_tail.cpp               LogTail: MQTL blocks, sector parity, record headers, transactions
  backup_fuzzy.cpp                  FuzzyStatus: E3 flag (v) per table
  backup_opened.cpp                 OpenedBackup: the cached object, staged build, cache key           [DuckDB]
src/backup/functions/               mssql_backup_info / _tables / _views / _catalog / _scan            [DuckDB]
src/catalog/backup/                 namespace duckdb, MSSQL prefix                                      [DuckDB]
  mssql_backup_storage.cpp          StorageExtension (attach, transaction manager)
  mssql_backup_catalog.cpp          MSSQLBackupCatalog
  mssql_backup_schema_entry.cpp     MSSQLBackupSchemaEntry
  mssql_backup_table_entry.cpp      MSSQLBackupTableEntry
  mssql_backup_transaction.cpp      MSSQLBackupTransactionManager, MSSQLBackupTransaction (read-only, no state)
src/include/backup/, src/include/catalog/backup/   headers, mirroring the sources
src/codec/code_page/                spec 091: code-page tables and kernels, reused here (D7)
```

```mermaid
flowchart BT
  src[ByteSource: DuckDB FileSystem, every family] --> mtf[MtfWalker / Container: MTF, MSSQLBAK, XPRESS, ZSTD]
  mtf --> idx[PageIndex: latest copy of each page]
  idx --> alloc[AllocationMap: Size, GAM, PFS → IsLive]
  idx --> store[PageStore: page bytes, diff overlay]
  alloc --> store
  mtf --> log[LogTail: MQTL records, transactions]
  store --> cat[CatalogReader → CatalogModel]
  store --> rec[RecordView / LobReader]
  cat --> rec
  log --> fz[FuzzyStatus]
  cat --> fz
  rec --> kern[value kernels → DuckDB vectors]
  kern --> fn[file functions]
  fz --> fn
  fn --> att[MSSQLBackupCatalog: ATTACH TYPE mssql_backup]
```

### Data structures

- **`OpenedBackup`** (shared, immutable once a stage is built; the `ObjectCache` entry). Built in
  stages, each cached when first needed:
  1. *headers*: per family the TAPE / SSET / VOLB / MSCI / MSLS fields, the container header, the
     data-stream boundaries. Enough for most of `mssql_backup_info`.
  2. *page index* (D4a): one pass over every family in parallel ranges (D3a; I/O plus
     decompression), building per pass the extent bitmap and its ranks (or explicit runs where the
     order breaks), and the `(AU, extent)` records the AU index is sorted from.
  3. *allocation* (D4a): per data file `Size`, GAM and the PFS allocated bit, applied to the AU index
     while it is built and then dropped; the DCM / BCM bitmaps and IAM summaries only for `verify`
     and spec 089.
  4. *catalog*: the decoded base tables and the derived model.
  5. *log tail*: the transaction table and the per-allocation-unit flags of D11.
- **Block index** (compressed media, D4a): one checkpoint per 64 blocks (physical offset, logical
  offset in the MTF data set); a block's own header (kind, payload length, output size `unit × 512`)
  is read inside the fetched range. Built while walking the chain in stage 2.
- **`TableLayout`**: per rowset of the table (one per partition) the physical columns from
  `sysrscols` (rscolid, hbcolid, ti, collation, ordkey, status, offset, nullbit, bitpos), the logical
  columns from `syscolpars`, the defaults from `sysseobjvalues`, the allocation units (IN_ROW_DATA,
  ROW_OVERFLOW_DATA, LOB_DATA) and `cmprlevel`. Rows are decoded **per rowset**, because partitions
  may differ in compression.

### Rules the reader applies

Byte offsets are from the experiments; each is **(2025)** unless marked otherwise.

**MTF** (X§3, `tools/mtf_dump.py`, E2 layouts):
- A DBLK starts with a 52-byte common header: type[4], u32 attributes @4, u16 offset to the first
  stream @8, string type @48, u16 checksum @50 = XOR of the 25 preceding 16-bit words. A stream header
  is 22 bytes: id[4], u16 file-system attributes @4, u16 media-format attributes @6, u64 length @8,
  u16 encryption @16, u16 compression @18, u16 checksum @20 = XOR of the 10 preceding words. Stream data
  is padded to 4 bytes; `SPAD` ends a block's chain. **Both checksums are validated on every header**:
  the string `VOLB` occurs inside data and has looped another reader (W§2).
- The walk follows header lengths only, never fixed offsets: BLOCKSIZE moves SFMB and SSET to 1× and
  2× the block size and APAD absorbs the gap; MAXTRANSFERSIZE changes the padding (E2 R10, §7). A
  block's streams end with `SPAD`, and the next DBLK starts where its data ends (4-aligned); an SFMB
  is TAPE's `soft_filemark_block_size` × 512 bytes long. Walked strictly, with no resync, this ends
  exactly at the end of all 95 uncompressed backups of the experiments (BLOCKSIZE 4 / 16 / 64 KB,
  MAXTRANSFERSIZE 64 KB / 4 MB, stripes, logs, task 3) **(2025)**: a DBLK that does not validate where
  the previous one ended is a named error.
- SQL Server's sequence: `TAPE SFMB SSET VOLB MSCI MSDA MSDA MSTL… MSLS SFMB ESET ESET SFMB` (X§3).
  MQDA = a 2-byte lead-in (`length mod 8192`) then 8 KB units, page-aligned in the file; an MQDA of
  0 bytes with media attribute 0x6 marks the end of data. The first MSDA is pass 1, the second pass 2.
  All data files share one MSDA per pass, in `file_id` order (E2 R9). MQTL = a 2-byte lead-in then log
  blocks. The trailer SCIN in MSLS carries the LSNs; the copy in MSCI does not (E1§3).
- A striped set is `FamilyCount` complete MTF files with one MediaSetId (TAPE RAID stream). Each
  extent is in exactly one family; the log tail is only in family 1 (E2§6).

**Header fields of the trailer SCIN** (task 3, the trailer MQCI's first record, `SCIN`, u32 length
@4; located against HEADERONLY on 39 backups of SQL Server 2025, 501 fields, 0 mismatches: E1's,
E3's, the probe samples and a set made for it -- SIMPLE / BULK_LOGGED / FULL, a 300 MB full whose
start and finish differ, a restore WITH RECOVERY, a log backup across the fork **(2025)**):

| offset | field |
|---|---|
| +32 / +42 / +52 / +62 | FirstLSN, LastLSN, CheckpointLSN, DatabaseBackupLSN (10-byte LSNs: u32 VLF, u32 block, u16 slot) |
| +72 / +77 / +82 | DatabaseCreationDate, BackupStartDate, BackupFinishDate (MTF dates) |
| +124 | BindingID |
| +156 | RecoveryForkID (the database's current fork) |
| +172 / +184 | the fork point LSN and the parent fork's GUID; zero until a restore WITH RECOVERY |
| +224 | FamilyGUID |
| +244 | recovery model bits: 0x08 SIMPLE, 0x04 BULK_LOGGED, neither FULL |
| +200 / +260 | a differential's base: its GUID and LSN (zero in a full) |

The MQCI stream is a run of records, each `tag[4] ‖ u32 length` (the length includes the 8 bytes),
covering the stream exactly: SCIN, one SFGI per filegroup, one SFIN per file. **SFIN** (task 3; 310
fields of FILELISTONLY on 15 backups, a second filegroup and a bounded MAXSIZE included, 0 mismatches
**(2025)**): u32 FileId +20, u32 type +28 (1 data `D`, 2 log `L`; any other value refused by name
until seen), u32 FileGroupId +36 (0 for the log), u64 Size in bytes +40, two string descriptors
(u16 byte length, u16 offset in the record, UTF-16LE) +48 LogicalName and +52 PhysicalName, u64
MaxSize in bytes +86 (unaligned), CreateLSN +138, UniqueId +186. **SFGI**: u32 filegroup id +12 and a
string descriptor +16 for its name. FILELISTONLY's `BackupSizeInBytes` is not in SFIN: it is the
file's pages in the data stream.

HEADERONLY's `FirstRecoveryForkID` and `ForkPointLSN` are derived: a log backup whose FirstLSN is not
past the fork point spans the fork, and reports the parent fork and the fork point; every other
backup reports `FirstRecoveryForkID` = `RecoveryForkID` and `ForkPointLSN` NULL (a full taken after a
restore carries +172 / +184 and still reports NULL). One fork case backs the rule **(2025, one case)**.
The record grows with its trailing strings (database and server names, 442–448 bytes seen); the
offsets above are in its fixed part.

**MSSQLBAK** (X§5, E4§1, `tools/bakz.py`):
- `MSSQLBAK` @0, u32 2 @8, u32 flags @0x0C: 3 = MS_XPRESS, 0x83 = ZSTD, 5 = encrypted, 7 = encrypted
  and compressed. Read as bits 0x2 compressed, 0x4 encrypted, 0x80 ZSTD (inferred from four values);
  an unknown combination is refused naming the word. The first block's payload starting `28 b5 2f fd`
  confirms ZSTD.
- Compressed: a plain SFMB @0x1000, the block chain @0x2000. A block is a 28-byte header (u8 kind,
  u8 unit = output size / 512, u16 payload length with 0 = 65,536, u64, u64, u32 0, u32 checksum) and
  its payload, back to back, unaligned. Kind 0x21 = compressed, 0x23 = stored (0x24, also read as
  stored by W§3's Internals Viewer and accepted by E4's script, was never observed here). The chain ends at the first header whose kind is none of these; after it come a seek
  index, an SFMB, one compressed block with ESET / OTCP / TSMP, and an SFMB (X§5).
- The concatenated block outputs are the MTF data set, byte-identical in layout to an uncompressed
  backup (1,403 of 1,419 units identical to one taken minutes earlier, X§5).
- XPRESS: per block a 256-byte table of 512 4-bit code lengths, then the LZ77+Huffman stream;
  decoding stops at the output size; symbol 256 is not end-of-stream (W§3, X§5).

**Page index**:
- Unit kinds: header version byte (offset 0) ≠ 1 → a never-formatted zero unit, skipped; `m_type`
  101 → a pad, skipped by type and never read further (§ Security); otherwise a page.
- Later pass wins over earlier; within one pass a page id twice is a named error (never observed).
- Positional cross-check (`verify`): a unit at position k of its 8-unit group carries a page id ≡ k
  (mod 8). 0 mismatches over every file of E2.

**Allocation and liveness** (E2 layouts, R3):
- GAM page 2, SGAM 3, DCM 6, BCM 7, repeated every 511,232 pages; bitmap at page offset 194, 7,988
  bytes, bit i (LSB first) = extent i; GAM 1 = free. The second GAM interval is decoded but was never
  exercised (E2§10).
- PFS page 1, then every 8,088 pages; byte for page p at `100 + (p mod 8088)` (page 1 covers pages 0–8087):
  0x40 allocated, 0x20 mixed extent, 0x10 IAM, 0x08 has ghosts, low 3 bits fullness.
- File header page 0, slot 0 record (its offset varies: 96 or 2,494 seen; read it through the slot
  array): u16 file id +167, u16 filegroup id +169, u32 `Size` in pages +171.
- IAM: header record at 96, u32 sequence +4, start page +40, eight single-page slots +46; extent
  bitmap at 194 relative to the start page.
- `IsLive(file, page)`: the allocation pages themselves (0, 1, 9, the PFS pages, GAM / SGAM / DCM / BCM)
  are live; any other page is live iff `page < Size` and its extent's GAM bit is 0 and its PFS byte has
  0x40.

**Catalog** (X§4, E1§4.8, E2§4, `experiments/E1/pages_ext.py`):
- Boot page 1:9, one record at 96: `dbi_version` u16 +4, `dbi_dbname` nvarchar(128) +52,
  `dbi_cmptlevel` +314, `dbi_collation` u32 +392, `dbi_familyGUID` +408, `dbi_firstSysIndexes` +516
  (6-byte page pointer, = 1:20, the first `sysallocunits` page).
- Bootstrap tables, found by the page header's `(m_objId, m_indexId)` on live level-0 data pages:
  `sysallocunits` (7, 0), `sysrowsets` (5, 0), `sysschobjs` (34, 1), `syscolpars` (41, 1), `sysrscols`
  (3, 0). Fixed-part offsets (record offsets):

  | table | fields |
  |---|---|
  | `sysallocunits` | auid i64 @4, type u8 @12, ownerid i64 @13, status i32 @21, fgid i16 @25, pgfirst @27, pgroot @33, pgfirstiam @39 (6-byte page pointers) |
  | `sysrowsets` | rowsetid i64 @4, ownertype u8 @12, idmajor i32 @13, idminor i32 @17, numpart i32 @21, status i32 @25, rcrows i64 @31, cmprlevel u8 @39 |
  | `sysschobjs` | id i32 @4, nsid i32 @8, status i32 @13, type char(2) @17, pid i32 @19, created datetime @28, modified datetime @36; name = variable column 1 |
  | `syscolpars` | id i32 @4, number i16 @8 (0 = table column), colid i32 @10, xtype u8 @14, utype i32 @15, length i16 @19, prec u8 @21, scale u8 @22, collationid i32 @23, status i32 @27 (bit 0 = NOT NULL); name = variable column 1 |
  | `sysrscols` | rsid i64 @4, rscolid i32 @12, hbcolid i32 @16, ti i32 @28 (low byte = xtype, then length or precision / scale), collation i32 @32 (OrcaMDF's "cid"), ordkey i16 @36, status i32 @40 (0x2 dropped, 0x10 uniquifier, 0x80 NOT NULL, 0x200 has default), offset @44 (low 16 bits signed: > 0 fixed offset, −n the n-th variable column), nullbit @48 (low 16 bits), bitpos @52 (low byte) |

- Generic tables needed by 085: `sysclsobjs` (class 50 = schemas; schema 1 = `dbo`), `sysseobjvalues`
  (defaults of metadata-only ADD COLUMN: valclass 1, `id` = rowset id, **`subid` = hbcolid**, valnum 0,
  value sql_variant; E1§4.8 — E2§4 says rscolid, which equals hbcolid only without a uniquifier),
  `sysobjvalues` (module text; valclass of a view definition to pin, task 11), and the partitioning
  tables below.
- **Partitioning** (E7§2, decoded generically and verified against the catalog views in 16 states,
  0 mismatches **(2025)**):

  | fact | base table |
  |---|---|
  | partition function | `sysclsobjs` class 30: `id` (65536+), `name`, type `'R '`, `intprop` = fanout (partitions; boundaries = fanout − 1), `status & 1` = RANGE RIGHT |
  | parameter type | `systypedsubobjs` class 30: `idmajor` = function, `subid` = parameter, `xtype`, `length`, `prec`, `scale`, `collationid`, `utype` |
  | boundary values | `sysobjvalues` valclass 30: `objid` = function, `subobjid` = boundary id 1…n (**positional: SPLIT / MERGE renumber**), `valnum` = parameter, `value` = sql_variant |
  | data spaces | `sysclsobjs` class 31: type `'FG'` (filegroups, id 1 = PRIMARY), `'PS'` (schemes, 65601+) |
  | scheme → function | `syssingleobjrefs` class 31: `depid` = scheme, `indepid` = function (two schemes may share one) |
  | scheme → filegroups | `syssingleobjrefs` class 30: `depid` = scheme, `depsubid` = destination (= partition number), `indepid` = filegroup; a row at fanout + 1 is NEXT USED |
  | index → data space | `syssingleobjrefs` class 7: `depid` = object, `depsubid` = index id (0 = heap), `indepid` = scheme or filegroup; **no row = PRIMARY**. `sysidxstats.dataspace` is 0 in every row and is not used; `sysmultiobjrefs` plays no part |
  | LOB data space | `syssingleobjrefs` class 8: `depid` = object (the scheme, for a partitioned table with LOBs) |
  | partitioning column | `sysiscols` of (object, base index): the row with `tinyprop2` = 1 (partition ordinal); `intprop` = column id, `tinyprop1` = key ordinal (0 when not in the key) |
  | partitions | `sysrowsets` per (object, index): `numpart`, `cmprlevel` per partition, `rcrows` (exact) |
  | filegroup of a partition | `sysallocunits.fgid` |

  A sql_variant value is `[base xtype][version 1][props][value]`; props: none (int, bigint, date,
  datetime, …), 1-byte scale (time, datetime2, datetimeoffset), precision + scale (decimal), u16 maximum
  length (binary), u16 maximum length + u32 collation (strings). The catalog's own sql_variant reader
  (ADD COLUMN defaults, boundaries) decodes it; E7 exercised int, bigint, date, datetime,
  datetime2(0/3/7), datetimeoffset(0), varchar and nvarchar; decimal, uniqueidentifier, binary and
  time boundaries ship behind fixtures (E7§7). A boundary that cannot be decoded refuses the table's
  `partitioning` by name, never a NULL boundary (D13).
- A table = `sysschobjs` type `'U '`; its base rowsets = `sysrowsets` with `idmajor` = object id and
  `idminor` ∈ {0 heap, 1 clustered}, one per partition (`numpart`); allocation units by `ownerid` =
  rowset id, type 1 IN_ROW_DATA, 2 LOB_DATA, 3 ROW_OVERFLOW_DATA. A rowset whose IN_ROW AU has type 0
  or owner 0 is a deferred drop: dead (E2§1.2).
- Collations: 085 maps the on-disk CollationId (`dbi_collation`, `sysrscols.collation`,
  `syscolpars.collationid`) onto spec 091's table, `tools/code_pages/data/collations.psv` (5,540
  rows **(2025)**), for its code page and name (D7). The on-disk id is encoded differently from the
  5 bytes of a TDS COLMETADATA collation, and its low byte is not the LCID. A collation is UTF-8 iff
  `id >> 24 == 0` and `id & 0x80` (exact over all 5,540, X§4). Code pages used: 65001, 1250–1258,
  874, 437, 850 (single byte: 091's tables, here) and 932, 936, 949, 950 (double byte: 091's tables,
  spec 086); 0 = Unicode-only collations, which no `varchar` can carry.

**Records** (X§4, E1§4.1, E1§5 rule 7, E2 R7):
- Slot array at the page end, u16 per slot, `m_slotCnt` @22; a slot offset below 96 is a freed slot
  (heap delete), skipped.
- statusA: record type = `(statusA >> 1) & 7`; 0x10 null bitmap present; 0x20 variable columns
  present; 0x40 a 14-byte version tag follows the record (RCSI / snapshot; unverified — no experiment
  ran with row versioning). Types: 0 primary, 1 forwarded, 2 forwarding stub, 3 index, 4 blob
  fragment, 5 ghost index, 6 ghost data, 7 ghost version. **Scan rows from types 0 and 1 only.**
- Layout: u16 end of the fixed part @2, fixed data from 4, u16 column count, null bitmap of
  `ceil(n / 8)` bytes, then (0x20) u16 variable count and that many u16 end offsets (bit 15 = complex
  column), then the data.
- Column value, per physical column of the rowset, in this order: dropped (status 0x2) → skipped, its
  bytes still occupy their place; `nullbit` > the record's column count → the column was added after
  the record was written: the `sysseobjvalues` default when status has 0x200, else NULL; NULL bit set
  → NULL; offset > 0 → fixed bytes at that offset (`bit`: bit `bitpos` of the byte); offset < 0 →
  variable column −offset−1. When that index is beyond the record's variable count and the column
  passed the two tests above, the value is empty: that is E1's prototype (`pages_ext.py`, 0 failures
  over its chains). E2 R7 states the beyond-the-count case as "the default, else NULL", which is the
  added-column case the `nullbit` test already catches. The oracle fixtures include trailing empty
  strings and binaries to settle it (trailing NULL variable columns are omitted, and are caught by the
  bitmap).
- Forwarding stub (type 2): 9 bytes, a pointer to the new location; skipped (the row is read where it
  lives). Forwarded record (type 1): its **last** variable column is the back pointer, complex,
  10 bytes: u16 0x0400 + the original RID; it is not a table column (E1§4.1 found the "first complex
  column after the table's columns" rule wrong).
- Uniquifier: `sysrscols` rscolid 0, status 0x10, the first variable column; zero-length = 0, else
  i32.

**LOBs and row-overflow** (X§4, E1 `LobReader`):
- A complex variable column starts with its kind: 2 = row-overflow / text pointer, 24 bytes (u32
  update counter +4, u32 length +12, root RID +16); 4 or 5 = in-row root: 12-byte header (u32 update
  counter `ts` +4), then 12-byte links `u32 cumulative size, u32 page, u16 file, u16 slot`.
- A LOB fragment record (statusA 0x08): u16 length @2, u64 blob id @4, u16 type @12: 0 SMALL_ROOT
  (u16 size @14, data @20), 2 INTERNAL (u16 maxlinks @14, curlinks @16, level @18, 16-byte links @20:
  `u64 cumulative size, u32 page, u16 file, u16 slot`), 3 DATA (data from 14 to the record length),
  5 LARGE_ROOT (12-byte links @24).
- The rebuilt value is checked at every node: a node's bytes equal the length its parent claims.
- Legacy `text` / `ntext` / `image` hold a 16-byte text pointer in row (W§6); **unverified** (E1§6:
  not tested), so these types are behind an oracle test before they ship.

**Value encodings that differ from TDS** (W§7, X§4):

| type | on disk |
|---|---|
| `datetime` | u32 ticks of 1/300 s, then i32 days since 1900-01-01 |
| `smalldatetime` | u16 minutes, then u16 days since 1900-01-01 |
| `money`, `smallmoney` | i64 / i32 in units of 1/10,000 |
| `bit` | bit `bitpos` of a fixed byte; the other bits of the byte are not zeroed |
| `date` | 3-byte days since 0001-01-01; `time(p)` 3–5 bytes of 10^−p s; `datetime2` = time then date; `datetimeoffset` = UTC time, date, i16 offset minutes (same as TDS) |
| `varchar` / `char` | code-page bytes, or UTF-8 under a UTF-8 collation |

**Fuzzy status** (E3§6, `experiments/E6/wal.py`):
- MQTL blocks in sector order. Block header: u16 record count @2, u16 end of the slot array @4, u16
  block size @6 (a multiple of 512), u32 VLF sequence @12, u32 block id @16, first record @72; u16
  record offsets before the slot array's end, last record first.
- **Sector parity**: the first byte of every 512-byte sector k ≥ 1 is overwritten; the original is at
  `block[size − 1 − k]` (E3§6: 1.6 % of records misparsed without it).
- Record header: u16 fixed length @2, previous LSN @4, xid @16 (u32, u16), op @22, context @23,
  page @24 (u32 page, u16 file), u16 slot @30; when the fixed length is ≥ 48, the allocation unit =
  `(u16 @46) << 48 | (u32 @32) << 16`. LSN = (VLF, block id, slot index + 1). Opcodes and contexts:
  E3§6's tables (verified against `fn_dblog` on 4,283 records).
- Transactions: BEGIN 0x80, COMMIT 0x81, ABORT 0x82; open = a BEGIN and no end before `LastLSN`.
  Row operations: FORMAT_PAGE 0x01, INSERT_ROWS 0x02, DELETE_ROWS 0x03, MODIFY_ROW 0x04,
  MODIFY_COLUMNS 0x06, DELETE_SPLIT 0x0b, EXPUNGE_ROWS 0x0d.
- A table's allocation units for the flags: those of its base rowsets (in-row, overflow, LOB), the
  pages the scan reads. (E3 does not record whether nonclustered-index AUs counted; they hold nothing
  the scan returns.)

### Scan

- Units: the live level-0 data pages (type 1) whose header AU is an IN_ROW AU of the table's base
  rowsets, in page-id order, cut into units of 64 pages; one unit per task, `MaxThreads` = units.
  Batch indexes follow unit order, so DuckDB can preserve insertion order.
- A compressed backup reads a page by decompressing its block; each thread keeps its last block.
- LOBs and overflow values are read only for projected columns; a chunk closes early on the codec's
  staging byte budget, as on the live read path.
- The bind data serialises and deserialises (the common-subplan optimizer could otherwise merge two
  scans); producers call `DataChunk::SetChildCardinality`; writes go through
  `FlatVector::GetDataMutable` (CLAUDE.md, 2.0 contracts).

### Resources (D17)

Each component of 085, with its home or its bound:

| component | grows with | home or bound |
|---|---|---|
| extent bitmaps and ranks, block checkpoints (stage 2, D4a) | the file's extents and blocks (≈ 85 MB for 10 TB) | resident while the backup is open; within D4a's 0.4 MB per GB |
| AU index (stage 2, D4a) | the runs of the tables' extents (tens of MB typically, ≈ 2 GB worst case for 10 TB) | buffer-managed segments built by DuckDB's sort; a scan pins its table's; they spill |
| opened-file cache (D9) | files opened | DuckDB's `ObjectCache`: an entry reports `GetEstimatedCacheMemory` (`DD/src/include/duckdb/storage/object_cache.hpp:48`), so the buffer pool evicts it under pressure (`DD/src/storage/buffer/buffer_pool.cpp:348`); an attached catalog pins its entry; an evicted entry costs a reopen |
| catalog (stage 4) | objects × columns | the decoded base tables in `ColumnDataCollection`s built with the `ClientContext`, spillable; the model over them indexes them by id, and a `TableLayout` is built only for a table a call binds |
| log tail (stage 5) | the tail's records | read block by block; the transaction table and the per-AU flags of D11 are buffer-managed (090 D9 extends them) |
| decode staging | the rows of a chunk | one chunk per running unit, closed at the codec's staging byte budget or a full vector, whichever comes first; reserved (D17) |
| a compressed block being read | ≤ 64 KB | one per thread (§ Scan) |
| fetched ranges (D3a) | the ranges in flight and cached | the buffer manager's: a remote file's cached blocks are counted and evictable once unpinned; a local file's reads are counted and freed on release; the ranges in flight are bounded by the units in flight |
| a LOB value | its length | not streamed: DuckDB has no streamed value, so a value is assembled whole in its vector, fragment by fragment, capped at 2^31 − 1 bytes. Its length, known from its root before a fragment is read, is reserved first, by D17's rule: a unit that cannot reserve it waits for the others, and a value that still cannot fit while its unit runs alone is refused by name (table, `_mssql_rid`, column, length, `max_memory`, `temp_directory`) |

So a scan holds at most `MaxThreads()` (≤ `max_threads`) chunks and LOB values at once, under the
reservations' half of `max_memory` (D17); the rest is buffer-managed and spills. 088 (rowgroup
buffers), 089 (changed pages, base inputs, the export's writers) and 090 (recovery state, its D9)
list their own; mssql-ducklake 022 D12 lists the importer's.

### Invariants

1. A row is returned only from a live page (R3) of the table's base rowset, from a record of type 0
   or 1.
2. Pads, zero units, ghost records, freed slots and non-live pages never reach a result, a log line,
   an error message or a debug dump.
3. A value is decoded or refused; it is never replaced by NULL (D13).
4. A cached `OpenedBackup` is used only while every family's size and version tag match the key.
5. No code path of `TYPE mssql` (pool, transactions, optimizer, pushdown, DML) runs for an
   `mssql_backup` catalog.
6. Nothing writes to a backup file.

## Security and hardening

A backup file is untrusted input (R§9). Every parser:

- **Bounds every read** through `ByteReader`, which fails with a named error on any read past its
  span. Every length, offset, count and pointer is checked against its container before use: DBLK and
  stream lengths against the file; block payload lengths against the file and output sizes against
  64 KB; slot counts against `(8192 − 96) / 2`; slot offsets against `[96, 8192 − 2 × slotCnt)`;
  record ends against the slot's span; column counts against the layout; variable end offsets
  monotonic and within the record; null bitmap within the record.
- **Terminates**: the MTF walk advances strictly; a block chain cannot loop (offsets increase); LOB
  trees carry a visited set of node RIDs, a depth limit (32) and a total-length limit (2^31 − 1);
  forwarding is one hop (a stub pointing at a stub is refused).
- **Decompresses with declared sizes**: XPRESS never writes past `unit × 512` and validates the
  Huffman table (lengths ≤ 15, a complete prefix code); a match before the block start or past its end
  is an error. ZSTD frames are decoded into a buffer of the declared size and refused if the frame
  claims more.
- **Treats pads as sensitive** (E2§7: in 26 of 35 files the pad bodies carried row data of the
  database, in three of them rows on no live page of that file). Pads are skipped by type at index
  time, and their bodies are never read further. The same holds for non-live pages and ghost records,
  which hold deleted rows (X§4: a deleted heap row's bytes stay on the page). The reader is not a
  forensic tool: it returns what RESTORE returns.
- **Limits the base tables it exposes**: `mssql_backup_catalog` serves an allow-list (the mirror set of
  E1§4.8 — `sysallocunits`, `sysrowsets`, `sysschobjs`, `syscolpars`, `sysrscols`, `sysseobjvalues`,
  `sysclsobjs` — plus `sysobjvalues`, `sysidxstats`, `sysiscols`, the columnstore tables of 088 and the
  partitioning tables of E7: `systypedsubobjs`, `syssingleobjrefs`). Others, such as `sysowners`
  (contained users' password hashes) or the certificate and key tables, are refused by name.
  `sysobjvalues` is served by value class only: view definitions, partition boundaries (valclass 30),
  and the computed-column and default definitions, the classes pinned by task 11; module source never
  leaves (DQ-11, accepted).
- **Fuzzing** (ClusterFuzzLite, `fuzz/`, D15): one harness per parser, each with a seed corpus cut from
  the committed samples: `fuzz_backup_mtf` (walker), `fuzz_backup_container` (MSSQLBAK chain),
  `fuzz_backup_xpress` (raw codec, with a differential oracle against a reference decode of the seed),
  `fuzz_backup_page` (header, slot array, record split), `fuzz_backup_record` (row decode against a
  synthetic layout), `fuzz_backup_lob` (tree walk over a synthetic page set), `fuzz_backup_catalog`
  (bootstrap over synthetic pages), `fuzz_backup_log` (blocks, parity, records). `src/backup/**` joins
  `cflite-pr.yml`'s paths.
- **Memory** (D17, § Resources): whatever grows with the file is buffer-managed; a LOB value is capped
  at 2^31 − 1 bytes and reserved before it is read; a chunk closes at the staging budget.

## Testing and acceptance

Fixtures follow the series strategy (`../README.md` § Test corpus): small committed samples for the
server-free lane and the fuzz seeds, and backups generated in CI for the oracle lane.

**Oracle** = RESTORE of the same file on the server that wrote it, then SELECT (R§9). In the CI lane
the comparison runs inside DuckDB: the restored database is attached as `TYPE mssql`, so both sides
are typed by the same map (D6), and each table is compared as a multiset in both directions
(`EXCEPT ALL` plus counts). LOBs are compared by SHA-256 and length. Locators against the server:
heap `_mssql_rid` after its shape byte = `%%physloc%%`, clustered `_mssql_page` = the physloc page
(E1§3).

**Fixtures** (generator T-SQL under `test/backup/gen/`):
- X§1's probe database: every common type, `nvarchar` with Cyrillic, a UTF-8 `varchar`, heap and
  clustered PK, LOBs of 20 KB and 324 KB, row-overflow, a view; backups uncompressed, MS_XPRESS, ZSTD.
- E1's shapes: forwarded rows (with trailing NULL variable columns), a non-unique clustered index with
  duplicates, ghosts left by deletes in the backup's own batch, ADD COLUMN NOT NULL DEFAULT (with a
  uniquifier), DROP COLUMN, TRUNCATE, DROP.
- E2's edges: SHRINKFILE, heap REBUILD, a second data file, a second filegroup, a striped backup,
  `CHECKSUM`, `BLOCKSIZE` 65536, `MAXTRANSFERSIZE` 64 KB and 4 MB.
- Partitioned tables: E7's structure database (`experiments/E7/sql/a00`–`a03`: 18 functions with rows
  on every boundary edge, RANGE LEFT and RIGHT, int / bigint / date / datetime / datetime2(0/3/7) /
  datetimeoffset / varchar / nvarchar parameters, a function with no boundaries, two schemes on one
  function, `ALL TO`, aligned and non-aligned indexes, a heap, a clustered PK, a clustered columnstore
  for 088); `partitioning` checked against the catalog views as E7's `e7part.py verify` does, rows
  per partition against `$PARTITION`. A partition with another compression than its neighbours is
  refused by name until 086 (E7 decoded a ROW partition).
- Fuzzy backups under E3's workloads, for the status only.

**Version matrix**: Linux images 2017, 2019, 2022, 2025 in the oracle lane, each fixture gated by
what the version supports (UTF-8 collations from 2019, ZSTD from 2025). 2016 and older: best effort,
committed samples only.

**Acceptance**:
1. Every fixture table of every version in the matrix equals RESTORE + SELECT, LOBs included.
2. `mssql_backup_info` equals `RESTORE HEADERONLY` / `LABELONLY` / `FILELISTONLY` on every field it
   reports as located; the others are NULL, never guessed.
3. Uncompressed, MS_XPRESS and ZSTD backups of one database give identical results.
4. A striped set gives the single-file result; a missing stripe is refused by name.
5. Full + differential equals RESTORE of full + diff `WITH RECOVERY` on quiet fixtures; a wrong base
   is refused by name.
6. On E3's fuzzy runs the per-table status equals flag (v): every differing table flagged, no
   consistent table flagged.
7. A second call on an opened file reads no file bytes (counted); a rewritten file is reopened.
8. `s3://` (MinIO in the lane) and `https://` reads give the local result. The open pass and a scan
   are measured on MinIO in requests and in time, sequential against D3a's parallel ranges; a second
   call on the same file, with nothing evicted in between, fetches no range again (D3a).
9. ATTACH: `SELECT` works; `CREATE`, `INSERT`, `UPDATE`, `DELETE`, `COPY … TO`, `ALTER`, `DROP`
   are refused by DuckDB's read-only check.
10. Every refusal of § Scope names its object and feature; no InternalException.
11. The fuzz harnesses replay their corpora clean under ASan and UBSan.
12. Opening a 100 GB backup (generated in a manual lane) reads the file at most once and leaves at
    most 40 MB resident (D4a's 0.4 MB per GB, `duckdb_memory()`'s `EXTENSION` tag after the open), the
    AU index included when nothing spills; a synthetic database whose tables interleave extent by
    extent (D4a's worst case) stays within the same bound for its resident tier.
13. A backup much larger than memory (a generated 20 GB full, the manual lane), every table scanned
    with `memory_limit = '1GB'` and every thread, completes: the peak memory DuckDB accounts
    (`duckdb_memory()`, the buffer manager's used bytes) stays under the limit, and every table equals
    the oracle. With `max_memory := '512MB', max_threads := 2` likewise: the memory the scan reserves
    (`duckdb_memory()`'s `EXTENSION` tag) stays under its half, 256 MB (D17), at most two tasks at a
    time. 089 acceptance 9 runs the import of the same file (D17).

## Tasks

Days are working days for one developer who knows the codebase. Order is the PR order; each PR
merges before the next opens (never stacked).

**PR 1 — container and pages (`mssql_backup_info`)**
1. Skeleton: folders, CMake, namespaces, `ByteReader`, the error type — 1 d
2. `ByteSource`: FileSystem handles for every family through `CachingFileSystem`, globs and lists;
   range requests coalesced, fetched as `AsyncTask`s on the ASYNC pool (D3a) — 3 d
3. MTF walker, checksums, SQL Server DBLKs; SSET / VOLB / MSCI / SCIN / SFIN / MSLS fields; locate
   `backup_start_date` / `backup_finish_date` / `recovery_model` and the recovery fork
   (`RecoveryForkID`, `FirstRecoveryForkID`, `ForkPointLSN`) against HEADERONLY. The fork is on the
   path to every apply by LSN: until it is read, 089's chain check refuses every differential and
   every full applied incrementally (089 D7) — 3 d
4. MSSQLBAK header, block chain, block index; whether the seek index after the chain gives the blocks'
   offsets, so the chain is fetched in parallel (D3a) — 2.5 d
5. XPRESS decoder, table-driven — 2 d
6. ZSTD: link DuckDB's bundled `duckdb_zstd` if every platform's host exports it, else vcpkg `zstd`
   statically with hidden visibility (as OpenSSL) — 0.5 d
7. E12: does a full backup's first pass carry exactly the GAM-allocated extents, in order (on every
   fixture of E2, then the version matrix)? Decides whether the open pass reads every page (D4a) —
   1.5 d
7a. Page index (D4a): units of every pass and family, latest copy, pads and zero units, positional
   check; the extent bitmaps and ranks with the order check and explicit runs where it breaks, the
   AU index sorted into buffer-managed segments, block checkpoints; the open pass over parallel
   ranges (D3a) — 5 d
8. Allocation map and `IsLive`, several files and GAM intervals — 2 d
9. `mssql_backup_info`, registered through `RegisterDocumentedFunction` — 1 d
10. Fuzz harnesses mtf / container / xpress / page; committed seed samples — 2 d

**PR 2 — catalog and rows (`mssql_backup_tables`, `_views`, `_catalog`, `_scan`)**, after spec 091's
module `src/codec/code_page/` has merged (D7)

11. Boot page, bootstrap layouts table, generic base-table decoder; pin the `sysobjvalues` value
    classes DQ-11 serves (view, computed-column and default definitions; boundaries are valclass 30)
    — 4 d
12. Catalog model: schemas, objects, rowsets, partitions, AUs, columns, defaults, views; the
    partitioning model of E7 (functions, typed boundaries through the catalog's sql_variant reader,
    schemes, destinations, index data spaces, the partition column), the `partitioning` column and
    E7's verify as a test — 5 d
13. 085 maps the on-disk CollationId onto spec 091's table; 091's single-byte code-page tables and
    kernel reused (D7) — 0.5 d
14. FixedVar records: types, forwarding, uniquifier, dropped and added columns, version tag — 3 d
15. Value kernels over the codec staging; own kernels for datetime, smalldatetime, money, bit (the
    code-page kernel is spec 091's, D7) — 4 d
16. `mssql_backup_scan`: bind, projection, units, parallelism, cardinality, virtual columns,
    serialisation; the D17 budget (`max_memory`, `max_threads`, the clamp's WARNING, the reservations
    of § Resources and their wait-then-refuse rule) and acceptance 13 — 5.5 d
17. `mssql_backup_tables`, `mssql_backup_views`, `mssql_backup_catalog` — 2 d
18. Fuzz harnesses record / catalog — 1 d
19. CI oracle lane: `docker run` of each image with a host volume at `/var/opt/mssql/backup`,
    generator scripts, RESTORE, the `TYPE mssql` comparison — 4 d

**PR 3 — LOBs**

20. LOB reader: roots, trees, row-overflow, text pointers, checks; lazy reads; the plain `_mssql_lob`
    output, root bytes and node list (DQ-05) — 5 d
21. Fuzz harness lob; LOB fixtures in the oracle lane — 1 d

**PR 4 — cache, ATTACH, overlay, remote**

22. Opened-file cache: staged build, key, eviction estimate — 1.5 d
23. ATTACH: storage extension, catalog / schema / table entries, transaction manager, isolation from
    `TYPE mssql` (a test that sets every pool / optimizer counter and asserts zero) — 4 d
24. Differential overlay and chain check — 1.5 d
25. `s3://` (MinIO) and `https://` tests in the lane; measure the open pass and decide filter pushdown
    with late materialisation (a follow-up spec if it is worth it) — 2 d

**PR 5 — fuzzy status**

26. Log tail reader: blocks, parity, record headers, transaction table, AU per record — 3 d
27. Flag (v), the status columns, the warning, `fuzzy :=`; E3-style fuzzy fixtures in the lane — 2 d
28. Fuzz harness log — 0.5 d

**Across PRs**

29. Docs per PR: README (user-facing), `DATAMODEL.md` (a "Backup reader" layer section with the
    diagram above), CLAUDE.md (functions, settings, layout), CHANGELOG, website — 2 d

**Total ≈ 75 days, 13.5–15 weeks** (72.5 before D4a's index and E12 joined task 7, +2.5 d; 69 before
D3a's ranged, asynchronous I/O joined tasks 2, 4 and 7, +3.5 d, both owner 2026-11-09; 66.5 before E7's partitioning model joined task 12, 68.5
before the plain `_mssql_lob` output joined task 20 from 089 task 7, DQ-05, 69.5 before spec 091
took the code-page tables out of task 13, −1 d, and D17's budget joined task 16, +0.5 d). Spec 091's
days are its own, outside this total. R§8 gave phase 0 6–8 weeks; this spec also carries the
differential overlay, the locators and the parallel scan from phase 1 (~2 weeks), and costs the
hardening, the eight fuzz harnesses, the CI oracle lane and the docs that R§9 lists but §8 did not
cost.

## Risks

- **Version drift is the main risk.** Every offset above is **(2025)**. The catalog layouts (D5), the
  boot page, SCIN offsets and the MSSQLBAK header may differ on 2017–2022. The CI matrix is what turns
  each into a verified row; until a version passes, it is refused (D14).
- **The open pass** reads the whole file once, from S3 too, in parallel ranges (D3a); a 10 TB backup
  is read whole before the first row unless E12 shows the allocation pages suffice (D4a). A saved
  index is a later option (D4a).
- **MS_XPRESS on older versions** may carry history across blocks (W§3; Redgate hints 2008 R2
  differs). D10 makes that a named error, not wrong data.
- **Legacy LOB types** (`text`, `ntext`, `image`) are unverified.
- **RCSI / snapshot version tags** (statusA 0x40) are handled by length only; no experiment ran with
  row versioning (E2§10, E3§8). ADR's in-row versions are unknown.
- **Header fields located on 2025 only**: the backup dates, the recovery model and the recovery
  fork are in the trailer SCIN (§ Rules), and the fork rule rests on one case. The importer tags
  snapshots with `backup_finish_date` (R§6.2) and refuses every apply by LSN until the fork id is
  read (089 D7), so each version of the matrix re-checks them against HEADERONLY before it is
  accepted (D14).
- **Appended backup sets** (`position`) were never produced.
- **Partitioning not covered by E7** (E7§7): RANGE LEFT SPLIT / MERGE, an `ALL TO` scheme after a
  SPLIT, PAGE compression per partition, boundary types beyond those listed above, a computed
  partitioning column. Each gets a fixture before it is trusted.
- Decided ([decisions.md](decisions.md)): browsing with `fuzzy := 'warn'` and `recovery := true`
  (D11; DQ-13); 2017 as the minimum version (D14; DQ-04); the allow-list of base tables and
  `sysobjvalues` value classes (§ Security; DQ-11); the service-column types (D8; DQ-02); the
  `backup_<component>.cpp` file names (D16; DQ-37); committed samples, ≤ 2 MB each and ≤ 16 MB in
  all (DQ-36).

## References

- RESEARCH.md §1–§3, §4 (consistency), §5 (row identity), §6.1 (architecture), §8–§10.
- research-experiment.md §3 (MTF), §4 (pages, catalog, records, LOBs, types), §5 (compressed), §6
  (differential), §7 (log).
- research-web.md §2 (MTF), §3 (compression), §6 (on-disk structures), §7 (types), §9–§10.
- research-architecture.md §1 (storage hook, catalog, reuse, live-connection dependencies).
- E1 §3 (header fields, locators), §4.1 (forwarded back pointer), §4.8 (catalog), §5 rules 1–9.
- E2 §0 rules R1–R11, §1.3, §2, §6, §7, §9 (corrections to RESEARCH).
- E3 §2 (how a backup is taken), §4 (flags), §6 (log format), §7 (v1 policy).
- E4 §1 (MSSQLBAK header fields shared with encryption).
- E7 §2 (where the partitioning lives, the sql_variant layout, the decoder and its verification), §7
  (not covered).
- `tools/mtf_dump.py`, `tools/bakz.py`, `tools/xpress_huff.py`, `tools/pages.py`,
  `experiments/E1/pages_ext.py`, `experiments/E6/wal.py` (the record parser).
