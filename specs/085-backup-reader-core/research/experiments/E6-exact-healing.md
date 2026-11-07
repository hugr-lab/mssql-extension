# E6: Exact healing of a lake fed by fuzzy backups

Date: 2026-11-05. Server: SQL Server 2025 RTM-CU8 (17.0.4075.5), container `mssql-v2test`. T-SQL went only through
`tools/sql.sh` / `tools/sqlh.sh`; files came out with `docker cp` only; no KILL. Database `bk_e6` (+ `bk_e6_meta` for
events, `bk_e6_r` for RESTORE truth). Research only, nothing in git. Code and data: `experiments/E6/`.

The owner's requirement (RESEARCH §4):

1. No dirty (uncommitted) data in the lake. Uncommitted work is deferred to the next file.
2. The next incremental file must restore the lake exactly.
3. LOB values may be approximate after the full import only.

## Short answer

- **DCM timing (Q1).** The DCM is cleared at the **start** of a full backup. A system transaction
  `Backup:InvalidateDiffMaps` logs `LOP_CLEAR_GAM_BITS` on the DCM page 1:6, 6–13 ms after BACKUP starts and before
  the backup's first checkpoint. The end of the backup (`Backup:CommitDifferentialBase`) does not touch the DCM.
  Differentials and COPY_ONLY fulls never clear it.
  - Measured: every page that changed after its copy in the full is in the next quiet differential, with the same
    image as a quiet COPY_ONLY full taken just after: 5,205 of 5,205 pages, and 4,739 of 4,739 in a second run.
  - That includes 1,062 of 1,062 pages changed only after CheckpointLSN, and 8,025 pass-2 pages.
  - The only pages written after the reset and missing from the next diff sit on extents freed since. A diff never
    copies a freed extent (E2 R1), and their rows are gone anyway.
- **The full cycle, exact (Q2).** A full import = page state + redo of the log tail + undo of the open transactions
  + pending pages. On three loaded full backups the result was **exact for all 17 tables**, LOBs included (0 LOB
  differences, so no approximation was needed), heap RIDs and clustered pages included.

  | file | load | after it, vs RESTORE of the same chain |
  |---|---|---|
  | F (full) | T1 commits later, T2 rolls back, T3 commits after touching other pages, committed LOB updates during the backup | **exact**; without recovery: 8,457 differing keys, 7,516 of them dirty |
  | D1 (diff) | quiet, applied with the pending-page rule | **exact** |
  | D2 (diff) | loaded, T4 / T6 open during it | **0 dirty**; 71 stale keys (21 stale, 11 deletes missing, 27 rows twice, 12 LOBs) |
  | D3 (diff) | quiet | **exact** |
  | F2 → D4 | second loaded full, applied incrementally on the lake; then quiet | **exact, exact**; identical to a lake built from F2 fresh |
  | F3 → D5 → D6 | T7 / T8 open across F3 **and** D5, then commit / roll back; D5 loaded, D6 quiet | **exact**, 0 dirty + 151 stale, **exact** |

  So requirement 1 holds at every file, including the loaded differentials, and requirement 2 at every file that
  follows a fuzzy one. A loaded differential is not exact by itself, though. Its tail changes pages it does not
  carry, and a lake that holds rows rather than pages has no base image to redo them on: 62 and 125 pages. Given
  those images, D2 and D5 come out exact too (oracle run, §3.4).
- **The pending-page inference (Q3) holds once it is gated by allocation.** "P absent from the next file and its
  transaction not open ⇒ apply P's stored image" was right for every allocated page: 822, 838 and 525 applied
  images, each equal record for record to the server's page at that time. It was wrong for 97, 115 and 96 pages of
  the rolled-back transactions. Their extents were freed by the rollback, so no differential carries them, and
  their images still hold the rolled-back rows.
  - The rule must first discard a pending page that the new file's GAM / PFS show as free.
  - "⇒ committed" is not the right conclusion either. One applied page belonged to a transaction that rolled back:
    splits had moved its rows off the page before the backup. What absence proves is that the page has not changed
    since its image.
