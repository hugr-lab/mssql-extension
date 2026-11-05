# E7: SQL Server partitioning in a backup

Date: 2026-11-05. Server: SQL Server 2025 RTM-CU8 (17.0.4075.5), container `mssql-v2test`. Research only, nothing in
git. T-SQL went through `tools/sql.sh` / `tools/sqlh.sh` only; files came out with `docker cp` only. Databases
`bk_e7a` (structure) and `bk_e7b` (maintenance), both FULL recovery, `SQL_Latin1_General_CP1_CI_AS`. Both databases are
dropped and `xp_delete_files N'/var/opt/mssql/data/bk_e7_*'` was run; `sys.dm_os_enumerate_filesystem(…, 'bk_e7*')`
returns nothing. Local copies of all 30 backups are in `experiments/E7/bak/` (361 MB).

## Short answer

1. **Everything needed to derive a table's partitioning is in seven base tables of the backup.** A decoder that reads
   them through their own `syscolpars` / `sysrscols` matched the catalog views (the oracle) in **16 database states**:
   - 18 functions and 72 typed boundaries;
   - 19 schemes with their destinations;
   - 21 index → data-space bindings, 32 index columns;
   - 115 rowsets, 122 allocation units;
   - the (partition number, partition-column value) multiset of all 19 tables.

   The 16 states are `bk_e7a` plus the 15 states after the maintenance steps, read as full + diff overlays. That is
   3,236 checks in the maintenance states and 0 mismatches overall.
2. **Two candidates were wrong.**
   - The index → scheme link is NOT `sysidxstats.dataspace`: that column is 0 in every row. The link is
     `syssingleobjrefs` class 7.
   - The parameter type lives in `systypedsubobjs`, which was not on the candidate list.
   - `sysmultiobjrefs` plays no part.
3. **The DuckLake rule works on the rows.** The rule is the finest of identity / epoch_day / epoch_month / epoch_year
   under which every **inner** partition holds one value. With the exact SQL Server values, no inner partition of any
   table holds two values. Two DuckDB facts change it:
   - **TIMESTAMP_NS is rounded to µs** before `date_diff` / `month()` / `CAST AS DATE`. A `datetime2(7)` value in the
     last 500 ns of a month therefore gets the next month's value.
   - **TIMESTAMPTZ transforms follow the session `TimeZone`.**

   DuckDB itself, run on the server's values, agrees with the model on all 70 temporal partitions.
4. **SWITCH is catalog-only.** The two rowsets exchange `sysrowsets.idmajor` (and `numpart`). Allocation units stay
   with their rowsets, and page headers and IAMs are unchanged.
   - **No data, LOB or index page of the switched partition is in the differential.** This held in 5 variants: out,
     in, partition-to-partition, columnstore, and SWITCH followed by DML.
   - SPLIT and MERGE of populated ranges **physically move** rows into another rowset, and those pages are in the diff.
     Empty SPLIT/MERGE and NEXT USED are catalog-only.
   - TRUNCATE PARTITIONS frees pages (small) or marks the AU dead (deferred). Neither puts a page in the diff.
   - REBUILD PARTITION gives new rowsets and in-row AUs but keeps the LOB AU.
5. **E1's general rule** ("delete by page in every table, including freed and dead-AU pages, then insert the changed
   pages' rows") **is exact for every operation except SWITCH.**
   - With one added **ownership rule**, all 15 differentials replay exactly. The rule: a lake row whose allocation unit
     (columnstore: rowset) now resolves to another (object, index) moves to that owner.
   - Without it, the 5 SWITCH variants fail: rows stay in the old table. After SWITCH + a one-row UPDATE only the 37
     rows of the touched page move; 263 rows stay behind.

## 1. Setup

### Structure database `bk_e7a` (`sql/a00`–`a03`)

PRIMARY plus filegroups `fg1` and `fg2`. There are 18 partition functions, each with one table, and rows on every
boundary edge (b − tick, b, b + tick) plus a spread.

