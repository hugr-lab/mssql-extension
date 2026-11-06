# E3: Fuzzy full backups read without log replay

Date: 2026-11-05. Server: SQL Server 2025 RTM-CU8 (17.0.4075.5), container `mssql-v2test`. T-SQL went only through
`tools/sql.sh` / `tools/sqlh.sh`; files came out with `docker cp` only. Scripts are in `experiments/E3/`. Research only;
nothing here is committed.

## Short answer

- **How wrong is a reader that takes the page copies as they are?** It shows the database roughly as of the
  backup's `CheckpointLSN`, dirty reads included. Two kinds of error dominate:
  - committed changes made after `CheckpointLSN` are missing (0.6–3 s of work here);
  - changes of transactions still open at `LastLSN`, written before `CheckpointLSN`, are visible.

  Under moderate load that came to 0.2–0.4% of the rows of the busy tables, 1.5–1.7% of the LOB table, and up to
  12% of an append-only table. Under heavy load it was 1–2%, 10.6% and 20%. Torn splits (a row twice or not at all)
  and torn LOBs are rare: 64 and 1 differing keys in 7.8 M rows compared.
- **The proposed flag "page LSN > CheckpointLSN" does not work.** Table-level recall is 0.42. The stale pages carry
  *old* LSNs, because they were copied before they changed. What the flag does catch well is the rare tears (63 of 65).
- **The flag that works comes from the log tail.** Flag the table when the tail holds either:
  - a committed record on one of its pages with an LSN above that page's copy; or
  - a record of a transaction still open at `LastLSN`.

  Over 54 table-runs this flag had precision 1.00 and recall 1.00. At difference level it covers 100% of the
  committed-change errors and tears, and 99.1% of the uncommitted ones.
- **A simple page redo from the tail** (7 operation types, open transactions skipped) removes 94–97% of the
  committed-change errors (99.9–100% of the rows that were simply stale). It removes none of the uncommitted ones;
  that needs undo.

## 1. Setup

**Database `bk_e3`.** FULL recovery, ADR off, no RCSI or snapshot, about 600 MB used:

| table | shape | rows | role |
|---|---|---:|---|
| `t_heap` | heap + unique NC index on `id` | 500 k | heap inserts, updates that grow rows (forwarding), deletes |
| `t_ci` | clustered PK on `id`, sequential | 500 k | appends at the end, growing updates |
| `t_split` | clustered PK on a scrambled key `k = id·2654435761 mod 2³²`, rebuilt FILLFACTOR 100 | 250 k | every insert lands mid-index: page splits |
| `t_lob` | clustered PK; `doc varchar(max)` (40% in-row, 60% 8–40 KB off-row), `ovf varchar(5000)` (row-overflow) | 10 k | whole-LOB updates, inserts, deletes |
| `t_dlog` | heap, insert only | grows | one row per committed delete |
| `t_static` | clustered | 5 k | control: never touched |
| `t_open_only` | clustered | 5 k | control: touched only by the open transaction (b), only before the backup |
| `t_pre_commit` | clustered | 5 k | control: committed changes after (b) began, before the backup |
| `t_mid_only` | clustered | 5 k | control: touched only by (c) before the backup; (c) commits during it |

Every row carries `ver`, `tag` and `ts`. The tag records the writer: `INIT`; `SHRT` / `SRBK` for a short
transaction committed / rolled back; `OPRE` / `ODUR` for the open transaction before / during the backup;
`MPRE` / `MDUR` for (c) before / during; `PCOM` for the pre-backup commits.

**Workloads.** All jobs start together. Each stops on its own, and every job stays under the helper's 30 s query
timeout.

- **(a)** Three sessions run short transactions, each on disjoint ids (`id % 4 = s`). The mix is 30% inserts into
  heap, ci and split; 35% updates to random lengths; 15% deletes, each logged in `t_dlog`; 13% LOB updates; 4% LOB
  inserts; 3% LOB deletes. 5% of the transactions roll back. The pause between transactions is 5 ms (heavy run:
  none). The sessions stop 1 s after the backup ends.
