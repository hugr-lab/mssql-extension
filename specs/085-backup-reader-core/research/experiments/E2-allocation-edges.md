# E2 — Allocation edges for a lake-as-base differential reader

Date: 2026-11-05. Server: SQL Server 2025 RTM-CU8 (17.0.4075.5), container `mssql-v2test`. Research only.
T-SQL went through `tools/sql.sh` / `tools/sqlh.sh`. Files came out with `docker cp` only. Databases
`bk_e2a` … `bk_e2i` and the backups `bk_e2_*` are dropped and deleted on the server
(`xp_delete_files`, re-listed: 0 files, 0 databases). Local copies of all 37 backups are in
`experiments/E2/bak/` (702 MB).

The question behind every item: can a reader apply a differential to a lake that holds the previous
state, **without the full backup**? Every claim below was checked on decoded backup pages. Where it
says "live", it was compared with `sys.dm_db_database_page_allocations`, `%%physloc%%` or
`sys.system_internals_partition_columns` captured at the time.

## 0. Rules in one page

| # | Rule for the reader | Evidence |
|---|---|---|
| R1 | A diff copies exactly the extents with **DCM = 1 and GAM-allocated**, per data file. A full copies every GAM-allocated extent. Both also copy the extent of every PFS page. **A freed extent is never copied, even with its DCM bit set.** | §1.1 (bk_e2a, bk_e2e, bk_e2g/h/i: every diff) |
| R2 | **The DCM is "pages written since the base", not "allocation changed".** DROP, TRUNCATE and deferred drop free extents *without* setting their DCM bits. The diff carries the evidence of freed space only through GAM, PFS, IAM and the catalog pages. | §1.2 |
| R3 | A page in a backup is live only if it is below the file's `Size` (file header 1:0), its extent is GAM-allocated, and its PFS byte has 0x40. Otherwise its header and rows are stale. This applies to full backups too. | §1.3, §2.3 |
| R4 | Every allocated page carries the correct allocation unit id, after SHRINKFILE and REBUILD too: 10,473 pages checked, 0 mismatches. Stale AU ids exist only on unallocated pages, and on pages of a dead (deferred-drop) AU. | §2 |
| R5 | Keep a lake **page map** (file, page → AU, LSN) for every page that holds rows, plus the latest image of every allocation page (file header, GAM/SGAM/PFS/DCM, IAM pages; a few pages per 64 MB). Per diff, delete the rows of every page that R3 now rejects, that names another AU, or whose AU is gone or has type 0 / owner 0. | §1, §2, §3 |
| R6 | REBUILD (heap or clustered) gives a new rowset id and new IN_ROW and ROW_OVERFLOW AUs. **The LOB_DATA AU id is kept and its pages are not rewritten**, so they are not in the diff. In-row LOB roots are byte-identical, so match LOB values by root bytes, not by RID. Heap RIDs all change. | §3 |
| R7 | Decode rows with `sysrscols` per rowset. Skip status 0x02 (dropped) columns; they still occupy their bytes. A column whose null-bitmap ordinal exceeds the record's column count is missing from the record: its value is the `sysseobjvalues` default (valclass 1, id = rowset id, subid = rscolid) when status has 0x200, else NULL. | §4 |
| R8 | LOB in-row roots carry an update counter (`ts`, +4): byte-identical root = unchanged value, so copy it from the lake. A changed root with every reachable fragment page in the diff is read from the diff. A `.WRITE` partial update leaves most fragments out of the diff. **With a per-value fragment map stored in the lake it applies exactly** (all six cases); without it, refuse. | §5 |
| R9 | All data files of all filegroups share **one** MSDA per pass, in file_id order; pages carry the real file id. A striped set = FamilyCount complete MTF files with one MediaSetId. Each extent lives in exactly one stripe: index all stripes into one page store. | §6 |
| R10 | CHECKSUM, BLOCKSIZE and MAXTRANSFERSIZE never change page images. Walk MTF and stream headers; never hard-code offsets. Skip `m_type 101` pads by type: they trail each stream, their count depends on the transfer size, which SQL Server sometimes picks itself, and their bodies are stale memory. | §7 |
| R11 | FULL, SIMPLE and BULK_LOGGED produce the same differential. Minimally logged operations set the DCM; the BCM is irrelevant to diffs. The compatibility level changes nothing on disk except `dbi_cmptlevel`. | §8 |