| function | kind | boundaries | table (shape) |
|---|---|---|---|
| `pf_int_l` | int LEFT | 100, 200, 300 | `t_int_l` heap |
| `pf_int_r` | int RIGHT | 0, 1000, 2000, 3000 | `t_heap_int` heap on scheme `(fg1, fg2, fg1, fg2, fg1)`; partition 3 rebuilt WITH ROW compression |
| `pf_int_ident` | int RIGHT, consecutive | 1…5 | `t_int_ident` clustered PK |
| `pf_bigint_r` | bigint RIGHT | 1e10, 2e10, 3e10 | `t_bigint_r` heap, `ALL TO (fg2)` |
| `pf_empty` | int RIGHT, **no boundaries** | – | `t_empty` heap |
| `pf_date_m_r` | date RIGHT monthly | 2026-01-01 … 07-01 | `t_ci_date` clustered PK on `(fg1, fg1, fg2, fg2, PRIMARY, fg1, fg2, PRIMARY)`, an **aligned** NC index (partition column not in its key), a **non-aligned** NC index ON PRIMARY; also `t_date_m_r_all`, a second scheme on the same function |
| `pf_date_m_l` | date LEFT month ends | 01-31 … 06-30 | `t_date_m_l` clustered |
| `pf_date_d_r` | date RIGHT daily | 03-01 … 03-05 | `t_date_d_r` heap |
| `pf_date_y_r` | date RIGHT yearly | 2023 … 2026 | `t_date_y_r` clustered |
| `pf_dt2_q_r` | datetime2(3) RIGHT quarterly | 2025-01-01 … 2026-04-01 | `t_cci_q` **clustered columnstore**, compressed rowgroups plus one open delta store |
| `pf_dt2_m_l` | datetime2(7) LEFT, ends `23:59:59.9999999` | Jan … Apr | `t_dt2_m_l` clustered |
| `pf_dt2_m_r7` | datetime2(7) RIGHT monthly | 2025-11-01 … 2026-03-01 | `t_dt2_m_r7` clustered; edges at −600, −500, −100, 0, +100 ns |
| `pf_dt_m_l` | datetime LEFT, ends `.997` | Jan … Mar | `t_dt_m_l` heap |
| `pf_dt2_bad_l` | datetime2(0) **LEFT with period starts** (misaligned) | 2025-11-01 … 2026-02-01 | `t_dt2_bad_l` clustered |
| `pf_dt2_nonal_r` | datetime2(3) RIGHT, non-aligned, crossing a year | 2025-12-15, 2026-01-15, 02-10, 04-01 | `t_dt2_nonal_r` heap |
| `pf_dto_r` | datetimeoffset(0) RIGHT, `2026-02-01T00:00+02:00` (aligned only in local time) | 3 | `t_dto_r` heap, values in −4 … +4 h offsets |
| `pf_vc_r` | varchar(10) RIGHT | 'G', 'N', 'T' | `t_vc_r` clustered (`''`, `'g'`, `'G '`, `'_x'`, `'9'`…) |
| `pf_nvc_l` | nvarchar(20) LEFT | N'beta', N'Жук', N'omega' | `t_nvc_l` heap |

The full backup is `bk_e7_a_full.bak` (16.9 MB); the oracle is `truth/a_full/`.

### Maintenance database `bk_e7b` (`sql/b00`–`b02`, `sql/s01`…`s14`)

| object | shape |
|---|---|
| `m_ci` | clustered PK `(d, id)` on `ps_m` (`pf_m` date RIGHT, monthly 2026-01-01 … 06-01, scheme as an explicit list, so no NEXT USED), aligned NC index, a `varchar(max)` column with 9 KB off-row values every 50th row; Dec 2025 … May 2026 populated, ≥ Jun empty |
| `m_heap` | heap on `ps_h` (int RIGHT 1000 … 4000, `ALL TO PRIMARY`); partition 5 holds 9,000 rows of `char(1000)`, 1,286 pages, so its TRUNCATE is deferred |
| `m_cci` | clustered columnstore on `ps_q` (datetime2(0) quarterly) |
| `stg_feb`, `stg_apr` | staging tables, same shape, with CHECK constraints so they can switch back in |
| `stg_mar` | staging table without a CHECK constraint |
| `stg_cci` | columnstore staging table |
| `m_ci_arch` | a second partitioned table on `ps_m` |

**Every step is: a fresh full (not COPY_ONLY), one operation, an optional wait, a differential, the oracle**
(`run_b.sh`). Each differential therefore carries exactly one operation. Fulls are 14.8–26.3 MB, differentials
2.2–4.3 MB.

