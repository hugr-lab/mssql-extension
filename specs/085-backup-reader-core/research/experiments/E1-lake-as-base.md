# E1: the lake as the base (applying backups with only the lake and the new file)

Date: 2026-11-05. Server: SQL Server 2025 RTM-CU8 (17.0.4075.5), Enterprise Developer, container `mssql-v2test`.
Database `bk_e1` (FULL recovery, collation `SQL_Latin1_General_CP1_CI_AS`, no RCSI/ADR). Every statement went through
`tools/sql.sh`; the only docker commands were `docker ps` and `docker cp`. Research only, nothing in git.
Scripts, backups, truth captures and logs are in `experiments/E1/`.

## Short answer

**Yes.** A Python prototype imported a full backup into a DuckDB "lake". It then applied differentials, and two new
fulls incrementally, using **only the lake and the one file being applied**: never the full backup, never an earlier
differential, never the server. After every one of **18 applied files and 3 fresh imports** (21 compared states, 5 chains), every table equalled the server's
`SELECT` at backup time, row for row. Equality was checked as multiset equality over all columns, with LOBs compared
by SHA-256. Locators matched too: heap `_mssql_rid` = `%%physloc%%`, clustered `_mssql_page` = the physloc page,
uniquifiers matched DBCC PAGE.

Lakes that reached the same backup by different routes are **identical**, catalog mirror and locators included:

- F0 → D1 → D2 → F1, against F1 imported fresh;
- D2 straight after F0 (D1 skipped), against D1 then D2;
- D4 straight after F1, against D3 then D4;
- F2 fresh → D6, against the long chain.

Wrong-base and out-of-order files are refused from header fields read out of the file.

Six rules made it work. Each one was shown necessary by a failure or a counterfactual run:

1. Changed pages are found by page LSN.
2. Freed pages are found from the **PFS**, not the DCM.
3. Rows are read only from pages the PFS calls allocated.
4. Rows are deleted **by page in every table**, not only in the page's new owner.
5. The catalog is applied first, from a mirrored `__mssql_sys`.
6. LOBs not wholly in the file are rebuilt from the file's nodes plus a **fragment map stored in the lake**. Plain
   "reuse the lake's value by `_mssql_rid`" (the RESEARCH §6.2 rule) **silently corrupts** a LOB changed with `.WRITE`.

## 1. Setup

### Tables (`sql/00_create.sql`)

| table | shape | rows at F0 | what it exercises |
|---|---|---:|---|
| `t_heap` | heap: int, int, decimal(12,2), datetime2(3), bit, varchar(3000), nvarchar(100) | 5000 (89 pages) | forwarded rows (growth to ~2.5-3 KB), deletes, inserts, DROP COLUMN, ADD COLUMN after a drop |
| `t_pk` | clustered PK, even ids only | 4000 (105) | page splits (odd ids inserted in the middle), range deletes that free pages, 3 × ADD COLUMN |
| `t_nuci` | NON-unique clustered index, no PK, ~7 duplicates per key | 3000 (65) | uniquifiers, DROP COLUMN, index REBUILD (new allocation unit), ADD COLUMN with a default |
| `t_lob` | clustered PK, varchar(max) 9-40 KB for id % 5 = 0, varbinary(max) > 10 KB for id % 7 = 0, small in-row values | 300 (25 + 289 LOB pages) | whole-value LOB updates, in-row ↔ off-row, LOB → NULL, key update, `.WRITE` |
| `t_rowc` | clustered PK, **ROW compression**: int, smallint, bigint, varchar(60) | 2000 (25) | CD records (decoder added here, section 4.9) |
| `t_drop`, `t_trunc` | clustered / heap | 500 / 800 | DROP TABLE, TRUNCATE + re-insert |
| `t_new`, `t_new_heap` (W2) | new tables | 600 / 300 | CREATE TABLE in a diff; heap REBUILD (every RID changes) |
| `t_heap_lob` (W2b) | heap with varchar(max) | 120 | forwarded rows whose LOB is untouched |

PAGE compression was **left out**: the decoder does not handle anchors or dictionaries.

### Workloads and backups