- **Effort (Q4).** Redo needed 8 operations plus the PFS byte, and nothing LOB-tree specific. LOB fragments are
  records on TEXT_MIX / TEXT_TREE pages, handled by the same operations. Undo is physical on heap and LOB pages and
  by key on clustered pages; 200 rows had moved because of splits.
  - The Python engine is about 500 lines.
  - Estimated **9–12 weeks in C++** for rowstore with LOBs, including the pending-page and stale-page bookkeeping
    and an oracle test harness.
  - ADR / RCSI, ROW / PAGE compression and columnstore come on top (§5).

## 1. Setup

**Database.** FULL recovery, ADR, RCSI and snapshot isolation off, about 240 MB used. Fillers are hex of random bytes,
so a ZSTD-HIGH backup takes 1.9 s quiet and 2.8–3.6 s under load (`sql/01..11`).

| table | shape | rows | written by |
|---|---|---:|---|
| `t_heap` | heap + unique NC index | 150 k | short transactions (inserts, growing updates → forwarding, deletes), every open transaction |
| `t_ci` | clustered PK | 150 k | same |
| `t_split` | clustered on a scrambled key, FILLFACTOR 100 | 60 k | same: page splits |
| `t_lob` | clustered; `doc varchar(max)` 40 % in-row / 60 % 8–36 KB, `ovf varchar(5000)` | 3 k | short transactions: whole-value updates, `.WRITE`, inserts, deletes |
| `t_dlog` | heap, append only | grows | one row per committed delete |
| `t_t1c`, `t_t1h`, `t_lob1` | clustered / heap / LOB | 3 k, 3 k, 600 | **only** T1 (cycle 1, 2) or T7 (cycle 3), only before the backup, plus T4 in cycle 1's D2 phase |
| `t_t2c`, `t_t2h`, `t_lob2` | same | same | **only** T2 / T8, only before the backup; T6 in D2's phase |
| `t_t3a` / `t_t3b` | clustered | 3 k each | T3 before the backup / T3 only after it |
| `t_t4c`, `t_t5c`, `t_t6c`, `t_static` | clustered | 3 k | T4, T5, T6, nobody |