Corrections to `RESEARCH.md` are collected in §9.

## Files

| Path | What |
|---|---|
| `E2/e2lib.py` | Backup loader on top of `tools/pages.py`: every MQDA unit with its pass; GAM/SGAM/DCM/BCM/PFS/IAM decoding; per-pass extent lists; positional check (does each page sit at unit `page % 8` of its 8-unit group?). |
| `E2/overlay.py` | The correct overlay (file size, GAM, PFS), also added to `tools/pages.py` as `apply_diff`. |
| `E2/q1_freed.py`, `q2_shrink.py`, `q2_au_check.py`, `q2_stale_census.py`, `q3_rebuild.py`, `q4_alter.py`, `q5_lob.py`, `q5_lobmap.py`, `q6_files.py`, `q6_stripe.py`, `q8_recovery.py` | One script per question. Outputs are in `E2/out/*.txt`. |
| `E2/truth.sh`, `rids.sh`, `pcols.sh` | Live-truth capture: page allocations (DETAILED), AU map, `%%physloc%%` per row, partition columns. Outputs are in `E2/truth/`. |
| `E2/sql/*.sql` | Every batch that was run, in order (a* = Q1, b* = Q2/Q3, c* = Q4, d* = Q5, e* = Q6, f* = Q7, g* = Q8, z99 = cleanup). |
| `tools/pages.py` (extended) | New: `bitmap_extents`, `interval_bits`, `pfs_byte`, `iam_info`, `file_size`, `is_live_page`, `live_pages`, `apply_diff`, command `alloc`. `FILE1+FILE2` now applies the diff with the allocation check (the naive "diff pages win" doubled every row after a shrink). The existing functions are unchanged. |

## Layouts calibrated here (against DBCC PAGE)

- **GAM 1:2, SGAM 1:3, DCM 1:6, BCM 1:7**, repeated every 511,232 pages. Bitmap at page offset 194, 7,988 bytes, bit i (LSB first) = extent i. GAM 1 = free.
- **PFS** (1:1, then every 8,088 pages). One byte per page from offset 100: 0x40 allocated, 0x20 mixed, 0x10 IAM, 0x08 has ghosts, low 3 bits fullness.
- **IAM.** Header record at 96: +4 u32 sequence, +40 start page (u32 page, u16 file), +46 eight 6-byte single-page slots. Extent bitmap at 194. IAM pages are single-page allocations in mixed extents even with `MIXED_PAGE_ALLOCATION OFF` (PFS 0x70).
- **File header 1:0**, slot 0 record (its offset varies: 96 or 2494 seen): +151 BindingID, +167 u16 file id, +169 u16 filegroup id, +171 u32 **Size** (pages), +175 MaxSize, +179 Growth.
- **TAPE's RAID stream** (32 bytes): +0 MediaSetId GUID, +20 u16 FamilyCount, +24 u16 FamilySequenceNumber. Checked against `RESTORE LABELONLY`.
- **SSET attributes**: 0x4 full, 0x2 COPY_ONLY (0x6 = copy-only full), 0x8 differential. Same over all 37 files.
- **Page header of an empty / pad page**: `m_type 101`, freeCnt 8096, freeData 96, LSN 0:0:1, everything else 0.

## 1. Freed space and the DCM (bk_e2a)

Setup (`sql/a0*.sql`). FULL recovery, one 64 MB file, eight tables with char(1000) rows at 7 rows per page:

| table | shape | change |
|---|---|---|
| t_ci_page | clustered | every row of page 1:448 deleted |
| t_ci_ext | clustered | ids 344..735 deleted, which empties extents 568..616 (7 extents) |
| t_heap_page | heap | every row of page 1:858 deleted, no TABLOCK |
| t_heap_tl | heap | 1000 rows deleted WITH (TABLOCK) |
| t_drop_small | clustered, 73 pages | DROP |
| t_drop_big | heap, 1289 pages, over the 128-extent deferred threshold | DROP |
| t_trunc | clustered, 321 pages | TRUNCATE |
| t_trunc_big | heap, 1289 pages | TRUNCATE |