| step | workload (`sql/`) | backup | type, base | file MB | pages in file |
|---|---|---|---|---:|---:|
| F0 | `00_create` | `bk_e1_f0` | full | 10.6 | 893 |
| D1 | `01_w1`: 1500 odd-id inserts (splits), updates, a range delete, 100 heap growths (96 forwarded), 180 duplicate keys, deletes of whole keys and single duplicates, 3 whole-value LOB updates, in-row → LOB, LOB → NULL, LOB row inserts and deletes, a retag that leaves the LOB alone, ROW-table DML | `bk_e1_d1` | diff, base F0 | 7.5 | 688 |
| D1b | none (see 4.7) | `bk_e1_d1b_nochange` | diff, base F0 | 8.5 | 697 |
| D2 | `02a/02b_w2`: ADD c_null (no default), ADD c_def NOT NULL DEFAULT 42, ADD c_vdef DEFAULT 'dv' WITH VALUES, 2 × DROP COLUMN, ADD after a drop, CREATE 2 tables, DROP TABLE, TRUNCATE + 50 inserts, updates of the new columns, forwarded rows shrink, grow and get deleted, LOB key update, LOB update, more DML | `bk_e1_d2` | diff, base F0 (cumulative) | 9.6 | 824 |
| F1 | `03_w2b`: TABLOCK heap delete (frees pages), range delete, `ALTER INDEX … REBUILD`, DML, new `t_heap_lob` | `bk_e1_f1` | **full, not COPY_ONLY** | 13.8 | 1244 |
| D3 | `04a/04b_w3`: heap-with-LOB rows forwarded, `ALTER TABLE t_new_heap REBUILD`, ADD y NOT NULL DEFAULT 7 on the uniquifier table, DML everywhere | `bk_e1_d3` | diff, base F1 | 7.5 | 672 |
| *optional* D4 | `05_w4_write`: `.WRITE` overwrite of 4 B, `.WRITE` append, `.WRITE` on a heap row | `bk_e1_d4` | diff, base F1 | 8.5 | 706 |
| *optional* D5 | `06_w5`: `.WRITE` inside a 63 KB LOB (two-level tree) | `bk_e1_d5` | diff, base F1 | 8.5 | 714 |
| *optional* F2 | `07_w6`: a 630 KB LOB (one INTERNAL node, 79 leaves) | `bk_e1_f2` | full | 14.8 | 1392 |
| *optional* D6 | `08_w7`: `.WRITE` of 13 B at offset 300000 of that LOB | `bk_e1_d6` | diff, base F2 | 3.3 | 118 |
| *optional* D7 | `09_w8`: deletes in the same batch as the backup (ghosts left on the pages) | `bk_e1_d7` | diff, base F2 | 3.3 | 183 |

The D1, D2, F1 and D3 workloads were followed by a 20 s wait, so the ghost cleanup could free emptied pages.

The truth was captured right after each backup by `truth.py`:

- a canonical line per row (all columns, LOBs as SHA-256:length) plus `%%physloc%%`;
- the user catalog (`sys.columns`, `system_internals_partition_columns`, allocation units);
- `sys.dm_db_database_page_allocations DETAILED`;
- `dm_db_index_physical_stats`;
- `RESTORE HEADERONLY` / `FILELISTONLY`.

All of it is in `truth/t0..t9/`.

## 2. The prototype

| file | role |
|---|---|
| `pages_ext.py` (717 lines) | Extends `tools/pages.py` without editing it (E2-E4 use that file at the same time). It adds: a page store per file; header fields read from the file; PFS; decoding of 7 system base tables (5 hard-coded, `sysseobjvalues` and `sysclsobjs` generic); `Catalog`/`Layout` built from rows; typed FixedVar and ROW (CD) decoding; ADD/DROP COLUMN; forwarding; the uniquifier; and the hybrid LOB reader. |
| `lake.py` (679 lines) | `import`, `apply`, `compare`. One DuckDB transaction per file. Counterfactual switches: `E1_NO_FREED`, `E1_HEADER_ONLY`, `E1_DELETE_BY_OWNER`, `E1_WATERMARK_LAST_LSN`, `--lob-reuse rid`, `--force`. |
| `truth.py` | Server truth, through `tools/sql.sh -q` only. |
| `run_all.sh [map\|rid]` | Replays every chain from `bak/` and compares with `truth/`. It needs no server and takes 23 s. |
| `lakediff.py`, `check_header_au.py`, `diag_unallocated.py` | Lake vs lake; page-header AU vs the server; what stale unallocated pages contain. |

