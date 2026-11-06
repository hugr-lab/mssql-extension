# Spec 089: what an incremental importer needs from the reader

Status: **draft**, 2026-11-05. Not started. Depends on spec 085; uses 086–088 where a table needs
them. Series overview: `../README.md`. Markers and source abbreviations as in 085 (DuckLake as
`DL/`, DuckDB as `DD/`); the owner's decisions as DQ-nn
([decisions.md](../085-backup-reader-core/decisions.md)).
This spec is the authority on the names and shapes mssql-ducklake consumes (its README says so);
022 §D4 and 023 §D2 were reconciled with it.

## Problem

The owner's operating model (R§6.2): **the lake is the base state.** A full backup is imported once;
every later file (a differential, a periodic new full, later a log backup) is applied on top of the
lake **without reading the full backup or any earlier file**. E1 proved it in Python: 18 applied files
and 3 fresh imports over 5 chains, every state equal to the server row for row, lakes reached by
different routes byte-identical (E1 short answer). E2 (R1–R11) and E6 corrected and extended the
rules.

The importer is mssql-ducklake. The reader must give it everything those rules need while knowing
nothing about the lake (research-ducklake-import §8: "mssql knows no lake"). This spec defines that
contract: chain metadata, the changed and freed pages with each page's previous owner,
allocation-page state, the catalog mirror, the LOB fragment maps, and a Parquet export. The recovery
of a loaded file's log tail, and the pending and stale pages it produces, are spec 090; this spec
leaves the hooks.

## What the importer needs (the rules, with their evidence)