`a_full` was taken before. **D1** was taken in the same batch as the changes (ghosts not cleaned, deferred
work pending). **D2** was taken 25 s later, after ghost cleanup had removed every ghost (checked live).

### 1.1 Copy rule

| backup | extents copied | DCM bits | DCM ∩ allocated | copied − (DCM ∩ alloc) | DCM bits on free extents |
|---|---:|---:|---:|---|---|
| full | 527 | 7 | 7 | every GAM-allocated extent + 8088 (2nd PFS page, GAM-free) | – |
| D1 | 36 | 52 | 36 | ∅ | 992-1112 (t_heap_tl) |
| D2 | 34 | 57 | 34 | ∅ | 568-616 (t_ci_ext) + 992-1112 |

The same equality held in every diff of bk_e2b/c/d/e/g/h/i. The extent of a later PFS page is copied by a
full even when GAM calls it free (1:8088 here). In a diff it is copied when its DCM bit is set, as seen for
1:8088 in bk_e2e (§6).

### 1.2 What each kind of freeing leaves in the diff

| case | DCM of freed extents | in D1 (same batch) | in D2 (after cleanup) |
|---|---|---|---|
| clustered, all rows of 1 page | set (the delete wrote the page) | page copied; 7 records of type 6 (ghost); `m_ghostRecCnt 7`; PFS 0x48 | page **still copied** (its extent is still allocated), PFS **0x08** (not allocated, ghost bit stale), header still names t_ci_page, 1 ghost record left. Live: is_allocated 0 |
| clustered, 7 whole extents emptied | set | 7 extents copied, all records ghosts | extents GAM-free, **DCM set, not copied**; IAM 1:378 rewritten (LSN 51:4192:20), lists 31 of the former 38 extents |
| heap, all rows of 1 page, no TABLOCK | set | page stays allocated (PFS 0x40), copied, 7 slots with offset 0, freeCnt 8082 | same |
| heap DELETE WITH (TABLOCK) | set | pages freed at once: 16 extents GAM-free with DCM set, not copied; 13 freed pages in still-allocated extents copied with PFS 0x00; IAM rewritten | same |
| DROP small | **not set** | object and AU rows gone from sysschobjs / sysallocunits; 9 data extents GAM-free; IAM 1:381 freed (PFS 0x30), bitmap emptied, still copied (its mixed extent 376 is allocated) | same |
| DROP big (deferred) | **not set** | object gone; **AU row kept with type 0, ownerid 0** (deferred-drop marker), pgfirstiam 1:382; GAM still allocates all 162 extents; IAM still lists 161; nothing of it in the diff | AU row gone; extents GAM-free; IAM page freed with an empty bitmap |
| TRUNCATE small | **not set** | **same AU id**, its row reset (pgfirst / pgroot / pgfirstiam = 0:0); extents GAM-free; IAM freed | same |
| TRUNCATE big (deferred) | **not set** | **new IN_ROW AU** 72057594054508544 under the **same** rowset (pgfirstiam 0:0); old AU row type 0 / owner 0, extents still allocated | old AU row gone, extents free |

**Answers.** The DCM bit is set only when a page of the extent was written. Every delete that goes through
the rows (ghosting, heap delete, TABLOCK delete) sets it; deallocation alone (DROP, TRUNCATE, deferred
drop, the ghost-cleanup deallocation that follows a delete) does not. Freed extents are never copied. So
"the diff carries the evidence" holds only through the allocation pages:
- GAM, SGAM, DCM, BCM and PFS 1:1 sit in extent 0 and are always in pass 2.
- Other PFS pages and every IAM page are in the diff when they changed.
- The catalog pages (sysallocunits, sysrowsets, sysschobjs) are in the diff when they changed.