Lake layout:

- **`dbo.<table>`** holds the columns plus:
  - `_mssql_rid` (VARCHAR): for a heap, the original `file:page:slot`, following the forwarded back pointer; for a
    clustered table, `key=…[,u=uniquifier]`;
  - `_mssql_page` (BIGINT, file<<32|page);
  - for tables with LOBs, `_mssql_lobptr` (the in-row root bytes) and `_mssql_lobmap` (the LOB tree as
    `[file, page, slot, offset, length, depth]` per node).
- **`__mssql_sys`** mirrors sysallocunits, sysrowsets, sysschobjs, syscolpars, sysrscols, sysseobjvalues and
  sysclsobjs, each with its own `_mssql_page` / `_mssql_rid`.
- **`__mssql_backup.imports`** is the provenance: GUIDs, the four LSNs, the watermark and statistics.
- **`__mssql_backup.tables`** registers object id → name, in-row AU and columns: the "before" side of every DDL
  comparison.

How a file is applied:

1. Check the chain.
2. Find the changed pages (`m_lsn` > watermark) and the freed pages (lake pages whose PFS byte has no 0x40).
3. `__mssql_sys`: delete the rows on changed and freed pages, then insert the rows of the changed pages.
4. Build the catalog from `__mssql_sys` and derive DDL against the registry.
5. Decode the changed, allocated DATA leaf pages whose header AU is a table's in-row AU; resolve LOBs.
6. Delete lake rows on changed or freed pages in every table, then insert the decoded rows.
7. Write the provenance row.

## 3. Results

### Final replay (`logs/run_all_map.txt`, fragment-map LOB rule)

| chain | files applied | result vs server |
|---|---|---|
| A | F0 → D1 → D2 → F1 (incremental) → D3 → D4 → D5 → F2 (incremental) → D6 → D7 | **EQUAL** at all 10 steps |
| A' | F0 → D1 → D1b (no-change diff) | EQUAL |
| B | F0 → **D2 (D1 skipped)** → F1 → D3 → D4 → F2 → D6 | EQUAL at all 7 steps |
| C | F1 imported fresh → **D4 (D3 skipped)** | EQUAL |
| D | F2 imported fresh → D6 | EQUAL |
| fresh imports | F0, F1, F2 | EQUAL |

The lake-vs-lake comparisons are over all tables, `__mssql_sys` and service columns included:

- A after F1 = F1 imported fresh;
- A after F2 = F2 imported fresh;
- A = D after D6;
- B = C after D4;
- B = D after D6.

All are **IDENTICAL**.

These files were **refused** without changes, on header fields read from the file:

- D3 onto F0+D1+D2: "differential base 63da3971… is not the lake's base full c53cd52b…";
- D1 or F0 onto F0+D1+D2: "FirstLSN … is not later than the last applied file's".

Forcing D3 onto F0+D1+D2 (F1 skipped) corrupts three tables: `t_rowc` 181 rows, `t_lob` 1 row, `t_heap_lob` 40
unresolvable LOBs. This is the RESEARCH rule "a non-COPY_ONLY full must be applied before its diffs", demonstrated.

### Locator checks

- **Heap `_mssql_rid` = `%%physloc%%`** for every row at every step, including 96 / 132 / 102 / 170 forwarded rows.
  After the first fix in 4.1, the 20 forwarded rows of `t_heap_lob` also matched.
- **Clustered `_mssql_page` = the physloc page** for every row at every step.
- **Uniquifier** values match `DBCC PAGE … 3` (field `UNIQUIFIER`) for all 237 rows on 5 `t_nuci` pages after D4.
  `_mssql_rid` was unique in every table at every step.