| # | need | rule | evidence |
|---|---|---|---|
| 1 | refuse a wrong chain before anything else | a differential's `DifferentialBaseGUID` = the lake's base full; files ordered by **CheckpointLSN**, not FirstLSN; a non-COPY_ONLY full is the new base and must be applied before its differentials | E1 rule 1 (forcing D3 onto a lake that skipped F1 corrupted 3 tables); E6§3.4 (a transaction open across two files gives both the same FirstLSN) |
| 2 | the changed pages | live pages with `m_lsn > W`, W = min(FirstLSN, CheckpointLSN) of the last applied file; never LastLSN | E1 rule 2; E3§7.5 (a fuzzy full's stale pages carry old LSNs; their newer versions in the next diff lie below the full's LastLSN) |
| 3 | the freed pages | from PFS, GAM and `Size`, never from the DCM; a freed extent is never copied | E1 rule 3, §4.2 (TRUNCATE left 800 rows behind without it); E2 R1, R2 |
| 4 | pages of dead allocation units | an AU of type 0 / owner 0 (deferred drop), or gone from the catalog | E2§1.2, R5; E7 s10: without this clause a deferred `TRUNCATE … WITH (PARTITIONS)` leaves all 9,000 rows in the lake (its pages are allocated and absent from the diff; only the AU row tells) |
| 5 | rows from the right pages only | live (R3), level-0 data pages whose header AU is a table's IN_ROW AU; ghosts, stubs and freed slots skipped | E1 rule 4, §4.4; E2 R3, R4 |
| 6 | delete by page in the table the base IAM image names, then insert | pages move between tables, so a page is deleted from the table of the AU that owned it in the previous file: freed or reallocated to another AU → its OLD AU's table only; changed inside its AU → that AU's table | E1 rule 5, §4.3; E2 R5 |
| 7 | the catalog first, from a mirror | base tables with their own locators, updated by the same delete / insert; a differential lacks every unchanged catalog page | E1 rule 6, §4.8; R§6.2 point 5 |
| 8 | LOBs | the in-row root and a fragment map with each value; a value not wholly in the file rebuilt from its nodes in the file plus the lake's old bytes by node RID; root byte-identical = unchanged; refuse when a node is in neither | E1§5 rule 8 (R§6.2 numbers it 7), §4.5 (reuse by `_mssql_rid` failed 10 of 21 steps); E2 R6, R8 |
| 9 | allocation-page state | the lake keeps the latest image of every allocation page; its IAM images name the AU that owned each page in the previous file (need 6) | E2 R5 |
| 10 | deletes by value | every row carries `_mssql_page`; the importer deletes with DuckLake's own `DELETE … WHERE _mssql_page IN (<the table's delete set>)` before the file's new files are registered, and keeps no page LSN or file position anywhere; the export writes in physical order, so a page's rows sit in few files, and the files' min/max on the hex `_mssql_page` prune a delete to them (§ Risks) | owner, 2026-11-05 (R§6.2 "Parquet first", "deletes by value"); research-ducklake-import §5 |
| 11 | row identity | `_mssql_rid`, `_mssql_page` (085 D8) | R§5, E1§3 |
| 12 | ownership after a SWITCH | after the mirror update, a lake row whose AU (columnstore: rowset) now resolves through `sysallocunits.ownerid` → `sysrowsets` to another (object, index) moves to that owner; a change of `numpart` alone means nothing. The key is the AU / rowset, not the table, and nothing is stored per row for it: a row's AU is its page's AU in the base IAM image (a SWITCH leaves IAMs unchanged; a delta-store row's too), a compressed columnstore row's rowset is in its rowgroup key (088 D3) | E7§5: with it all 15 differentials of E7 replay exactly; without it the 5 SWITCH variants leave the rows in the old table (after SWITCH + a one-row UPDATE only the 37 rows of the touched page move, 263 stay) |

## Decisions

- **D1 — the reader never names a lake object.** Lake state reaches it as **files the importer writes**
  (`base :=`, D10) and scalar parameters (`since_lsn :=`); its outputs are rows and files. Reason:
  mssql stays lake-agnostic (research-ducklake-import §8), the same reader serves any importer, and a
  frozen input makes a retry deterministic.

- **D2 — LSNs are `DECIMAL(25,0)`** in `RESTORE HEADERONLY`'s numeric form (`vlf × 10^15 + block ×
  10^5 + slot`), comparable with `msdb.backupset`; `since_lsn` also accepts `'vlf:block:slot'`.

- **D3 — the changed set is `m_lsn > since_lsn` on live pages**, where the importer passes W (need 2).
  Re-processing a page is idempotent (E1 rule 2). Spec 090 adds to it the pending images it applies
  (kind `pending_applied`) and the pages formatted in the tail or rebuilt (`changed` rows with a
  reason) (E6§3.1 step 5).

- **D4 — liveness of a lone differential needs the lake's allocation images.** A differential carries
  an allocation page only when it was written since the base full (R1; pass 2 always re-copies the
  extents at pages 0 and 8 of file 1 and at page 0 of every other file, E2§6). E1 read an absent PFS page as "nothing in its interval was freed", which
  is right for **deleting** (E1§4.2). It is not enough for **reading rows**: a page freed before the
  base full, whose extent another page keeps in the differential, carries its last write's LSN, which
  can exceed W when a long transaction pulled the previous file's FirstLSN early, and its PFS page may
  be absent. Its stale rows would come back. So in lake mode the reader evaluates R3 per interval
  with the file's allocation page when present, else the lake's latest image of it (unchanged since,
  by R1), and refuses to read a differential alone without those images. This is E2 R5's reason to
  keep them. The edge case is reasoned, not observed; a fixture constructs it (task 9).

- **D5 — a differential's catalog = the mirror updated by the file.** The base mirror's rows not on a
  changed or freed catalog page of the file, plus the rows of the file's changed live catalog pages,
  decoded by 085's catalog reader into the model the scan uses. The importer applies the same rows to
  its own mirror from `mssql_backup_catalog(…, since_lsn := W)`. The mirror set is E1§4.8's (the five
  bootstrap tables, `sysseobjvalues`, `sysclsobjs`) plus `sysobjvalues`, `sysidxstats`, `sysiscols`,
  the columnstore tables (088) and E7's partitioning tables (`systypedsubobjs`, `syssingleobjrefs`;
  `sysclsobjs` classes 30 / 31 and `sysobjvalues` valclass 30 are already in): the allow-list of 085,
  listed by `mssql_backup_catalog(path)`. Of `sysobjvalues` it serves only view definitions,
  partition boundaries (valclass 30), and computed-column and default definitions; no module source
  leaves, and the mirror takes `syssingleobjrefs` classes 7, 8, 30 and 31 only (DQ-11, accepted).
  E7 read the partition model of every maintenance state from full + diff overlays this way (0
  mismatches over 15 states).

- **D6 — LOBs travel with their map.** A virtual column `_mssql_lob BLOB` (opaque, serialized by this code with a leading format
  byte — DQ-02, decided) holds, per entry: column id, root bytes, the nodes (file, page, slot, offset,
  length, depth) and, where DQ-01's hybrid keeps them, the raw old bytes — one entry per off-row value of the
  row (LOB root or row-overflow pointer), its nodes in DFS order as E1 stored them. (DQ-02 decided: bytes, not a typed MAP / LIST; DQ-01: the hybrid.) A value not wholly in the
  file is resolved in this order:
  1. its root is byte-identical to a base entry's root → the base value (E2 R8.2; this also covers a
     REBUILD, whose LOB pages are not rewritten, E2 R6; E7 s11: REBUILD PARTITION too, 24 values);
  2. otherwise the hybrid walk (E1§4.5): a node whose page is in the file is read from it (a node
     keeps its page in binary inside the BLOB, and this code compares the decoded page with the
     file's pages); an absent node is found **by node RID** in the base maps: a leaf yields its byte
     range of the old value, an internal node is expanded through its old children, each again from
     the file when present;
  3. a node in neither → refused by name (table, `_mssql_rid`, column, node RID). Spec 090 instead
     keeps the old value and its map when the missing node lies on a page its loaded-differential
     recovery marks stale, and reports that page `stale` with reason `lob` (E6§6.8: dropping the map
     lost 3 values for good); there is no parameter for it.

  **The old value's bytes come from design C (DQ-01, accepted).** The splice in step 2 copies byte
  ranges of the old **stored** form, so the old bytes must be reproduced exactly. The lake keeps the
  text, plus the stored bytes (`raw`) wherever `text_exact` is false. `raw` is kept for `xml` /
  `json` / CLR off-row values only once a fixture shows that a partial update can leave their
  fragments out of a differential; until then a splice over one is refused by name, as in B. The
  three designs weighed:
  - **(A) store the bytes** — E1 rule 8: "Store LOB bytes (or a lossless mapping), not decoded text:
    the splice re-encodes the old value", which is exact for cp1252 ASCII in E1 and not for code-page
    bytes a decoder cannot map. Every off-row value is kept twice in the lake (text and bytes): roughly
    double LOB storage (more for ASCII `nvarchar(max)`, whose UTF-16 bytes are twice its UTF-8 text).
    Never refuses, covers `xml`, `json` and CLR values, and needs no re-encoder.
  - **(B) re-encode, guarded per value** — the first draft: the old bytes are re-encoded from the lake's
    typed value (UTF-16LE, the column's code page, UTF-8, or the bytes of a binary); `text_exact`
    records at decode time whether that reproduces the stored bytes. A splice over a value whose
    `text_exact` is false, or of a type whose stored form is not its DuckDB value (`xml` stored as
    BINXML, `json`, CLR types), is refused by name unless step 1 applies; the table then needs a
    rebase from a later full. No extra storage; values that do not round-trip — an unpaired surrogate
    in `nvarchar(max)` (not representable in a valid UTF-8 `VARCHAR`), a code-page byte with no
    mapping or a best-fit mapping that is not one-to-one, invalid UTF-8 under a UTF-8 collation —
    block that table's incremental import at their next partial update; a re-encoder bug would be a
    silent wrong splice, which the guard reduces to the values it checked.
  - **(C) re-encode, bytes for the exceptions** — 022 D3's `raw BLOB`, chosen: the bytes are stored
    only when `text_exact` would be false, and for `xml` / `json` / CLR off-row values as above.
    Never refuses once those fixtures exist; storage grows with the exceptions; two code paths.

  `lob.parquet` (D10) carries the value as C keeps it: the text, plus `raw` where kept.

- **D7 — the chain check is a function, not importer code.** E1 showed a forced wrong-base apply
  silently corrupts tables; the rule should exist once. `mssql_backup_chain_check` (§ Surface)
  answers from the file's header fields alone. **The fork check fails closed**, in two cases (023 C1
  cites this rule):
  - (a) the file's fork id cannot be read (085 task 3 not done): every apply by LSN is refused, a
    differential or a full applied incrementally alike. The way out is a full imported as a new base
    (rebase, DQ-14), which takes no LSN from the lake's chain: a rebase lets fulls in, but
    differentials stay refused until 085 reads the fork id;
  - (b) the lake has no fork recorded but the file's can be read: the apply is refused, and a rebase
    on a full records the fork.

- **D8 — the evidence of deletes is one function.** `mssql_backup_changed_pages` returns the changed
  pages, the freed runs, and the dead, gone and moved allocation units in one result, each page with
  the AU that owned it in the previous file (`base_au_id`, from the base IAM images), that AU's
  owner in the base mirror, and whether lake rows can be on it (`holds_rows`), and each dead, gone or
  moved AU with its pages in the base IAM image. The importer deletes each page from the table the
  base mirror names (need 6) and finds a SWITCH's rows (need 12), with no format knowledge of its own.

- **D9 — Parquet first: an export function in mssql, the commit in mssql-ducklake** (owner, R§6.2;
  research-ducklake-import §4, §8). `mssql_backup_export` writes the data Parquet itself, in
  parallel and in physical order unless a sort is passed; it registers nothing. mssql-ducklake turns the
  returned rows into DuckLake files in one transaction. **The layout is DuckLake's (owner,
  2026-11-05: follow DuckLake's rules, do not replicate SQL Server):** the files are split by the lake
  table's DuckLake partition values and written in its DuckLake sort order and file size, all passed in
  by the importer (§ Surface: `partition_by`, `order_by`, `target_file_size`); SQL Server partitions
  are only work units. The partition value is computed with the expression DuckLake itself uses
  (`DuckLakePartitionUtils::ApplyPartitionTransform`), bound in the importing session as for any
  DuckLake writer and rendered to SQL by the importer. At the pin that expression computes an
  `epoch_*` transform on a TIMESTAMPTZ's UTC instant and floors TIMESTAMP_NS / TIMESTAMP_TZ_NS to µs
  (`DL/storage/ducklake_partition_data.cpp:224-255`), so no session setting changes it. The calendar
  transforms (`year` / `month` / `day` / `hour`) are DuckDB's own functions (`:263-267`): on a
  TIMESTAMPTZ they are ICU's and follow the session `TimeZone`, and on TIMESTAMP_NS they round to µs
  (E7§3, measured with DuckDB's functions). The export never derives a partition value of its own.
  Before the export exists, the v1 is still Parquet first, in plain SQL (R§6.2: "v1 can be plain
  SQL"): `COPY (SELECT … FROM mssql_backup_scan(…)) TO '….parquet'`, registered with
  `ducklake_add_data_files`, and DuckLake's own `DELETE … WHERE _mssql_page IN …` for the deletes, as
  with the export (need 10). That registration gets the importer's bounded retry on a conflict at
  commit, as its apply does (mssql-ducklake 022 D2.4, 023 D4). Not `INSERT` / `MERGE` into the lake
  (owner, R§6.2; research-ducklake-import §4.3 rejects that option).

- **D10 — side inputs are Parquet files under one `base` prefix, read on a side connection.**
  `base := '<run_path>/<BackupSetGUID>/<run>/base/'` (022 D9's run directory, outside the lake's
  `DATA_PATH` so that DuckLake's orphan cleanup never sees it: by default its parent +
  `mssql_backup_runs/<lake id>/`, set explicitly when `DATA_PATH` is a bucket root, whose parent is
  undefined; any FileSystem path works) holds, by fixed names (each may be a set `name-*.parquet`):

  | file | content | needed for |
  |---|---|---|
  | `catalog/<base_table>.parquet` | the mirror rows, as `mssql_backup_catalog` returns them, with `_mssql_page` and `_mssql_rid` | D5, gone AUs, `au_moved`, each base AU's owner (D8) |
  | `allocation.parquet` | `(file_id, page_id, page_type, page_lsn, au_id, image)`, the latest image of every allocation page; `au_id` is the AU an IAM page tracks | D4, exact freed runs, each page's base AU (D8) |
  | `lob.parquet` | `(object_id, _mssql_rid, column_id, root, nodes, text_exact, value, raw)` for every off-row value of every lake row the apply deletes (rows on changed, freed or dead-AU pages: R§6.2's "rows before"; this includes a REBUILD's old rows, whose pages are freed); `value` the text and `raw` the stored bytes where design C keeps them (DQ-01) | D6 |
  | `pending.parquet` | spec 090: `mssql_backup_pending_pages`'s rows from the previous file | 090 |

  The stale page set is **not** an input: it stays lake state, and the importer checks its healing
  against `mssql_backup_changed_pages` (023 D7). The reader opens a `Connection` on the same database
  instance and reads them with `read_parquet` (the worker-connection precedent:
  research-ducklake-import §8). Rejected: a SQL string run on a side connection (it reads committed
  lake state, not the caller's transaction, and ties the reader to what the string names); a session
  object filled by an aggregate (order-dependent, no precedent). Accepted as drafted (DQ-12); the
  side connection is prototyped first (task 2).

## Surface

```sql
-- 1. the chain: refuse or proceed, and the values to record
SELECT * FROM mssql_backup_chain_check('s3://bak/d5.bak',
    base_backup_set_guid := '…', base_lsn := 52000000012340001,
    last_backup_set_guid := '…', last_checkpoint_lsn := 52000000123450001,
    family_guid := '…', recovery_fork_id := '…');