**Timing.** D1 and D2 are both consistent for a reader that applies R3-R5 and skips ghosts. Ghost cleanup
only moves the evidence from ghost records (D1) to PFS/GAM/IAM (D2). Deferred drop leaves a window (D1)
in which the AU is dead (type 0 / owner 0) while GAM and IAM still allocate its extents. Treat the AU's
status as authoritative. Later the extents are freed with no DCM bit, and if they are reused, the new
owner's writes put them in a later diff.

### 1.3 Unallocated pages inside copied extents (R3)

They are in fulls as well as diffs (`out/q2_stale_census.txt`):
- `a_full`: 42 pages, almost all type 20 `BULK_OP_PREALLOC` (pages pre-formatted by the bulk insert, no slots, header naming the table).
- `a_d2`: 7 freed DATA pages whose extent now belongs to another AU, still naming the old one.
- `b_d1` / `b_d2` (after shrink and rebuild): **34 and 53 freed DATA pages that still hold primary records**, naming a dead AU, inside an extent the DMV now gives to another AU.

Without the PFS check these rows are resurrected.

## 2. The page header's allocation unit id (bk_e2b: SHRINKFILE)

Setup (`sql/b0*.sql`). A heap `h_heap` (2000 rows, LOB on every 100th row, a nonclustered index, 3
forwarded records) and a clustered `c_ci` (same, with LOBs). Both were allocated between three filler
tables. After `b_full`: DROP the fillers, then `DBCC SHRINKFILE (bk_e2b, 1)`: the file went from 6144 to
1056 pages. D1 was taken after that.

- **Header AU correct on every allocated page.** h_heap in-row (253 pages), LOB (51), NC index (12),
  c_ci in-row (252), LOB (51): all moved, all in D1, every header AU equal to the DMV's. Over every
  allocated page of every object, system tables included, in a_full, a_d1, a_d2, b_full, b_d1:
  **10,473 pages, 0 mismatches** (`out/q2_au_check.txt`).
- **Where it is stale** (R3/R4): PFS-free pages in allocated extents (freed by ghost cleanup, TABLOCK
  delete, rebuild); `BULK_OP_PREALLOC` pages; freed IAM pages (PFS 0x30); pages of a deferred-drop AU
  (allocated, AU dead); old images past the new end of file. The last kind never appears in the diff,
  only in the base the reader holds. On these pages the DMV, which reports the extent's IAM owner, often
  disagrees with the header.
- **Shrink moved heap rows, not heap pages.** All 2000 heap RIDs changed, and the slot was kept for only
  1. The forwarded records were collapsed (forwarded_record_count 3 → 0; id 5 is a plain row at 1:672:0).
  **Clustered pages moved whole**: all 2000 rows changed page, all 2000 kept their slot. LOB pages moved
  too, so the in-row roots were rewritten.
- **A naive overlay doubles rows**: full + D1 by header gave 4000 rows for each table (truth 2000). All
  614 stale user pages were past the new end of file. With `Size` + GAM + PFS (`apply_diff`): 0 stale and
  0 missing pages, 2000 / 2000 rows.
- **The DCM keeps bits for extents past the end of the shrunk file** (D1: 1296-1448, 2360-2528,
  3440-3792), while GAM shows them free. Clip the DCM to `Size`.

**Rule.** Use the header AU as the page → AU map. It is exact on allocated pages, and the IAM of an
unchanged AU is not in a diff anyway. Gate it with R3 and keep IAM as a validation. A shrink shows as
`Size` going down and as pages past it; it needs nothing special. For a heap it is a delete + insert of
every moved row (new RIDs). For a clustered table the keys do not change.

## 3. Heap REBUILD (bk_e2b, D2)

`ALTER TABLE h_heap REBUILD` and `ALTER INDEX ALL ON c_ci REBUILD`, after making 3 forwarded records
again. Catalog decoded from the backups (`out/q3_rebuild.txt`):

