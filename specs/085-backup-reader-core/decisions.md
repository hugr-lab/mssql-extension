# Decisions for the owner: the backup reader and its DuckLake import

**Status (owner, 2026-11-05): DQ-02 and DQ-10 decided as stated in their sections; every other item
is ACCEPTED AS RECOMMENDED.** The recommendations below are therefore the decisions.

2026-11-05. One deduplicated list of the questions raised by the two draft series:
- mssql 085–090 (`mssql/README.md` and the specs);
- mssql-ducklake 021–023 (`mssql-ducklake/README.md` and the specs).

It also holds E7's datetimeoffset question, now decided (DQ-10), and the disagreements found when the two
series were checked against each other and against `../RESEARCH.md` (R§n). Ids are `DQ-nn`; the specs
cite them so, and their own question lists map to them (§ Where each question came from).

Ordered by impact: the lake's on-disk format and correctness first, then scope and schedule, then
defaults, then names. Each entry gives the question, the options, a recommendation with its reason, and
the specs it affects. **Blocking** means the named PR cannot open until the question is answered.

## Blocking, by when the answer is needed

| needed before | questions |
|---|---|
| 085 PR 1 (the first PR of the series) | DQ-04 minimum version, DQ-36 committed samples, DQ-37 file names |
| 085 PR 2 (the catalog and the scan) | DQ-02 service-column types, DQ-11 what base tables leave the backup |
| the first lake (021 phase A) and 089 PR 1 | DQ-01 LOB splice source, DQ-02, DQ-05 LOB maps in phase-A lakes |
| scheduling anything after 085 | DQ-03 the order of 088 and 090 |

## Summary

| id | question | recommendation | blocks |
|---|---|---|---|
| DQ-01 | where a LOB splice's old bytes come from | re-encode the lake value; keep raw bytes only for values that do not round-trip (021's hybrid) | 021 phase A, 089 PR 1 |
| DQ-02 | physical types of the service columns | **decided** (owner, 2026-11-05): all opaque `BLOB`s serialized by the C++ code | — |
| DQ-03 | order of 088 and 090; order of 090's guard extensions | 090 before 088 for one developer; ROW / PAGE guards first | planning |
| DQ-04 | minimum SQL Server version | 2017 | 085 PR 1 |
| DQ-05 | phase-A lakes have no LOB maps | move the plain `_mssql_lob` output into 085 PR 3 | 021 phase A |
| DQ-06 | a stale page the next file neither carries nor frees | a named error | 022 I4, 090 |
| DQ-07 | a columnstore table whose loaded tail recovery cannot cover | refuse the file | 022 I5, 088, 090 |
| DQ-08 | users' writes to imported tables | refuse the import by default | 022 |
| DQ-09 | where pending page images (uncommitted rows) live | in the lake, `__mssql_sys.pending_pages` | 021 D11, 022 I4 |
| DQ-10 | partition values, file layout and SWITCH in the lake | **decided** (owner, 2026-11-05): follow DuckLake's rules | — |
| DQ-11 | which base tables and rows leave the backup | allow-list of tables and of `sysobjvalues` value classes; no module source | 085 PR 2 |
| DQ-12 | how lake state reaches the reader | Parquet side files under `base`, side connection | 089 PR 1 |
| DQ-13 | browsing defaults | `recovery := true`, `fuzzy := 'warn'` | 085 PR 5, 090 |
| DQ-14 | `mode := 'rebase'` in v1 | yes | 022 |
| DQ-15 | the diff base of a lake born from a copy-only full | the base its header reports, if verified; else refuse diffs | 022 |
| DQ-16 | which fulls start a generation | non-copy-only fulls only | 022, 023 |
| DQ-17 | offer `loaded_diff := 'partial'` | yes, as a non-default option | 022, 090 |
| DQ-18 | the Parquet export lives in mssql | yes | 089 PR 2 |
| DQ-19 | `VARIANT` for `sql_variant`, `FLOAT[n]` for `vector` | yes | 086 PR 3 |
| DQ-20 | spatial Z / M and CRS | 2D WKB as `STAsBinary()`, optional per-column CRS | 086 PR 5 |
| DQ-21 | import entry points | one PRAGMA for any data backup, plus the strict diff form | 021 |
| DQ-22 | one source database per lake in v1 | yes | 021 |
| DQ-23 | nullability of lake columns | all nullable | 021 |
| DQ-24 | Parquet field ids or name maps | name maps in v1 | 021 phase B, 089 |
| DQ-25 | which sort orders the importer proposes | only those the physical read order already satisfies | 021, 089 |
| DQ-26 | `bucket` overrides at creation | allow, with a warning | 021 |
| DQ-27 | renames on both sides | apply a SQL Server rename only while the lake name is the importer's | 022 |
| DQ-28 | encrypted lakes | refuse in v1 | 021 |
| DQ-29 | an explicit `key_secret` override | none in v1 | 087 |
| DQ-30 | what `keep_snapshots` counts | the last *n* over all kept generations | 023 |
| DQ-31 | `dry_run` default of the expiry | `true` | 023 |
| DQ-32 | `include_other` and the importer's compaction snapshots | keep them | 023 |
| DQ-33 | `auto_maintain` by default | expiry only | 023 |
| DQ-34 | the run id in file names | keep it | 021 |
| DQ-35 | the helper naming family | keep as drafted | 021–023 |
| DQ-36 | committed samples or a release asset | committed, ≤ 2 MB each, ≤ 16 MB in all | 085 PR 1 |
| DQ-37 | `backup_<component>.cpp` source names | yes | 085 PR 1 |