- `ALTER INDEX … REBUILD` on `t_nuci` gave a new allocation unit and moved every row to a new page (3188/3188), but
  **key + uniquifier stayed the same for all 3188 rows**. A heap REBUILD changed all 300 RIDs of `t_new_heap`. The
  page-based apply needs nothing special for either (section 5, rule 11).

### Header fields read from the file (`pages_ext.backup_info`)

These fields equal `RESTORE HEADERONLY` for all 7 files checked (F0, D1, D1b, D2, F1, D3, D4):

- BackupSetGUID, FirstLSN, LastLSN, CheckpointLSN, DatabaseBackupLSN, BackupType, IsCopyOnly;
- for diffs, also DifferentialBaseGUID and DifferentialBaseLSN.

| field | where |
|---|---|
| BackupSetGUID | SSET block +100 (16 B GUID); also trailer SCIN +842 in a full |
| kind | SSET attributes: 0x4 full, 0x6 copy-only full (seen on `samples/bak_probe_full2.bak`), 0x8 differential |
| FirstLSN / LastLSN / CheckpointLSN / DatabaseBackupLSN | trailer SCIN record (MSLS block, stream MQCI) at +32 / +42 / +52 / +62, each 10 B (u32 vlf, u32 block, u16 slot) |
| DifferentialBaseLSN / DifferentialBaseGUID | SCIN +830 / +842. In a diff these are the base full's values; in a full they are the file's own checkpoint LSN and GUID, i.e. "the base this file establishes" |

The header copy of SCIN (MSCI block) lacks the LSNs. Read the trailer copy.

### Timings and sizes

All runs are single-threaded Python on an M-series Mac, with other experiments loading the server and the machine.

| lake / file | pages in file | changed (m_lsn > W) | freed (not changed) | lake pages touched | rows del / ins | `__mssql_sys` rows replaced | DDL | LOBs from file / rebuilt with the lake | apply s (catalog / rows) |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| A / F0 import | 893 | all | – | – | 0 / 15600 | all | – | 102 / 0 | 0.50 |
| A / D1 | 688 | 471 | 8 (0) | 214 | 12804 / 14304 | 1083 | 0 | 16 / 84 | 0.42 (0.22 / 0.13) |
| A' / D1b | 697 | 52 | 0 | 11 | 0 / 0 | 946 | 0 | 0 / 0 | 0.27 |
| A / D2 | 824 | 398 | 13 (12) | 258 | 11283 / 11568 | 1641 | 9 | 23 / 46 | 0.41 |
| B / D2 (D1 skipped) | 824 | 595 | 19 (12) | 233 | 13542 / 15327 | 1641 | 9 | 23 / 77 | 0.47 |
| A / F1 incremental | 1244 | 227 | 77 (50) | 152 | 7555 / 5790 | 1359 | 2 | 72 / 0 | 0.35 |
| F1 fresh import | 1244 | all | – | – | 0 / 15120 | all | – | 141 / 0 | 0.45 |
| A / D3 | 672 | 438 | 0 | 311 | 12372 / 12377 | 1347 | 2 | 10 / 99 | 0.43 |
| A / D4 (`.WRITE`) | 706 | 21 | 0 | 12 | 253 / 253 | 576 | 0 | 11 / 60 | 0.27 |
| A / F2 incremental | 1392 | 111 | 0 | 12 | 113 / 114 | 951 | 0 | 44 / 0 | 0.28 |
| A / D6 (`.WRITE`, 630 KB LOB) | 118 | 23 | 0 | 11 | 114 / 114 | 825 | 0 | 0 / 44 | 0.26 |
| A / D7 (ghosts) | 183 | 19 | 0 | 14 | 458 / 254 | 211 | 0 | 5 / 19 | 0.27 |

Notes on the numbers:

- A differential carries whole extents, so only 3-68 % of its pages are actually changed. The LSN filter is what
  keeps the work proportional to the change.
- A constant ~0.22 s goes to rebuilding the catalog from `__mssql_sys` (~6,300 rows, read twice). A real
  implementation would keep the catalog in memory between the two reads.
- The lake is a 19 MB DuckDB file for a database of ~1,500 used pages (12 MB). It holds the LOB values, the fragment
  maps and the free blocks left by deletes.