| object | before (full+D1) | after (full+D2) |
|---|---|---|
| h_heap rowset | 72057594047299584 | **72057594047627264** |
| IN_ROW AU | …054049792 | **…054639616** |
| ROW_OVERFLOW AU | …054115328 (empty) | **…054705152** (2 pages: rebuilt rows went off-row) |
| LOB_DATA AU | …054180864, first 1:1312, IAM 1:387 | **same id, same first page and IAM**, now owned by the new rowset |
| NC index rowset / AU | …047561728 / …054574080 | new / new |
| c_ci rowset, IN_ROW AU | …047430656, …054311936 | new, new; LOB_DATA kept |

- **RIDs:** all 2000 heap rows got a new RID. All 2000 clustered rows changed page; that is harmless
  because they are keyed. The forwarded records were removed.
- **LOB pages untouched:** 51 + 51 pages with the same page ids and the same LSN. The in-row roots of the
  20 + 20 LOB rows are byte-identical.
- **Old pages in the diff:** D2 carries 25 PFS-free pages that still name the old IN_ROW AU; R3 removes
  them.
- **How a reader detects it:** in the diff's sysrowsets, (idmajor, idminor) maps to a new rowsetid, and
  sysallocunits shows a new IN_ROW AU id while the old one is gone. Then replace the table's rows from
  the new AU's pages, all of which are in the diff (newly written). Take its LOB values from the lake
  by matching root bytes, because the LOB pages are **not** in the diff.
- The same signal (a new AU id under a known rowset) covers the big deferred TRUNCATE of §1.2. The small
  TRUNCATE keeps its AU id; there the signal is pgfirst = 0:0 and its extents GAM-free.

## 4. ALTER TABLE (bk_e2c)

A clustered table `c_alt` and a heap `h_alt` (id, a int, v varchar(10), dropme char(20), w varchar(30),
pad char(200); 700 rows each). One differential after each step (`out/q4_alter.txt`). The table below
gives the clustered results; the heap matched except for the int → bigint row.