## The questions

### DQ-01 — Where does a LOB splice take the old value's bytes from? *(the known disagreement)*

**Blocks** 021 phase A (the first lake's format) and 089 PR 1 (task 7). Affects 089 D6 / D10, 021 D3,
022 D6, and DQ-02 / DQ-05.

A `.WRITE` leaves most of a value's fragments out of the differential (736 of 749 for 6 MB, E2 §5). The
reader rebuilds the value from the file's nodes plus the lake's fragment map, copying **byte ranges of
the old stored form** (E1 §4.5). So the old bytes must be reproduced exactly. The drafts disagree on how:

| option | what the lake keeps | cost |
|---|---|---|
| **A. Store the bytes** (E1 rule 8: "store LOB bytes (or a lossless mapping), not decoded text: the splice re-encodes the old value") | every off-row value twice, as text and as its stored bytes | roughly double LOB storage (more for ASCII `nvarchar(max)`: its UTF-16 bytes are twice its UTF-8 text). Never refuses; covers `xml`, `json` and CLR values; no re-encoder to get wrong |
| **B. Re-encode, guarded per value** (089 D6 as drafted) | the text only, plus a `text_exact` flag set at decode | no extra storage. A splice over a value that does not round-trip is refused by name, and the table then needs a rebase from a later full. Values that do not round-trip: an unpaired surrogate in `nvarchar(max)` (no valid UTF-8 for DuckDB's `VARCHAR`), a code-page byte with no mapping or a best-fit mapping that is not one-to-one, invalid UTF-8 under a UTF-8 collation; and `xml` (stored as BINXML), `json` and CLR values are never spliceable. A bug in a re-encoder would be a wrong splice, which the guard catches only for the values it checks |
| **C. Re-encode, bytes for the exceptions** (021 D3 as drafted, `raw BLOB`) | the text, plus the stored bytes only where re-encoding would not reproduce them, and for every off-row `xml` / `json` / CLR value | storage grows with the exceptions only. Never refuses. Two code paths (re-encode and copy) |

**Recommendation: C**, with one refinement: keep `raw` for `xml` / `json` / CLR values only if a fixture
shows that a partial update can leave their fragments out of a differential (E1 and E2 tested
`varchar(max)` `.WRITE` only); until then such a splice is refused as in B. Reason: C is exact in every
case and never blocks a table, which the owner's "the next file must restore exactly" asks for. Its
storage cost is proportional to the rare values that need it. A doubles LOB storage, the same order of
cost for which the owner rejected keeping raw record bytes per row (R§4, variant 1). B turns an
unusual character into a forced rebase.

### DQ-02 — What are the service columns' physical types?

**DECIDED (owner, 2026-11-05): every service column is opaque bytes (`BLOB`), serialized and read only by
the C++ code that writes and applies backups — nothing else uses them.** Each value starts with a
format/shape byte so the encoding can evolve. `_mssql_rid` BLOB (as drafted); `_mssql_page` BLOB
(`u16 file ‖ u32 page`, big-endian, so byte order is page order and min/max still prune the
native-DELETE fallback); `_mssql_lob` BLOB (the serialized list of off-row entries: column id, root
bytes, fragment nodes, and the raw old bytes where DQ-01's hybrid keeps them); no `_mssql_rowgroup`
(the rowgroup is inside `_mssql_rid`). Internal `__mssql_sys` tables use the same byte encodings
wherever they are compared with these columns.


**Blocks** 085 PR 2 (D8) and 021 phase A. Affects 085 D8, 088 D3, 089 D6, 021 D3.

These columns are materialised in every lake table, so they are fixed once lakes exist.
- **`_mssql_rid`**: an opaque `BLOB` with a tag byte per shape (085 D8, 088 D3; as both series draft),
  or a `STRUCT` per table shape (021 Q3).
- **`_mssql_lob`**: one `MAP(INTEGER, STRUCT(root, nodes, …))` keyed by column id (021), one
  `LIST(STRUCT(column_id, root, nodes, …))` (089), or one service column per LOB column (021 Q3).
  The node's page is drafted as `page BIGINT` in the `_mssql_page` form (021), or as
  `file_id USMALLINT, page_id UINTEGER` (089). The last field depends on DQ-01.
- **`_mssql_rowgroup INTEGER`** for columnstore rows (088 D3), or none.

**Recommendation**:
- `_mssql_rid`: `BLOB`. A table can change shape between files (heap → clustered → columnstore), so a
  per-shape STRUCT would need a lake ALTER, while the BLOB compares byte-wise whatever the shape.
- `_mssql_lob`: one `MAP` keyed by column id, so a lookup by column is direct, keys are unique, and a
  column rename changes nothing. Nodes use `page BIGINT`, one page encoding with `_mssql_page`,
  `page_map` and `changed_pages`. One column per LOB column would add DDL whenever a LOB column is
  added or dropped. DuckLake's support for `MAP` is to verify (`ducklake_types.cpp`).
- No `_mssql_rowgroup`. The rowgroup is already in `_mssql_rid`, and the page map's columnstore runs
  carry it; the page-map rebuild reads it from `_mssql_rid` (021 D6).

### DQ-03 — In what order do 088 (columnstore) and 090 (tail recovery) come, and 090's guard extensions?

**Blocks** planning past 085. Affects the mssql README's tracks, 088, 090, and mssql-ducklake I4 / I5.
R§10.4 left it open.

Options:
- 090 first (track A: 085 → 089 PR 1 → 090), as the mssql README's single-developer interleave does;
- 088 first;
- two developers on both tracks (about 34 weeks instead of 51).

The guard extensions of 090, each refused by name until built: ROW / PAGE compression (2–3 wk), ADR /
RCSI (3–4 wk), DDL in the tail (1–2 wk), columnstore delta stores and bitmaps (1 wk, after 088).

**Recommendation**: 090 before 088 for one developer; among the guards, ROW / PAGE first, then DDL in
the tail, then ADR / RCSI, then columnstore. Reason: after 089 PR 1 the importer already takes quiet
backups; 090 unblocks every busy rowstore database, while 088 unblocks only columnstore tables. ROW /
PAGE compression is the commonest guard refusal in real databases (090 risks).

### DQ-04 — What is the minimum SQL Server version?

**Blocks** 085 PR 1 (the D14 version gate) and PR 2 (the CI oracle lane). Affects every mssql spec's
version matrix. R§10.5 left it open.

Options: 2016 (Windows images only: no CI), 2017 (the oldest Linux image), 2019.

**Recommendation: 2017.** It is the oldest version CI can verify. Older backups are accepted only with
`unsupported_version := true`, or through committed samples contributed with their generator.

### DQ-05 — Do lakes written before 089 carry LOB fragment maps?

**Blocks** 021 phase A (I1). Affects 085 PR 3, 089 task 7, 021 D2.4 and D3, and the mssql-ducklake
README's I1.

Phase A (I1) needs only 085 (+086), but `_mssql_lob` is 089's column. A lake imported before 089 PR 1
would have no maps, and 022 would then refuse a partial LOB update on it.

Options:
- (a) move the plain `_mssql_lob` output (root bytes and the node list the LOB reader already walks; no
  hybrid walk, no `base`) into 085 PR 3, about one day out of 089 task 7;
- (b) phase-A lakes have no maps and need a rebase before their first differential with a partial LOB
  update;
- (c) phase A waits for 089 PR 1.

**Recommendation: (a).** Every lake is then born ready for its next file, at no extra cost. Its format
follows DQ-01 and DQ-02.

### DQ-06 — A stale page the next file neither carries nor frees: error or report?

Affects 090 (stale invariant), 022 D7 and its I4 phase. A disagreement between the series; RESEARCH
does not settle it.

E6 never saw it: the next file carried or freed 551 / 551 and 1,419 / 1,419 stale pages.
- **090 as drafted**: a named error at that file.
- **022 D7 as drafted**: report it by name, keep the page stale and proceed.

The check runs in the importer either way, since the stale set is lake state, not a reader input
(089 D10).

**Recommendation: a named error**, with `mode := 'rebase'` on the next full as the way out (DQ-14).
Reason: such a page means a broken assumption of the model, not an expected state. Proceeding would
leave committed work missing for good, against the owner's "nothing lost".

### DQ-07 — Columnstore under load: refuse the file, or mark the table stale?

Affects 088 D6, 090 D7, 022 D9, and mssql-ducklake I5. A disagreement between the series.

When a loaded tail touches a columnstore table (delta stores, delete bitmap, the tuple mover, new
segments), recovery cannot cover it until 090's columnstore guard extension exists.
- **090 D7**: the table is `unrecoverable`, so the importer refuses the file.
- **022 Q4 as drafted**: mark the table stale for that file and let the next file heal it.

**Recommendation: refuse the file** until 090's columnstore extension and E11. Reason: "stale" is
dirt-free only if no open transaction touched the table and the tuple mover's state is not torn (a
rowgroup compressed while its delta store still holds the rows would show them twice). Neither is
measured (E5 §6.6), and the owner allows no dirt.

### DQ-08 — What if users write to imported tables?

Affects 022 D12 and 021 D2.3. It is the mssql-ducklake README's question 3.

Options:
- refuse the import, naming the snapshot, its author and the table (as drafted;
  `foreign_writes := 'ignore'` accepts the divergence and records it);
- accept silently;
- present imported schemas as read-only by some other means (DuckLake has no per-table read-only).

**Recommendation: refuse by default**, as drafted. Reason: a user's insert has no page and survives every
later delete by page; an update re-inserts rows that name their old page, which the next page delete
misses. Partition, sort, option and comment changes stay allowed, since manual layout changes win
(DQ-10).

### DQ-09 — Where do the pending page images live?

Affects 021 D3 / D11, 022 D7, 023 D3, and 090. It is the mssql-ducklake README's question 4.

`__mssql_sys.pending_pages` holds pre-undo page images, that is, rows of transactions still open. Healing
needs them (E6: without them a differential loses 2,282 committed keys for good). But they are
uncommitted data, kept in a hidden table and in its history until expiry.

Options:
- in the lake, as drafted;
- a sidecar outside the lake (the run directory, with its own retention);
- encrypted in the lake.

**Recommendation: in the lake**, as drafted, documented, and expired with their snapshots (023).
Reason: the images are then atomic with the import's snapshot, so a retry or time travel sees the
images that belong to that state. A sidecar needs its own consistency and retry protocol. User tables
never show them, so the owner's "no dirt in restored data" holds.

### DQ-10 — Partition values, file layout and SWITCH in the lake — **DECIDED** (owner, 2026-11-05)

This closes E7's datetimeoffset question ("derive nothing for datetimeoffset unless every writer runs in
UTC?") and the drafts' per-rowset files. The owner's decision: **follow DuckLake's rules, do not
replicate SQL Server.**
1. SQL Server's partitioning only **proposes** the initial DuckLake spec at table creation (which
   transform to declare). Nothing beyond that mirrors SQL Server.
2. Partition values are computed exactly as DuckLake computes them, with DuckLake's own
   `DuckLakePartitionUtils::ApplyPartitionTransform` bound in the importing session, like any DuckLake
   writer:
   - the session `TimeZone` for TIMESTAMPTZ. DuckLake does not normalise to UTC: the same instant is
     month 672 in UTC and 673 in Europe/Berlin (E7 §3);
   - DuckLake's own TIMESTAMP_NS → µs rounding.

   UTC is not forced. The docs say the values depend on the importing session's TimeZone.
3. Files are laid out by DuckLake partition values, sort order and file size, not per SQL Server
   partition or rowset. SQL Server partitions may still serve as work units of the parallel writers.
4. A SWITCH is done with DuckLake means: a lake-side copy of the affected rows into the target, plus
   positional deletes in the source. The rows are found by E7's AU / rowset rule. It is not a file move.
   The page map still records the AU / rowset per page, because that rule needs it.
5. The sort order and file sizes follow DuckLake's settings; defaults derived from SQL Server (the
   clustered key as the sort order) are proposals at creation only.