- **(b)** One transaction begins about 3 s before the BACKUP:
  - OPRE: 300 updates, 100 deletes and 200 inserts per busy table; 60 LOB updates and 40 LOB inserts; all of
    `t_open_only`'s changes;
  - ODUR: then one change per table every 30 ms for as long as the BACKUP is visible in `sys.dm_exec_requests`;
  - it rolls back 2 s after the backup ends.
- **(c)** One transaction:
  - first the committed `t_pre_commit` changes, then BEGIN;
  - MPRE changes: `t_mid_only` plus the busy tables;
  - it waits until `percent_complete` ≥ 40 (runs 5/6: 95 and 100);
  - MDUR changes, then COMMIT.
- **BACKUP** starts 3 s in:
  `WITH INIT, FORMAT, CHECKSUM, COMPRESSION (ALGORITHM = ZSTD, LEVEL = HIGH), BUFFERCOUNT = 1, MAXTRANSFERSIZE = 65536`.
  Uncompressed, the database backed up in 0.5 s (about 1 GB/s). This setting stretches it to 2.8 s quiet and
  4–8 s under load, and gives 25–75 MB files. The decompressed stream is the same MTF stream as an uncompressed
  backup (research-experiment §5).

**Truth.** The same file is restored as `bk_e3_restored` (RESTORE … WITH MOVE). Every table is dumped as one
canonical string per row: `CONCAT_WS` of the values, MD5 of the filler, DATALENGTH + SHA1 of the LOBs, and
`%%physloc%%`. The reader (`e3lib.py`, built on `tools/pages.py` and `tools/bakz.py`) builds the same strings from
the file alone. Multisets are compared per table, and each differing key is classified by the tags involved and by
the log tail.

**Control.** Run 0 is a quiet COPY_ONLY backup. The reader matches the restore exactly: 0 differences in all 9
tables in all three page-selection modes, LOBs byte-exact. Over the fuzzy runs `t_static`, `t_pre_commit` and
`t_mid_only` never differed either.

| run | load | backup | duration | commits in tail | of them after CheckpointLSN | pass-2 units |
|---|---|---|---:|---:|---:|---:|
| r1 | 5 ms pause | full | 3.9 s | 4146 | 447 | 19169 |
| r2 | 5 ms (repeat) | full | 3.8 s | 3722 | 396 | 18912 |
| r3 | no pause | full | 7.7 s | 29630 | 6956 | 64727 |
| r4 | 5 ms | COPY_ONLY | 3.9 s | 2971 | 249 | 18050 |
| r5 | 5 ms, (c) commits at 95% | full | 4.3 s | 2843 | 311 | 19489 |
| r6 | 5 ms, (c) commits at 100% | full | 4.0 s | 2983 | 290 | 19121 |

## 2. How the backup is really taken (measured)

