# E11: columnstore in differentials and fuzzy backups

Date: 2026-11-28 (the steps on 2022, then on 2019; the tuple mover in the tail, the second part below). Spec
088 D7 / task 9: what a differential carries for a clustered columnstore index (CCI),
whether a rowgroup id is ever reused within a rowset (D3's keys), what a SWITCH of a partition with an OPEN
delta store does, and what a full backup copied while the tuple mover runs holds. SQL Server 2022 RTM-CU27-GDR
(16.0.4305.5), linux/amd64 under Rosetta. Background tuple mover off (trace flag 634, on the throwaway container
only), so a delta store changes only where a step says so.

## Result

**A full backup and its differential, read as one state, equal the server's rows after every step**: 21
steps, 19 run and 2 refused by the server (SPLIT / MERGE of non-empty partitions). Pages are
overlaid in restore order (`pages.apply_diff`: the differential's pages win, then the result's allocation maps
decide what is live), and the state is decoded by E5's `cs_decode.py`, unchanged. That is 21 steps × 5 tables,
105 checks, each equal as a multiset in E5's canonical form. So a CCI needs no columnstore-specific overlay:
085's page overlay (`PageIndex::SetBase`) is enough, and 089's columnstore path is a question of which rows
changed, not of reading the state.

Each step is: a full backup, the operation, a differential backup, the server's rows (`truth_rows.py`) and a
snapshot of `sys.dm_db_column_store_row_group_physical_stats` / `sys.partitions` / `sys.internal_partitions`.
That gives one differential per operation (`e11.py`), analysed by `analyze.py`.

| step | the differential carries | rowgroups (`syscsrowgroups`, keyed by rowset + id) |
|---|---|---|
| trickle insert, 1,000 rows | delta-store pages (new delta rowset) | added: OPEN, its `ds_hobtid` the new rowset |
| bulk insert, 150,000 rows | 101 + 2 LOB pages of the columnstore rowset (the new segments) | added: COMPRESSED, its 3 segments, 1 new dictionary |
| delete 40,000 compressed rows | 57 delete-bitmap pages | none (a compressed rowgroup's row is immutable; `rcrows` of the bitmap rowset 0 → 40,000) |
| delete 100 delta rows | delta-store pages | none (rows deleted in place) |
| update 400 compressed rows | delete-bitmap + delta-store pages | none (delete + delta insert) |
| `REORGANIZE … COMPRESS_ALL_ROW_GROUPS` (the tuple mover's work) | 512 + 2 LOB pages | the delta rowgroup TOMBSTONE; a new COMPRESSED one of its rows; and the three compressed ones merged into a new one, themselves TOMBSTONE |
| `REORGANIZE` | 511 LOB pages, delete-bitmap pages (some freed) | TOMBSTONEs removed with their segments, the delta rowset dropped, the bitmap emptied; one more merge |
| `REBUILD` | 512 LOB pages | **new rowsets** (columnstore and delete bitmap, new hobt ids), their rowgroups numbered from 0; the old rowsets gone |
| `TRUNCATE` + reload | 83 LOB pages | **rowgroup 0 of the same rowset again**, other contents (see D3 below) |
| SWITCH OUT a partition with an OPEN delta store | system pages only, **no data page** | the rowgroups (by hobt) unchanged; their columnstore, delete-bitmap **and delta-store** rowsets change owner (`idmajor`) and `numpart`, the staging table's empty ones go the other way |
| SWITCH IN | as SWITCH OUT, reversed | as SWITCH OUT, reversed |
| SPLIT of an empty partition | system pages, one LOB page | new columnstore + delete-bitmap rowsets for the new partition |
| SPLIT / MERGE of non-empty partitions | — | refused by the server (35346 / 35344: a CCI's partitions must be empty to split or merge) |
| MERGE of empty partitions | system pages | the empty partition's rowsets dropped |
| `TRUNCATE … WITH (PARTITIONS (3))` | system pages | the partition's rowgroup removed; rowset kept, its LOB unit emptied |
| 1,100,000 trickle rows | 2,869 delta-store pages | a CLOSED rowgroup (1,048,576 rows) and an OPEN one; both decode |

`syscsrowgroups.status`: the low byte is the state, and the high byte varies with how the rowgroup was made.

| low byte | state | statuses seen |
|---|---|---|
| 1 | OPEN | 257 |
| 2 | CLOSED | 258 |
| 3 | COMPRESSED | 515, 1283, 1539, 1795 |
| 4 | TOMBSTONE | 260, 1284, 1540, 1796 |

## Answers

**D7, the differential evidence.** The expected shape (R§6.2) holds, with three corrections:

- A **REBUILD** does not tombstone rowgroups. It builds new rowsets (new hobt ids) and drops the old ones.
  **085 reports no schema change for it** (`mssql_backup_schema_changes` on the step: no row,
  `schema_changes.txt`). For a rowstore that is right: a rebuild moves rows and keeps them. For a CCI every row's
  key (`02 ‖ rowset …`, 088 D3) changes, so 088 and 089 have to treat a CCI whose columnstore rowset was
  replaced as a reload of that partition. That is a task, not something 085 does today.
- The **tuple mover** (here REORGANIZE with COMPRESS_ALL) does not free the delta store's pages at once. The
  delta rowgroup becomes TOMBSTONE and keeps its rowset until a later REORGANIZE (or the background cleanup)
  removes it. In the meantime its rows are in both places, and only the TOMBSTONE state tells the reader not to
  count them twice. REORGANIZE also merges compressed rowgroups, by the server's own policy:
  - rowgroups 0, 1 and 3 (200,000 + 200,000 + 150,000 rows, 40,400 deleted) became one of 509,600;
  - in `t_reuse`, a rowgroup whose rows were all deleted and its two neighbours became one of 220,000.
- A **SWITCH** with an OPEN delta store is catalog-only, like E7 s12's: the delta store's rowset changes owner
  with the others, and its pages are not in the differential. 089 moves its rows by rowset, as it does the
  compressed ones, not by page (D3's `03` key carries the rowset, so it stays valid).

So, between two states, the changed rows of a CCI are:

- rowgroups that appeared (new keys), and rowgroups that disappeared or became TOMBSTONE;
- delete-bitmap entries added (or dropped with their rowgroup);
- delta-store rows, as a rowstore's changes.

A COMPRESSED rowgroup's own rows never change.

**D3, rowgroup ids.**
- **They are not reused while the rowset lives.** Deleting every row of a rowgroup, REORGANIZE (TOMBSTONE),
  cleanup and a new bulk load gave the new rowgroup id 4 after 0-3.
- **A TRUNCATE restarts them at 0 in the same rowset.** The table and the partition form both keep the rowset
  (and its allocation unit id), so `02 ‖ rowset ‖ rowgroup` names other rows after a TRUNCATE.
- **It is still a safe key, because 085 sees the TRUNCATE.** The columnstore rowset's LOB unit gets another
  first page and first IAM page (1:360 / 1:865 → 1:328 / 1:238), or none in the partition form
  (`truncate_aus.txt`). `mssql_backup_schema_changes` reports `table_truncated` for both steps
  (`schema_changes.txt`). Whether the importer always applies it before the new rows is 089's ordering, not
  measured here.
- D3's text has to say so: keys are unique between two truncates of the rowset, and a TRUNCATE is always
  reported.

**D6, fuzziness: the tuple mover during a full backup** (`fuzzy.sh`).
- **Setup.** The backup was written into a FIFO that a reader in the container held twice, test/backup/gen's
  tail_window harness simplified: once early in the first data pass, then inside the second.
  - At the first hold: 20,000 compressed rows deleted and the OPEN delta store compressed. The server said 0 %
    into the backup there, so these changes came before the table's pages were copied, not during it.
  - At the second hold: 5,000 rows inserted, 500 deleted and a REORGANIZE. The server said 100 %: the data had
    been copied, so these went to the tail only.
- **What the copy holds** (`fuzzy/read.txt`). Read alone, without its log, it is the state after the first
  hold:
  - 230,000 rows: the state before minus exactly the first hold's 20,000 deleted rows;
  - no row twice;
  - the delta rowgroup TOMBSTONE, its rows in a new COMPRESSED rowgroup.

  The second hold's changes are in the tail only. `mssql_backup_tables` reports `t_fz` `stale` and the heap
  beside it `consistent`. The 20 committed transactions after the checkpoint touched `syscsrowgroups`,
  `syscscolsegments`, `syscsdictionaries`, `sysrowsets`, `sysallocunits`, `sysrscols`, `sysobjvalues` and
  `t_fz`.
- **So this run tested the before / after placement, not a change during the copy.** The interesting case,
  where the table's own pages are copied while the tuple mover runs, needs a hold placed within the table's
  pages.
- **Not caught:** a copy that holds the catalog's rowgroups from before a tuple-mover step and the segments or
  delete bitmap from after it. Such a copy would read rows twice or lose them, and D11's `dirty` / `stale` is
  what tells the importer. Whether the decoder can refuse nothing and still be right there is 090's question.

## The tuple mover in the tail (task 9)

Three more held backups on 2022, each with one change: `REORGANIZE … COMPRESS_ALL_ROW_GROUPS` over a table with a
compressed rowgroup and an OPEN delta store of 50,000 rows (`fuzzy.sh`, now parametrised:
`FZ_HOLD1` / `FZ_HOLD2`, and `FZ_HOLD1_AFTER_TABLE`, which places the first hold just past the table's pages
as a trial backup lays them out, via `table_offsets.py`). Evidence: `fuzzy_reorg*/read.txt`.

| hold | where | the copy | the tail | D11 |
|---|---|---|---|---|
| inside the second pass, its usual size | the server had read ahead past it | the state before (delta OPEN) | no record of it: the change came after the backup's last LSN | `consistent` |
| just past the table's pages, in the first data pass (69 %) | after the table was copied | the state **after** (TOMBSTONE + a new COMPRESSED rowgroup), equal to the server: the second data pass copied the changed extents again | none of it | `consistent` |
| inside a second pass made large (the 96 MB filler rewritten at the first hold) | during the last pass | the state **before** (delta OPEN), equal to it | the whole step | **`stale`** |

**Two data passes protect the copy** (2019 on; 2017 copies once). A change during the first pass is copied
again by the second, so its copy is the state after, and its records come before the CheckpointLSN, not in the
tail. Only a change during the last pass reaches the tail. In all three runs the copy was one consistent state:
no row twice, none missing. The torn copy D6 worries about would need the table's own pages copied in the last
pass on both sides of a change. The holds cannot place a change that finely, because the server reads ahead of
its writes. It stays unproduced, and D11's status is what covers it.

**What the tuple mover writes to the log** (the third run's tail, its 3 transactions):
- the new segments as **`LOP_FORMAT_PAGE` records carrying the page's image**: 27 on `LCX_TEXT_MIX` and 2 on
  `LCX_TEXT_TREE`, with images of 312 to 8,192 bytes (the page's used part), and no row records for their
  content;
- the catalog's rows as row operations: `LOP_INSERT_ROWS` into `syscscolsegments` (3) and `syscsdictionaries`
  (1), `LOP_INSERT_ROWS` / `LOP_MODIFY_ROW` / `LOP_MODIFY_COLUMNS` on `syscsrowgroups`;
- the extents' allocation (`LCX_GAM` / `LCX_IAM` / `LCX_PFS`).

So:
- **D11 counts the tuple mover.** The table is flagged `stale` by its FORMAT_PAGE images, as for a TABLOCK load
  (085's review of #591), with no other row operation on the table.
- **090 can redo it** by applying the images and the catalog rows. Nothing columnstore-specific is needed
  beyond what a TABLOCK load's redo already takes.
- **The TOMBSTONE fallback is not needed for these placements.** It would read a torn rowgroup from its
  TOMBSTONE's delta store, which the copy keeps until a later REORGANIZE. No torn copy was produced to need it,
  so it stays an idea, for a copy that reads rows twice.

## On SQL Server 2019

The 21 steps ran again on 2019 RTM-CU32-GDR (15.0.4500.2), `E11_2019/` (with `dbo.e5_gs` in place of
`GENERATE_SERIES`):
- 105 of 105 reads of full + differential equal the server;
- the same catalog changes step by step (rowgroups, statuses, rowsets);
- the same events from `mssql_backup_schema_changes`: `table_truncated` for both TRUNCATEs, none for the
  REBUILD, `table_reload` and `partition_switched_out` for the SWITCH (`E11_2019/schema_changes.txt`);
- the same refusals of SPLIT / MERGE on non-empty partitions.

Only the page counts differ: the reorganize's delete-bitmap pages are 32 against 36.

## Procedure

`design/001-mssql-backup-reader/experiments/E11/` is gitignored.

- `e11.py run` runs the steps (setup tables `t_cs`, partitioned `t_pcs` with staging `t_stage`, `t_reuse`,
  `t_closed`). Every load runs with `OPTION (MAXDOP 1)`: on this server a parallel `INSERT … WITH (TABLOCK)`
  into a CCI splits the rows between threads, and each thread's share under 102,400 rows lands in its own delta
  store (measured: 400,000 rows became 12 delta stores, no compressed rowgroup).
- `analyze.py` does the overlay, the comparison and the catalog / page attribution; the output is
  `analysis.txt`.
- `fuzzy.sh` is the held backup; `fuzzy/read.txt` its decode and the extension's report.
- `truncate_aus.txt` and `schema_changes.txt` are the TRUNCATE evidence and 085's schema changes for the
  REORGANIZE, REBUILD, TRUNCATE and SWITCH steps.
- `tools/pages.py`'s `file_size` reads the file header at a fixed offset that holds on 2025 only. The analyzer
  reads the record's variable column 4 instead, as `backup_record.cpp` does.