Applied in: RESEARCH §6.2; 089 D9 and § Surface (`partition_by`, `order_by`); 021 D1, D2, D4, D5; 022
D3, D8; the mssql-ducklake README. The `datetimeoffset` proposal is still derived from the boundaries'
UTC instants, as SQL Server defines its partitions; it is a proposal only. Open follow-ups: DQ-25 and
DQ-26.

### DQ-11 — Which base tables and rows leave the backup?

**Blocks** 085 PR 2 (`mssql_backup_catalog`). Affects 085 § Security, 089 D5, 021 D3. It merges the mssql
README's question 3 with 021's question 9.

The reader's `mssql_backup_catalog` serves an allow-list:
- E1's mirror set;
- `sysobjvalues`, `sysidxstats`, `sysiscols`;
- the columnstore tables;
- E7's `systypedsubobjs` and `syssingleobjrefs`.

It refuses the rest by name, such as `sysowners` (contained users' password hashes) and the key and
certificate tables. 021 mirrors `sysobjvalues` only for view text and partition boundaries, without
module source.

Options:
- the reader serves `sysobjvalues` whole and the importer filters it;
- the reader filters it too;
- allow-list tables only, with no row filter.

**Recommendation: the reader allow-lists tables and `sysobjvalues` value classes.** The classes are
view definitions, partition boundaries (valclass 30), and the computed-column and default definitions
that `__mssql_backup.columns` records; the classes are pinned by 085 task 11. Module source of
procedures, functions and triggers never leaves. The mirror takes `syssingleobjrefs` classes 7, 8, 30
and 31 only. Reason: the least exposure that still gives the importer everything it needs (E1 §4.8,
E7 §2).

### DQ-12 — How does lake state reach the reader?

**Blocks** 089 PR 1 (task 2). Affects 089 D10, 090, 022 D2 / D3.

Options:
- Parquet files under one `base` prefix with fixed names, read by the reader on a side connection (as
  drafted);
- a SQL string run on a side connection (it reads committed lake state, not the caller's transaction);
- a session object filled by an aggregate (order-dependent, no precedent).

**Recommendation: as drafted.** Prototype the side connection first. Reason: it keeps mssql
lake-agnostic, makes a retry deterministic (the inputs are frozen files), and has precedents in DuckLake
and duckdb-iceberg.

### DQ-13 — What are the defaults for browsing a backup?

Affects 085 D11, 090 D3. It merges the mssql README's questions 1 and 12. R§10.3 is settled for the
lake: the importer always refuses (R§4).

Options for `mssql_backup_scan` and ATTACH:
- `recovery := true` (RESTORE's answer) or `false` (the page state as of CheckpointLSN);
- `fuzzy := 'warn'`, `'error'` or `'ignore'`. With recovery on, `fuzzy` matters only for tables recovery
  cannot cover.

**Recommendation: `recovery := true`, `fuzzy := 'warn'`.** Reason: RESTORE is the oracle, so the default
answer should be RESTORE's. A browsing user sees the per-table status and the warning in `duckdb_logs`;
refusing by default would make busy backups unbrowsable until 090's guards exist.

### DQ-14 — Is `mode := 'rebase'` in v1?

Affects 022 D10.

`rebase` re-reads every page of a full whatever its LSN. It is the only repair, the only way through a
recovery fork or a filter change, and the way out of DQ-01 B and DQ-06.

**Recommendation: yes** (about 2 days, 022 task 12).

### DQ-15 — What diff base does a lake born from a copy-only full record?

Affects 022 C6.

Options:
- the base that file's header reports, if it reports the database's current diff base (to verify on
  E1's copy-only sample);
- no base: refuse differentials until a non-copy-only full is applied.

**Recommendation: as drafted**: take the reported base once it is verified, else refuse. Reason: a
differential holds every page changed since its base, which is a superset of the changes since a later
copy-only full, so applying it is correct when the base is right.

### DQ-16 — Which fulls start a generation?

Affects 023 D1, 022 C5. A contradiction inside mssql-ducklake:
- 022 C5 (and D10) said "a non-copy-only full ... starts a generation";
- 023 D1 says any full, copy-only included.

R§6.2 says only "an applied full".

**Recommendation: non-copy-only fulls only.** Reason: COPY_ONLY means "outside the backup sequence".
Counting ad hoc copy-only fulls would let `keep_fulls := m` expire the scheduled history faster than the
user expects.

### DQ-17 — Offer `loaded_diff := 'partial'` at all?

Affects 090 D4, 022 D1 / D7.

`'skip'` is the default (the owner's variant 1). `'partial'` gives fewer stale keys but shows rows twice
and torn LOBs until the next file (E6 §3.4).

**Recommendation: offer it as a non-default option, documented.** Reason: the owner chose skipping
"by default", which implies an alternative may exist, and the code is E6's main variant.

### DQ-18 — Does the Parquet export live in mssql?

Affects 089 D9 and PR 2.

**Recommendation: yes.** Reason: only the reader knows where each page's rows land in a file. The
export writes files and registers nothing, so mssql stays lake-agnostic.

### DQ-19 — `VARIANT` for `sql_variant` and `FLOAT[n]` for `vector`?

Affects 086 D2. The backup reader would type these columns differently from `TYPE mssql`, which returns
`VARCHAR`.

**Recommendation: yes.** Reason: both are lossless (the base type and precision survive; the exact
vector type). DuckLake stores `VARIANT`, and the importer casts `FLOAT[n]` to `FLOAT[]`. The live
catalog may follow later.

### DQ-20 — How are spatial values returned: Z / M, and the CRS?

Affects 086 D6.

**Recommendation: 2D WKB, byte-equal to `STAsBinary()`.** Z and M are dropped, as the live catalog
drops them, and documented; curves are refused by name; the column is `GEOMETRY` without a CRS, with an
optional per-column `crs := MAP {…}` that refuses other SRIDs. Reason: parity with `TYPE mssql`, and an
oracle the server provides.

### DQ-21 — How many import entry points?

Affects 021 D1, 022 D1. It is the mssql-ducklake README's question 1.

**Recommendation: as drafted.** `ducklake_import_mssql_backup` takes any data backup and decides from
its header; `ducklake_import_mssql_diff_backup` is the strict form that refuses anything but a
differential. Reason: it keeps the owner's names, gives scripts one call, and gives a schedule that
expects a differential a safety catch.

### DQ-22 — One source database per lake in v1?

Affects 021 D2.1.

**Recommendation: yes.** A `schema_prefix` and per-database `__mssql_sys_<db>` can come later without
changing a v1 lake.

### DQ-23 — Are lake columns nullable?

Affects 021 D3, 022 D5.

**Recommendation: all nullable**, as drafted, with SQL Server's NOT NULL kept in
`__mssql_backup.columns`. Reason: `SET NOT NULL` scans the table in DuckLake, and SQL Server changes
nullability as metadata only.

### DQ-24 — Parquet field ids, or name maps?

Affects 021 D2.3 / D7, and 089 `field_ids`.

Field ids save T·(1 + C) name-map rows per commit, but they mean predicting DuckLake's depth-first ids
for a new table.

**Recommendation: name maps in v1**, as `ducklake_add_data_files` does. Field ids come later, if 021
task 12's commit measurement asks for them (E8 was not run).

### DQ-25 — Which sort orders does the importer propose at creation?

Affects 021 D5, 089 `order_by`. With DQ-10 the export writes in the table's DuckLake sort order. A sort
the physical read order does not already satisfy forces a sort per unit and scatters a page's rows,
towards one page-map run per row.

Options:
- (a) the clustered rowstore key, when every key column orders in DuckDB as on disk;
- (b) as drafted: (a) plus an ordered CCI's `ORDER (col)`;
- (c) every clustered key;
- (d) none.

**Recommendation: (a).** Reason: the leaf-chain order already satisfies it, so it is free and keeps
page runs. An ordered CCI's order does not hold row by row inside a rowgroup (VertiPaq reorders), so
(b) would sort every rowgroup and lose 088's tuple runs. A string key under a collation orders unlike
DuckDB. The user can still declare any sort with DuckLake's own ALTER.

### DQ-26 — `bucket` overrides at creation?

Affects 021 D5.

The draft refuses them, because a bucket scatters a page's rows over n files (one run per row).

**Recommendation: allow, with a warning naming the cost.** Reason: under DQ-10 the layout is DuckLake's,
and the importer must write correctly under a bucket spec a user sets later anyway, since manual changes
win. Refusing it at creation saves nothing.

### DQ-27 — Renames on both sides?

Affects 022 D5.

**Recommendation: as drafted.** A SQL Server rename is applied only while the lake name is still the
one the importer last set; a manually renamed lake table keeps its name, with a notice. Reason: the
same "manual changes win" rule as for partitioning.

### DQ-28 — Encrypted lakes in v1?

Affects 021 § Enforcement.

**Recommendation: refuse in v1.** Reason: the export cannot encrypt with DuckLake's per-file keys, and
`ducklake_add_data_files` refuses encrypted catalogs too.

### DQ-29 — An explicit `key_secret := 'name'` override beside scope-then-thumbprint?

Affects 087 D3.

**Recommendation: none in v1.** Reason: one selection rule (the owner's, R§6.1), and a scope already
pins a secret per path.

### DQ-30 — What does `keep_snapshots := n` count?

Affects 023 D2.

**Recommendation: the last *n* import snapshots over all kept generations**, as drafted, plus each kept
generation's full snapshot. Reason: "the last 14 days" is what users mean.

### DQ-31 — The expiry's `dry_run` default

Affects 023 D3.

**Recommendation: `true` for a call by hand.** The automatic policy passes `false`. Reason: an expiry is
irreversible and can drop many snapshots at once. This departs from DuckLake's own default.

### DQ-32 — Does `include_other` cover the importer's own compaction snapshots?

Affects 023 D2 / D5.

**Recommendation: no; keep them**, as drafted. Reason: they reference files that later imports
reference too, so keeping them costs almost nothing.

### DQ-33 — Does `auto_maintain` compact after imports by default?

Affects 023 D5.

**Recommendation: expiry only**, until 023 task 6's bench sets the compaction thresholds.

### DQ-34 — The run id in file names?

Affects 021 D4. The owner asked for names keyed by BackupSetGUID / table / chunk.

**Recommendation: keep the run id** (`mssql-<guid>-<run8>-o<object>-p<partition>-u<unit>-<seq>`).
Reason: two concurrent attempts on one lake cannot overwrite each other's files, and resume keys on it.

### DQ-35 — The helper naming family

Affects 021–023. It is the mssql-ducklake README's question 2.

**Recommendation: as drafted.** The owner's user-facing names stay (`ducklake_import_mssql_*`,
`ducklake_mssql_backup_expire`); the helpers use `ducklake_mssql_backup_*`.

### DQ-36 — Committed samples, or a pinned test-data release asset?

**Blocks** 085 PR 1 (task 10: the fuzz seeds and the server-free lane). Affects the mssql README's T1.

**Recommendation: committed samples**, at most 2 MB each and 16 MB in all, each with its generator, its
HEADERONLY output and golden results. Reason: `make test` and the fuzz seeds then work offline on every
platform. Only synthetic data made on a clean instance is ever committed (T4).

### DQ-37 — `backup_<component>.cpp` source names?

**Blocks** 085 PR 1 (task 1: the skeleton). Affects 085 D16 and CLAUDE.md's file rule, which names only
`mssql_` and `tds_`.

**Recommendation: yes.** `backup_<component>.cpp` is the peer of `tds_<component>.cpp`, and CLAUDE.md's
rule is extended to name it.

## Settled during the consistency pass (no question)

RESEARCH or an experiment already answers these, and the drafts were aligned to it:

- **No dirt in the lake** (R§4):
  - the importer always refuses an inconsistent file. 021's `fuzzy := 'allow'` is gone; the
    counterfactual is a test-only switch;
  - a TDE backup is refused until E10 (087 D9);
  - a table recovery cannot cover refuses the whole file (090 D7).
- **An unresolved LOB node**: the old value and its map are kept only when the node lies on a page that
  a loaded differential's recovery marks stale (E6 rule 8 amended), and the page is reported `stale` with
  reason `lob`. Otherwise the value is refused by name (E1 rule 7). 022 D6 said "keep" in every case, and
  089 mentioned a `lob_unresolved` parameter that 090 never defined. Both are fixed.
- **Database identity** is the FamilyGUID (R§3) plus the RecoveryForkID (022 C1). 089's chain check also
  takes the base LSN (R§3 "DifferentialBaseGUID, base LSN, FamilyGUID"). 085 reports the fork id, still
  to locate.
- **The page LSN** lives in the page map, not in every row (R§6.2 point 4, E2 R5). The export writes it
  into its page-run files, and lake tables do not get `_mssql_page_lsn`; before, 089's export wrote a
  column that 021's tables did not declare.
- **Columnstore runs** carry `first_tuple`, because only surviving tuples are written (088 D3). 021 and
  022 had assumed `first_pos + t`.
- **E7 integrated**:
  - where the partitioning lives (085);
  - the ownership rule, `au_moved`, the dead-AU clause and the maintenance evidence (089);
  - the page map records the AU / rowset (021);
  - SWITCH as a lake-side copy (022), per DQ-10.
- **One vocabulary across the series**:
  - `position` (was 021's `backup_set`);
  - `loaded_diff` (was 022's `diff_tail`);
  - `reflected_lsn` (was `prev_last_lsn`);
  - `base :=` with 089's fixed file names (was `catalog_base`, `lob_side`, `alloc_base`, `pending`);
  - `xids`;
  - `mssql_backup_catalog(path)` with no table lists the allow-listed base tables;
  - pending pages come from `mssql_backup_pending_pages`, not from the export.
- **The stale set** is lake state, not a reader input. The importer checks healing (089 D10, 022 D7);
  whether a failed check is an error is DQ-06.
- **Experiments**: E8 (the commit cost) was not run; E9 (`json`), E10 (the TDE log tail) and E11
  (columnstore in differentials) are planned, and R§12 lists them.

## Where each question came from

| source | items → DQ |
|---|---|
| mssql README decisions 1–14 | 1 → 13, 2 → 04, 3 → 11, 4 → 37, 5 → 19, 6 → 20, 7 → 29, 8 → 02, 9 → 12, 10 → 01, 11 → 18, 12 → 13, 13 → 03, 14 → 36 |
| mssql-ducklake README questions 1–4 | 1 → 21, 2 → 35, 3 → 08, 4 → 09 |
| 021 questions 1–10 | 1 → 34, 2 → 24, 3 → 02, 4 → 23, 5 → settled (no dirt), 6 → 22, 7 → 26, 8 → 25, 9 → 11, 10 → 28 |
| 022 questions 1–6 | 1 → 15, 2 → 27, 3 → 17, 4 → 07, 5 → 08, 6 → 14 |
| 023 questions 1–5 | 1 → 30, 2 → 16, 3 → 31, 4 → 32, 5 → 33 |
| E7 (datetimeoffset) | → 10, decided |
| found in this pass | DQ-01's three-way split (E1 rule 8 vs 089 vs 021), DQ-02's shape split, DQ-05, DQ-06 (090 vs 022), DQ-07 (090 vs 022), DQ-16 (022 vs 023) |