**Workloads** (`gen.py`; each job is one batch under the helper's 30 s timeout, all start together).

- **Short transactions.** Three sessions on disjoint ids (`id % 4` = 0, 1, 2), 5 % of them rolled back. They run
  until 1 s after the backup ends, about 2,400–3,800 transactions per run. Ops: inserts into heap / ci / split,
  updates, deletes (logged in `t_dlog`), LOB whole-value updates, LOB `.WRITE`, LOB inserts and deletes.
- **Open transactions** use ids `≡ 3 mod 4` in their own ranges. Each is BEGIN; a few thousand row changes (updates,
  deletes, inserts, LOB updates, `.WRITE`, LOB inserts and deletes); then one change per busy table every 30 ms while
  the backup runs; then the end below.

  | run | transaction | ends with |
  |---|---|---|
  | F, F2 | T1 | COMMIT 1 s after the backup, touching nothing more |
  | F, F2 | T2 | ROLLBACK 1.5 s after |
  | F, F2 | T3 | touches `t_t3b` + another busy range after the backup, then COMMIT |
  | F, F2 | T5 | COMMIT at 40 % of the backup |
  | D2 | T4 | COMMIT after D2 |
  | D2 | T6 | ROLLBACK after D2 |
  | F3 | T7 | open across F3 and D5 (`run_span.sh`), then COMMIT |
  | F3 | T8 | open across F3 and D5, then ROLLBACK |
- **Backups**: `WITH CHECKSUM, COMPRESSION (ZSTD, HIGH), BUFFERCOUNT = 1, MAXTRANSFERSIZE = 65536`. Every quiet
  differential was followed by a quiet COPY_ONLY full (FC1–FC4) as the reference page state.

| file | kind | load | duration | log tail | committed after CheckpointLSN | open at LastLSN |
|---|---|---|---:|---|---:|---|
| X0 = **F** | full | yes | 3.2 s | 31.9 MB, 70,535 records | 454 of 3,803 | T1, T2, T3 (11,126 records) |
| D1 | diff | quiet (20 s later) | 1.5 s | 3 records | 0 | – |
| D2 | diff | yes | 2.8 s | 24.0 MB, 44,858 | 315 of 2,694 | T4, T6 (7,549) |
| D3 | diff | quiet | 2.0 s | 3 | 0 | – |
| F2 | full | yes | 3.4 s | 27.1 MB, 48,911 | 320 of 2,598 | T1, T2, T3 (10,917) |
| D4 | diff | quiet | 1.6 s | 3 | 0 | – |
| F3 | full | yes | 3.6 s | 23.0 MB, 36,919 | 374 of 2,429 | T7, T8 (6,818) |
| D5 | diff | yes, T7 / T8 still open | 2.9 s | 36.9 MB, 60,930 | 298 of 4,216 | **T7, T8 again** (7,642) |
| D6 | diff | quiet | 1.9 s | 3 | 0 | – |

Loaded differentials have a second pass too: D2 had 8,784 pass-2 units and D5 8,223.

**Truth** (`truth.sh`) is RESTORE of the same chain (F; F + Dn; …) as `bk_e6_r`, dumped as one canonical string per
row (E3 form: `key|ver|tag|ts|grp|MD5(val)|MD5(filler)`, LOBs `DATALENGTH|SHA1`) plus `%%physloc%%`.

**Comparison** (`compare6.py`) is a multiset per table, then per key:

| class | meaning |
|---|---|
| `dirty` | the lake shows a value of a transaction open at that file's LastLSN, or hides a row it deleted; deleted keys come from the tail's before-images (`hidden_keys.py`) |
| `stale` | the server's committed row is newer than the lake's, or missing from it |
| `stale_del` | a committed delete is missing |
| `dup` | a row is in the lake twice |
| `lob_only` | only the LOB value differs |

Locators are checked on rows that are equal: heap `_mssql_rid` against physloc, clustered `_mssql_page` against the
physloc page.

## 2. Q1: when is the DCM reset?

**From the log** (`out/fn_dblog_backup_dcm.txt`, `out/q1_dcm_*.txt`), every non-COPY_ONLY full looks the same. X0:

| when | LSN | record |
|---|---|---|
| 15:24:07.395 | | BACKUP starts |
| 15:24:07.403 | 51:205752 | `Backup:InvalidateDiffMaps`: LOP_MODIFY_ROW on `sysprufiles`, LOP_PREP_XACT, COMMIT |
| | 51:205792:1 | **LOP_CLEAR_GAM_BITS, LCX_DIFF_MAP, page 1:6** (op 0x11; the log reader showed it as an unknown op) |
| | 51:205912:22 | the backup's first BEGIN_CKPT |
| | 51:221576:165 | the third checkpoint = CheckpointLSN |
| | 51:228032:1 | LastLSN |
| 15:24:10.593 | 51:229992 | `Backup:CommitDifferentialBase`: LOP_MODIFY_COLUMNS on `sysprufiles` only, **no DCM change** |
| 15:24:10.617 | | BACKUP ends |

- F2 and F3 show the same records, 6 ms and 13 ms after their start.
- In X0's tail there are 1,650 DCM SET_BITS records after the clear (F2: 1,702), i.e. pages marked as changed while
  the backup ran.
- D1–D6 and FC1–FC4 contain neither transaction and no clear.

**On pages.** X0 → quiet D1 → quiet COPY_ONLY FC1, and F2 → D4 → FC3 (`q1_dcm.py`):

| check | F (X0) | F2 |
|---|---|---|
| pages live in COPY whose image differs from the full's copy (changed after it), or are new | 5,205 | 4,739 |
| … of them in the next diff with COPY's exact image | 5,204 + boot page | 4,738 + boot page |
| pages with a tail record newer than the full's copy (all after CheckpointLSN) | 1,062 | 929 |
| … of them in the next diff | 1,062 | 915; the other 14 are on extents freed later (not live in COPY either) |
| pages copied again in pass 2 and unchanged since | 8,025 | 8,626 |
| … of them in the next diff (pass-1 changes stay marked) | 8,025 | 8,626 |
| diff's extents = DCM ∩ GAM-allocated (E2 R1) | yes (+3) | yes (+2) |

Pages written after the reset that the next diff lacks are only ever on extents that are GAM-free in it. In F's
tail that was 3 pages of aborted short transactions and 8 of T2.

**Answer.** The DCM is cleared at the start of a full, before pass 1, and never again by that backup. Every page
changed after its copy in the full (during pass 1, during pass 2, in the CheckpointLSN → LastLSN window, or later)
is in every following differential, unless its extent has been freed. That is what makes healing possible. It also
means that **the only pages a next diff can lack are pages untouched since the reset**, which is the basis of the
pending-page rule.

## 3. Q2: the cycle, exact

### 3.1 The importer (`wal.py`, `lake6.py`)

The importer is E1's lake (`lake.py`), plus:

1. **Page store** = latest copy of each page (pass 2 over pass 1), PFS-checked (E1 / E3).
2. **Redo: repeat history.** In LSN order, apply every tail record newer than the page copy, for committed, aborted
   *and* open transactions, compensation records included, on the user tables' data and LOB pages. PFS allocation
   bytes are redone from LCX_PFS MODIFY_ROW records.
3. **Undo** the transactions open at LastLSN, in reverse LSN order, from their logged images:
   - heap and LOB pages physically (slots are stable);
   - clustered pages by key: the key comes from the row image, or from element 2 of MODIFY_ROW / MODIFY_COLUMNS
     (`0x16`, the key columns, the column count). It is searched on the logged page, then across the allocation
     unit, because a split may have moved the row.
4. **Pending pages**: a copy-on-write image of every page taken before undo touches it, plus every page an open
   transaction logged a row operation on, with the transaction ids. Stored in the lake (`__mssql_backup.pending`).
5. **Next file**, before its own redo (§4 has the final rule): pending pages resolved; images of the pages applied
   join the file's page store as if the file carried them. Then E1's apply, with the changed set = pages with
   m_lsn > W ∪ applied images ∪ pages formatted in the tail.
6. **Fuzzy-diff bookkeeping**:
   - pages the tail changes but the file does not carry are recorded as **stale** (`__mssql_backup.stale`); the next
     file must carry them, and it always did;
   - an unresolved LOB keeps its old value **and fragment map**;
   - tail records older than the previous file's LastLSN on pages the file does not carry are already in the lake
     and are ignored, FORMAT_PAGE included.

### 3.2 Results (`out/replay_all.txt`; `replay_all.sh` rebuilds every lake from the files, no server, 6 min)

Cycle 1, differing keys vs RESTORE:

| variant | F | D1 | D2 (loaded) | D3 |
|---|---|---|---|---|
| **main** (every rule) | **0** | **0** | 71: 21 stale, 11 deletes missing, 27 twice, 12 LOB; **0 dirty** | **0** |
| no recovery at all (E1 as is) | 8,457: **7,516 dirty**, 819 stale, 122 deletes missing | 11 LOB | 5,205: **4,486 dirty** | 24 LOB |
| no pending-page rule | 0 | **2,282** stale (T1's / T3's committed work lost) | 2,277 | **2,187**: it never heals |
| unresolved LOB → NULL, map dropped (E1) | 0 | 0 | 71 | **3 LOB lost for good** |
| transaction-consistent skip at D2 (§3.4) | 0 | 0 | 383, all stale: no duplicates, no torn LOB, 0 dirty | 0 |
| oracle: D2's missing base pages from FC1 | 0 | 0 | **0** | 0 |

Cycle 2: F2 applied incrementally on cycle 1's lake gives **0** and then D4 **0**. F2 imported fresh gives 0 and
then D4 0. The two lakes are **identical** (`lakediff.py`, `__mssql_sys` and service columns included).

Cycle 3:

- F3: **0**.
- D5, with T7 / T8 still open: main 151 (42 stale, 21 deletes missing, 79 twice, 9 LOB, **0 dirty**); skip 518 (all
  stale, 0 dirty); oracle **0**.
- D6, after T7 commits and T8 rolls back: **0** in all three variants.
- The lake chained since F and a lake built from F3 fresh are identical after D6.

**LOBs at the full imports: 0 differences** (F, F2, F3). The approximation the owner allowed was not needed. The
committed LOB updates during the backups (whole-value and `.WRITE`) and the open transactions' LOB updates (whole-
value rewrites in place, `.WRITE`, inserts, deletes) are all exact after redo + undo.