| step | operation | wait |
|---|---|---|
| s01 | `ALTER TABLE m_ci SWITCH PARTITION 3 TO stg_feb` (Feb, 560 rows with LOBs) | – |
| s02 | `ALTER TABLE stg_feb SWITCH TO m_ci PARTITION 3` | – |
| s03 | SWITCH PARTITION 5 (Apr) TO stg_apr, then UPDATE 10 % / DELETE 10 % of stg_apr, INSERT 50 rows into the emptied m_ci partition | 25 s |
| s04 | `ALTER PARTITION SCHEME ps_m NEXT USED [PRIMARY]` | – |
| s05 | `SPLIT RANGE ('2026-07-01')`: the last partition, empty | – |
| s06 | NEXT USED + `SPLIT RANGE ('2026-03-16')`: inside March, populated | 25 s |
| s07 | `MERGE RANGE ('2026-07-01')`: both sides empty | – |
| s08 | `MERGE RANGE ('2026-02-01')`: Jan + Feb, both populated | 25 s |
| s09 | `TRUNCATE TABLE m_ci WITH (PARTITIONS (May))`: 620 rows, 35 pages | 10 s |
| s10 / s10b | `TRUNCATE TABLE m_heap WITH (PARTITIONS (5))`: 1,286 pages. s10 is the differential right after; s10b is a second differential on the same base 30 s later | 0 / 30 s |
| s11 | `ALTER INDEX ALL ON m_ci REBUILD PARTITION = 2` and `ALTER TABLE m_heap REBUILD PARTITION = 2` | 10 s |
| s12 | `ALTER TABLE m_cci SWITCH PARTITION 2 TO stg_cci` (columnstore, 1,993 rows) | – |
| s13 | SWITCH PARTITION 3 TO stg_mar, then UPDATE **one** row of stg_mar | 10 s |
| s14 | `ALTER TABLE m_ci SWITCH PARTITION 4 TO m_ci_arch PARTITION 4` (partitioned to partitioned) | – |

## 2. Where the partitioning lives (Q1)

The view definitions (`OBJECT_DEFINITION`, `out/view_defs_*.txt`) named the base tables. The backup confirmed every
column (`e7sys.py … dump`).