-- → ok, reason, kind ('full' | 'copy_only_full' | 'differential'), new_base_backup_set_guid,
--   new_base_lsn, since_lsn (W of this file, for the next one), reflected_lsn (its LastLSN, spec 090),
--   already_applied

-- 2. delete and move evidence (D8)
SELECT * FROM mssql_backup_changed_pages('s3://bak/d5.bak', since_lsn := 52000000098760001,
                                         base := '<run>/base/');

-- 3. the mirror (D5) and the allocation images (need 9)
SELECT *, _mssql_page, _mssql_rid FROM mssql_backup_catalog('s3://bak/d5.bak', 'syscolpars',
                                     since_lsn := …, base := …);
SELECT * FROM mssql_backup_allocation('s3://bak/d5.bak', since_lsn := …, base := …);

-- 4a. rows, v1: through the scan
SELECT *, _mssql_rid, _mssql_page, _mssql_lob
FROM mssql_backup_scan('s3://bak/d5.bak', 'dbo', 'orders', since_lsn := …, base := …);

-- 4b. rows, Parquet first: plan the units, export them (any process, any machine)
SELECT * FROM mssql_backup_export_plan('s3://bak/d5.bak', since_lsn := …, base := …);
SELECT * FROM mssql_backup_export('s3://bak/d5.bak', out := 's3://lake/data/',
    units := [3, 4, 5], since_lsn := …, base := …,
    partition_by := MAP {'dbo.orders':  -- epoch_month(order_date) of a TIMESTAMP, rendered (D9)
                         'date_diff(''month'', DATE ''1970-01-01'', order_date)'},
    order_by := MAP {'dbo.orders': 'id'},
    target_file_size := '512MB', parquet_options := MAP {'compression': 'zstd'});
