# Spec 090: redo and undo of a backup's own log tail

Status: **draft**, 2026-11-05. Not started. Depends on 085 (page store, log-tail reader) and 089 (lake
mode, `base`, the changed set). Series overview: `../README.md`. Markers and source abbreviations as
in 085; owner questions as DQ-nn ([decisions.md](../085-backup-reader-core/decisions.md)).

## Problem

A full backup is fuzzy: pages are copied while the database changes, and RESTORE replays the backup's
log tail, redo then undo (W§9). Without that, a reader shows the database as of `CheckpointLSN`, dirty
reads included (E3§2): 0.2–0.4 % of the rows of busy tables differ at moderate load, 1–2 % under heavy
load, up to 12–20 % of an append-only heap; uncommitted work of open transactions is visible; torn
page splits show a row twice or not at all (E3§3).

**The owner's decision (R§4, 2026-11-05): no dirt in restored data.** Uncommitted work is deferred to
the next file, and the next file must restore exactly. For a loaded differential, **variant 1**:
tail transactions whose pages the file lacks are skipped whole, the affected pages are tracked as
stale, and the next file heals them.

E6 met it in Python on three loaded full backups and the differentials after them: 17 tables, LOBs
included, exact against RESTORE after every full and after every file that follows a fuzzy one, and
dirt-free at every file (E6 short answer). This spec builds that engine in the reader and surfaces it
through the scan and the incremental API. It replays only a backup's **own** tail, never a chain of
log backups (R§8 phase 4 stays out).

## Two modes

- **Restore mode**: the reader holds every base page, because it has the full (alone, or with its
  differential through `diff :=`). The result is what `RESTORE … WITH RECOVERY` gives: redo the last
  file's tail, then undo the transactions open at its `LastLSN`.
  - A full alone: exact on E6's F, F2 and F3 (0 differences, 8,457 differing keys without it).
  - A full with its differential: the page store is the overlay (085 D12) and the tail is the
    differential's. RESTORE redoes the full's tail first (`WITH NORECOVERY`: redo, no undo, W§9),
    then lays the differential over it and recovers with the differential's tail. The reader can skip
    the full's redo and get the same pages: every page that redo would change after the full's DCM
    reset is replaced by the differential's copy (the DCM is cleared at the start of a full, E6§2), and
    every record before the reset is already in the page copies (pass 1 reads pages as of the first
    checkpoint, E3§2). This matches E6's oracle run (D2 and D5 exact once the missing base pages were supplied, E6§3.4) — **inferred, to
    verify** (task 5).
- **Lake mode**: the reader has one file and the importer's `base` (089 D10), not the pages the lake
  was built from. Exact after a full; after a loaded differential, dirt-free and possibly stale, healed
  by the next file (E6§3.2).

## Decisions