| fact | base table and columns | notes |
|---|---|---|
| partition function | **`sysclsobjs` class 30**: `id` (65536+), `name`, `type` `'R '`, **`intprop` = fanout (partitions)**, **`status & 1` = RIGHT** | boundary count = fanout − 1; `created`/`modified` move on SPLIT/MERGE |
| parameter type | **`systypedsubobjs` class 30**: `idmajor` = function, `subid` = parameter, `xtype`, `length`, `prec`, `scale`, `collationid`, `utype` | not on the candidate list |
| boundary values | **`sysobjvalues` valclass 30**: `objid` = function, `subobjid` = boundary id (1 … n, **positional: renumbered by SPLIT/MERGE**), `valnum` = parameter, `value` = sql_variant | layout below |
| filegroups, schemes | **`sysclsobjs` class 31**: type `'FG'` (id 1 = PRIMARY), `'PS'` (65601+) | `sys.data_spaces` |
| scheme → function | **`syssingleobjrefs` class 31**: `depid` = scheme, `depsubid` 0, `indepid` = function | two schemes may share a function |
| scheme → filegroups | **`syssingleobjrefs` class 30**: `depid` = scheme, **`depsubid` = destination (= partition number)**, `indepid` = filegroup | **a row `fanout + 1` is NEXT USED.** `ALL TO (fg)` creates it at once (fanout + 1 rows); an explicit list does not; SPLIT consumes it |
| index → data space | **`syssingleobjrefs` class 7**: `depid` = object, `depsubid` = index id (0 = heap), `indepid` = scheme or filegroup. No row means PRIMARY (the view's `isnull(…, 1)`) | **`sysidxstats.dataspace` is 0 in every row.** `sysidxstats.rowset` holds the rowset id only when the index has exactly one rowset (unpartitioned, or a 1-partition scheme), else 0 |
| LOB data space | `syssingleobjrefs` class 8: `depid` = object, `depsubid` ≤ 1 | for a partitioned table with a LOB column it is the **scheme** (m_ci → ps_m) |
| partitioning column | **`sysiscols`**: `idmajor`/`idminor` = object / index, `status & 2`, **`tinyprop2` = partition ordinal**, `intprop` = column id, `tinyprop1` = key ordinal | a heap has a row for index 0; an aligned NC index whose key lacks the column carries it with key ordinal 0 and partition ordinal 1 |
| partitions | **`sysrowsets`**: `rowsetid`, `idmajor`, `idminor`, **`numpart`**, `ownertype`, **`cmprlevel` per partition**, `rcrows` | columnstore: per partition, ownertype 1 (columnstore, cmprlevel 3), 2 (delete bitmap), 3 (delta stores), same `numpart` |
| allocation units | `sysallocunits`: `auid`, `type`, **`ownerid` = rowset**, **`fgid` = the destination's filegroup**, `pgfirst` / `pgroot` / `pgfirstiam`, `pcused` / `pcdata` / `pcreserved` | |
| row count | `sysrowsets.rcrows` | equal to the live count (`ALUCOUNT`) for every rowset in all 16 states, and to the records on the leaf pages (100 + 385 rowstore rowsets) |

**sql_variant on disk** (all 72 boundaries; it carries the parameter's own scale):

```
[base xtype][version 1][props][value]
props:
  none                          int, bigint, date, datetime …
  1 byte scale                  time, datetime2, datetimeoffset
  prec + scale                  decimal
  u16 max length                binary
  u16 max length + u32 collation   strings
```

For example, `2a 01 07 ffbf692ac9 3e490b` is datetime2(7) `2026-01-31 23:59:59.9999999`, and `a7 01 0a00 08d00034 47`
is varchar(10) 'G' with collation 0x3400d008.

### The decoder (`e7part.py`)

`e7sys.SysCatalog` decodes every base table above generically: the five bootstrap tables come from pages_ext, the rest
from their own `syscolpars` / `sysrscols`. `e7part.Model` then gives, per table:

1. Its base index is the heap (0) or the clustered index (1).
2. The data space is from class 7; it is a scheme when it is a class-31 `PS` row.
3. The function is from class 31.
4. The boundaries come from valclass 30, typed by `systypedsubobjs` and decoded exactly: integer keys in the type's own
   tick, the server's style-121 text.
5. The partition column is the `sysiscols` row with `tinyprop2` = 1.
6. For each `sysrowsets` row with that (object, index): `numpart`, the destination filegroup, `cmprlevel`, `rcrows`
   and its AUs.

`e7part.py BAK verify TRUTH` checks every item against the views. It also decodes every partition's rows from its own
pages and compares the (partition, column value) multiset with `$PARTITION.fn(col)` on the server:
- rowstore rows by `sysrscols` of the partition's rowset, including the ROW-compressed partition;
- columnstore rows through E5's decoder, per partition.

Results (`out/verify_*.txt`):

| state | checks | failed |
|---|---|---|
| a_full | functions 18, boundaries 72, data spaces 22, schemes 19 + destinations 19, index data spaces 21, index columns 32, rowsets 115, compression 107, page row counts 100, AUs 122, tables' rows 19 | 0 |
| 15 states after s01 … s14, as full + diff overlays (pages.apply_diff) | boundaries 199, rowsets 539, AUs 924, index columns 269, page row counts 385, tables' rows 47 … | 0 |

## 3. Mapping to DuckLake (Q2)

**The rule:**
- An inner partition is a closed range: `[b(i-1), b(i) − tick]` for RIGHT, `[b(i-1) + tick, b(i)]` for LEFT. The tick
  is the parameter type's own: 1, 1 day, 10^-s s, or 1/300 s.
- The rule takes the finest T of identity < epoch_day < epoch_month < epoch_year with T(low) = T(high) for every inner
  partition. Endpoints suffice because T is monotone.
- No inner partition (0 or 1 boundary) gives no spec.
- Non-temporal types: only identity (consecutive integers).
- Strings: never.

DuckLake's epoch transforms are `date_diff(part, DATE '1970-01-01', col)` (`DL/storage/ducklake_partition_data.cpp:238-254`).

| function | exact rule | DuckDB-evaluated | why |
|---|---|---|---|
| int L / int R / bigint R | none | none | partitions hold many values |
| int RIGHT 1…5 | **identity** | identity | |
| `pf_empty` | none | none | no inner partition |
| date RIGHT monthly / date LEFT month ends | **epoch_month** | epoch_month | |
| date RIGHT daily | **identity** (= epoch_day for a date) | identity | |
| date RIGHT yearly | **epoch_year** | epoch_year | |
| datetime2(3) RIGHT quarterly | **epoch_year** | epoch_year | epoch_month splits every quarter |
| datetime LEFT `.997` ends | **epoch_month** | epoch_month | |
| datetime2(7) LEFT `.9999999` ends | **epoch_month** | **epoch_year** | DuckDB rounds `.9999995`+ into the next day |
| datetime2(7) RIGHT monthly | **epoch_month** | **none** | `b − 100 ns` rounds into the next month; 31 Dec rounds into the next year |
| datetime2(0) LEFT with period starts | none | none | `(Nov 1, Dec 1]` contains Dec 1 00:00:00; `(Dec 1, Jan 1]` crosses a year |
| datetime2(3) non-aligned, crossing a year | none | none | |
| datetimeoffset(0), `+02:00` boundary | **epoch_year** (UTC) | epoch_year | `[Jan 31 22:00Z, Mar 1Z)` spans two months in UTC |
| varchar / nvarchar | none | none | |

**Checked on the rows** (`out/transforms_a_full.txt`). For every table and partition I counted the distinct transform
values of the decoded rows:
- No inner partition holds more than one exact value.
- The open first and last partitions hold 1–4 values, which the writer splits.
- The misaligned functions show what the next transform would have split: e.g. `t_dt2_bad_l`, where epoch_month splits
  p2–p4 and epoch_year splits p3.

**DuckDB itself** (2.0-pin CLI, `duck_check.sh`) computed epoch_day / month / year on the server's values, typed as the
lake stores them: DATE, TIMESTAMP, TIMESTAMP_NS, TIMESTAMPTZ. Its distinct counts equal the model's on **70 of 70**
temporal partitions (`out/lakecounts_utc.txt`).

### Two DuckDB facts

1. **TIMESTAMP_NS is rounded to µs, half up, by every calendar function.**
   - `CAST(TIMESTAMP_NS '2026-01-31 23:59:59.9999999' AS TIMESTAMP)` is `2026-02-01 00:00:00`.
   - `CAST(… AS DATE)`, `month()` and `date_diff('month', …)` give February; `.9999994` gives January.
   - `datetime2(7)` is read as TIMESTAMP_NS (CLAUDE.md). So DuckLake's own value for a row in the last 500 ns of a
     period is the next period's. In `t_dt2_m_r7` and `t_dt2_m_l`, exactly the rows at b − 100 ns and b − 500 ns move.
2. **TIMESTAMPTZ transforms use the session time zone.**
   - `date_diff('month', DATE '1970-01-01', TIMESTAMPTZ '2026-01-31 23:30:00+00')` is 672 under `TimeZone='UTC'` and
     673 under `Europe/Berlin`.
   - `t_dto_r` under Berlin: the first partition gets 2 years, p3 gets 3 months (`out/duck_check_berlin.csv`).

At this DuckLake pin, file pruning uses partition values only for `bucket`
(`DL/storage/ducklake_metadata_manager.cpp:2183-2258`). A mismatched temporal value is therefore not a wrong-result
bug today. It would still disagree with what DuckLake writes for its own INSERTs and with how it groups files in
compaction.

### What the importer should do

- **Choose the spec by the exact rule** (it describes the data's partitioning). **Compute every row's partition value
  with DuckLake's own expression on the lake value**, and split files by that value.
  - For date, datetime and datetime2(≤ 6) the two coincide.
  - For datetime2(7) the cost is one extra small file per SQL Server partition, only when rows exist in the last
    500 ns of a period.
  - Never declare a value for a file that DuckLake would compute differently.
- **datetimeoffset:** derive in UTC (SQL Server orders by UTC). This is only valid while every writer runs with
  `TimeZone='UTC'`. Unless the owner accepts that constraint, **derive no default** for datetimeoffset columns.
- **SPLIT/MERGE later do not invalidate a declared spec.** Files are split by each row's own value. After s08 (Jan + Feb
  merged), a fresh derivation would pick epoch_year, but the epoch_month spec set at creation keeps holding: the merged
  partition simply yields two values. This fits the owner's rule that incremental imports never change the spec.

## 4. Partition maintenance in a differential (Q3)

`e7diff.py FULL DIFF` lists three things (`out/diff_sNN.txt`):
- the diff's pages, classed as allocation, catalog (per base table) or user (per AU, with owner before → after);
- the changed catalog rows, full against full + diff;
- the partition model before and after.

W = min(FirstLSN, CheckpointLSN) of the full; "changed" means m_lsn > W.

| op | data pages in the diff | catalog change | allocation | how the importer recognises it (mirror vs new catalog) |
|---|---|---|---|---|
| **SWITCH out** (s01) | **none**: 16 in-row + 14 LOB + 3 NC pages of the partition absent | 2 rowset pairs (CI, NC) **swap `idmajor` and `numpart`**; rowset ids and AUs unchanged (AU → rowset unchanged); `sysidxstats.rowset` of the single-partition side swaps; `sysschobjs.modified`; `sysrscols.rcmodified` | none; IAMs unchanged | a rowset id whose (idmajor, idminor) changed, AUs unchanged |
| **SWITCH in** (s02) | none | the mirror image: the original rowset ids return to `m_ci` p3 | none | same |
| SWITCH + DML (s03) | the DML'd pages of the switched AU (16 of 17 in-row, 2 NC), **under their unchanged AU id**; LOB pages absent (roots unchanged) | as s01, plus `rcrows` | PFS/GAM for the new rows | same; then the page rule |
| SWITCH + 1-row UPDATE (s13) | **1** of 10 pages | as s01 | – | same |
| SWITCH part → part (s14) | none | rowsets swap `idmajor` only (same `numpart`) | none | same |
| SWITCH columnstore (s12) | none (segments are LOB pages of the rowset's AU) | columnstore (ot 1) and delete-bitmap (ot 2) rowsets swap `idmajor` / `numpart`; `syscsrowgroups` / segments are keyed by hobt id and **unchanged** | none | same, keyed by rowset |
| NEXT USED (s04) | none | **one `syssingleobjrefs` class-30 row, `depsubid` = fanout + 1** | none | destinations = fanout + 1 |
| SPLIT, empty (s05) | none | fanout +1; a boundary added; **new rowsets (one per index) for the new last partition**, new empty AUs; the NEXT USED slot becomes a destination | none | function boundary list grew; new rowsets |
| **SPLIT, populated** (s06, RIGHT at Mar 16 inside March) | the **moved** rows on new pages of a **new rowset** (10 in-row, 7 LOB, 1 NC); the old rowset's emptied pages freed (8 + 7 + 1), 2 changed | boundary ids **renumbered** (values shift up from the insert point); the old rowset keeps `[Mar 1, Mar 16)`, the new rowset takes `[Mar 16, Apr 1)`; **every later rowset `numpart + 1`** (catalog only) | PFS, GAM, 6 IAMs | boundary list; new rowset; renumbered rowsets |
| MERGE, empty (s07) | none | fanout −1; boundary removed; the **right** partition's rowsets and AUs deleted | none | |
| **MERGE, populated** (s08, Jan + Feb) | Feb's rows on new pages of the **surviving left rowset** (Jan's) (17 in-row, 15 LOB, 4 NC); the dropped rowset's 16 + 14 + 3 pages freed | the right rowset (`numpart` 3) and its AUs removed; boundary ids renumbered; later rowsets `numpart − 1` | PFS, GAM, IAMs | boundary removed; vanished rowset |
| TRUNCATE partition, small (s09) | none | `rcrows` → 0; **same AU ids**, `pgfirst`/`pgroot`/`pgfirstiam` → 0 | **pages freed (PFS)**, not in the diff | (E2 small TRUNCATE) |
| TRUNCATE partition, big (s10) | none | **old AU: type 0, owner 0** (deferred drop); **a new empty in-row AU under the same rowset** | pages **still allocated** | AU type 0 / owner 0 |
| … 30 s later (s10b) | none | the old AU row gone | 1,286 pages freed | AU vanished, pages freed |
| REBUILD PARTITION (s11) | all rows on new pages: m_ci p2 33 + 5 NC, heap p2 143 | **new rowset ids for that partition number** (CI, NC, heap; the old ones removed); new in-row AUs; **the LOB_DATA AU kept, `ownerid` → the new rowset** | old pages freed (9 of m_ci's 33, all 143 of the heap's) or reused by the new AU (they show as changed pages with a new header AU) | vanished + new rowset with the same numpart |

**SWITCH in detail:**
- The oracle's `dm_db_database_page_allocations` shows the switched AU on the **same 17 + 15 page ids** before and
  after, with `object_id` changed from `m_ci` to `stg_feb`.
- The page headers name the AU, and the AU names the rowset. Only the rowset's `idmajor` changes, and that sits on a
  `sysrowsets` page, which is in the diff.
- An importer that knows each page's **AU id** from the base IAM images (the allocation images the lake keeps,
  E2 R5; a SWITCH leaves the IAMs unchanged) finds every affected row without reading a page:
  1. after updating the mirror, resolve AU → `sysallocunits.ownerid` → `sysrowsets.(idmajor, idminor, numpart)`;
  2. compare with the table the lake holds the rows in.

  Knowing page → table alone is not enough, because the table is exactly what changes. The key is the AU, which
  the base IAM image gives per page.
- Columnstore rows have no data pages; key them by rowset (hobt) id.

Also seen (noise, but the same rules handle it):
- In s03 the tuple mover deleted `m_cci`'s two TOMBSTONE delta stores: their rowsets, AUs and `syscsrowgroups` rows
  vanished and 16 pages were freed.
- Every differential carries 12–47 unchanged IAM pages copied with mixed extents.

## 5. The general rule against each operation (Q4)

`e7lake.py` is an importer keyed by page and AU. It holds:
- **rows**: table, rowset id = file key, AU, page, slot, canonical text;
- **page → AU** (what the base IAM images give an importer);
- a **mirror** of catalog and allocation page images;
- **LOB roots**.

It never holds user page images. It imports the step's full, applies the step's differential, and compares every table
as a multiset of (partition number from the rowset's current `numpart`, canonical row) with the oracle. Every import
was equal to the oracle before the operation. `run_lake.sh` replays all 15 differentials under two rules in 20 s, with
no server (`out/run_lake.txt`).

- **general** (E1 rule 5 with E2 R5):
  1. Rows on every changed page, every freed page (R3: PFS / GAM / size) and every page whose AU vanished or is
     type 0 / owner 0 are deleted in every table. Reasoned from E2 R4, not measured here: every lake row of a page
     sits in the table of the page's AU in the previous state, so this deletes the same rows as deleting from the
     table the base IAM image names.
  2. Then the changed live pages' rows are inserted under the owner the NEW catalog gives their header AU.
  3. LOBs absent from the file are taken from the lake by identical root (E2 R6): 12 values in s03, 24 in s11.
- **owner** = general plus the ownership rule below.

| step | general | owner | what the apply did |
|---|---|---|---|
| s01 SWITCH out | **DIFFERENT** (560 rows left in m_ci, stg_feb empty) | EQUAL | 0 pages read; 560 rows moved m_ci → stg_feb |
| s02 SWITCH in | **DIFFERENT** | EQUAL | 560 rows moved back |
| s03 SWITCH + DML | EQUAL (by luck: the DML touched every data page) | EQUAL | 600 deleted, 590 inserted |
| s13 SWITCH + 1-row UPDATE | **DIFFERENT** (only the 37 rows of the touched page moved) | EQUAL | 263 rows moved |
| s14 SWITCH part → part | **DIFFERENT** | EQUAL | 320 rows moved |
| s12 SWITCH columnstore | **DIFFERENT** | EQUAL | 1,993 rows moved by rowset |
| s04 NEXT USED, s05 SPLIT empty, s07 MERGE empty | EQUAL | EQUAL | nothing (catalog only) |
| s06 SPLIT populated | EQUAL | EQUAL | 285 rows of freed pages + 37 of a changed page deleted, 322 inserted |
| s08 MERGE populated | EQUAL | EQUAL | 560 + 16 deleted, 576 inserted |
| s09 TRUNCATE small | EQUAL | EQUAL | 620 rows of 17 freed pages deleted |
| s10 TRUNCATE big, immediately | EQUAL | EQUAL | 9,000 rows of 1,286 dead-AU pages deleted |
| s10b TRUNCATE big, 30 s later | EQUAL | EQUAL | 9,000 rows of freed pages deleted |
| s11 REBUILD PARTITION | EQUAL | EQUAL | 2,180 rows deleted and re-inserted; 24 LOBs by root |

Counterfactual: s10 without the dead-AU clause leaves all 9,000 truncated rows in the lake. The deferred-drop pages are
allocated and absent from the diff, so only the AU row tells.

### The exact rule needed

**Ownership rule.** After the catalog mirror has been updated from the file, and after the page rule's deletes and
inserts:
- **for every lake row**, resolve the AU of its page in the base IAM image (columnstore: the rowset in its rowgroup
  key) through the new catalog (`sysallocunits.ownerid` → `sysrowsets`);
- **if the rowset's (idmajor, idminor) differs from the table the row is in, move the row to that table**, keeping its
  values, LOB bytes, fragment map and page;
- **if only `numpart` differs, do nothing.** That is SPLIT/MERGE renumbering: 670 rows were renumbered in s06 with no
  page change. The partition number is never stored with a row; it is derived from the rowset when needed.
- A rowset or AU that vanished is already covered by the page rule (freed / AU gone).

Order: catalog first, then the page rule, then ownership. The rows inserted from changed pages already carry their new
owner, so the order of the last two is free.

**In DuckLake** (DQ-10: files follow DuckLake's layout, and a SWITCH is a lake-side copy):
- DuckLake cannot move a file between tables (research-ducklake-import.md §7.4).
- A lake row's AU is its page's AU in the base IAM image, which a SWITCH leaves unchanged; a columnstore row's
  rowset id is in its rowgroup key (088 D3). Nothing is stored per row for it.
- In the import's transaction, before the new files: `INSERT INTO target SELECT … FROM source WHERE _mssql_page IN
  (the moved AU's pages minus the changed pages)`, then `DELETE FROM source WHERE _mssql_page IN (the moved AU's
  pages)`. The changed pages' rows come from the file through the page rule.
- Not by partition boundary values: a range predicate evaluated in DuckDB would compare a string key by DuckDB's
  order, not the column's collation. The pages are exact for every key type.
- A SWITCH out and back within one differential's interval (s01 then s02 against the same base) nets to no change,
  because only the final owner is compared.

**What each other operation means in the lake** (DuckLake's own `DELETE … WHERE _mssql_page IN …`; DuckLake drops a
file whose rows all go):

| operation | in the lake |
|---|---|
| SPLIT populated | the old rowset's freed and changed pages deleted; the moved rows inserted from the new rowset's pages |
| MERGE populated | the dropped rowset's pages (every one freed) deleted; the moved rows inserted in the survivor |
| TRUNCATE partitions | the freed or dead-AU pages deleted, taken from the base IAM image |
| REBUILD PARTITION | the old rowset's pages deleted, the new ones inserted; LOB bytes from the lake by root |
| NEXT USED, empty SPLIT / MERGE | mirror rows only |

Recognising the operation by name is needed only for the commit message and the log, never for correctness. The rules
above are mechanical.

## 6. Against RESEARCH.md §6.2 / research-ducklake-import.md §7

**§7.2 "To verify (E7)":**

| claim | result |
|---|---|
| functions and schemes are class rows in `sysclsobjs` | **confirmed** (class 30, class 31 `PS` / `FG`) |
| boundary values are sql_variants in `sysobjvalues`, typed by `sys.partition_parameters` | **confirmed**; the base table behind `partition_parameters` is **`systypedsubobjs` class 30** |
| scheme → function and → filegroup in `syssingleobjrefs` / `sysmultiobjrefs` | **`syssingleobjrefs` only** (class 31 / class 30); `sysmultiobjrefs` holds nothing for partitioning |
| index → scheme is `sysidxstats.dataspace` | **wrong**: `dataspace` is 0 everywhere; it is `syssingleobjrefs` class 7, absent = PRIMARY |
| partitioning column in `sysiscols` | **confirmed**: `tinyprop2` = partition ordinal |
| `sysprufiles` unrelated | not needed |

**§7.3** The derived default is confirmed on rows, with three additions:
- "no inner partition gives no spec";
- the DuckDB rounding of TIMESTAMP_NS (datetime2(7));
- the TimeZone dependence of TIMESTAMPTZ transforms.

**§7.4 SWITCH** "swaps rowset ownership; data pages untouched; only catalog pages in the diff; MOVE the AU's
rows" is **confirmed**, in 5 variants and in the replay. Refinements:
- compare (object, index); a `numpart` change alone is a relabel;
- columnstore rows are keyed by rowset.

**§7.4 SPLIT / MERGE** "rows that change partition are physically moved, … general delete-then-insert applies, an
empty range is catalog-only" is **confirmed**. Details:
- RIGHT SPLIT: the new rowset takes the range starting at the new boundary.
- RIGHT MERGE: the rowset starting at the removed boundary is dropped into its left neighbour.
- Later rowsets are renumbered in the catalog only.
- Boundary ids are positional; compare value lists, not ids.

**§7.4 TRUNCATE PARTITIONS** is **confirmed** (E2's small / deferred forms, per partition). The deferred form gives
the rowset a new AU.

**RESEARCH §6.2** "REBUILD keeps the LOB_DATA AU" also holds for `REBUILD PARTITION`.

## 7. Not covered

- RANGE LEFT SPLIT/MERGE. Which side gets the new or dropped rowset was observed for RIGHT only.
- SPLIT/MERGE on an `ALL TO` scheme: is NEXT USED kept after the split?
- Partitioned tables in two files of one filegroup.
- PAGE compression per partition. ROW was decoded.
- SWITCH with `TRUNCATE_TARGET` (2016+); a partitioned columnstore SPLIT / MERGE.
- Functions with more than one parameter (SQL Server allows only one).
- Partitioning on a computed column.
- `sql_variant` boundary types other than those listed (decimal, uniqueidentifier, binary, time).
- A real DuckLake: the lake here is Python in memory, with a page-image mirror standing in for E1's `__mssql_sys` rows.
- A chain of several operations inside one differential. Each differential here carries one; E1 showed that the page
  rule composes, and the ownership rule compares final owners only.

## 8. Files (`experiments/E7/`)

- **`sql/`**: every batch, in order:
  - `a00`–`a03`: bk_e7a;
  - `b00`–`b02`: bk_e7b;
  - `s01`–`s14`: the operations;
  - `z99_cleanup`.
- **`truth.py`**: the oracle through `sql.sh -q`: the partition views, `system_internals_partitions` /
  `_allocation_units`, `dm_db_database_page_allocations`, rows with `$PARTITION`, `RESTORE HEADERONLY`.
  `truth/a_full`, `truth/b_sNN[_before]`.
- **`e7sys.py`**: page store (one file, or full + diffs gated by allocation) and generic base-table decoding; `dump`.
- **`e7part.py`**: the partition model; `show`, `verify`, `transforms`, `lakecounts`.
- **`duck_check.sh`**: DuckDB's own transform values on the oracle's rows.
- **`e7diff.py`**: what a differential carries.
- **`e7lake.py`, `run_lake.sh`**: the importer replay. `E7_NO_DEAD_AU=1` is the counterfactual.
- **`run_b.sh`**: the step driver (server).
- **`bak/`**: 30 backup files.
- **`out/`**: every verify, transform, diff and replay output.
- **`logs/`**: per-step replay logs.