What redo and undo did on F: tail 70,535 records.

- **Redo**: 988 pages rebuilt.

  | op | records |
  |---|---:|
  | FORMAT_PAGE | 325 |
  | INSERT_ROWS (clustered / heap / LOB) | 483 / 358 / 55 |
  | DELETE_ROWS (ghost / heap / LOB) | 110 / 58 / 15 |
  | MODIFY_ROW (clustered / heap / LOB) | 360 / 237 / 147 |
  | MODIFY_COLUMNS | 8 |
  | DELETE_SPLIT | 127 |
  | EXPUNGE_ROWS | 7 |
  | PFS bytes | 214 |
- **Undo**: 9,278 row records of the 3 open transactions, on 3,304 pages.
  - Clustered: 1,916 inserts, 640 ghosting deletes, 2,513 modifies, 61 multi-column modifies.
  - Heap: 1,361 inserts, 340 deletes, 1,711 modifies.
  - LOB: 267 inserts, 39 deletes, 430 modifies.
  - **200 clustered rows were found on another page than the log names**: a split had moved them.
- **Pending pages: 3,372** (12 % of the database here; the open transactions were large).
- Timings: 0 redo / undo failures in every run. Recovery took 1.2 s of a 6.6 s Python import; differentials apply in
  2.5–4 s.