- The 7-15 MB files themselves decode in under 0.05 s.

## 4. Every failure and its root cause

### 4.1 Forwarded back pointer at the wrong index (decoder bug, also in `tools/pages.py`)

- **Seen:** first D1 apply. Three heap rows had `_mssql_rid` ≠ `%%physloc%%`; their values were right. All three were
  forwarded rows with a NULL trailing `nvarchar`.
- **Cause:** a FixedVar record omits **trailing NULL variable columns** from its offset array. In a forwarded record
  (type 1) the back pointer (complex column `0x0400` + original RID) is simply the **last** variable column. It lands
  at the index of the NULL column, so "the first complex column after the table's columns" misses it.
  `pages.py decode_row` has the same check.
- **Rule:** in a type-1 record, take the back pointer from the last variable column and exclude it from the table's
  columns. After the fix all forwarded rows matched (96 / 132 / 102 / 170 / 20).

### 4.2 TRUNCATE: deleted rows with no page in the diff

- **Seen:** counterfactual `E1_NO_FREED` on D2 (after D1 and after F0). `t_trunc` kept its 800 pre-TRUNCATE rows
  (850 rows against the server's 50).
- **Cause:** TRUNCATE deallocated `t_trunc`'s uniform extent (pages 8952-8959) **without setting its DCM bit** and
  without touching the pages. They are **not in D2 at all**; old images, old LSNs. The only evidence of the delete is
  the PFS page 1:8088 (and GAM/IAM), which is in the file.
- **Rule:** freed pages = pages the lake has rows on whose PFS byte lacks 0x40. Never infer frees from the DCM or from
  page images.
- Related observation in D3-D5: extent 77 (freed by the REBUILD) had its DCM bit **set** but was **not copied**,
  because the GAM marks it free. Checked on all 8 differentials (D1, D1b, D2-D7): the file's extent set is exactly
  (DCM bits ∩ GAM-allocated) ∪ {extents 0-1} ∪ {extents of the PFS pages it carries}.
- **A PFS page is in a differential only when its interval changed.** PFS 1:1 sits in extent 0 and is always there.
  PFS 1:8088 was in every file but **D6**: its interval had no allocation change since F2, so its DCM bit was clear.
  In D6, 80 lake pages (t_rowc, t_trunc, t_heap_lob in 8088-16175) had no allocation information. They were treated
  as not freed, which was right: a PFS page absent from a diff means no page of its interval was freed since the
  base, and frees before that were applied from an earlier file.

### 4.3 A page that changed owner keeps the old owner's rows

- **Seen:** counterfactual `E1_DELETE_BY_OWNER` on D2 after D1. `t_trunc` kept row 801.
- **Cause:** that row had been on page 1:1120. TRUNCATE freed the page and t_pk's page splits reused it. In D2 the
  page is allocated and changed, and its header names `t_pk`.
- **Rule:** delete by `_mssql_page` in **every** lake table, user and `__mssql_sys` alike, then insert from the new
  image. A production version should keep a page → table index (`__mssql_backup.pages`) instead of probing every
  table.

### 4.4 Page header AU: reliable on allocated pages, stale on freed ones

- **Allocated pages:** checked against `dm_db_database_page_allocations` captured right after each backup. The header
  AU (`m_indexId << 48 | m_objId << 16`) was right for every page the file's PFS calls allocated: 855 (F0), 594 (D1),
  739 (D2), 1133 (F1), 580 (D3). The one apparent miss was page 1:881 in F1: unallocated in the file (PFS 0x00) with
  a stale header (type 20, t_lob's AU), and taken by a system object between the backup and the truth query.
- **Unallocated pages keep their old header:** 16-57 per file still name their previous owner. D1 held 12 freed
  `t_pk` pages with m_lsn > watermark and the header still `t_pk`; D2 skipping D1 held the dropped `t_drop` pages
  with 391 **live** records on them.
- **Rule:** read rows only from pages whose PFS allocated bit is set.
- **What the data showed:** the `E1_HEADER_ONLY` counterfactual did *not* corrupt anything here. Every changed and
  unallocated page held only one ghost record, and the pages with live stale rows had old LSNs or belonged to a
  dropped table. The PFS check is still the only thing that separates them; a TRUNCATE right after DML on the same
  page would resurrect rows.

### 4.5 LOB reuse "by `_mssql_rid`" is wrong after `.WRITE` (the RESEARCH §6.2 rule)

- **Seen:** D4 (`.WRITE` append on `t_lob` id 150). The lake kept the 24000-byte value instead of the server's
  24015 bytes; a warning was printed, the value was still wrong. D6 (`.WRITE` inside the 630 KB LOB, id 500) was
  **silently** wrong: no warning, same length, wrong hash. With that rule (`./run_all.sh rid`) 10 of the 21 steps fail.
  The error **persists through the incremental F2**: a full applied incrementally re-reads only pages with a newer
  LSN, so it does not heal earlier damage.
- **Cause:** every `.WRITE` (5 of 5: overwrite, append, heap row, 2-level tree, 630 KB tree) **changes the in-row
  LOB root**. Its `ts` field moves, e.g. 0x1 → 0x2, 0x1 → 0x29c. So the data page changes and the row is re-decoded,
  which is good: the change is always noticed. But the diff holds only the extents that changed, so the LOB is
  partly in the file and partly not. "Take the lake's value for this rid" then returns the old bytes. Whole-value
  updates never hit this: they write a new LOB, all of it in the file. A clustered key update also **copies** the
  LOB: new pointer, new pages, all in the file. So rid-based reuse is never asked for after a key change.
- **Fix (`--lob-reuse map`, the default now):**
  1. The lake keeps, per LOB value, its in-row pointer and its **fragment map**: every node's RID, offset, length
     and depth.
  2. A LOB not wholly in the file is rebuilt by a hybrid walk. A node whose page is in the file is read from it. A
     node whose page is absent is looked up **by RID** in the lake's maps; a leaf takes its byte range from the old
     value, an internal node is expanded through its old children, each again from the file when present.
  3. This is exact because a page absent from a differential is unchanged since the base full, hence since the last
     apply.
- **What the fix covered:**
  - D6 exercised the deep path: INTERNAL node 1:697 absent and expanded from the lake, 8 leaves of the changed
    extent read from the file (the changed one included), 71 leaves from the lake. The value is byte-exact.
  - In D1-D7, 19-125 LOBs per diff needed the lake.
  - A node that is neither in the file nor in any map leaves the value unresolved and is reported. It happened only
    in the forced wrong-base apply.

### 4.6 False alarms of an LSN-based "LOB may have changed" warning

- **Seen:** the first prototype flagged LOBs partly in the file with a present page whose m_lsn > watermark: 6 in D1,
  4 in D2 after F0, 2 in D3. All were correct values.
- **Cause:** TEXT_MIX pages are shared, so another LOB's change bumps the page LSN.
- **Rule:** page LSNs cannot tell which fragment changed. The pointer plus the fragment map can.

### 4.7 Operational: a T-SQL batch fails as a whole

The first W2 batch referenced `c_null` in the same batch as the `ALTER TABLE … ADD c_null`. SQL Server rejected the
whole batch at compile time (error 207), so nothing ran, and the D2 taken after it carried no user change. It was
kept as `bk_e1_d1b_nochange`: 52 changed pages, all system (checkpoint, statistics); 946 `__mssql_sys` rows
replaced; 0 user rows; lake still equal. W2 was then rerun as a DDL batch plus a DML batch.

### 4.8 Catalog

There were no failures; the `__mssql_sys` mirror was always enough. What it had to contain:

- **The five bootstrap tables** plus **`sysseobjvalues`**, which holds metadata-only ADD COLUMN defaults (valclass 1,
  `id` = rowset id, `subid` = **hbcolid**, valnum 0, value = sql_variant). Without it, untouched rows of `t_pk` would
  read NULL instead of 42 / 'dv' (6450 rows in D2), and `t_nuci.y` NULL instead of 7 (8787 rows in D3).
- **`sysclsobjs`** for schema names.
- Not needed: sysidxstats/sysiscols. The key order is `sysrscols.ordkey` at +36. `pages.py` read the key ordinal
  from the high half of `nullbit`, which is the *internal* null bit.

Facts confirmed on the server (`system_internals_partition_columns` definition and data):

- `sysrscols.status`: 2 = dropped, 16 = uniquifier, 128 = NOT NULL.
- A dropped column keeps its row but gets a **new rscolid** (0x4000000 + n) and keeps its hbcolid.
- Live columns map `rscolid` = `syscolpars.colid`, also after a drop and an add.
- With a uniquifier, `hbcolid` = `rscolid` + 1 (uniquifier: rscolid 0, hbcolid 1). That is why the default lookup
  must use hbcolid.
- A record written before ADD COLUMN has fewer columns than the new column's null bit. It reads the stored default,
  else NULL.
- A truncated heap kept its allocation unit; the rebuilt index got a new one. No rule was needed for either.

### 4.9 ROW compression

A CD decoder was written (`pages_ext.parse_cd`) and verified by hand against DBCC PAGE, then against the truth at
every step: 2000-2199 rows, inserts, sign flips, NULL / empty / long values, ghosts.

- Header byte: bit 0 = CD, bits 1-3 = record type, 0x20 = long data region.
- Then a 1-byte column count and a 4-bit CD array (0 NULL, 1 empty, 2-9 = 1-8 bytes, 10 = long).
- Integers are big-endian with the top bit flipped (0x81 = 1, 0x68 = -24).
- Long region: u8 flags, u16 count, u16 end offsets relative to the start of the long data.
- Not handled: clusters (> 30 columns), CD 11/12 (bit / page symbol), types other than integers and strings.

### 4.10 Ghosts

D7 deleted rows in the same batch as the backup. The file holds **204 ghost records on 10 allocated data pages in 4
tables**, 21 of them ROW-compressed (CD) ghosts. Record types 5/6/7 are skipped and the result is equal. Freed slots
(offset 0, heap deletes) are skipped too.

## 5. Rules a real implementation needs

1. **Chain check from the file, before anything else.**
   - A differential's base GUID (SCIN +842) must equal the base full the lake records.
   - The file's FirstLSN must be later than the last applied file's.
   - A full that is not COPY_ONLY (SSET attributes without 0x2) becomes the new base, and must be applied before
     any differential taken on it.
   - Refuse otherwise; the forced counter-example corrupts three tables.
2. **Changed pages:** `m_lsn > W`, where W = min(FirstLSN, CheckpointLSN) of the last applied file. Using LastLSN gave
   the same change sets here: no later file held a page with an LSN in (FirstLSN, LastLSN] of the previous one.
   FirstLSN is the choice that stays right for fuzzy backups (E3). Re-processing a page is idempotent.
3. **Freed pages from the PFS:** a lake page whose PFS byte lacks 0x40 has its rows deleted. Deallocations need not
   set the DCM bit (TRUNCATE), and unallocated extents are not copied even when it is set. A PFS page is in a
   differential only when its interval's allocations changed. When it is absent (PFS 1:8088 in D6), treat every
   lake page of that interval as still allocated: nothing there was freed since the base.
4. **Row sources:** PFS-allocated DATA pages, level 0, whose header AU is a known table's in-row AU. Skip record
   types 2, 5, 6, 7 and freed slots. Never trust the header of an unallocated page.
5. **Delete by page in every table**, then insert. All deletes happen before all inserts, and all of it is one
   transaction: one DuckLake snapshot per file. Page splits, moves, forwarding, rebuilds and page reuse across
   tables then need nothing special.
6. **Catalog first.** Apply the same delete/insert to `__mssql_sys` (sysallocunits, sysrowsets, sysschobjs,
   syscolpars, sysrscols, sysseobjvalues, sysclsobjs), rebuild the catalog from the mirror, and diff it against a
   registry by object id and colid:
   - CREATE / DROP TABLE;
   - DROP COLUMN;
   - ADD COLUMN with `DEFAULT <stored default>` for the lake rows, taken from sysseobjvalues by **hbcolid**;
   - RENAME.
   Type changes are not handled. The incrementally kept mirror was identical to a fresh import's at F1 and F2.
7. **Record decoding:**
   - A record with fewer columns than a column's null bit reads that column's stored default or NULL.
   - Dropped columns: sysrscols status 2.
   - Uniquifier: status 16, first variable column, 0 when empty.
   - Forwarded record: the back pointer is the last variable column.
   - Key order: `ordkey`.
8. **LOBs:** store the in-row pointer and the fragment map with the value. Read a LOB from the file when it is
   wholly there, else by the hybrid walk (4.5). Refuse, naming table, row and node, when a node is in neither.
   This replaces "reuse by rid" and the `base := full.bak` fallback for `.WRITE`. It also covers key changes,
   because the lookup is by node RID, not row RID. Store LOB **bytes** (or a lossless mapping), not decoded text:
   the splice re-encodes the old value. That is exact for cp1252 ASCII here, but not for code-page bytes Python
   cannot decode.
9. **`_mssql_rid` semantics:**
   - Clustered key + uniquifier is stable across splits, updates and REBUILD.
   - A heap RID is stable across updates and forwarding, but **every** RID changes on a heap REBUILD. A downstream
     consumer of `_mssql_rid` (CDC-like) sees that as delete + insert.
   - The lake itself does not care, since it is page-based.
10. **An incremental full does not repair the lake.** It re-reads only pages with a newer LSN. Correctness of every
    earlier apply is therefore essential. A periodic "verify" mode (a full imported fresh, compared with the lake,
    as `lakediff.py` does) is cheap insurance.
11. **Allocation-unit changes need no rule.** REBUILD gives a new AU and TRUNCATE kept the old one; both just show
    up as freed plus changed pages. Note them in the registry, and note heap RID renumbering for consumers.

## 6. Not covered, caveats

- **Quiet database.** No writes ran during any backup, so fuzziness was not tested (E3).
- **Single data file**, 2 PFS intervals, below the first GAM interval boundary (511,232 pages). No multi-file,
  partitioned or columnstore tables. No row-overflow `varchar(n)` (the code path exists, untested here).
- **No PAGE compression.** No compressed or encrypted backups; the container is orthogonal to E1.
- `.WRITE` partial updates were tested beyond the plan (E2 owns their on-disk form). The observation "`.WRITE`
  always bumps the in-row root" rests on 5 cases, inline roots only. A LOB whose in-row value is a 24-byte text
  pointer to a LARGE_ROOT was not tested.
- **sql_variant defaults** were decoded for int and varchar only.
- **Plain DuckDB file, not DuckLake.** Python duckdb 1.5.2. `_mssql_rid` is text here; production would use
  BLOB/STRUCT. The fragment map is JSON per value: about 30 bytes per 8 KB leaf.
- **Duplicate references:** two references to one LOB, or a LOB moved between rows by a key swap, would still
  resolve through the RID map, but this was not tested.

## 7. Files (`experiments/E1/`)

- **`sql/`:** every T-SQL batch, in order: `00_create`, `01_w1`, `02a/02b_w2`, `03_w2b`, `04a/04b_w3`, and the
  optional `05`-`09`.
- **`bak/`:** the 10 backup files.
- **`truth/t0..t9/`:** the server truth (t1b is the no-change diff).
- **Code:** `pages_ext.py`, `lake.py`, `truth.py`, `run_all.sh`, `lakediff.py`, `check_header_au.py`,
  `diag_unallocated.py`.
- **`logs/`:**
  - `run_all_map.txt` (final rules, 0 failures) and `run_all_rid.txt` (rid-reuse rule, 10 failures);
  - per-step logs `run_map_*` / `run_rid_*`;
  - `first_pass/`: the first iteration, before the LOB map;
  - `neg_forced.log`: the wrong-base apply.
- **`lakes/run_map/`:** the final lakes A-D, F1fresh, F2fresh. All can be regenerated with
  `./run_all.sh map` in 23 s.

Server state after the experiment: `bk_e1` dropped (SINGLE_USER WITH ROLLBACK IMMEDIATE), and
`xp_delete_files N'/var/opt/mssql/data/bk_e1*'` run. `sys.dm_os_enumerate_filesystem('/var/opt/mssql/data/',
'bk_e1*')` returns nothing.