```

`base` (D10) is accepted by every function of 085 too (`mssql_backup_tables`, `_views`, `_catalog`,
`_scan`), so a lone differential is listed and browsed with the catalog the scan uses (D5); 023 asks
`mssql_backup_tables(diff, base := …)` for the file's table list. Spec 090 adds the recovery
parameters `recovery`, `loaded_diff` and `reflected_lsn`, which recover with `base` (its
`pending.parquet`), to the scan, `changed_pages`, `allocation`, `export_plan` and `export`.

**`mssql_backup_chain_check(path, base_backup_set_guid, base_lsn, last_backup_set_guid,
last_checkpoint_lsn, family_guid, recovery_fork_id)`**: the lake's base full and its LSN (the
`differential_base_lsn` a full reports as the base it establishes, E1§3), the last file it applied,
that file's CheckpointLSN, and the database. With NULL arguments it answers for a fresh import (a full
only). Rules, in order:
1. A data backup (full or differential) of the database whose `dbi_familyGUID` = `family_guid`
   (R§3), on the lake's recovery fork: the file's `recovery_fork_id` (HEADERONLY `RecoveryForkID`,
   085) = the argument (023 C1), else refused naming both and the way out, a rebase on a new full
   (DQ-14). The check fails closed (D7): for every apply by LSN, a differential or a full applied
   incrementally, a file whose fork id cannot be read, or a lake that recorded none, is refused with
   that reason, never applied unchecked. `first_recovery_fork_id` and `fork_point_lsn` go into the
   refusal, to name the fork.
2. `backup_set_guid = last_backup_set_guid` → `ok`, `already_applied` (a retry after a commit that
   succeeded unseen, research-ducklake-import §9). This function sees only the last import; older
   ones are the importer's check (023 C2).
3. `checkpoint_lsn > last_checkpoint_lsn`, else refused ("not later than the last applied file").
   CheckpointLSN, not FirstLSN: a transaction open across two files gives both the same FirstLSN
   (E6§3.4).
4. A differential: `differential_base_guid = base_backup_set_guid` and `differential_base_lsn =
   base_lsn`, else refused naming both (R§3, E1 rule 1).
5. A full, not COPY_ONLY: `new_base_backup_set_guid` and `new_base_lsn` are its own. A COPY_ONLY full:
   the base is unchanged (it resets no DCM, E6§2).

It returns `since_lsn = min(first_lsn, checkpoint_lsn)` and `reflected_lsn = last_lsn` of the file
for the importer to record.

**`mssql_backup_changed_pages(path [, schema, table], since_lsn, base [, expand])`**, one row per kind:

| `kind` | rows | rule |
|---|---|---|
| `changed` | one per live page of the file with `m_lsn > since_lsn`, every page type (data, index, LOB, catalog, allocation, IAM) | D3 |
| `freed` | runs `(file_id, first_page, page_count)`, each within one base AU: with `base`, pages live by the base images and not live now (exact); without it, pages not live by the allocation pages the file carries (a superset, harmless for deletes) | need 3 |
| `dead_au` | AUs of type 0 or owner 0 in the file's catalog: a deferred drop, or a big `TRUNCATE`, of a table or of a partition (E7 s10). With `base`, one row per run of the AU's pages in its base IAM image (its IAM is not in the file) | need 4 |
| `gone_au` | AUs in the base mirror and absent from the file's catalog (needs `base`): a dropped table, a merged-away or rebuilt partition, a deferred drop completed (E7 s08, s10b, s11). One row per run of the AU's pages in its base IAM image | need 4 |
| `au_moved` | (needs `base`) an AU, or for a columnstore a rowset, whose rowset's `(idmajor, idminor)` in the file's catalog differs from the base mirror's; AU and rowset ids are unchanged by a SWITCH. Its owner in the base mirror (`from_*`) and in the file (`object_id`, `index_id`, `partition_number`); one row per run of the AU's pages in its base IAM image (the moved rows are the lake rows on them). A columnstore's columnstore rowset: one row per COMPRESSED rowgroup of the base mirror, `page` = its `02` rowgroup key carrying the moved rowset id (088 D3); its delta-store and delete-bitmap rowsets move as AUs, one row per run of their base IAM pages in the `01` form (a delta-store row's `_mssql_page`; no lake row is on a delete-bitmap page). A SWITCH of a partition with an OPEN delta store is unmeasured (088 E11). **Not reported when only `numpart` changed**: that is SPLIT / MERGE renumbering (E7 s06: 670 rows renumbered with no page change), and the partition number is never stored with a row | need 12, E7§5 |
| `dead_rowgroup` | (needs `base`; shape fixed after 088's E11) a columnstore rowgroup of the base mirror that is TOMBSTONE (merged, rebuilt or moved by the tuple mover) or gone in the file, with its `rowset_id`: a compressed rowgroup is one row, `page` = its `02` rowgroup key; a TOMBSTONE's delta store that still owns pages is one row per run of the delta-store AU's pages in its base IAM image, `page` in the `01` form | 088 D3, D7, 023 D9 |

Columns: `kind`, `file_id`, `first_page`, `page_count`, `page` (`VARCHAR`, the `_mssql_page` hex
form of the first page, `01…`, or a columnstore rowgroup's `02…` key, 085 D8), `au_id`,
`rowset_id`, `rowgroup_id`, `object_id`, `index_id`, `partition_number` (the file's owner),
`base_au_id` (needs `base`: the AU whose base IAM
image holds the page — an extent bit, or a mixed-extent single-page slot —, i.e. its owner in the
previous file, by rule 10: only IAM pages on the AU's chain from its first IAM in the base mirror,
only pages live in the base GAM / PFS, and a page two chains claim is a named error; NULL when no
such chain holds it, so no lake row is on it; for the AU kinds, the AU itself), `holds_rows BOOLEAN`
(NULL without `base`: true when lake rows can carry the page as `_mssql_page`, because its base AU is
an `IN_ROW_DATA` AU of a heap or clustered index or a delta store's, or `page` is a rowgroup key;
false for LOB, row-overflow, nonclustered-index and allocation pages), `from_object_id`, `from_index_id`,
`from_partition_number` (the owner of `base_au_id` in the base mirror: the table a page is deleted
from, need 6, which the importer maps to its lake table through its registry; for `au_moved`, the
table the rows move from; `base_au_id` itself serves diagnostics and the SWITCH page lists),
`schema_name`, `table_name`, `page_type`, `page_lsn`, `reason` (`past_size` / `extent_free` /
`page_free` for `freed`). The optional `schema`, `table` keep a `changed` row when its current owner
or its base owner (`from_*`) passes them, so a page reused out of an imported table into a
filtered-out one still deletes its old rows; `freed` and the AU kinds are never filtered, because
pages move between tables (need 6).

`expand := true` returns one row per page instead of runs (`page_count` 1, `page` set). The delete
set of a lake table is

```sql
SELECT page FROM mssql_backup_changed_pages(…, expand := true)
WHERE kind IN ('changed', 'freed', 'dead_au', 'gone_au', 'dead_rowgroup', 'pending_applied')
  AND holds_rows AND from_object_id = <its object id>