| step (r1 wall clock) | evidence |
|---|---|
| 20.171 BACKUP starts. Two checkpoints follow at once (≈20.19 and ≈20.31) | `LOP_BEGIN_CKPT` records in the tail, timestamped by the `ts` of rows inserted next to them |
| **Pass 1**: every allocated extent. It is read **from disk**, not from the buffer pool, so it holds each page as of the starting checkpoint | the largest page LSN in pass 1 lies just after that checkpoint, in all 6 runs, although pass 1 lasts 2.5–3.5 s under writes |
| ≈22.87 **a third checkpoint = the header's `CheckpointLSN`** | the last `BEGIN_CKPT` in the tail equals HEADERONLY `CheckpointLSN`, every run |
| **Pass 2**: the extents changed during pass 1, as of that checkpoint | pass-2 extents = DCM bits for a normal full (2402 of 2403). Under COPY_ONLY, pass 2 is 2257 extents while the DCM holds 11285, so SQL Server tracks pass 1's changes separately. Pass 2 was 18–20 k pages under moderate load and 65 k (73% of the database) under heavy load |
| ≈23.70 `LastLSN`: log copied from `FirstLSN` (here the open transaction's BEGIN) to `LastLSN` | MQTL 29–37 MB (r3: 236 MB) |

So, with the latest copy of every page taken, the data equals the on-disk image at `CheckpointLSN`. That image
includes whatever was dirty and uncommitted at that moment. In the window `CheckpointLSN → LastLSN`
(0.6–0.8 s at moderate load, 3.0 s heavy) only lazily written pages change: 6–69 pages per run carry
`m_lsn > CheckpointLSN`, and they are where the tears happen. The window is the pass-2 copy plus the log copy, and
pass 2 grows with the write rate during pass 1.

Reading only pass 1 (ignoring the second MSDA) doubles the error: r1 goes from 3839 to 6173 differing keys, r3
from 19819 to 40791. Pass 2 must overlay pass 1 (latest copy wins), as `pages.py` already does.

## 3. Measured error, no replay

Differing keys per table (the key is `id`, or `k` for `t_split`, or `lid`); `+`/`−` are rows extra / missing in the
reader:

| run | t_heap | t_ci | t_split | t_lob | t_dlog | t_open_only | 3 controls |
|---|---|---|---|---|---|---|---|
| r1 | 999 (0.20%) +807 −672 | 1011 (0.20%) | 999 (0.40%) | 153 (1.53%) | 127 (12.0%) | 550 (11%) | 0 |
| r2 | 978 (0.20%) | 978 (0.20%) | 980 (0.39%) | 173 (1.72%) | 89 (4.0%) | 550 | 0 |
| r3 heavy | 5211 (1.03%) +3494 −4091 | 5246 (1.04%) | 5165 (2.03%) | 1093 (10.6%) | 2554 (20.4%) | 550 | 0 |
| r4 COPY_ONLY | 949 (0.19%) | 949 (0.19%) | 958 (0.37%) | 159 (1.53%) | 118 (0.7%) | 550 | 0 |
| r5 | 1024 | 1024 | 1017 | 159 | 91 | 550 | 0 |
| r6 | 1021 | 1071 | 1069 | 158 | 111 | 550 | 0 |

Classes (all 6 runs):

| class | keys | what happened |
|---|---:|---|
| committed change missing: insert / update / delete | 9429 / 8026 / 3097 | committed after `CheckpointLSN` (window). A deleted row shows as a zombie, an update as the old version, an insert as nothing |
| uncommitted visible: (b)'s OPRE inserts / updates | 5040 / 7560 | written before the backup and flushed by its checkpoints: **all** of them visible in every run |
| uncommitted visible: (b)'s ODUR inserts / updates | 1755 / 1862 | those made before `CheckpointLSN` are visible (r1: 85 of 124 iterations); later ones are not |
| uncommitted visible: (b)'s deletes (row hidden) | 2100 | ghosted by the open transaction; RESTORE's undo brings them back |
| a short transaction that rolled back (1668 aborted transactions in the tails) | **0** | never caught dirty at a checkpoint |
| (c) committed before `CheckpointLSN` (r1–r5) | **0** | all its rows correct, including those of the MDUR phase during pass 1 |
| (c) committed inside the window (r6) | 250 rows | changes made after `CheckpointLSN` (MDUR: 50 heap, 100 ci, 100 split rows) missing; MPRE correct |
| torn split / move: row twice | 49 | 44 the same version twice, 5 old + new (heap forwarding / split of a row updated mid-move) |
| torn split / move: row lost | 15 | moved to a page that was copied before the move |
| torn LOB | 1 | in-row root flushed after `CheckpointLSN`; its fragment page is still an older DATA page in the backup |
| LOB value differs with the in-row part current | 0 | — |

The three requested workloads, in one line each:
- **(a)** loses exactly the commits of the last 0.6–3 s.
- **(b)** shows everything the open transaction had written before `CheckpointLSN`.
- **(c)** is correct if it commits before `CheckpointLSN`, and partially missing if it commits inside the window.

## 4. The flags

Definitions:

- **(i)** page LSN > `CheckpointLSN`.
- **(i0)** page LSN > the first checkpoint of the tail.
- **(ii)** page LSN > `FirstLSN`.
- **(iii)** a transaction open at `LastLSN` has a record on the table.
- **(iv)** a committed record in the tail is newer than the page copy (or the page is not in the backup).
- **(ivc)** a committed record on the table with LSN > `CheckpointLSN`.
- **(v)** (iii) ∪ (iv).

Pages are mapped to tables by allocation unit: the page header in the backup, or the AU in the log record.

Table level, 6 runs × 9 tables; *affected* = any differing key:

| flag | TP | FP | FN | TN | precision | recall |
|---|---:|---:|---:|---:|---:|---:|
| (i) page LSN > CheckpointLSN | 15 | 0 | 21 | 18 | 1.00 | **0.42** |
| (i0) page LSN > first (backup-start) checkpoint | 30 | 2 | 6 | 16 | 0.94 | 0.83 |
| (ii) page LSN > FirstLSN | 36 | 12 | 0 | 6 | **0.75** | 1.00 |
| (iii) open transaction in the tail | 30 | 0 | 6 | 18 | 1.00 | 0.83 |
| (iv) committed record newer than the page | 30 | 0 | 6 | 18 | 1.00 | 0.83 |
| (ivc) committed record after CheckpointLSN | 30 | 0 | 6 | 18 | 1.00 | 0.83 |
| **(v) = (iii) ∪ (iv)** | **36** | **0** | **0** | **18** | **1.00** | **1.00** |

Difference level: the share of differing keys that have at least one flagged page among the pages involved (the
reader's page, the restored page, the LOB fragment pages):

| flag | committed missing (20552) | open txn (18317) | torn split/move (64) | torn LOB (1) | flagged pages holding a difference |
|---|---:|---:|---:|---:|---:|
| (i) | 36.9% | 0.2% | 96.9% | 100% | 40% of 70 |
| (ii) | 68.8% | 100% | 96.9% | 100% | 18% of 92008 |
| (iii) | 6.6% | 98.8% | 9.4% | 0% | 92% of 11389 |
| (iv) | 100% | 3.5% | 98.4% | 100% | 75% of 20430 |
| **(v)** | **100%** | **99.1%** | **100%** | **100%** | **81% of 31081** |

Why each behaves as it does:

- **(i)** cannot see a page that was copied before it changed: its copy carries the old LSN. It flags only the
  lazily written pages, which is exactly where tears are.
- **(ii)** flags everything written since the oldest open transaction began. With a long transaction (here 3 s
  before the backup) that includes the clean `t_pre_commit` and `t_mid_only`.
- **(iii)** alone misses tables that only lost committed work (`t_dlog`).
- **(iv)** alone misses tables with only uncommitted dirt (`t_open_only`).
- **(v)** misses 0.9% of the uncommitted rows at page level: rows the open transaction changed and a later page
  split then moved, so the log names the old page. At table level it still flags them.
- **DB-level shortcut.** No open transaction and no committed row operation after `CheckpointLSN` means the backup
  is consistent. In run 0 the tail held only the checkpoint records, and `FirstLSN = CheckpointLSN`.

## 5. Optional: the simplest redo (time-boxed)

`redo.py` works per page, in LSN order. It applies only records with an LSN above the page copy's and below
`LastLSN`, on the user tables' data and LOB pages, from committed and rolled-back transactions; open transactions
are skipped and there is no undo. It handles 7 operation types:

- FORMAT_PAGE;
- INSERT_ROWS: one row, or several for a split;
- DELETE_ROWS: heap slot to 0, MARK_AS_GHOST to a ghost, clustered physical;
- MODIFY_ROW: byte range at an offset;
- MODIFY_COLUMNS: several ranges;
- DELETE_SPLIT;
- EXPUNGE_ROWS.

Rebuilt pages are re-read with the same decoder.

| run | committed-missing + torn, before → after | payload mismatches after (redo artefacts, mostly LOB values) | uncommitted, before = after | pages rebuilt / overflowed |
|---|---|---:|---:|---|
| r1 | 862 → 0 | 26 | 2977 | 870 / 6 |
| r2 | 759 → 0 | 41 | 2989 | 825 / 11 |
| r3 | 16677 → 67 | 913 | 3142 | 14289 / 138 |
| r4 | 626 → 1 | 28 | 3057 | 559 / 4 |
| r5 | 764 → 0 | 46 | 3101 | 713 / 9 |
| r6 | 929 → 1 | 38 | 3051 | 793 / 11 |

Net effect on the committed-side errors: 94–97% removed. `t_dlog` (append-only) and the stale heap/ci/split rows go
to zero. What remains:

- **LOB values in `t_lob`** decode as empty or wrong after redo. The in-row root is redone but the LOB tree is not
  rebuilt correctly: TEXT_TREE pages, links added by MODIFY_ROW on LOB roots.
- **A few rows with a damaged filler, and pages that overflow when rebuilt.** Both are bugs or edge cases in this
  prototype. Their pages are left as the backup copy.

Undo of the open transaction is the other half and was not attempted. Every one of its records carries a row image
or byte range (§6), so a logical undo is possible.

## 6. Log-format facts learnt (needed by any of the above)

- **Sector parity, solved.** In every log block the first byte of each 512-byte sector *k* ≥ 1 is overwritten by a
  flag byte (0x40, 0x48 …). The original is saved at `block[size − 1 − k]`. Without the fix, 1.6% of the records
  misparsed: the slot-array entries sit on sector boundaries. With it, 4283 records joined on LSN against
  `fn_dblog` match on op, transaction id and page, 100%.
- **Opcodes** (verified against `fn_dblog`):

  | code | operation | code | operation |
  |---|---|---|---|
  | 0x01 | FORMAT_PAGE | 0x0d | EXPUNGE_ROWS |
  | 0x02 | INSERT_ROWS | 0x12 | COUNT_DELTA |
  | 0x03 | DELETE_ROWS | 0x16 | INSYSXACT |
  | 0x04 | MODIFY_ROW | 0x80 / 0x81 / 0x82 | BEGIN / COMMIT / ABORT_XACT |
  | 0x05 | MODIFY_HEADER | 0x8c | HOBT_DELTA |
  | 0x06 | MODIFY_COLUMNS | 0x8d | LOCK_XACT |
  | 0x07 | SET_BITS | 0x8f | MIGRATE_LOCKS |
  | 0x0a | SET_FREE_SPACE | 0x96 / 0x98 / 0x99 | BEGIN_CKPT / XACT_CKPT / END_CKPT |
  | 0x0b | DELETE_SPLIT | | |

  The guesses in `tools/logblocks.py` (0x11 FORMAT_PAGE, 0x05 MODIFY_COLUMNS) are wrong.
- **Contexts:**

  | code | context | code | context |
  |---|---|---|---|
  | 0 | NULL | 8 | GAM |
  | 1 | HEAP (also used by FORMAT_PAGE of B-tree leaves) | 10 | IAM |
  | 2 | CLUSTERED | 11 | PFS |
  | 3 | INDEX_LEAF | 19 | MARK_AS_GHOST |
  | 4 | INDEX_INTERIOR | 23 | BOOT_PAGE_CKPT |
  | 5 | TEXT_MIX | 24 | DIFF_MAP |
  | 6 | probably TEXT_TREE | | |
- **Allocation unit of a row operation:** u32 objId at +32, u16 indexId at +46, giving
  `AU = indexId << 48 | objId << 16` (1446 of 1446 matches).
- **Operation fields:**
  - MODIFY_ROW: offset in row at +56, modify size at +58.
  - DELETE_SPLIT: rows deleted at +54.
  - Row-log contents: at `fixlen`, u16 n, n × u16 lengths, then the elements, 4-byte aligned.
  - INSERT_ROWS: the row images are all the elements but the last two; a split moves many rows in one record.
  - DELETE_ROWS: element 0 is the full before-image.
  - MODIFY_COLUMNS: element 0 = range offsets (pairs), element 1 = range lengths, elements 4+ = before/after per
    range.
  - FORMAT_PAGE carries no header image.
- **IAM header record:** start page RID at +40, the 8 single-page slots at +46. **PFS:** the byte for page *p* is at
  offset `100 + p mod 8088`. The PFS-allocated counts equal `sys.dm_db_database_page_allocations` exactly.

Reader variants on the heavy run:
- header object id only: 19830 differing keys, 11 of them from stale deallocated pages;
- PFS-allocated: 19819;
- IAM chain + PFS: 19819.

So check allocation (PFS is enough with uniform extents) and always overlay pass 2.

## 7. Recommended v1 policy

1. **Take the latest copy of every page** (pass 2 over pass 1), skip type-101 pads, and check the PFS allocated bit.
2. **Always parse the log tail.** It is one sequential pass over the MQTL (30–240 MB here) with the parity fix.
   From it:
   - the transactions open at `LastLSN`, and the tables (AUs) they touched;
   - the committed records with LSN above their page's copy, giving tables and pages that miss committed work;
     counting only records after `CheckpointLSN` is an equivalent table-level test.
3. **Per-table status** in `mssql_backup_info` / `mssql_backup_tables`:
   - `consistent`;
   - `stale` (committed changes after `CheckpointLSN` missing; give the number of transactions and the window
     `CheckpointLSN → LastLSN`);
   - `dirty` (uncommitted rows of N open transactions visible);
   - `stale+dirty`.

   Also a database-level `consistent` boolean: no open transaction and no committed row operation after
   `CheckpointLSN`. Do **not** use page LSN > `CheckpointLSN` as the fuzzy test (recall 0.42). `FirstLSN` is only a
   coarse database-level hint (precision 0.75).
4. **Default: warn.** Read the rows, attach the status, and log a warning naming the tables. An option
   `fuzzy := 'error'` refuses non-consistent tables. A third option, `'redo'`, belongs to a later phase.
5. **For the lake import (§6.2, E1).**
   - Record `CheckpointLSN` as the point a no-replay full import reflects, not `LastLSN`. Which pages the next
     file must re-read is then decided by that file (its DCM and `m_lsn > since_lsn`), with no page LSN kept
     in the lake (089 D3).
   - The stale pages carry old LSNs, and their newer versions in the next differential lie *below* the full's
     `LastLSN`. An importer that skips pages with LSN ≤ `LastLSN` would therefore keep them stale forever.
   - The next diff does contain them: the full resets the DCM at its start, and the extents changed during it are
     marked.
6. **Redo is closer than §8 suggests.** The tail-only page redo of 7 operation types fixed ≥94% of committed-change
   errors. With LOB-tree operations and a logical undo of open transactions (row images are in the log) it should
   reach consistency without a full log engine. It could precede phase 4; the owner decides.

## 8. Not tested / limits

- One version (SQL Server 2025 CU8, Linux), one data file.
- ADR, RCSI and snapshot isolation were off. With ADR, in-row version tags and the PVS change both the dirty-read
  picture and the undo.
- Backups were 4–8 s long, bounded by the helper's 30 s timeout. On a large database pass 2 and the window grow
  with pass-1 duration × write rate (r3 already re-copied 73% of the database).
- Only compressed (ZSTD) backups were used. That is the same MTF stream, but the pass structure of an uncompressed
  or multi-file / striped backup was not re-checked under load.
- The redo prototype does not handle LOB trees, does not undo, and has edge-case bugs (page overflow).

## 9. Files

`experiments/E3/`:

| file | what |
|---|---|
| `sql/01..06_*.sql` | database and table setup |
| `sql/w_*.tmpl` | workloads (a), (b), (c) and the BACKUP |
| `run.sh` | one run: jobs in parallel, events, `docker cp` |
| `truth.sh` | RESTORE + canonical dump |
| `e3lib.py` | reader: container, page selection hdr/pfs/iam/pass1, rows, LOBs |
| `logtail.py` | MQTL records with the parity fix, opcode map, contents, transaction status |
| `analyze.py` | classes and flags → `out/rN_analysis_*.json` |
| `summarize.py` | all runs → `out/summary_all.txt` |
| `timeline.py` | checkpoint times, window → `out/timeline.txt` |
| `redo.py` | the redo prototype → `out/rN_redo.txt` |
| `quickdiff.py`, `explore_alloc.py` | early probes |

`out/` keeps the measurements: headers, events, analyses, summaries, redo results, and the `fn_dblog` slices used
to verify the log formats. Kept locally: `bak/bk_e3_r0.bak` (quiet, 25 MB) and `bak/bk_e3_r1.bak` (fuzzy, 39 MB),
with r1's truth dumps in `out/r1_truth.tar.gz` (extract into `out/` to re-run `analyze.py r1`). The other backups and truth dumps were deleted after the measurements.

Cleanup on the server: `bk_e3`, `bk_e3_restored` and `bk_e3_meta` dropped, and `xp_delete_files` removed
`/var/opt/mssql/data/bk_e3_*`.
