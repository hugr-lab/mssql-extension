# Spec 088: columnstore tables

Status: **draft**, 2026-11-05; revised 2026-11-28 after E5b (the format on 2019 and 2022) and E11 (columnstore
in differentials and fuzzy backups). Not started. Order (owner, 2026-11-19): after 087, before 089 and 090.
Depends on spec 085 (pages, LOB reader, the MS-XCA decoder, the differential overlay) and on 086 PR 1's CD
records (merged). Series overview: `../README.md`. Markers and source abbreviations as in 085.

## Problem

Columnstore is **required** (owner, R§10.4). Spec 085 lists a clustered columnstore table and refuses
its scan. E5 decoded the whole format clean-room: 21 tables in three backups, 3.74 M rows, 168 column
checks, every one equal to the server's `SELECT` as a multiset, byte for byte in canonical form
(E5§1, §4) **(2025)**. E5b ran E5's batches on 2019 and 2022 and read every table equal to the server, with
the decoder unchanged (`E5b-columnstore-versions.md`). E11 did the same with each operation's full and
differential backups read as one state, 105 of 105 checks equal (`E11-columnstore-diff.md`). This spec turns
E5's decoder into the reader's columnstore path.

## Scope

In: clustered columnstore indexes (CCI), `COLUMNSTORE` and `COLUMNSTORE_ARCHIVE`, every rowgroup state,
delete bitmaps, delta stores, partitions, ordered CCI, every type a CCI accepts, `(max)` values in
and off row; the locators the lake needs; a CCI read through `diff :=` (085's overlay, E11). A CCI table is an
entry of an attached backup's catalog (`FROM bk.dbo.t`, owner 2026-11-22: the catalog is the primary
interface), read by the same scan as `mssql_backup_scan` and `mssql_backup_catalog`; `mssql_backup_tables` reports it
`decodable` from this spec on. No table read is refused (owner, 2026-11-22): what the
decoder does not know becomes NULL with a WARNING (D5).