```

`pending_applied` is spec 090's kind for an applied pending image: one row per page with the same
owner columns, not also reported as `changed`. The set holds data pages only (`holds_rows`): pages
of an `IN_ROW_DATA` AU of a heap or clustered index, delta-store pages and rowgroup keys. LOB and
row-overflow pages reach their rows through the LOB map (D6), and index and allocation pages hold no
lake rows. A SWITCH's moved pages are the `au_moved` rows with `holds_rows`; they go to the SWITCH
copy (need 12), not to the delete set.

**`mssql_backup_allocation(path [, since_lsn, base])`**:
`(file_id, page_id, page_type, page_lsn, au_id, image BLOB)` for every allocation page the file carries
(file header, PFS, GAM, SGAM, DCM, BCM, IAM); `au_id` is the AU an IAM page tracks (its header), NULL
for the other types. A few pages per 64 MB (E2 R5). Its lake-mode output (`since_lsn`, `base`) is the
images the importer keeps, over the base's, as the next file's `base/allocation.parquet` (D10): an
image it returns replaces the stored one; the stored image of a page the file shows freed, or carries
as a non-allocation page, is dropped; a full replaces the whole set. Rule 10 does not rely on that: an
image no AU's chain reaches attributes no page. After a recovered file (spec 090) it takes 090's
parameters and returns the images of the recovered state, so that every page the lake holds rows of
stays in the IAM chain of the AU whose table holds them.

It runs the **attribution check**, because it builds the images and knows which pages give the lake
rows: every page whose rows `mssql_backup_scan` and `mssql_backup_export` return with the same
arguments (the changed data pages; with 090 also those formatted in the tail or rebuilt, and the
`pending_applied` ones; not a rowgroup key) must lie, live, on its header AU's IAM chain and on no
other, in the images it returns over the base's (rule 10; a differential needs `base` for it, as for
its rows, D4). Otherwise it refuses by name (table, page, AU), and the import then refuses the file:
the next file could not attribute that page. So a missed delete is never silent on either path, rows
from the scan (phase A) or from the export. A full's own images always pass; the check guards the
recovered images (090).

**`mssql_backup_scan(…, since_lsn, base)`** returns the rows of the changed pages only (need 5) and,
with `base`, decodes a differential alone (D4, D5, D6). Without `since_lsn` on a differential with
`base`, it returns every live row the file carries: the importer's choice for a verify mode (E1 rule
10).

**`mssql_backup_export_plan(path [, since_lsn, base, target_rows])`**: one row per unit: `unit`,
`object_id`, `schema_name`, `table_name`, `partition_number`, `au_id`, `rowset_id`, `kind`
(`heap_pages` / `key_range` / `rowgroups` / `changed_pages`), `first_page`, `last_page` or the key
range bounds as text, `est_pages`, `est_rows`. Units are disjoint and each belongs to one rowset: heap
units are ascending page ranges; clustered units are key ranges cut at level-1 index pages, read in
leaf-chain order, so a unit's rows are in key order (research-ducklake-import §5); a SQL Server
partition is its own set of units (research-ducklake-import §7), as a **work unit only** (the files
follow the lake's layout, D9); columnstore units are rowgroups (088); with `since_lsn` a unit is a set
of changed pages of one rowset.

**`mssql_backup_export(path, out, units [, since_lsn, base, partition_by, order_by,
target_file_size, parquet_options, naming, field_ids, max_memory, max_threads])`**: writes each unit's
rows as Parquet through DuckDB's `parquet` copy function, as DuckLake writes its delete files
(`ducklake_delete.cpp:54-135`), one writer per thread.
- Columns: the table's columns plus `_mssql_rid`, `_mssql_page` and, for a table with off-row-capable
  columns, `_mssql_lob` (opaque values, DQ-02: `_mssql_page` hex `VARCHAR`, the others `BLOB`). No
  page LSN is written, in the rows or beside them: which pages changed is decided by the file (D3;
  R§6.2 point 4).
- Attribution check: the export repeats `mssql_backup_allocation`'s check on the pages it writes
  rows from (not a rowgroup key): each must lie, live, on its header AU's IAM chain and on no other,
  in the images `mssql_backup_allocation` returns for the same file and arguments, which the next
  file's `base` will hold (rule 10). Otherwise the export refuses by name (table, page, AU), and the
  import then refuses the file: the next file could not attribute that page, and a missed delete is
  never silent.
- `partition_by`: per table (`'schema.table'`), the SQL text the importer renders from the
  expression DuckLake binds for the lake table's current spec (`ApplyPartitionTransform`, D9),
  evaluated in the calling session (its `TimeZone` and `Calendar` matter only to a calendar
  transform of a TIMESTAMPTZ). The value is computed for every row, whatever the transform, and a
  file holds one value: one open file per (unit, partition value). A written file's value can be
  checked from its min/max of the source column only where the spec is monotone in that column:
  identity, `year`, `epoch_*`, and `month` / `day` / `hour` only beside every coarser calendar field
  of the same column (a `day` beside only `month` wraps every year). `bucket`, and a calendar part
  alone (`month(ts)` puts every January in one value), are checked per row: by the export's own
  computation, and again by the importer's apply, which evaluates the bound transform over each
  manifest file's partition column, a one-column scan that refuses any disagreeing row, so files from
  external workers are verified too (022 D2.3). Absent: the table has no spec.
- `order_by`: per table, the lake table's current DuckLake sort expressions. When they are the
  clustered key in key order and every key column orders in DuckDB as on disk (`OrdersLikeDuckDB`),
  the physical read order already satisfies them and nothing is sorted; otherwise each unit is sorted
  (DuckDB's sort, spilling) before it is written, which scatters a page's rows over files (§ Risks).
  Absent: physical order.
- `target_file_size`, `parquet_options` (compression, row group size, Parquet version): the lake's own
  settings, passed by the importer; files rotate at the size.
- Memory (the cap is the reader's; 085 D17's split gives the writers a quarter of `max_memory`):
  each open file buffers one row group, sized in bytes, not rows. With B = the lake's row group in
  bytes (`parquet_options`' `row_group_size_bytes`, else its `row_group_size` rows × the table's
  estimated row width), the export keeps at most F = max(1, ⌊`max_memory` / (4 · B)⌋) files open,
  runs at most min(`max_threads`, F) units at once, and gives each writer a Parquet
  `ROW_GROUP_SIZE_BYTES` ≤ `max_memory` / (4 · F), less one chunk: DuckDB's writer flushes a row
  group at either bound, checked after each chunk it takes
  (`DD/extension/parquet/parquet_extension.cpp:446-454`), and a chunk closes at the staging budget
  (085 § Resources). A row wider than the estimate makes a smaller row group, never more memory; a
  row that cannot fit even alone (a LOB value of up to 2^31 − 1 bytes) is refused by name (table,
  `_mssql_rid`, column, length; 085 D17's rule). A writer's row group is reserved when its file
  opens; a unit holds one open file per partition value it meets. `max_memory` and `max_threads`
  default to DuckDB's `memory_limit` and `threads`, and a value above them is clamped to them, with a
  WARNING. A unit that needs a file while F are open first closes its own least recently written one
  (more, smaller files, never more memory), and one that holds none itself waits until another unit
  closes one. The bound: the writers' buffers stay under their share, a quarter of `max_memory`,
  whatever the partition spec and the rows' widths (acceptance 8). A sort (`order_by`) is DuckDB's:
  buffer-managed, it spills, and it obeys `memory_limit` and the calling connection's
  `operator_memory_limit`, which mssql-ducklake sets to `max_memory` / 2 on its side connections
  (085 D17), not the writers' share.
- `naming`: the file-name pattern; default `mssql-{backup_set_guid}-o{object_id}-p{partition}-u{unit}-{seq}.parquet`
  (research-ducklake-import §4.3); 022 D4 passes its own, with a run id.
- `field_ids`: write Parquet field ids (DQ-24).

It returns one row per written file: `path`, `object_id`, `schema_name`, `table_name`,
`partition_number` (the unit's SQL Server partition, informational), `partition_value`, `unit`,
`au_id`, `rowset_id`, `row_count`, `file_size`, `column_statistics` (the shape of DuckLake's
`WRITTEN_FILE_STATISTICS`, with min/max of `_mssql_page`, at most 26 characters, which a delete
prunes by, § Risks).

Export guarantees: output in physical order emits a page's rows together, so a page is split only at
a rotation or a partition-value change; nothing is registered anywhere; a retry with the same `units`
and arguments rewrites the same files.

## Rules the reader applies in lake mode

1. **Changed**: live ∧ `m_lsn > since_lsn` (D3).
2. **Liveness**: per data file and per interval, the file's allocation page if present, else the
   base image (D4); `Size` from the file's file header (always present: extent 0).
3. **Freed**: `live(base) ∧ ¬live(now)`, as runs.
4. **Catalog**: D5; a table in the file's catalog with no rowset change keeps the mirror's layout.
5. **Rows**: 085's record rules over the D5 layout, per rowset; columns added since the record was
   written read the `sysseobjvalues` default by **hbcolid** (E1§4.8).
6. **LOBs**: D6.
7. **What needs no rule** (E1 rule 11, E2§3, E7§5): REBUILD (a new rowset and IN_ROW / ROW_OVERFLOW
   AUs; the old pages show as freed, the new ones as changed; the LOB_DATA AU is kept and its values
   are found by root identity, D6 step 1), REBUILD PARTITION likewise; a small TRUNCATE (same AU,
   pages freed); a big TRUNCATE (a new IN_ROW AU under the same rowset, the old one `dead_au`), per
   table or per partition; SPLIT and MERGE of populated ranges (rows physically moved: changed and
   freed pages); SHRINKFILE (`Size` shrinks; pages past it freed; a shrink renumbers every heap RID, a
   clustered index keeps its slots, E2§2); a page reused by another table (changed; deleted from its
   `base_au_id`'s table, inserted under its header AU's).
8. **Heap RIDs** change on a heap REBUILD and a shrink (E1 rule 9): a consumer that tracks
   `_mssql_rid` sees a delete and an insert. Documented; the lake does not care, it is page-based.
9. **Ownership** (need 12, E7§5): `au_moved` compares, for every AU of the base mirror (every rowset
   of a columnstore), its owning `(idmajor, idminor)` through `sysallocunits.ownerid` → `sysrowsets`
   in the base mirror and in the file's catalog. The importer applies it after the page rule; rows
   inserted from changed pages already carry their new owner (E7: catalog, then the page rule, then
   ownership). The moved rows are the source table's rows on the AU's pages in the base IAM image
   (columnstore: on the columnstore rowset's rowgroup keys, and on its delta stores' base IAM pages),
   minus the changed pages, which the page rule handles.
10. **Old owner**: a page's `base_au_id` is the AU whose base IAM image holds it, through the extent
    bitmap of the IAM page covering its GAM interval or one of the IAM's single-page slots (E2 §
    Layouts: IAM header record at 96, slots at +46, bitmap at 194). Only IAM pages on the AU's chain
    count: from its first IAM (`sysallocunits.pgfirstiam` in the base mirror) through each image's
    next pointer; an image no chain reaches (an IAM page since freed or reused, kept by mistake) is
    ignored. Only pages live in the base GAM / PFS get an owner. A page two chains claim is a named
    error. It names the owner of every page that held lake rows: on every allocated page the header AU
    equals the AU the DMV reports from the IAMs (E2 R4: 10,473 pages, 0 mismatches), and the
    attribution check (`mssql_backup_allocation`, repeated by the export) keeps every page the lake
    gets rows of on its header AU's chain, and on no other, in the images the lake keeps.

## Resources (085 D17)

Beyond the scan's (085 § Resources):

| component | grows with | home or bound |
|---|---|---|
| `mssql_backup_changed_pages`' output, the delete sets, the moved-page lists | changed pages | streamed, AU by AU in page order; the function holds no set of pages. The sets exist only as the importer's Parquet files and DuckDB tables (mssql-ducklake 023 D4, `delete_pages.parquet`) |
| the base inputs (D10): the mirror rows, the allocation images, the LOB maps | the database's objects, its size (images: ~0.02 %, E2 R5), the LOB rows on deleted pages | read with `read_parquet` on the side connection into buffer-managed storage, the LOB maps indexed by node RID there; a large `lob.parquet` after a heavy LOB change spills (§ Risks). Rule 10's chain walk and the attribution check work over these images |
| the export's writers | units, partition values | F open files and min(`max_threads`, F) running units, from `max_memory` / 4, each row group capped in bytes (§ Surface, `mssql_backup_export`) |
| a sort under `order_by` | a unit's rows | DuckDB's sort, which spills; DuckDB's limits and the connection's `operator_memory_limit` (`max_memory` / 2 on the importer's side connections), not the writers' share |

## Partitioning (E7)

What a differential carries for each partition-maintenance operation (E7§4), and the output that shows
it. The general rules plus the ownership rule replay all 15 of E7's differentials exactly (E7§5):

| operation | data pages in the diff | catalog change | shown by |
|---|---|---|---|
| SWITCH out, in, partition → partition, columnstore (s01, s02, s14, s12) | none | the two rowsets swap `idmajor` (and `numpart`); AU → rowset unchanged; page headers and IAMs unchanged | `au_moved` (a columnstore by rowset) |
| SWITCH, then DML on the switched rows (s03, s13) | only the DML'd pages, under their unchanged AU (s13: 1 of 10) | as SWITCH, plus `rcrows` | `changed` + `au_moved` |
| NEXT USED; SPLIT or MERGE of empty ranges (s04, s05, s07) | none | a destination row; a boundary added or removed; empty rowsets and AUs created or removed | catalog pages only |
| SPLIT, populated, RANGE RIGHT (s06) | the moved rows on new pages of a new rowset; the old rowset's emptied pages freed | boundary ids renumbered; later rowsets `numpart + 1` | `changed`, `freed` (no `au_moved`: `numpart` only) |
| MERGE, populated, RANGE RIGHT (s08) | the right partition's rows on new pages of the surviving left rowset; the dropped rowset's pages freed | its rowset and AUs removed; later rowsets `numpart − 1` | `changed`, `freed`, `gone_au` |
| TRUNCATE … WITH (PARTITIONS), small (s09) | none; pages freed | same AU id, `pgfirst` / `pgroot` / `pgfirstiam` → 0, `rcrows` → 0 | `freed` |
| TRUNCATE … WITH (PARTITIONS), big (s10, s10b) | none; pages still allocated, freed later | old AU type 0 / owner 0, a new empty in-row AU under the same rowset; later the old AU row is gone | `dead_au`, then `gone_au` + `freed` |
| REBUILD PARTITION (s11) | every row on new pages | new rowsets and in-row AUs for that partition number; the LOB_DATA AU kept, `ownerid` → the new rowset | `changed`, `freed`, `gone_au`; LOB values by root (D6 step 1) |

Recognising an operation by name is needed only for messages and logs, never for correctness (E7§5).
The partition metadata (functions, schemes, boundaries) is 085's `partitioning` column; mssql-ducklake
proposes a DuckLake spec from it once, when it creates the lake table, and nothing after that follows
SQL Server (owner, 2026-11-05; 022 D5).

Not covered by E7, so fixture-gated before it is trusted (E7§7): RANGE LEFT SPLIT / MERGE (which side
gets the new or dropped rowset), an `ALL TO` scheme after a SPLIT, SWITCH … WITH TRUNCATE_TARGET, a
partitioned CCI's SPLIT / MERGE (E11), several operations inside one differential (the page rule
composes, E1; the ownership rule compares final owners only, E7§5).

## Security and hardening

- Base files are as untrusted as backups: their schemas are checked exactly (names, types, a
  `format` key-value in the Parquet metadata written by the reader's own outputs and checked on
  input); node maps must be well-formed trees (depths consistent, offsets and lengths cover the
  parent's range, no cycles); allocation images must be 8 KB pages of an allocation type whose
  header names the claimed page, and an IAM image's `au_id` the AU its header names; an IAM chain
  that loops is a named error (rule 10); a mirror row on
  a page the file also carries is ignored for that page (the file wins).
- A base value larger than its map's total length, or a map whose leaves do not tile the value, is a
  named error.
- The export writes only under `out`, with names it generates; `units` out of range are refused.
- Fuzz harness: `fuzz_backup_base_inputs` (node maps and allocation images through the validators;
  the Parquet layer is DuckDB's).
- Pads, ghosts and non-live pages stay out of every output (085 invariants), including allocation
  images: those are allocation pages only.

## Testing and acceptance

**A reference importer for the tests** (`test/backup/ref_importer.sql`, not a product): plain SQL over
this API into a DuckDB database standing in for the lake — the chain check, delete by page in the
table the base IAM image names (`base_au_id`), insert from the scan, the mirror, the allocation
images, the LOB base — E1's `lake.py` algorithm with need 6's delete. It is the oracle of the
contract; mssql-ducklake owns the real importer.

**Chains** (oracle lane, generated from E1's scripts `experiments/E1/sql/`): chain A F0 → D1 → D2 → F1
(incremental) → D3 → D4 → D5 → F2 (incremental) → D6 → D7; A' with a no-change diff; B with D1
skipped; C F1 fresh → D4 (D3 skipped); D F2 fresh → D6. At every step every table equals RESTORE of
that chain (multiset, LOBs by SHA-256), locators included; lakes reached by different routes are
identical, mirror included (E1§3). E2's cases are added as chains: DROP and TRUNCATE (small and
deferred), SHRINKFILE, heap and clustered REBUILD, ALTER ADD / DROP / ALTER COLUMN, `.WRITE` at every
shape of E2§5 (20 KB to 6 MB), a second data file, a striped differential. E7's maintenance steps
are added as fifteen one-operation chains (`experiments/E7/sql/b00`–`b02`, `s01`–`s14`: a fresh full,
one operation, a differential): every SWITCH variant, NEXT USED, SPLIT and MERGE (empty and populated),
TRUNCATE PARTITIONS (small, deferred, and 30 s later), REBUILD PARTITION; the reference importer
applies the ownership rule (need 12) and every table equals RESTORE as a multiset of (partition number
from the rowset's current `numpart`, row), as E7's `e7lake.py` checked.

**Counterfactuals become assertions on the reader's outputs** (E1§4): the TRUNCATE'd pages of D2 are
in `freed`; a page that changed owner is `changed` in D2 with the truncated table's AU as
`base_au_id`, and its old rows go through delete by page in that table; deleting every page from the
table its `base_au_id` names gives the same lake as deleting it from every table, which E1 and E7
replayed; `since_lsn` = LastLSN misses the stale pages of a fuzzy full (E3§7.5, a fixture from E3's workload);
a `.WRITE` value resolved through D6 step 2 equals the server's, and reuse by `_mssql_rid` would not;
the five SWITCH variants report `au_moved` for exactly the switched AUs (columnstore: rowsets), and
the reference importer without the ownership rule fails them as E7§5 measured (560 rows left in
`m_ci`; after SWITCH + one UPDATE, 263 rows behind); without `dead_au`, s10 keeps 9,000 truncated rows;
SPLIT / MERGE renumbering reports no `au_moved`; a base image of an IAM page the file freed, left in
`base`, attributes no page; `mssql_backup_allocation` over images that leave a page the lake gets
rows of off its header AU's chain refuses by name, and so does an export over them.

**D4's edge case**: a fixture that frees a page before a full while another page of its extent keeps
changing, with a long transaction open across the full, then a differential whose PFS page for that
interval is absent: the page's rows must not return, and without `base` the scan refuses.

Version matrix: the chains on 2017 / 2019 / 2022 / 2025.

Acceptance:
1. Every chain step of every version equals RESTORE; route-independent lakes are identical.
2. `mssql_backup_chain_check` refuses the wrong-base and out-of-order files of E1§3 and accepts E6's
   overlapping D5 (same FirstLSN as F3, later CheckpointLSN).
2a. E7's fifteen differentials replay exactly through the reference importer, ownership rule included.
3. `DELETE … WHERE _mssql_page IN (…)` with each table's delete set (the `changed`, `freed`,
   `dead_au`, `gone_au`, `dead_rowgroup` and, with 090, `pending_applied` pages with `holds_rows`
   whose `from_object_id` is that table) removes exactly the lake rows the file no longer has there,
   on every chain step, and the `au_moved` pages name exactly the rows a SWITCH moves (E7's five
   variants).
4. A differential alone without `base` is refused by name; with `base`, it decodes.
5. Every D6 refusal names table, row, column and node.
6. An export retried with the same units produces identical files (bytes, given the same writer
   settings).
7. With `partition_by`, every file's `partition_value` equals DuckLake's own value for each of its rows
   (computed by the same expression in the same session); E7's `datetime2(7)` edge rows (b − 100 ns,
   b − 500 ns) land in the file DuckLake would put them in, under an `epoch_*` spec (floored) and a
   calendar one (rounded) alike, and a `datetimeoffset` table gets the same `epoch_*` values under
   two session TimeZones.
8. An export of a wide table under a spec with many values per unit, with a small `memory_limit`
   and several `threads`: the writers' buffers stay under their share, a quarter of that limit
   (DuckDB's memory accounting), and every row is written; only the file count grows. With a
   `max_memory` below the limit, under a quarter of `max_memory`; a table whose LOB values are far
   wider than its estimated row width stays under it too, in smaller row groups.
9. The 20 GB full of 085 acceptance 13, exported and imported by the reference importer with
   `memory_limit = '1GB'` and every thread, completes: the peak memory DuckDB accounts
   (`duckdb_memory()`, the buffer manager's used bytes) stays under the limit, and every table equals
   RESTORE. With `max_memory := '512MB', max_threads := 2` on the export likewise: the memory it
   reserves (`duckdb_memory()`'s `EXTENSION` tag) stays under its half, 256 MB (085 D17), at most two
   units at a time.

## Tasks

**PR 1 — the incremental scan path (v1 importable)**
1. LSN type and parsing; `since_lsn` on scan and catalog — 1.5 d
2. Base inputs: side connection, `read_parquet`, schemas, validators — 3 d
3. Lake-mode liveness with base allocation images; the catalog from mirror ⊕ file — 4 d
4. `mssql_backup_changed_pages`: changed, freed runs, dead and gone AUs, `au_moved` (E7's
   ownership comparison); each page's `base_au_id` from the base IAM chains (the walk from each AU's
   first IAM, liveness in the base images, the double-claim refusal), `holds_rows`, the base pages of
   every dead, gone and moved AU, `expand` — 6 d
5. `mssql_backup_allocation`: lake mode, the attribution check (task 4's chain walk over the
   returned images) — 1.5 d
6. `_mssql_rid` of base tables; `mssql_backup_catalog` with `since_lsn` — 1 d
7. The hybrid walk over 085's plain `_mssql_lob` (085 task 20, DQ-05), root identity, `text_exact`,
   refusals — 4 d
8. `mssql_backup_chain_check` — 1.5 d
9. Reference importer, E1 / E2 chains and E7's fifteen maintenance chains in the oracle lane,
   counterfactual assertions, D4's fixture, lake-vs-lake identity — 7 d
10. `fuzz_backup_base_inputs` — 1 d

**PR 2 — the export**

11. `mssql_backup_export_plan`: heap ranges, clustered key ranges from level-1 pages, partitions,
    changed sets — 3 d
12. `mssql_backup_export`: parallel Parquet writers, rotation, naming, statistics rows, retry
    determinism; `partition_by` and `order_by` (the sort when physical order does not satisfy it),
    `parquet_options`; the attribution check repeated on the written pages; the memory cap on open
    files, running units and row-group bytes from `max_memory` / `max_threads` (085 D17's split), with
    acceptance 8's and 9's tests — 7 d

**Across PRs**

13. Docs (README, `DATAMODEL.md`: the incremental flow and its diagram, CLAUDE.md, CHANGELOG) — 1 d

**Total ≈ 41.5 days, 8 weeks** (PR 1 ≈ 30.5 days; PR 2 ≈ 10 days; docs 1 day; 37 before E7's
ownership rule and the DuckLake layout parameters joined tasks 4, 9 and 12, 40 before the IAM chain
walk and the attribution check joined tasks 4, 5 and 12, 41.5 before the export's memory cap joined
task 12, 42 before a day of task 7, the plain `_mssql_lob` output, moved to 085 task 20, DQ-05, 41
before `max_memory` / `max_threads` and acceptance 9 joined task 12).
R§8 did not cost the reader's incremental API separately (phase 2's 3–4 weeks are mssql-ducklake's
import).

## Risks

- **D4 rests on reasoning** about allocation pages absent from a differential; the fixture is the
  check. If R1's "copied iff written since the base" fails for an allocation page, D4 still holds
  because it uses the base image only when the file has none.
- **Old LOB bytes** follow design C (D6; DQ-01, accepted). `raw` for `xml` / `json` / CLR off-row
  values waits for a fixture that shows a partial update leaving their fragments out of a
  differential; until then a splice over one is refused by name.
- **Delete pruning: one check left.** DuckLake builds no file filter for a `BLOB` column
  (`DL/storage/ducklake_metadata_manager.cpp:1764-1765`). mssql-ducklake measured it on 5 files, each
  holding a distinct page range: a BLOB `=` / `IN` / `IN (SELECT …)` / `BETWEEN` read 5 of 5 files,
  the same values as BIGINT 1 of 5, as hex VARCHAR 1 of 3; HUGEINT is not pruned at all. So
  `_mssql_page` is hex `VARCHAR` (085 D8), and its min/max per file prunes a delete. Files written in
  physical order (the default) hold contiguous pages, so a changed page touches few files. A lake
  sort order (022 D5 / DuckLake `SET SORTED BY`; `order_by` not satisfied by physical order) scatters
  a page's rows over many files and makes a delete scan more of them — a cost, never a wrong row;
  the importer proposes no such sort (DQ-25, accepted), but a user may declare one. DuckLake runs the
  file filter as string comparisons in the metadata database
  (`DL/storage/ducklake_metadata_manager.cpp:1693-1745`). On a SQL Server catalog mssql-ducklake
  stores every `VARCHAR` column, the min/max statistics columns included, under
  `Latin1_General_100_BIN2_UTF8`, which compares by code point, so fixed-width uppercase hex orders
  as bytes; its comment says this is what makes DuckLake's pushed min/max prune correctly (its
  `src/include/mssql_metadata_manager.hpp:165-168`, `src/mssql_metadata_manager.cpp:161`,
  `src/mssql_catalog_shape.cpp:833-847`), and a test keeps it. To verify (mssql-ducklake's 022 task
  11, 023 D4 / task 6), after N differentials and after a `merge_adjacent_files`: a
  `DELETE … WHERE _mssql_page IN (SELECT …)` prunes the VARCHAR column through DuckDB's dynamic join
  filter, as measured for BIGINT (if not, the expansion passes the delete set as constants: an `IN`
  list cut into chunks, with `BETWEEN` bounds per contiguous range).

  `_mssql_page` keeps its statistics: never in `skip_stats_columns`, and at 26 characters at most
  under mssql-ducklake's `stats_length` (1024 by default, its spec 018).
- **The side connection** inside a running query is a precedent in DuckLake and duckdb-iceberg, not
  yet in this extension: prototype it first (task 2).
- **Base files can be large** after a heavy LOB change (they carry every deleted row's LOB values);
  they are read into buffer-managed storage and spill (§ Resources).
- **Second GAM interval** (files over 4 GB) and **SHRINKFILE … EMPTYFILE** were not exercised
  (E2§10).
- **Columnstore** under a differential is unmeasured (088 E11); its units, the delete-bitmap
  output (newly deleted tuples by rowgroup key and `_mssql_rid`) and the SWITCH of a partition with
  an OPEN delta store wait for it.
- **DuckLake compaction** and `CHECKPOINT` move rows between files; that changes nothing the
  importer keeps, which refers to no file position, only the delete cost above when a compaction
  re-sorts. A compaction committed during an import that merges or rewrites a file the import
  deletes from makes the import's commit conflict. In autocommit `ducklake_mssql_backup_commit` runs
  validate, DDL, DELETEs, the SWITCH copy and apply in one transaction on a side connection and, on a
  conflict, retries them on the export's files up to N times (N = the importer's
  `import.conflict_retries`, 3 by default; N+1 attempts); inside an explicit transaction the COMMIT
  fails and a re-run resumes from the export (022 D1, 023 D11).
- **The export's files wait unregistered** in the lake's `DATA_PATH` until the importer's commit.
  DuckLake's orphan cleanup, which every `CHECKPOINT` runs (`DL/storage/ducklake_checkpoint.cpp:21-24`),
  deletes unregistered files older than `delete_older_than` (2 days by default,
  `DL/functions/ducklake_cleanup_files.cpp:56`). The importer warns when an export may outlive it and
  refuses to register a file its manifest names that is gone (022 D9).
- Decided ([decisions.md](../085-backup-reader-core/decisions.md)): old LOB bytes by design C (D6;
  DQ-01); opaque service columns, `_mssql_lob` a `BLOB` and `_mssql_page` hex `VARCHAR` (DQ-02);
  Parquet side files under `base`, read on a side connection (D10; DQ-12); the export in mssql (D9;
  DQ-18).

## References

- RESEARCH.md §5 (row identity), §6.2 (the import: one snapshot per file, Parquet first, deletes by
  `_mssql_page`, the operating model, why a differential needs nothing but itself and the lake, what still
  needs older bytes, prototype rules 1–8).
- research-ducklake-import.md §4 (export then one commit), §5 (deletes by `_mssql_page`, the delete
  set, physical order, the cost of a delete), §7
  (partitioning), §8 (where things live, worker connections), §9 (pipelines, side state).
- E1 short answer, §2 (lake layout), §3, §4.2–§4.5, §4.8, §5 rules 1–11.
- E2 §0 R1–R11, §1–§5, §9.
- E3 §7.5. E6 §3.1, §3.4, §6.5–§6.8.
- E7 §3 (DuckDB's transform caveats, measured with DuckDB's own functions; DuckLake's `epoch_*`
  differ, D9), §4 (each maintenance operation in a differential), §5 (the general rule, the
  ownership rule, the lake replay), §7 (not covered).
- 085 D4, D8, D9, D17; 088 D3, D7; 090 (pending pages, stale pages, recovery in lake mode).