| step | pages rewritten | records | sysrscols | other |
|---|---|---|---|---|
| ADD n_null int NULL; ADD n_def int NOT NULL DEFAULT 1515852340; ADD s_def varchar(20) NOT NULL DEFAULT 'ZZE2_DEFV'; +1 row | 1 of 23 (the insert's page): **metadata-only** | old rows still **6 columns**, fixed end 232, 2 var columns; the new row has 9 columns, fixed end 240, 3 var columns | new rscolid 7 / 8 / 9 at offsets 232, 236, −3; status 0x0 (nullable) or **0x280** (0x80 NOT NULL + **0x200 has default**); rcmodified 1 | defaults in **`sysseobjvalues` (object 9)**: valclass 1, id = rowset id, subid = rscolid, value = sql_variant (int 1515852340; varchar(20) 'ZZE2_DEFV'). The constraint text is in sysobjvalues |
| DROP COLUMN dropme | **0**: metadata-only | the bytes `ZZE2_DROPME_n` are still in all 701 rows | its row renumbered to **rscolid 0x04000001**, status **0x82** (0x02 = dropped), offset 12 and hbcolid 4 kept | the syscolpars row is gone |
| a int → bigint | **all** (45 of 45; the clustered index grew from 23 to 45 pages; heap 26 of 26, with 66 rows forwarded: 66 type-2 stubs plus 66 type-1 records carrying a back pointer): **size-of-data** | every row 10 columns, fixed end 248 | **new physical column**: rscolid 2, hbcolid 10, offset **240**, nullbit 10, ti 0x7f; the old 4 bytes kept as dropped rscolid 0x04000002 at offset 8 | every page in the diff |
| v varchar(10) → varchar(50) | **0**: metadata-only | unchanged | only ti 0x0aa7 → 0x32a7 (max length) | syscolpars length 50 |
| REBUILD | all; new rowset and AU | 8 columns; dropped bytes gone | compacted (offsets 4, 8, 16, 216, 220); defaults now status 0x80 | the sysseobjvalues rows are deleted (the values are in every record) |

**Rule (R7).** Decode by `sysrscols` of the page's rowset; never by syscolpars order:
- Column identity is rscolid = syscolpars colid. Dropped columns have 0x04000000 | n and status 0x02;
  skip them, but they still occupy their fixed offsets.
- A record lacks a column when its null-bitmap ordinal (sysrscols nullbit) exceeds the record's column
  count, or a var column's index exceeds its var count. Its value is then the sysseobjvalues default when
  status has 0x200, else NULL.
- The lake mirror needs `sysseobjvalues` next to the base tables listed in RESEARCH §6.2.
- Size-of-data ALTERs put the whole table in the diff. Metadata-only ones change only catalog pages, so
  DDL can be derived from the catalog diff alone.

## 5. Partial LOB update (bk_e2d)

Seven varchar(max) rows on one data page (`out/q5_lob.txt`). In-row roots are type 4 (BLOB_INLINE_ROOT).
The 6 MB value's root points at two INTERNAL nodes. "In place" below means the same page:slot with a
newer LSN.

| id | change | root | fragments after the change | fragments in the diff / only in the full |
|---|---|---|---|---|
| 1 | 20 KB, `.WRITE` 100 B | ts 1→2, links same | 2 of 3 DATA rewritten in place | 3 / 0 |
| 2 | 300 KB, `.WRITE` 100 B @150000 | ts 1→2 | 2 of 38 DATA rewritten in place, INTERNAL unchanged | 15 / 24 |
| 3 | 6 MB, `.WRITE` 100 B @3000000 | ts 1→2 | 2 of 747 DATA rewritten, both INTERNAL nodes unchanged | 13 / **736** |
| 4 | whole-value UPDATE, same length | ts 1→2 | **all 38 DATA rewritten in place**, INTERNAL kept | 39 / 0 |
| 5 | `.WRITE` append 10 KB | ts, size 300000→310000 | last DATA + INTERNAL rewritten, 1 new DATA | 8 / 32 |
| 6 | `.WRITE` 10 B → 1000 B in the middle | ts, size → 300990 | 2 DATA + INTERNAL rewritten, 1 new DATA | 15 / 25 |
| 7 | untouched (and later: UPDATE of another column) | **byte-identical** | none | 0 / 39 |

Second round (D2):
- A whole-value UPDATE 300 → 400 KB rewrote all 38 old fragments in place and added 13 new ones.
- 300 → 80 KB rewrote 11 fragments and dropped 28 from the tree.
- In both, `ts` went 2→3 and the blob id stayed the same.
- An UPDATE of a non-LOB column left the root byte-identical (ts 1), and no LOB page entered the diff.

**The data row page always changes** when a LOB value changes, because the root is in-row. So the diff
always carries the new root.

**Proof of the rule** (`q5_lobmap.py`). The "lake" is each value from the full plus a fragment map: per
tree node (page, slot), its kind and either its byte range in the value (DATA) or its link list
(INTERNAL). Rebuild a value from its new root as follows: a node whose page is in the diff is read from
the diff; any other node comes from the map, a DATA node as the old value's bytes over its old range.
Result: **all six changed values came out byte-exact**, including the 6 MB one with 736 of its 749
fragments from the lake. The unchanged value was recognised by its identical root.

**Rule (R8).**
1. Store each LOB value's root bytes and its fragment map in the lake; a few bytes per 8 KB of value.
2. Root unchanged → copy the value from the lake. This also covers REBUILD (§3).
3. Root changed → walk the tree, taking each node from the diff when its page is there, else from the
   map.

Without the map: a whole-value update is readable from the diff alone, because every fragment is
rewritten. A partial update shows as a reachable fragment page missing from the diff; refuse it then.

## 6. A second data file; filegroups; striped backups (bk_e2e)

Setup. File 1 (100 MB, filled past page 1:8088), then file 3 (40 MB) added to PRIMARY. File 2 is the
log. Two tables of 7000 rows each landed about half in each file. Later file 4 was added in filegroup
fg2, with a table on it (`out/q6_files.txt`).

- **One MSDA per pass for all data files.** Inside the pass-1 MQDA the files come one after another, in
  file_id order: file 1 groups 0-1372, then file 3 groups 1373-1500. With fg2: files 1, 3, 4. Pass 2
  re-copies extents 0 and 8 of file 1 and extent 0 of every other file. The MSDA block-specific bytes are
  `0000000000000100` in every pass and every file count, so they carry no file id.
- Each data file has its own file header, PFS, GAM, SGAM, DCM and BCM. The copy rule (R1) holds per file:
  D1 copied 86 of file 1's 135 DCM extents (the allocated ones) and 67 of file 3's 67.
- `m_fileId` in each header is the real file id (3, 4), not an ordinal. There were 0 positional
  mismatches.
- The PFS page **1:8088** changed (pages 1:8200-8600 freed) and is in D1 with its new LSN, through its own
  DCM bit.

**Striped backup** `TO DISK = a, DISK = b` (a COPY_ONLY full and a differential; `out/q6_stripe.txt`):
- Each stripe is a complete MTF file: its own TAPE with its own MediaFamilyId, the same MediaSetId in the
  RAID stream (FamilyCount 2, FamilySequenceNumber 1 / 2), and its own SSET, VOLB, MSCI, both MSDA
  passes, the MSTL chain, MSLS and ESET. The **MQTL log tail is only in family 1**; family 2 has empty
  MSTL blocks.
- Data is split in chunks, each chunk written whole to one stripe. In the full they were transfer-sized
  runs of 16 / 32 / 48 / 96 extents (multiples of 1 MB); in the diff, contiguous runs of variable length
  (1, 8, 12, 14, 17 … extents). Assignment is dynamic, not alternating.
- Every extent is in exactly one stripe, and A ∪ B = the single-file backup (1501 = 749 + 752 extents;
  diff 153 = 73 + 80). Each stripe's stream is ascending. Both are padded with type-101 units to the
  same length.

**Rule (R9).** Read all FamilyCount files that share one MediaSetId, refuse an incomplete set, index every
page of every stripe by its header into one page store, and take the log tail from family 1. No
positional mapping across stripes is possible, and none is needed.

## 7. Backup options (bk_e2f, `out/layout_all.txt`)

- **CHECKSUM**:
  - Every page is byte-identical to the default backup, except the boot page 1:9, which differs between
    any two backups.
  - Every MQDA and MQTL stream gets media-format attribute bit **0x100** (2 → 258) and is followed by a
    4-byte **CSUM** stream.
  - HEADERONLY: `HasBackupChecksums = true`. SSET attributes unchanged.
  - The CSUM algorithm is not CRC32 or CRC32C, and not an XOR or sum of the stream words or of the page
    checksums (time-boxed). Verify the page checksums instead.
- **BLOCKSIZE**:
  - 512 is refused (error 3272: 4096-byte device sectors). 4096 gives the default layout.
  - 16384 / 65536: SFMB at 1 × BLOCKSIZE, SSET at 2 ×, and the first data page at a BLOCKSIZE multiple
    (49152 / 196608), with APAD absorbing the gap.
  - TAPE's format logical block size stays 1024. Page stream identical.
- **MAXTRANSFERSIZE** decides the padding. The data is written in transfer-sized chunks and the last one
  is filled with type-101 units:
  - 64 KB: no pads at all (an extent is 64 KB); pass 2 = 16 units.
  - 1 MB (default): pass 2 = 128 units (14 pages, 2 zero, 112 pads).
  - 4 MB: pass 2 = 512 units with 496 pads; pass 1 rounded up to 4 MB.
  - Over 74 streams **every pad was trailing** (0 interior) and fewer than one transfer's worth.
  - SQL Server sometimes picks a smaller transfer itself: the default-option diff of the shrunk 8 MB
    database (b_d1) had **no pads**, and its stream was not a multiple of 1 MB.
- **Pad bodies** are uninitialised buffer memory: all zeros, or page images. In 26 of the 35 files that
  have pads, the bodies carried row data of the database being backed up. In three of them (both stripes
  of the striped diff, and stripe B of the striped full) these were rows that are on no live page of that
  file, i.e. data the file does not otherwise contain. No other database's data was seen. That cannot be
  ruled out.

**Rule (R10).** Never compute positions from block size or transfer size; follow the MTF and stream
headers. Skip m_type 101 by type. Treat pads as sensitive.

## 8. Recovery model and compatibility level (bk_e2g FULL/170, bk_e2h SIMPLE/110, bk_e2i BULK_LOGGED/170)

The same workload ran in all three: UPDATE, DELETE, an `INSERT … WITH (TABLOCK)` of 3000 rows into a
heap, `SELECT INTO`, CREATE INDEX. The last three are minimally logged outside FULL. Results
(`out/q8_recovery.txt`):
- All three diffs: SSET 0x8, the same skeleton, MQTL 64 KB, copied extents = DCM ∩ allocated.
- Every page allocated live is in the full or the diff (0 missing). Rows after the overlay equal the
  live tables (t_base 2899, t_heap 5000, t_new 1000) in all three.
- Minimally logged work **sets the DCM**: its extents are in the diff.
- The BCM has bits only under BULK_LOGGED (130 extents, the bulk-loaded tables). A diff ignores it, and
  HEADERONLY says `HasBulkLoggedData = false` for the diff.
- The compatibility level appears only as boot `dbi_cmptlevel` (110 / 170) and as HEADERONLY
  `CompatibilityLevel`. `dbi_version` is 998 in all three, and the layout does not change.

**Rule (R11).** One reader for every recovery model. Chain validation uses HEADERONLY / SSET fields
(RecoveryModel, DifferentialBaseLSN / GUID); the BCM is ignored.

## 9. Corrections to RESEARCH.md

1. §3 / §6.2 "Exactly the extents whose DCM bit is set": it is **DCM ∩ GAM-allocated**. DCM bits stay set
   on freed extents and past the end of a shrunk file.
2. §6.2 point 4, "the allocation maps are re-copied into every backup, so pages freed since the last import
   are known": pass 2 re-copies only extents 0 (and 8) of each file. Later PFS pages and IAM pages arrive
   only when they changed. DROP / TRUNCATE / deferred drop free extents **without** a DCM bit. The lake
   must keep the allocation pages and a page map (R5).
3. §3 "Map pages to objects through IAM chains, not the header's object id (stale after shrink)": the
   header AU is **correct on every allocated page**, after shrink and rebuild too. It is stale only on
   unallocated pages and dead AUs. In a diff the IAMs of unchanged AUs are absent, so the header AU gated
   by R3 is the practical map.
4. §6.2 "a whole-value UPDATE writes a new LOB, all of its pages new": it **rewrites the existing fragments
   in place** (same page ids, newer LSN) and adds or drops some. The conclusion stands: every fragment is
   in the diff. A partial `.WRITE` need not be refused when the lake keeps a fragment map (R8).
5. §6.2 "Heap rebuild or shrink …, a new allocation unit: replace that table, its new pages are all in the
   file anyway": true for in-row data. **REBUILD keeps the LOB_DATA AU and does not rewrite its pages**,
   so those values must come from the lake by root identity. A small TRUNCATE keeps its AU id; a big
   (deferred) one gets a new AU under the same rowset.
6. §6.2 "ALTER TABLE ADD/DROP COLUMN, metadata-only, rows keep the old column count": confirmed. ADD … NOT
   NULL DEFAULT needs `sysseobjvalues` in the mirror. ALTER COLUMN int → bigint is size-of-data (the whole
   table is in the diff); varchar widening is metadata-only.
7. §3 "type-101 pad units … their bodies … contained copies of real pages": they trail each stream to a
   multiple of the transfer size (MAXTRANSFERSIZE, sometimes chosen smaller by SQL Server). Bodies are
   zeros or stale page images.

## 10. Not covered

- A database larger than one GAM interval (4 GB per file): the second GAM/DCM page at 511232 is decoded by
  the code but was not exercised.
- SHRINKFILE with EMPTYFILE (pages moving between files).
- ADR / RCSI version ghosts and heap ghost records under row versioning.
- Compressed backups (MS_XPRESS / ZSTD) under non-default MAXTRANSFERSIZE.
- The CSUM algorithm.
- Columnstore under REBUILD or TRUNCATE.