- **D1 — repeat history, then undo (E6§6.1).** Redo every tail record newer than its page copy, for
  committed, aborted **and** open transactions, compensation records included; then undo the
  transactions open at `LastLSN` from their logged images. Skipping the open transactions instead
  (E3's prototype) left 7,519 dirty keys and broke a split (E6§5).

- **D2 — seven row operations and the PFS byte, nothing LOB-specific (E6§5,** which counts them
  as "8 operations plus the PFS byte"; its table, like `wal.py`'s `ROW_OPS`, has seven plus PFS). LOB fragments are records
  on TEXT_MIX / TEXT_TREE pages, changed by the same operations; whole-value updates, `.WRITE` and
  appends all redid exactly. Only data and LOB contexts of the user tables' base rowsets are redone;
  NC-index and interior pages, MODIFY_HEADER, SET_BITS, SET_FREE_SPACE, IAM / GAM / DCM records and the
  delta records are ignored.

- **D3 — recovery is on by default.** `mssql_backup_scan(…, recovery := true)` returns what RESTORE
  returns; `recovery := false` returns 085's page state with its fuzzy status. A table recovery cannot
  cover (D7, the guards) falls back to 085's page state under the `fuzzy` policy: rows and a warning
  under `'warn'`, refused under `'error'` (the importer always passes `'error'`). Reason: the owner's
  rule, and an oracle that is RESTORE. The default for browsing is **for the owner to confirm**
  (DQ-13, with 085 D11's `fuzzy` default).

- **D4 — variant 1 for loaded differentials (owner).** `loaded_diff := 'skip'` (default) or
  `'partial'` (E6's main variant: fewer stale keys, but rows twice and torn LOBs until the next file;
  whether to offer it at all is DQ-17). mssql-ducklake's import option of the same name passes it
  through (022 D1).
  Keeping raw record bytes per row to make a loaded differential exact on its own is not done (owner:
  it roughly doubles storage).

- **D5 — pending pages are resolved with allocation first (E6§4).** The rule order below; the
  inference is "absent means unchanged since the image", not "committed".

- **D6 — files are ordered by `CheckpointLSN`** (E6§6.5), enforced by 089's chain check.

- **D7 — refuse what the engine does not cover, by name, per table.** Recovery either makes a table
  exact (restore mode, a lake-mode full) or dirt-free (a loaded differential), or the table is
  `unrecoverable` with the reason, and the importer refuses the file, naming the table (one snapshot
  per file leaves no way to apply the rest without it). Never dirt with a warning. For a columnstore
  table 022 Q4 drafted "mark it stale" instead; that disagreement is DQ-07.

- **D8 — the page-state model is the reader's.** A page under recovery is its 96-byte header plus a
  slot list of record bytes (E6 `PageState`); its serialisation (versioned, checksummed) is the pending
  image format. Rebuilt pages join the page store as page sources like any other; the decoders above
  do not know a page was rebuilt.

## Rules

Log-format facts are E3§6 and 085's log-tail reader: sector parity, record header, opcodes, contexts,
the AU of a row operation (`u32 @32`, `u16 @46`), row-log contents at the record's fixed length: u16
n, n × u16 lengths, the elements 4-byte aligned. Op-specific fields: MODIFY_ROW offset u16 @56, size
u16 @58; DELETE_SPLIT rows u16 @54. Element layouts (E6 `wal.py`): INSERT_ROWS `[row…, '', lock]`
(rows = all but the last two; a split moves many rows in one record); DELETE_ROWS `[before, lock]`;
MODIFY_ROW `[before, after, key, lock, …]`. Every fact **(2025)**.

**Redo** — records with `page_lsn(copy) < lsn < LastLSN`, in LSN order, on contexts 1 HEAP,
2 CLUSTERED, 5 TEXT_MIX, 6 TEXT_TREE, 19 MARK_AS_GHOST of a user table's base-rowset AUs, plus PFS:

| operation | effect on the page state |
|---|---|
| FORMAT_PAGE (0x01) | an empty page, allocated: TEXT_MIX for context 5, TEXT_TREE for 6, data otherwise |
| INSERT_ROWS (0x02) | clustered: insert each row at slot s + i (shift). A compensation record re-inserting over **its own ghost** (same key) replaces the ghost. Heap / LOB: set the slot |
| DELETE_ROWS (0x03) | context 19: the record's type becomes 6 (ghost); clustered: remove the slot (shift); heap / LOB: free the slot |
| MODIFY_ROW (0x04) | replace **`size` (u16 @58)** bytes at `offset` (u16 @56) with the after image — not `len(before)`: a compensation record has an empty before-image (5 rows damaged otherwise). On a LOB fragment (record type 4, context 5 / 6) rewrite its u16 length @2 (21 LOB values wrong otherwise) |
| MODIFY_COLUMNS (0x06) | element 0 = offset pairs, element 1 = replaced lengths, after images at elements 5 + 2i; ranges applied last to first |
| DELETE_SPLIT (0x0b) | remove `rows` (u16 @54) slots from slot s |
| EXPUNGE_ROWS (0x0d) | remove the ghost: clustered shifts, heap / LOB frees the slot |
| PFS MODIFY_ROW (context 11) | the byte of page `offset − 4` of the interval; bit 0x40 sets the page's allocation |

A page formatted in the tail is live; liveness after redo reads the redone PFS byte. (GAM records are
not redone: a page in a newly allocated extent is formatted and its PFS byte redone, which is what E6
needed.)

**Undo** — the row operations of transactions open at `LastLSN`, in reverse LSN order:
- heap and LOB pages, physically (slots are stable): an insert frees its slots; a delete puts back its
  before image (element 0); a modify puts the before range back over the after range (and rewrites a
  LOB fragment's length);
- clustered pages, **by key**: the key comes from the row image (inserts, deletes) or from element 2
  of MODIFY_ROW / MODIFY_COLUMNS (`0x16`, the key columns, u16 column count, the null bitmap); the row
  is searched on the logged page, then across the allocation unit through a per-AU key index, because a
  split may have moved it (200 rows on F). An insert's row is deleted; a ghost is replaced by the before
  image (re-inserted in key order if the ghost is gone); a modify is reverted. E6 had only `int` /
  `bigint` keys: the uniquifier, variable-length and multi-column key forms of element 2 are
  **unverified**;
- a heap or LOB record on a page the file does not carry is not undone: the lake never received it, or
  holds it undone already as a pending page.

**Pending pages** (lake mode). Recorded after a file's recovery: the pre-undo image (copy-on-write) of
every page undo changed, including pages where it found a moved row, and of every page an open
transaction logged a row operation on; each with its transaction ids (for a moved row, the transaction
that undid there, E6§4 counterexample 4). At the next file, for each pending page P, **in this order,
before the file's own redo**:
1. P in the file → processed normally; cleared.
2. P free by the file's allocation (GAM free, or PFS without 0x40; the base image when the file lacks
   the interval, 089 D4) → the image is discarded; its rows go by the freed rule. (Rolled-back
   transactions freed whole extents: 97, 115 and 96 pages whose images still held rolled-back rows.)
3. Any of P's transactions open in the file's tail → kept pending; the lake keeps P's rows undone.
4. Otherwise → P's image is applied as if the file carried it, and cleared. (Right for 822 / 822,
   838 / 838 and 525 / 525 pages, equal record for record to the server's page; one of them belonged
   to a transaction that rolled back after splits had moved its rows away.)

A differential resets no DCM, so its pending pages are always in the next file or freed (0 applied of
2,341, E6§3.3); the rule matters after a full.

**Reflected records** (lake mode): a tail record older than the previous file's `LastLSN`
(`reflected_lsn`), on a page the file does not carry and with no pending image, is already in the lake;
it is ignored, FORMAT_PAGE included (rebuilding such a page lost 3 rows for good, E6§3.4).

**Stale pages** (lake mode, a loaded differential):
- A tail record on a page with no base (not in the file, no pending image, no earlier FORMAT_PAGE, not
  reflected) cannot be redone: a lake of rows has no image to apply it to (86 records on 62 pages in
  D2; 142 on 125 in D5, all after the file's `CheckpointLSN`).
- `skip` (D4): a committed or aborted transaction with any record on a baseless page is skipped whole,
  and so is every later transaction with a record on a page a skipped transaction left behind, to a
  fixed point (127 transactions in D2, 174 in D5); open transactions are never skipped (their records
  there are already absent from the lake). Every page those transactions touched is **stale**. Result:
  older, never a duplicate, never a torn value (383 and 518 stale keys, 0 dirty).
- `partial`: redo what has a base; the baseless pages are stale (71 and 151 differing keys, 0 dirty).
- A LOB whose missing node lies on a stale page keeps its old value **and** its fragment map (E6§6.8:
  dropping the map lost 3 LOBs for good), and the row's page is reported `stale` with reason `lob`;
  any other unresolved LOB is 089's named refusal.
- **Invariant**: the next file carries every stale page or frees it (62 / 62, 125 / 125; with skip 551
  / 551, 1,419 / 1,419). The stale set is lake state, not a reader input (089 D10), so the importer
  checks it at that file against `mssql_backup_changed_pages` (022 D7). This draft makes a page the
  file neither carries nor frees a named error; 022 drafted "report it, keep it stale and proceed".
  That disagreement is DQ-06.

**The changed set in lake mode** (089 D3) adds the applied pending images, the pages formatted in the
tail, and every page recovery rebuilt (E6§3.1 step 5).

## Surface

- `mssql_backup_scan(…, recovery := true, loaded_diff := 'skip', reflected_lsn := …)`, and the same
  three parameters on `mssql_backup_changed_pages`, `mssql_backup_export_plan` and
  `mssql_backup_export` (089), so the importer's export, delete evidence and pending set come from
  one recovery. The recovered page state is kept in the `OpenedBackup` (085 D9), keyed by
  (`since_lsn`, `base`, `reflected_lsn`, `loaded_diff`), so the functions an import calls on one file
  recover once. In lake mode `base` also carries `pending.parquet`, the previous file's
  `mssql_backup_pending_pages` rows `(file_id, page_id, page, xids LIST(UBIGINT), lsn, image BLOB)`.
- `mssql_backup_tables`: `fuzzy_status` after recovery is `recovered`, `stale` (with
  `stale_pages`, `skipped_transactions`), or `unrecoverable` (with `reason`); `raw_status` keeps 085's
  pre-recovery status.
- `mssql_backup_changed_pages` gains kinds `pending_applied`, `pending_discarded`, `pending_kept`,
  `stale` (with `reason`: `baseless` / `skipped_transaction` / `lob`, and the skipped transaction ids),
  and `changed` rows with reason `formatted_in_tail` / `rebuilt`.
- `mssql_backup_pending_pages(path, since_lsn, base, reflected_lsn)`: `(file_id, page_id, page, xids,
  lsn, image)`, the pending pages this file leaves, which the importer stores and passes back as
  `base/pending.parquet` with the next file.
- `mssql_backup_info`: `recovered_consistent` (database level after recovery), `skipped_transactions`.

What mssql-ducklake stores between files (its own tables; R§6.2, research-ducklake-import §9):
pending images with their transaction ids (`__mssql_sys.pending_pages`, the rows above plus its
import sequence), the stale page set (`__mssql_sys.stale_pages`), `reflected_lsn` per import
(`__mssql_backup.imports.last_lsn`). Where the pending images, which hold uncommitted rows, are kept is
DQ-09.

## Guards: refused by name (D7), each with its extension estimate

| case | why | detection | extension |
|---|---|---|---|
| ROW-compressed rowset with tail records | byte-range redo applies to CD records as they are (E6§5), but record lengths need 086's CD walk | `cmprlevel` 1 | ROW and PAGE together 2–3 wk (E6§5), after 086 PR 1–2; ROW is the smaller part |
| PAGE-compressed rowset with tail records | the anchor and dictionary need their own operations | `cmprlevel` 2 | (with ROW) |
| ADR, RCSI, snapshot isolation | in-row version tags change record lengths; ADR reverts logically from the version store | version tags (statusA 0x40) in records the tail touches; a boot-page flag to locate | 3–4 wk |
| DDL in the tail | catalog pages would need redo | row operations on catalog base tables beyond the counters plain DML writes (which ones it writes is measured first, task 9) | 1–2 wk |
| TDE | the log tail is encrypted (087 D9) | 087 | after E10 |
| columnstore delta stores and delete bitmaps | rowstore records (the bitmap is CD) | 088's rowset roles | 1 wk, after 086 PR 1 and 088 (refuse or mark stale until then: DQ-07) |
| the tuple mover or a compression in the tail | new segments and rowgroup rows | 088 D6 | open |
| an operation outside the seven and the ignored list, on a data or LOB context | unknown effect | opcode | — |
| a record that cannot apply (slot out of range, modify past the record) | the state is not what the log assumes | the engine | — (E6: 0 in every run) |

## Security and hardening

- Log contents are bounded: element counts ≤ 64 (`wal.py`), lengths within the record, offsets and
  sizes within the target record, slots within the slot list, rows within 8,060 bytes.
- The page-state model never grows a page past the reconstructed limit of its type; a page that would
  is `unrecoverable`, named (E3's prototype overflowed pages: a sign of a wrong rule, not data).
- Pending images from `base` are untrusted: version, checksum, header naming the claimed page, record
  lengths, xids; a malformed image is a named error, never applied.
- Undo by key searches only the logged page and the AU's own pages through the key index; no unbounded
  scans.
- Fuzz harnesses: `fuzz_backup_log_contents` (elements and op fields), `fuzz_backup_redo` (synthetic
  page states and records, with an oracle: a synthetic transaction redone then undone returns the
  original page), `fuzz_backup_pending_image` (the serialisation). DuckDB-free.

## Testing and acceptance

The oracle lane ports E6's harness: `experiments/E6/gen.py`'s workloads (three sessions of short
transactions, 5 % rolled back; open transactions T1–T8 with E6§1's outcomes, including two open across
a full and the next differential), backups stretched `WITH CHECKSUM, COMPRESSION (ALGORITHM = ZSTD,
LEVEL = HIGH), BUFFERCOUNT = 1, MAXTRANSFERSIZE = 65536`, truth = RESTORE of the same chain, and
E6's comparison classes (`dirty`, `stale`, `stale_del`, `dup`, `lob_only`) plus the locators. In CI the
workloads run as background `sqlcmd` jobs, free of the 30 s helper timeout that bounded E3 / E6
(E6§7). Added beyond E6: E3's runs (r1–r6, heavy r3); a uniquifier, a variable-length key and a
three-column key; heap and clustered tables with row-overflow.

Version matrix: 2017 / 2019 / 2022 / 2025 for restore mode and cycle 1 at least.

Acceptance:
1. **Restore mode, full**: every table of every loaded full equals RESTORE; 0 dirty.
2. **Restore mode, full + differential**: equals RESTORE of the pair (verifies the inference above).
3. **Lake mode**: E6's three cycles reproduce: after every full and every file that follows a fuzzy
   one, exact; after a loaded differential, 0 dirty and the stale set healed by the next file; a lake
   built by F2 applied incrementally equals F2 imported fresh, mirror and service columns included.
4. **Counterfactuals** fail as E6§5 measured, each rule switched off in a test build: no repeat
   history, no undo, `len(before)` for MODIFY_ROW, no LOB length rewrite, no allocation-first pending
   rule, no reflected-LSN rule, FirstLSN ordering.
5. Every guard case is refused by name for its table; the other tables recover.
6. Fuzz harnesses replay clean under ASan and UBSan.
7. Recovery of a 1 GB loaded full stays within a measured bound (E6: 1.2 s of a 6.6 s Python import
   on 240 MB); the C++ target is set by the first measurement.

## Tasks

1. Log contents on 085's reader: elements, op-specific fields, the transaction table with outcomes —
   2 d
2. Page-state model: header plus slot list, record lengths (FixedVar, blob fragment, stub, version
   tag), the overlay into the page store, the pending-image serialisation — 5 d
3. Redo: the seven row operations, PFS, compensation, ghost replace, LOB length, reflected records — 8 d
4. Undo: physical; by key with a per-AU key index; uniquifier, variable-length and multi-column keys;
   transaction attribution — 10 d
5. Restore mode: a full; a full with its differential — 2 d
6. Lake mode: pending images, the four-step resolution before redo, `mssql_backup_pending_pages`,
   `base/pending.parquet` — 6 d
7. Stale pages, the `skip` planner (fixed point), `partial`, LOB keep-old on stale pages, the
   next-file invariant — 3 d
8. Surfaces: `recovery`, the status columns, the new `changed_pages` kinds — 2 d
9. Guards, and the measurement of which catalog row operations plain DML writes — 2 d
10. Oracle harness: E6 / E3 workloads in CI, RESTORE truth, the three cycles, restore-mode checks,
    counterfactual builds, the new key shapes, the version matrix — 12 d
11. Fuzz harnesses log_contents / redo / pending_image — 2 d
12. Docs (README: what "recovered" means; `DATAMODEL.md`: the recovery layer and the lake-mode
    flow with a diagram; CLAUDE.md; CHANGELOG) — 1 d

**Total ≈ 55 days, 10–12 weeks.** E6§5 estimated 9–12 weeks (~3,000 lines) including the log-tail
reader, of which 085 carries about a week. R§8 phase 1c: 9–12 weeks. The guard extensions are on top.

## Risks and open questions

- **Only E6's shapes are measured**: one 2025 server, one data file, uncompressed rowstore, `int` /
  `bigint` clustered keys, no ADR / RCSI, no DDL during the backups, backups of 3–3.6 s (E6§7). Real
  backups have longer windows, so a loaded differential leaves more stale pages.
- **Restore mode with a differential** is inferred (task 5 verifies it).
- **The pending set** is proportional to what open transactions touched: 12 % of E6's small database,
  about 8 KB per page in the lake (E6§7).
- **Undo by key** for the key shapes E6 lacked.
- **Guards will refuse real tables** (ROW / PAGE compression is common): the order of the guard
  extensions, against 088 and the import, is for the owner (DQ-03).
- Open for the owner ([decisions.md](../085-backup-reader-core/decisions.md)): the order of 090 relative to 088 (R§10.4; DQ-03);
  `recovery := true` as the default for browsing too (D3; DQ-13); an unhealed stale page as an error
  or a report (DQ-06); a columnstore table recovery cannot cover (DQ-07); `'partial'` offered at all
  (DQ-17); where pending images live (DQ-09).

## References

- RESEARCH.md §4 (consistency: E3's measurements, the owner's decisions, E6's verification), §6.2
  ("For the lake model"), §8 phase 1c.
- E3 §2 (how a backup is taken), §3 (the error), §5 (the first redo), §6 (log format).
- E6 short answer, §2 (DCM timing), §3 (the cycle; §3.1 the importer, §3.3 pending at work, §3.4 loaded
  differentials), §4 (the pending rule and its counterexamples), §5 (operations, undo, which rule
  mattered, effort), §6 (changes to the design), §7 (caveats); `experiments/E6/wal.py`, `lake6.py`,
  `gen.py`.
- research-web.md §9 (consistency of a full backup), §11 (log format).
- 085 (log-tail reader, page store), 087 D9 (TDE), 088 D6, 089 D3, D4, D10.