### 3.3 The pending-page rule at work

| next file | in the file (processed normally) | applied from the image | discarded (freed) | kept (still open) |
|---|---:|---:|---:|---:|
| F → D1 | 2,453 | 822 | 97 | – |
| F2 → D4 | 2,583 | 838 | 115 | – |
| F3 → D5 (T7, T8 open) | 1,073 | – | 1 | **1,225** |
| D5's pending → D6 (T7 committed, T8 rolled back) | 1,902 | 525 | 96 | – |
| D2 → D3 (a differential's pending pages) | 2,219 | 0 | 122 | – |

Differentials do not reset the DCM, so a differential's pending pages are always in the next file or freed. The
rule only matters after a full.

### 3.4 Why a loaded differential is not exact on its own

- **Where the gaps are.** All D2 tail records that the redo could not apply (86, on 62 pages; `q2_d2_nobase.py`)
  are **after D2's CheckpointLSN**, on pages first changed since F inside D2's own window. D2 does not carry those
  pages, and a full would.
  - About 70 of the records are committed changes: row updates, deletes, LOB fragment changes, and **the
    DELETE_SPLIT half of page splits** whose new page D2 does carry. That half is the source of the rows shown twice.
  - D5: 125 pages, 142 records.
- **The missing piece is the base image.** With base images for exactly those pages (the oracle: taken from FC1 for
  D2, from F3 for D5), both files come out exact. A lake that holds rows can only rebuild such a page if it also
  keeps each row's raw record bytes and slot (and the ghost records).
- **Without base images, two interim states are possible.** Both have 0 dirt, and both heal in the next file, which
  carried 62 / 62 and 125 / 125 of the stale pages, and 551 / 551 and 1,419 / 1,419 with the skip.
  - *Partial redo* (main): fewest stale keys, but torn splits (a row twice) and stale LOBs until the next file.
  - *Transaction-consistent skip*: a committed transaction with any record on a baseless page is skipped whole, and
    so is every later transaction on a page it left behind, to a fixed point (127 skipped in D2, 174 in D5). More
    stale keys (383 / 518), but never a duplicate or a torn value.
- **Two rules that only matter when files overlap** (found in cycle 3, where D5's tail starts at T7 / T8's BEGIN,
  before F3):
  - **Chain order by CheckpointLSN, not FirstLSN.** D5 and F3 have the same FirstLSN (52:120720:1). E1's rule
    "FirstLSN later than the last file's" refused a valid D5.
  - **Records older than the previous file's LastLSN on pages the file lacks are already in the lake.** Rebuilding
    such a page from its FORMAT_PAGE overwrote the lake's rows: 3 rows were lost for good in skip mode before this
    rule.

## 4. Q3: validating "absent and not open ⇒ apply the image"

Measured with `q3_pending.py` and `q3_absent.py`, against the transactions' real outcomes (events) and the server's
pages at the next file's time (COPY_ONLY FC1 / FC3 / FC4):

| | F → D1 | F2 → D4 | F3 → D5 → D6 |
|---|---|---|---|
| applied images equal to the server's page (live records, slot by slot) | **822 / 822** | **838 / 838** | **525 / 525** |
| committed transactions' pending pages absent and applied | T1 587, T1+T3 7, T3 228 | T1 563, T1+T3 1, T3 274 | T7 524, T7+T8 1 |
| **rolled-back transaction's pending pages absent from the next file** | **97** | **115** | **97** |
| … on an extent the rollback freed (GAM-free, PFS 0x00 / 0x08) | 97 | 115 | 96 |
| … allocated, applied | 0 | 0 | **1** (below) |
| rolled-back transaction's pages present (processed normally) | 1,283 | 1,289 | 1,173 |
| pages written after the DCM reset by committed transactions, in the next diff | 2,814 / 2,814 | 2,551 / 2,551 | 2,645 / 2,645 |
| T3's pages written only after the backup (`t_t3b`), in D1 | 211 / 211 | | |

**Counter-examples found:**

1. **Rolled-back pages on freed extents (97 + 115 + 96).** T2 / T8 inserted into newly allocated extents; the
   rollback removed the rows and freed the extents; a differential never copies a freed extent. The pages are absent,
   the transaction is not open, and the image still holds the rolled-back inserts. Applying it would put dirt in the
   lake whenever the file lacked the PFS page of that interval. Here the PFS page was always present, because the
   deallocation rewrites it, so the rows were never decoded. That was luck, not a rule. **Fix: discard a pending
   page that the new file's GAM marks free or whose PFS byte lacks 0x40.**
2. **A rolled-back transaction's page, absent and allocated (1, cycle 3).** T8 had inserted 16 rows on page 1:46066.
   Two splits, both before F3, moved them off it, and T7 then inserted there. T8's rollback undid its rows where they
   now lived, never on 1:46066. Applying the image (T7's rows) was correct and equals the server's page. So the
   inference "⇒ the transaction committed" is false here; what absence actually proves is that **the page has not
   changed since its image was taken**, whatever the outcome.
3. **Diffs do not need the rule.** They do not reset the DCM, so every page an open transaction touched is in the
   next file or freed: 0 applied out of 2,341.
4. **xid attribution.** Undo can find a row on a page the log does not name. The prototype first gave such pages
   every open xid, which mislabelled pages; it now records the transaction that undid there.

Cases considered and not seen, each of which would put P in the next file:

- a rollback writes CLRs to wherever the row now is;
- ghost cleanup after the commit writes the page;
- a page reused by another object is written;
- a transaction still open at the next file's checkpoint has its BEGIN inside that file's tail (FirstLSN ≤ its
  BEGIN), so "open" is decidable from the file;
- a COPY_ONLY full does not reset the DCM, so its successor diff carries everything.

**The refined rule (RESEARCH §4, step 3).** For each pending page P, in this order:

1. P in the file → process it normally and clear it.
2. P free in the file's GAM / PFS → discard the image; its rows go by rule 3 (freed pages).
3. Any of P's transactions open in the file's tail → keep P pending; the lake keeps P's rows undone.
4. Otherwise → apply P's image as if the file carried it, then clear it.

Apply the images **before** the file's own redo, so a tail record on P finds its base.

## 5. Q4: what redo and undo needed, and the effort

**Redo** (8 operations; everything else is ignored: NC-index and interior pages, MODIFY_HEADER, SET_BITS,
SET_FREE_SPACE, IAM / GAM / DCM, the delta records):

| operation | what it does to the page state |
|---|---|
| FORMAT_PAGE | an empty data / TEXT_MIX / TEXT_TREE page, allocated |
| INSERT_ROWS | clustered: insert at slot (shift); heap / LOB: set the slot. Rows are `elements[:-2]`, many for a split's move. A compensation record re-inserting over **its own ghost** (same key) replaces it |
| DELETE_ROWS | MARK_AS_GHOST: record type 6; heap / LOB: free the slot |
| MODIFY_ROW | replace **`size` (raw +58) bytes** at `offset` (+56) with the after image. Not `len(before)`: a compensation record has an empty before-image. On a LOB fragment, rewrite its u16 length (+2) |
| MODIFY_COLUMNS | element 0 offset pairs, element 1 replaced lengths, after images at elements 5 + 2i |
| DELETE_SPLIT | remove `rows` (+54) slots from the slot |
| EXPUNGE_ROWS | remove the ghost (clustered: shift) |
| PFS MODIFY_ROW | byte for page `offset − 4` of the interval: allocation bit 0x40 |

**Undo**:

- heap / LOB: the physical inverse (insert → free, delete → before image, modify → swap the ranges);
- clustered: by key, as above;
- pending images taken copy-on-write.

Page splits and delete-splits need no special code (multi-row INSERT_ROWS + DELETE_SPLIT). **LOBs need no tree
logic**: whole-value updates rewrite fragments in place with MODIFY_ROW, `.WRITE` modifies a range, appends add
fragments and links; all of it is the same ops on TEXT_MIX / TEXT_TREE records. F2's tail had TEXT_TREE ops and they
redid exactly.

**Which rule mattered.** Each switched off alone on F's import (`out/replay_all.txt`):

| rule switched off | differing keys |
|---|---|
| repeat history (E3: skip the open transactions, no undo) | 7,530: 7,519 dirty, 11 rows twice, one split failed to apply (its slots were off) |
| undo | 7,836 dirty |
| `size` from +58 (use `len(before)`, E3's rule) | 5 rows damaged (compensation records) |
| LOB fragment length rewrite | 21 LOB values wrong (E3's "LOB trees decode empty or wrong") |
| ghost-replace on a compensating insert | 0 here; it keeps the slot numbers right for later records on that page |

E3's redo prototype removed 94–97 % of the committed-side errors; these three fixes take it to 100 %, LOBs included.

**C++ estimate**, for rowstore heap / clustered with LOBs and row-overflow, in mssql (reader) and mssql-ducklake
(lake bookkeeping):

| piece | LOC | weeks |
|---|---:|---:|
| log-tail reader: MQTL blocks, sector parity, record header, row-log elements, transaction table, CheckpointLSN ordering | ~600 | 1–1.5 |
| page-state model (header + slot list, record length), overlay into the page store | ~400 | 1 |
| redo: 8 ops, PFS, compensation semantics, ghost replace, LOB length | ~600 | 1.5–2 |
| undo: physical + by key (uniquifier, variable-length and multi-column keys, which E6 did not have); per-allocation-unit key index | ~600 | 2 |
| pending pages: images in the lake, allocation-gated resolution, applied before redo; stale pages; LOB keep-old | ~500 | 1.5 |
| optional transaction-consistent skip for loaded differentials | ~200 | 0.5 |
| oracle harness: workloads during BACKUP, RESTORE truth, per SQL Server version | – | 2–3 |
| **total** | **~3,000** | **9–12** |

The Python prototype is `wal.py`, 672 lines: about 150 for the log, 140 for the page model and 360 for the engine,
including the switches. `lake6.py` adds 220 lines to E1's `lake.py`. This is far below RESEARCH §8 phase 4 (3–6
months), because only a backup's own tail is replayed, never a log chain.

Not covered, each with a rough addition:

- **ADR / RCSI / snapshot** (+3–4 wk). In-row version tags change record lengths; ADR replaces physical undo with
  logical revert from the persistent version store.
- **ROW / PAGE compression** (+2–3 wk). Byte-range redo applies to CD records as they are, but the page dictionary
  and anchor (CI) record need their own ops.
- **DDL inside the tail** (+1–2 wk). The catalog pages would need redo too.
- **Columnstore**: open (delta stores are rowstore; delete bitmaps, tuple mover).
- Multiple files and partitions should be straightforward.

## 6. Changes to the design (RESEARCH §4, §6.2, E1's rules)

1. §4 step 1 confirmed, and stronger: redo must repeat history for **all** transactions, then undo the open ones.
   LOBs need no forgiveness.
2. §4 step 2: pending pages = pre-undo images of every page undo changed (including pages where it found a moved
   row) and of every page an open transaction logged a row operation on.
3. §4 step 3: the refined rule in §4 above. Gate by allocation first; "absent" means "unchanged since the image";
   apply images before the file's own redo.
4. §4 step 4 answered: the DCM is reset at the start of a full.
5. E1 rule 1: order files by **CheckpointLSN** (a transaction open across two backups gives both the same FirstLSN).
   E1 rule 2 (W = min(FirstLSN, CheckpointLSN)) stands.
6. New: a **loaded differential** cannot be redone completely by a lake that keeps rows, not pages.
   - Record its unredoable pages as stale; the next file carries them.
   - Choose partial redo or transaction-consistent skip for the interim state; both are dirt-free.
   - Or keep raw records per row so the base pages can be rebuilt (the oracle shows that makes it exact). This is a
     storage decision for the owner.
7. New: ignore tail records older than the previous file's LastLSN on pages the file does not carry.
8. E1 rule 8 amended: an unresolvable LOB keeps its old value **and fragment map**. Dropping the map made a later
   partial update unresolvable for good (3 values).

## 7. Caveats

- One server (2025 CU8, Linux), one data file, rowstore without compression, int / bigint clustered keys (no
  uniquifier, no variable-length key), no ADR or RCSI, no DDL during backups.
- Backups lasted 3–3.6 s, bounded by the 30 s batch limit; real backups have longer windows and more stale pages in
  loaded differentials.
- The pending set is proportional to what open transactions touched: 12 % of this small database, about 8 KB per
  page in the lake.
- The truth is RESTORE of the same files, i.e. SQL Server's own recovery of each file.

## 8. Files (`experiments/E6/`)

- **Workload and capture**:
  - `sql/01..11` set the database up; `gen.py` generates the workloads, in `sql/gen/`;
  - `run.sh` runs a loaded backup, `run_span.sh` the F3 + D5 run, `quiet_backup.sh` the quiet ones;
  - `truth.sh` does RESTORE + dump.
- **Engine**:
  - `wal.py`: log records, page model, recovery with counterfactual switches;
  - `lake6.py`: E1's lake + recover / pending / stale;
  - `pages_ext.py`: E1's, plus rebuilt-page iteration and LOB errors caught.
- **Analysis**:
  - `compare6.py`, `q1_dcm.py`, `q3_pending.py`, `q3_absent.py`, `q2_d2_nobase.py`, `hidden_keys.py`, `facts.py`,
    `lakediff.py`;
  - `census.py`, `probe_*.py` were used to learn the record layouts.
- **Replay** (no server):
  1. `../../.venv/bin/python decomp.py X0 D1 D2 D3 F2 D4 F3 D5 D6 FC1`, which needs Python ≥ 3.14 for zstd;
  2. `./replay_all.sh`, about 6 min.

  `truth.tar.gz` is extracted on demand.
- **Data**:
  - `bak/` holds the 12 backups, 961 MB: the chain files plus FC1, FC3, FC4. FC2 was deleted after use.
  - `truth.tar.gz`, 185 MB, holds all 9 truths.
  - `out/` holds the measurements: `replay_all.txt`, `q1_dcm_*`, `q3_*`, `facts.txt`, `fn_dblog_backup_dcm.txt`,
    `events_all.txt`, per-step comparison JSON.
  - `logs/` holds the per-step importer statistics.
  - The decompressed `mtf/` and the lakes are not kept; they regenerate.

Server cleanup: `bk_e6`, `bk_e6_r` and `bk_e6_meta` were dropped, and `xp_delete_files N'/var/opt/mssql/data/bk_e6_*'`
was run. `sys.dm_os_enumerate_filesystem('/var/opt/mssql/data/', 'bk_e6*')` and `sys.databases LIKE 'bk_e6%'` both
return nothing.