Out: reading a nonclustered columnstore (NCCI) as a table. An NCCI duplicates columns of a rowstore
table whose base rowset 085 reads; the scan ignores it. E5's NCCI decoding (delete buffers, locators,
index-record delta stores) stays in the experiment. Native `json` text (086's spike) applies here
unchanged: a CCI stores the same binary (E5§6.2).

## The format (E5§3, quoted; every fact **(2025)** unless marked)

**Pieces.** `sysrowsets` rows with `idmajor` = object, `idminor` = index id, `numpart` = partition:

| `ownertype` | rowset | `cmprlevel` | layout |
|---|---|---|---|
| 1 | the columnstore itself | 3 COLUMNSTORE, 4 COLUMNSTORE_ARCHIVE | no pages of its own; segments and dictionaries live in its LOB allocation unit |
| 2 | delete bitmap | 2 (PAGE) | (rowgroup id, tuple id); CD records |
| 3 | delta store, one per OPEN or CLOSED rowgroup (CLOSED seen in E11: 1,048,576 rows, status 258, read as an OPEN one; a TOMBSTONE's old one remains until a REORGANIZE removes it, E11) | 0 | B-tree of data records (CCI) |
| 4 | delete buffer (NCCI only) | 0 | not read here |

- `syscsrowgroups` (obj 35): `hobt_id`, `segment_id` (= rowgroup id), `version`, `ds_hobtid` (the delta
  store of an OPEN or TOMBSTONE rowgroup, E5§3.1), `row_count`, `status`, `flags`, … State = `status & 0xFF`:
  1 OPEN, 2 CLOSED, 3 COMPRESSED, 4 TOMBSTONE (matched the DMV on 30 rowgroups; the high byte varies with how
  the rowgroup was made, e.g. 515 / 1283 / 1539 / 1795 all COMPRESSED, E11).
- `syscscolsegments` (obj 62): `hobt_id, column_id, segment_id, version, encoding_type, row_count,
  status, base_id, magnitude (float), primary_dictionary_id, secondary_dictionary_id, min_data_id,
  max_data_id, null_value, on_disk_size, data_ptr binary(16), …, collation_id, min_deep_data,
  max_deep_data`. `status` bit 0 = has NULLs. `column_id` here is the **hbcolid** (the view's
  `column_id + 1`).
- `syscsdictionaries` (obj 63): `hobt_id, column_id (hbcolid), dictionary_id, version, type, flags,
  last_id, entry_count, on_disk_size, data_ptr, container_id`.
- `data_ptr` = u64 blob id + RID (u32 page, u16 file, u16 slot) of an ordinary LOB root (type 5
  LARGE_ROOT); 085's LOB reader returns exactly `on_disk_size` bytes (279 of 279 blobs).
- Column mapping (CCI): `rscolid 0` is a hidden int with no segment; table column k has `rscolid` k
  and `hbcolid` k + 1. Types from `syscolpars`.
- **Blob container**: `version` bit 31 set = the blob is wrapped (every segment and dictionary of a
  COLUMNSTORE_ARCHIVE index, and the `(max)` and `json` columns of an ordinary CCI): `u32 0`, then
  frames `(u32 uncompressed ≤ 65,536, u32 compressed, MS-XCA LZ77+Huffman bytes)` to the end. A frame
  with compressed = uncompressed is taken raw (assumed, not observed).

**Segment, encodings 1–4** (lob type 3):

```
+0   u32 1, u32 1, u32 0, u32 w3      1, 1, 0 on 2019 / 2022 / 2025; w3 is NOT a constant (E5b): 10 on 2025;
                                       9, 0, 17799, 0xFFFFFFFF on 2022; 10, 0 on 2019. Not read.
+16  u32 3                             lob type
+20  u32 B   bookmark count = floor(rows / D) + 1
+24  u32 D   bookmark distance (64, 256, 512, 16384 seen)
+28  u32 R   RLE array size in 8-byte units
+32  u16 8,  u16 W   bit-pack width (0..64)
+36  u32 U   bit-pack 64-bit words
+40  i64 M   bit-pack MinId
+48  B x 8 bookmarks (not needed to decode), R x 8 RLE array, U x 8 bit-pack array
     total = 48 + 8(B + R + U) = on_disk_size (unwrapped)
```

- RLE entries in row order, ending in an all-zero entry. Narrow (encodings 1–3): `(u32 value, u32
  count)`; bit 31 of value set = a bit-packed run of `count` values starting at bit-pack index
  `~value`; else `count` rows of data id `value`. Wide (encoding 4): `(u64 value, u32 count, u32
  flag)`, flag 1 = a bit-packed run at `~value`. Bit-packed runs consume the array contiguously.
- Bit-pack: `floor(64 / W)` values per little-endian u64, least significant first, never straddling
  a word; data id = `M + raw` (int64 wrap). W ∈ {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 12, 16, 21, 32, 64}
  (all 15 seen and decoded).
- Data id → value; NULL ⇔ has NULLs and `id == null_value`:

  | encoding | value |
  |---|---|
  | 1 VALUE_BASED | integer domains `(id + base_id) × magnitude`; float / real `(id + base_id) / magnitude`; ids usually start at 3, NULL id 2 |
  | 2 VALUE_HASH_BASED | ids 3..`last_id` index the primary dictionary at `id − 3`; NULL id 2 |
  | 3 STRING_HASH_BASED | id 2 NULL, **id 3 the empty string** (not in the dictionary), ids 4..`primary.last_id` → primary entry `id − 4`, larger → secondary entry `(id − primary.last_id) − 4`; with no primary, ids index the secondary directly |
  | 4 STORE_BY_VALUE_BASED | `id + base_id`; float / real: the IEEE-754 bits; `null_value` a free id |
  | 5 STRING_STORE_BY_VALUE_BASED | its own body (VLD, below) |

- Canonical value per type (what dictionaries hold and encodings 1 / 2 / 4 produce; encoding 5 and
  delta stores hold the row bytes): E5§3.3's table. The non-obvious rows: `datetime` = `days_since_1900
  × 2^32 + ticks`; `smalldatetime` = `days << 16 | minutes`; `datetime2(p)` = `days_since_0001 << 40 |
  time in 100 ns` for every p; `time(p)` in 100 ns; `datetimeoffset(p ≤ 2)` = `offset_minutes << 48 |
  UTC days << 24 | UTC time in 10 ms`, signed; `datetimeoffset(p ≥ 3)` and `decimal(p > 18)` always
  encoding 5; `real` widened to double; `char` / `nchar` / `binary` stored padded to n; case and
  trailing-space variants are distinct entries.

**Dictionaries.** Non-string (type 1 int, type 4 float): a 48-byte header, `u32 entry size (4 | 8),
u32 count N`, then N entries in hash order; entry k ↔ data id k + 3. String (type 3):

```
+0x00 Dictionary Header           u32 1, u32 3, u32 0
+0x0c String Store Header         u32 4, u32 last index (N-1), u32 8192, u32 2, zeros
+0x38 String Handle Array Header  u32 1, u32 8, u32 N
+0x44 Page Size Array Header      u32 1, u32 4, u32 P
+0x50 N handles (u32 offset, u32 page): offset = byte offset (plain page) or bit offset (Huffman page)
      P x u32 page sizes, then the P pages
```

- Strings are `[len][bytes]`: `len = b0` if `b0 < 0x80`, else `(b0 & 0x7F) | (b1 & 0x80) | ((b1 &
  0x7F) << 8)`.
- Page sub-lob type 5 (plain): `u32 5, u32 0, u32 count, u32 0, u32 0, u32 data size`, the strings.
  Type 6 (Huffman): `u32 6, u32 0, u32 count, u32 blob type, u32 bit count, u32 decoder bit size, u32
  data size`, `u8` character-set code, 128 bytes of 4-bit code lengths (byte values 0..255, low nibble
  first), 3 bytes padding, the bit stream. Canonical Huffman (codes in (length, byte) order, Kraft sum
  exactly 1), bits read from 16-bit little-endian words MSB first (MS-XCA's reader). Blob type 2: the
  `[len][bytes]` strings byte by byte; blob type 1: UTF-16 text whose characters share one high byte
  (the character-set code), only the prefix bytes and low bytes coded.
- `(max)` / `json` entries carry a prefix byte: `0x21` = inline; `0x11` = a 22-byte pointer `11 01 |
  u32 length | 8 bytes | RID` to an ordinary LOB tree whose content is the blob container.
- A full primary dictionary spills into a per-rowgroup secondary one (≈10 MB seen). REBUILD gives
  global dictionaries; bulk load and the tuple mover give local ones.

**Encoding 5 (VLD, lob type 7)**, 2-byte aligned: `u32 1, 1, 0, w3, 7, B, D, R; +32 u16 8` (w3 as in a
segment), B
bookmarks, R narrow RLE entries; `VLD Header (u32 8, u32 S strings, u32 8192)`; `Page Size Array
Header (u32 1, u32 4, u32 P)`, P sizes; pages `u32 9, u8 compression, u8 0, u16 common size, u32
count`: compression 0 raw, 1 `u16 (size − 1)` + MS-XCA. Uncompressed page: fixed width
(`count × common`) or, with common = 0xFFFF, the strings then `count × u16` start offsets **in reverse
order**; 0xFFFE = NULL. RLE value: bits 31..30 kind (2 = consecutive strings from the addressed one,
across pages; 1 = repeat the addressed string), bits 29..15 index in page, bits 14..0 page.

**Rows** (E5§3.7), per partition's columnstore rowset, rowgroups in `segment_id` order:
1. COMPRESSED: tuple t = position t in every segment (VertiPaq reorders rows within a rowgroup, the
   same way in every column); drop t if `(rowgroup, t)` is in the delete bitmap.
2. OPEN / CLOSED: the delta store `ds_hobtid` as a rowstore B-tree; live leaf records, ghosts skipped
   (085's record walk). Deletes there are physical.
3. TOMBSTONE: skipped (its rows are in the rowgroup that replaced it); INVISIBLE (0): skipped
   (presumed, not seen).
4. Delete-bitmap records are CD records: `01`, column count `02`, two CD nibbles, the short data;
   integers big-endian sign-flipped; 4 trailing bytes unexplained and ignored; no page carried a
   compression-info record, even at 226 pages and 120,300 entries.

A CCI on 2025 accepts `json`, `vector` and every `(max)` type, and refuses `sql_variant`, `xml`,
`rowversion`, CLR types, `text`, `ntext`, `image` (error 35343).

## Decisions

- **D1 — clean room, with E5's provenance kept.** The C++ follows E5§3 and its PROVENANCE table
  (E5§8); no columnstore decoder source is opened (Internals Viewer is GPL, R§7). The provenance moves
  next to the code as `src/backup/columnstore/PROVENANCE.md`.

- **D2 — decode into DuckDB vectors per segment, not per value.** Bit-unpacking is a per-width kernel
  over whole words (vectorised); encodings 2 and 3 produce DuckDB dictionary vectors over the decoded
  dictionary (E5§7: "dictionary vectors for 2/3"), so a string column of 1 M rows and 50 k distinct
  values is decoded 50 k times, not 1 M. Encoding 1 / 4 values are an affine map over the data ids.

- **D3 — locators (owner, 2026-11-05; `_mssql_page`'s form 2026-11-06).** A compressed row has no
  page. Its `_mssql_page` holds its **rowgroup key**, in 085 D8's hex `VARCHAR` form: `02` + 16 hex
  digits of rowset id + 8 of rowgroup, 26 characters (a page is `01` + file + page, 14 characters).
  Its `_mssql_rid` is the `BLOB` `0x02 ‖ u64 rowset_id ‖ u32 rowgroup ‖ u32 tuple`. A delta-store row
  has `_mssql_page` = its delta page in the `01` form, and `_mssql_rid` =
  `0x03 ‖ u64 rowset_id ‖ u32 rowgroup ‖` its hidden key (`rscolid 0`). `rowset_id` is
  the columnstore rowset's id; it is in both because rowgroup ids are numbered per partition and one
  lake table holds every partition. A SWITCH keeps rowset ids (E7 s12; E11: an OPEN delta store's rowset
  changes owner with the others, no page moves), so the keys stay valid.

  Rowgroup ids (E11):
  - **Not reused while the rowset lives.** After every row of a rowgroup was deleted, a REORGANIZE, its cleanup
    and a new bulk load, the new rowgroup got id 4 after 0-3. A REBUILD builds new rowsets (new hobt ids),
    numbered from 0.
  - **A TRUNCATE restarts them at 0 in the same rowset**, the table form and `WITH (PARTITIONS …)` alike. The
    columnstore rowset's LOB unit then has another first page and first IAM page, or none, and
    `mssql_backup_schema_changes` reports `table_truncated` for both (E11).
  - So a key is unique between two truncates of its rowset. 089 must apply a `table_truncated` before the new
    state's rows (acceptance 8).
  - **A REBUILD replaces the rowsets, and 085 reports nothing for it** (no schema change: a rebuild keeps a
    rowstore's rows). Every key of the rebuilt partitions changes, so this spec reports a CCI whose columnstore
    rowset was replaced as a `table_reload` of the partition (acceptance 8).

  Reason for the keys:
  R§5's "(rowgroup, tuple), only within a rowgroup — the tuple mover renumbers; handle per rowgroup",
  and DuckLake's own `DELETE` by `_mssql_page` (research-ducklake-import §5): a removed, merged or
  rebuilt rowgroup is deleted by `_mssql_page` like a page; a delete-bitmap entry for tuple t deletes
  `WHERE _mssql_page = <rowgroup key> AND _mssql_rid IN (…)`, the rowgroup key pruning the files. The
  scan drops bitmap-deleted tuples; a tuple deleted before the import is not in the lake and needs
  nothing. No separate rowgroup column: service-column types are decided (DQ-02) — opaque values,
  `_mssql_page` hex `VARCHAR` and the others `BLOB`, no `_mssql_rowgroup`.

- **D4 — units are rowgroups.** One unit per COMPRESSED rowgroup and one per delta store, in
  `segment_id` order; projection reads only the projected columns' segments and dictionaries, which is
  most of a columnstore's I/O saving. Rowgroup elimination from `min_data_id` / `max_data_id` needs
  filter pushdown, which 085 defers; it is task 7's measured decision, because a columnstore is where
  it pays most.

  Resources (085 D17): a unit holds the segment and dictionary buffers of its own rowgroup's projected
  columns only, reserved before they are read (a segment is at most its `on_disk_size`, a dictionary
  up to ~10 MB seen) and freed when the unit ends; a global dictionary that several rowgroups share is
  kept in buffer-managed blocks, pinned while a unit decodes against it. So at most `MaxThreads()`
  (≤ `max_threads`) rowgroups' buffers are held at once, and a unit whose reservation would pass the
  reservations' half of `max_memory` waits; one that cannot fit alone is refused by name (085 D17's
  rule). A delta store is a rowstore unit (085 § Resources).

- **D5 — drift is absorbed, nothing is refused.** The three `syscs*` tables are decoded generically through
  their own `syscolpars` (085 D5), and fields are read by name: `collation_id` and `min_deep_data` /
  `max_deep_data` exist only from 2022 (E5§6.1; E5b: absent on 2019) and are optional.

  A word the decoding does not use is not checked. E5b found segment header word 3, which E5 had called a
  constant, to differ by version and segment, so a check would have refused valid 2019 and 2022 backups.

  What the decoder cannot read makes the values it would have produced NULL, never the table's read (owner,
  2026-11-22: no read refusals; 085's rule for a value). Cases:
  - a segment or dictionary `version` with low bits other than 1 (bit 31 is the container flag, set for
    every ARCHIVE blob and every `(max)` / `json` one);
  - an unknown encoding, page kind, Huffman blob type or VLD compression;
  - a segment that fails an invariant (Design).

  The NULLs are those of the segment's column in its rowgroup. Each case logs one WARNING per column per scan,
  naming the rowgroup and the case.

- **D6 — fuzziness is detected, not repaired, here.** 085's D11 covers a CCI through its rowsets, measured in
  E11:
  - **The setup.** A full backup was held twice (tail_window's FIFO). At the first hold, which the server put
    at 0 %, before the table was copied, deletes and a tuple-mover step (`REORGANIZE …
    COMPRESS_ALL_ROW_GROUPS`) ran. At the second, at 100 %, an insert, a delete and a REORGANIZE ran.
  - **What it showed.** The copy held the state after the first changes, consistent: the state before minus
    exactly the deleted rows, no row twice. The later changes were in the tail only, and D11 called the table `stale` and the heap beside it
    `consistent`.
  - **The policy.** A CCI's status, warning and `mssql_backup_fuzzy` policy are those of any table, and the
    read is never refused for it. Records of the tuple mover or of a compression (new segments,
    `syscsrowgroups` rows changing) must count as changes of the table.

    E11's `stale` may have come from the second hold's inserts and deletes alone. So task 9 checks a tail that
    holds only a REORGANIZE, and adds those records to D11's row operations if they are not counted.
  - **Not caught.** A change during the table's own copy was not caught, and with it a copy holding the
    rowgroups from before a tuple-mover step and its segments or delete bitmap from after it. Such a copy can
    read a row twice or lose one, and D11's status is what tells the importer. Task 9 places a hold within the
    table's pages.

  The principle (owner, 2026-11-28) is to read the utmost a backup holds, under load too; consistency and the
  import rules are the importer's. So nothing here refuses a CCI, and a torn tuple-mover step is read as far
  as it can be. A rowgroup whose segment fails its invariants is a candidate to be read from the TOMBSTONE's
  delta store: E11 found it kept until a REORGANIZE, the same rows. To be checked in task 9, with a hold
  inside the table's pages.

  Recovery is 090's, which treats a CCI as any table (DQ-07 superseded). Its columnstore extension redoes
  delta-store and delete-bitmap records as rowstore, and the tuple mover's records if the tail carries their
  pages as images. Task 9 measures that, as tail_window found for a TABLOCK load.

- **D7 — the differential evidence (E11, measured on 2022).** A full backup and its differential, overlaid in
  restore order, equal the server after each of 21 steps (19 run, 2 refused by the server), so 085's overlay reads a CCI as it is; nothing
  columnstore-specific. What 089 builds on:
  - a bulk insert adds a COMPRESSED rowgroup (its segment LOBs in the differential);
  - a trickle insert, a delta delete and an update change delta-store pages;
  - a delete of compressed rows adds delete-bitmap entries, and so does an update of them (with a delta
    insert). The rowgroup's own rows never change, and `syscsrowgroups` does not move;
  - the tuple mover (REORGANIZE COMPRESS_ALL) makes the delta rowgroup TOMBSTONE, keeping its rowset and
    pages, and adds a COMPRESSED one of its rows. REORGANIZE also merges compressed rowgroups into new ones,
    tombstoning them, and a later one removes TOMBSTONEs, their segments and their delta rowsets;
  - a REBUILD replaces the rowsets, and 085 reports no schema change for it (D3: this spec adds one);
  - a TRUNCATE empties the rowset and restarts rowgroup ids (D3);
  - a SWITCH, an OPEN delta store's included, is catalog-only: no data page in the differential;
  - SPLIT / MERGE are possible only on empty partitions (35346 / 35344) and add or drop empty rowsets.

  So a CCI's changed rows between two states are rowgroups that appeared, disappeared or became TOMBSTONE,
  delete-bitmap entries added or dropped, and delta-store rows as rowstore changes.

## Design

```text
src/backup/columnstore/        namespace duckdb::mssql::backup::columnstore; DuckDB-free unless noted
  cs_catalog.cpp               rowset roles, rowgroups, segments, dictionaries, column mapping
  cs_container.cpp             the blob container (version bit 31, Xpress frames)
  cs_segment.cpp               segment header, narrow / wide RLE, bit-unpack kernels (15 widths)
  cs_dictionary.cpp            non-string and string dictionaries, plain and Huffman pages
  cs_vld.cpp                   encoding 5 pages and addressing
  cs_values.cpp                data id → DuckDB values per type; dictionary vectors        [DuckDB]
  cs_rowgroup.cpp              row assembly: states, delete bitmap, delta stores
  PROVENANCE.md
```

The columnstore path plugs into 085's scan as another unit kind; `mssql_backup_tables` reports
`shape = 'columnstore'`, `compression` = COLUMNSTORE / COLUMNSTORE_ARCHIVE, and per partition the
rowgroups (state, row count, deleted count from the bitmap). Ordered CCI keys (2022+) are reported
for information; the importer does not propose them as a sort order (DQ-25, accepted: only the
clustered rowstore key, since VertiPaq reorders a rowgroup's tuples). The partition metadata is
085's `partitioning` column (E7). A partitioned CCI has, per partition, its own columnstore
(ownertype 1), delete-bitmap (2) and delta-store (3) rowsets with one `numpart`; its compressed rows
are keyed by **rowset**, not by page: `syscsrowgroups` and the segments are keyed by hobt id, and a
SWITCH leaves them unchanged while the columnstore and delete-bitmap rowsets swap `idmajor` /
`numpart` (E7 s12, catalog-only). 089's ownership rule moves a compressed row by the rowset its rowgroup key
names (`02`, D3).

A SWITCH of a partition with an OPEN delta store is catalog-only too (E11). Its delta-store rowset changes
owner and `numpart` with the columnstore and delete-bitmap ones, and no page of it is in the differential. So a
delta-store row keeps its `01` page and moves by its rowset, which the `03` key names. No lake row is on a
delete-bitmap page.

Values go through the reader's codec where the stored form is row bytes: a delta store's records, and
encoding 5's strings and decimals. That is `RecordStager` / `FinalizeStagedColumn`, with 093's kernels and
086 PR 6's / 095's `json`. Encodings 1, 2 and 4 produce the DuckDB value from the data id per type (E5§3.3), in
the column's DuckDB type as `mssql_backup_columns.duckdb_type` reports it (085 D6), the same type a rowstore
table of those columns reads as.

Invariants:
1. Every decoded segment satisfies `48 + 8(B + R + U) = on_disk_size` (unwrapped), and its RLE plus
   bit-packed runs cover exactly `row_count` rows.
2. Every column of a rowgroup yields the same number of tuples.
3. A dictionary id outside its dictionary, a VLD address outside its pages, a Huffman table whose Kraft sum
   is not exactly 1: never a wrong value. The segment's column is NULL in its rowgroup, with a WARNING naming
   the case (D5).
4. A rowgroup's surviving rows = `row_count` minus its delete-bitmap entries.

## Security and hardening

- Every count (B, R, U, N, P, S, frame sizes) is bounded by `on_disk_size` before allocation; bit
  widths outside the 15 are not decoded (D5: the segment's column NULL + WARNING); `~value` bit-pack
  indexes stay within U × (64 / W).
- Container frames: uncompressed ≤ 65,536, compressed ≤ the remaining bytes, total equal to the
  LOB's length.
- Huffman: 4-bit lengths, Kraft sum exactly 1, decoding bounded by the page's count and data size;
  the length prefix is bounded by the page.
- VLD: page and index fields within P and the page's count; reverse offsets monotonic; 0xFFFE only as
  NULL.
- Fuzz harnesses, seeded from E5's blobs (`experiments/E5/blobs/`) and the committed samples:
  `fuzz_backup_cs_segment`, `fuzz_backup_cs_dictionary` (both page kinds), `fuzz_backup_cs_vld`,
  `fuzz_backup_cs_container`, `fuzz_backup_cs_delete_bitmap`. DuckDB-free.

## Testing and acceptance

Fixtures: E5's scripts (`experiments/E5/sql/`) become the generator — round 1 (`cs_int`, `cs_num`,
`cs_dt`, `cs_str`, `cs_str_tm`, `cs_misc`, `cs_rg`, `cs_tomb`, `cs_arch`, `cs_ord`, `cs_dict2`),
round 2 (`e2_dto`, `e2_mag`, `e2_vld`, `e2_runs`, `e2_part`) and round 3 (`e3_del`, `e3_arch`), minus
the NCCI tables.

Added:
- E11's operations, with their full and differential backups: a CLOSED rowgroup (E11's `t_closed`, trace
  flag 634 holding the tuple mover), a TOMBSTONE whose delta store remains, a TRUNCATE that reuses rowgroup
  ids, a SWITCH out and in with an OPEN delta store, and a REBUILD;
- a page-compressed delete-bitmap page, if one can be produced;
- a non-string secondary dictionary with entries;
- the E5§5 cells "not produced on this server": nvarchar, UTF-8 and `(max)` under encoding 5. E5b did not
  produce them on 2019 or 2022 either, so they need values that defeat the dictionary.

Every insert of a fixture runs with `OPTION (MAXDOP 1)`. A parallel `INSERT … WITH (TABLOCK)` splits the rows
between threads, and each share under 102,400 rows lands in its own delta store (E11: 400,000 rows became 12
delta stores).
Generated in the oracle lane; a few small blobs and one small CCI backup committed for the
server-free lane and the fuzz seeds. Canonical comparison as E5's `truth_rows.py` (floats as IEEE
hex, strings and binaries as bytes, dates in styles 23 / 120 / 121), plus the `TYPE mssql` comparison
of 085.

Version matrix: 2017 / 2019 / 2022 / 2025, ordered CCI from 2022, `json` / `vector` in a CCI on 2025 only.
E5§6.1 called the untested versions "the main residual risk".

E5b retired it for 2019 and 2022. Every fixture table read equal, the segment and dictionary versions,
dictionary headers and encodings per type were the same as 2025's, and the only differences were header word 3
and 2019's missing catalog columns (D5). 2017 remains for the oracle lane: its image crashes at start under
Rosetta.

Acceptance:
1. Every fixture table of every version in the matrix equals the server's `SELECT` (E5's canonical
   form and the `TYPE mssql` comparison).
2. Invariants 1–4 hold on every segment of every fixture.
3. Projection reads only the projected columns' blobs (counted).
4. `_mssql_rid` is unique per table, across its partitions, and stable across two backups with no
   change between them; a compressed row's `_mssql_page` is its rowgroup key.
5. Every E11 operation's CCI, read through `diff :=` and as an attached backup with `diff`, equals the
   server after the operation.
6. Fuzz harnesses replay clean under ASan and UBSan.
7. A CCI table of an attached backup reads through `FROM bk.<schema>.<table>`, and its rows equal those of
   `mssql_backup_scan`.
8. `mssql_backup_schema_changes` reports `table_reload` for a CCI partition whose columnstore rowset a REBUILD
   replaced, and `table_truncated` for a TRUNCATE (E11's steps); 089's tests apply them before the rows.

## Tasks

Costs follow E5§7, without its NCCI share.

1. System tables, rowset roles, column mapping, partitions, blob container — 3 d
2. Segment decoder: header, narrow / wide RLE, bit-unpack kernels for the 15 widths, invariant 1 — 3 d
3. Values for encodings 1 / 2 / 4, ~25 types, dictionary vectors — 5 d
4. Dictionaries: non-string; string plain and Huffman (blob types 1 / 2), length prefix, `(max)`
   prefix, off-row values — 5 d
5. VLD (encoding 5) — 3 d
6. Row assembly: states, delete bitmap, delta stores, partitions — 4 d
7. Scan integration: rowgroup units, projection, locators; measure rowgroup elimination with filter
   pushdown and decide — 4 d
8. Oracle matrix (E5's scripts) and the five fuzz harnesses — 6 d
9. E11's remainder (the spike itself was done 2026-11-28: `experiments/E11-columnstore-diff.md`) — 1.5 d:
   - a tail that holds only a REORGANIZE, and D11's count of it (D6);
   - a hold placed within the table's pages, the tuple mover running there (D6);
   - E11 on 2019, so far measured on 2022 only;
   - the REBUILD event (acceptance 8);
   - whether the tail carries the tuple mover's segment pages as images (090's redo), and reading a torn
     rowgroup from its TOMBSTONE's delta store (D6).
10. Docs (README, `DATAMODEL.md` columnstore layer, CLAUDE.md, CHANGELOG) — 1 d

**Total ≈ 35.5 days, 6–8 weeks** (E5§7: ≈ 34 days, 5–8 weeks; plus E11's remainder, minus the NCCI work). R§8
phase 3 gave 5–8 weeks at medium risk after E5.

## Risks

- **Version drift**: 2019 and 2022 measured (E5b), same as 2025 but for D5's two cases; 2017 only in the
  oracle lane. An unknown version is NULL + WARNING (D5), never a refusal.
- **Unobserved states** (E5§6.4, after E11, which produced CLOSED):
  - INVISIBLE rowgroups;
  - raw container frames;
  - VLD compression other than 0 / 1;
  - Huffman blob types other than 1 / 2;
  - dictionary page types other than 5 / 6;
  - a delete-bitmap page with a compression-info record (086's PAGE reader, merged, reads it).

  Each is NULL + WARNING (D5) until a fixture shows it.
- **Constants not understood** (E5§6.3) are not checked (D5). Checking them would have refused valid backups
  (E5b's word 3). A format change shows as an invariant failing or a value the oracle lane does not match.
- **A fuzzy copy that mixes a tuple-mover step's before and after** was not produced (D6); D11's status covers
  it.
- **Order and DQ-07**, from [decisions.md](../085-backup-reader-core/decisions.md):
  - Order: DQ-03's "090 before 088" was superseded by the owner's order of 2026-11-19 (088 before 089 and 090,
    series.md).
  - DQ-07, "a columnstore table whose tail the recovery cannot cover refuses the file", was **superseded on
    2026-11-28** (owner): read the utmost, under load too, and leave consistency and the import rules to the
    importer. A CCI is read, flagged and recovered like any table (D6).

## References

- RESEARCH.md §3 ("Columnstore: FULLY DECODED"), §5 (row identity table), §6.2 (columnstore under a
  diff), §8 phase 3, §10.4.
- E5 §1 (result), §3 (format), §4 (verification), §5 (coverage), §6 (unknowns), §7 (effort), §8
  (provenance); `experiments/E5/cs_decode.py`, `csbak.py`, `truth_rows.py`, `sql/`, `blobs/`.
- E5b (`../085-backup-reader-core/research/experiments/E5b-columnstore-versions.md`): the format on 2019 and
  2022.
- E11 (`../085-backup-reader-core/research/experiments/E11-columnstore-diff.md`): differentials, rowgroup ids,
  SWITCH with an OPEN delta store, the tuple mover during a backup.
- research-ducklake-import.md §5 (columnstore rowgroup keys; delete-bitmap entries deleted by
  `_mssql_page` and `_mssql_rid`).
- 085 (LOB reader, MS-XCA decoder, scan units), 086 (CD records), 089 (incremental), 090 (recovery).
