# Spec 088: columnstore tables

Status: **draft**, 2026-11-05. Not started. Depends on spec 085 (pages, LOB reader, the MS-XCA
decoder) and on a CD integer decoder (086 PR 1, or the minimal one E5 used). Series overview:
`../README.md`. Markers and source abbreviations as in 085.

## Problem

Columnstore is **required** (owner, R§10.4). Spec 085 lists a clustered columnstore table and refuses
its scan. E5 decoded the whole format clean-room: 21 tables in three backups, 3.74 M rows, 168 column
checks, every one equal to the server's `SELECT` as a multiset, byte for byte in canonical form
(E5§1, §4) **(2025)**. This spec turns E5's decoder into the reader's columnstore path.

## Scope

In: clustered columnstore indexes (CCI), `COLUMNSTORE` and `COLUMNSTORE_ARCHIVE`, every rowgroup state,
delete bitmaps, delta stores, partitions, ordered CCI, every type a CCI accepts, `(max)` values in
and off row; the locators the lake needs; the evidence a differential carries (an experiment first).

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
| 3 | delta store, one per OPEN rowgroup (CLOSED: presumed the same, never observed, E5§6.4; a TOMBSTONE's old one may remain) | 0 | B-tree of data records (CCI) |
| 4 | delete buffer (NCCI only) | 0 | not read here |

- `syscsrowgroups` (obj 35): `hobt_id`, `segment_id` (= rowgroup id), `version`, `ds_hobtid` (the delta
  store of an OPEN or TOMBSTONE rowgroup, E5§3.1), `row_count`, `status`, `flags`, … State = `status & 0xFF`:
  1 OPEN, 2 CLOSED, 3 COMPRESSED, 4 TOMBSTONE (matched the DMV on 30 rowgroups).
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
+0   u32 1, u32 1, u32 0, u32 10      constants
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

**Encoding 5 (VLD, lob type 7)**, 2-byte aligned: `u32 1, 1, 0, 10, 7, B, D, R; +32 u16 8`, B
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

- **D3 — locators.** A compressed row has no page. `_mssql_rid` = `0x02 ‖ u32 rowgroup ‖ u32 tuple`; a
  delta-store row `0x03 ‖ u32 rowgroup ‖` its hidden key (`rscolid 0`). No separate rowgroup column (DQ-02, decided 2026-11-05: service
  columns are opaque bytes; the rowgroup is inside `_mssql_rid`, and the page map's runs carry it);
  `_mssql_page` is the delta page for delta rows and NULL for compressed rows. Reason: R§5's
  "(rowgroup, tuple), only within a rowgroup — the tuple mover renumbers; handle per rowgroup", and the
  lake's page map (research-ducklake-import §5). The scan drops bitmap-deleted tuples, so a rowgroup's
  surviving tuples are written in tuple order as runs `(rowgroup, first_tuple, first_pos, count)`, one
  per contiguous range of survivors; a later delete-bitmap entry for tuple t maps to `first_pos + (t −
  first_tuple)` of the run that holds t (089's export writes these runs; a tuple in no run was deleted
  before the export and needs nothing). Service-column types: decided (DQ-02, owner 2026-11-05) — opaque BLOBs, no `_mssql_rowgroup`.

- **D4 — units are rowgroups.** One unit per COMPRESSED rowgroup and one per delta store, in
  `segment_id` order; projection reads only the projected columns' segments and dictionaries, which is
  most of a columnstore's I/O saving. Rowgroup elimination from `min_data_id` / `max_data_id` needs
  filter pushdown, which 085 defers; it is task 7's measured decision, because a columnstore is where
  it pays most.

- **D5 — catalog drift is absorbed by name.** The three `syscs*` tables are decoded generically
  through their own `syscolpars` (085 D5), and fields are read by name: `collation_id` and
  `min_deep_data` / `max_deep_data` exist only from 2022 (E5§6.1) and are optional. A segment or
  dictionary whose `version` has low bits other than 1 is refused by name until the matrix shows it
  (bit 31 is the container flag, set for every ARCHIVE blob and every `(max)` / `json` one).

- **D6 — fuzziness is detected, not repaired, here.** 085's flag (v) covers a CCI through its
  rowsets: tail records on the delta stores or the delete bitmap flag the table. Records of the tuple
  mover or of a compression (new segments, `syscsrowgroups` rows changing) mark the table `dirty` with
  the reason. Spec 090 refuses recovery of columnstore tables by name at first; its listed extension
  (after 086 PR 1 and this spec) redoes delta-store and delete-bitmap records as rowstore, and the tuple
  mover's records stay refused (E5§6.6: never tested).

- **D7 — the differential evidence is measured before 089 uses it.** The expected shape (R§6.2):
  new rowgroups are new segment LOBs, in the file; deletes are new delete-bitmap entries on changed
  pages; the tuple mover appears as a delta store's pages freed plus a new COMPRESSED rowgroup; a
  REBUILD or REORGANIZE tombstones rowgroups and adds new ones. None of it is observed. Spike E11
  (task 9) observes it, and 089's columnstore path is built on what it finds.

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
for the sort order the importer proposes at table creation (R§6.2 "Partitioning ownership"). The
partition metadata is 085's `partitioning` column (E7). A partitioned CCI has, per partition, its own
columnstore (ownertype 1), delete-bitmap (2) and delta-store (3) rowsets with one `numpart`; its rows
are keyed by **rowset**, not by page: `syscsrowgroups` and the segments are keyed by hobt id, and a
SWITCH leaves them unchanged while the columnstore and delete-bitmap rowsets swap `idmajor` /
`numpart` (E7 s12, catalog-only). 089's ownership rule resolves a columnstore row through its
rowset.

Invariants:
1. Every decoded segment satisfies `48 + 8(B + R + U) = on_disk_size` (unwrapped), and its RLE plus
   bit-packed runs cover exactly `row_count` rows.
2. Every column of a rowgroup yields the same number of tuples.
3. A dictionary id outside its dictionary, a VLD address outside its pages, a Huffman table whose
   Kraft sum is not exactly 1: a named error, never a NULL.
4. A rowgroup's surviving rows = `row_count` minus its delete-bitmap entries.

## Security and hardening

- Every count (B, R, U, N, P, S, frame sizes) is bounded by `on_disk_size` before allocation; bit
  widths outside the 15 are refused; `~value` bit-pack indexes stay within U × (64 / W).
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
the NCCI tables. Added: a CLOSED rowgroup (a delta store past 1,048,576 rows before the tuple mover
runs), a page-compressed delete-bitmap page if one can be produced, a non-string secondary dictionary
with entries, and the E5§5 cells "not produced on this server" where another version produces them.
Generated in the oracle lane; a few small blobs and one small CCI backup committed for the
server-free lane and the fuzz seeds. Canonical comparison as E5's `truth_rows.py` (floats as IEEE
hex, strings and binaries as bytes, dates in styles 23 / 120 / 121), plus the `TYPE mssql` comparison
of 085.

Version matrix: 2017 / 2019 / 2022 / 2025. Ordered CCI from 2022, `json` / `vector` in a CCI on 2025
only. E5§6.1: "Only 2025 was tested … the main residual risk"; the matrix is where it is retired.

Acceptance:
1. Every fixture table of every version in the matrix equals the server's `SELECT` (E5's canonical
   form and the `TYPE mssql` comparison).
2. Invariants 1–4 hold on every segment of every fixture.
3. Projection reads only the projected columns' blobs (counted).
4. `_mssql_rid` is unique per table and stable across two backups with no change between them.
5. E11's report states the differential evidence of every case in D7.
6. Fuzz harnesses replay clean under ASan and UBSan.

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
9. **E11 spike**: columnstore in differentials (inserts, deletes, updates, the tuple mover, REORGANIZE,
   REBUILD, TRUNCATE, SPLIT / MERGE of a partitioned CCI — E7 covered only its SWITCH, s12, which is
   catalog-only) and the tuple mover running during a full backup; report
   `experiments/E11-columnstore-diff.md` — 3 d
10. Docs (README, `DATAMODEL.md` columnstore layer, CLAUDE.md, CHANGELOG) — 1 d

**Total ≈ 37 days, 6–9 weeks** (E5§7: ≈ 34 days, 5–8 weeks; plus E11, minus the NCCI work). R§8
phase 3 gave 5–8 weeks at medium risk after E5.

## Risks and open questions

- **Version drift** (E5§6.1): only 2025 was decoded; older builds may lack the Huffman pages or use
  other header constants. The matrix is the gate; D5 refuses an unknown segment version by name.
- **Unobserved states** (E5§6.4): CLOSED and INVISIBLE rowgroups, raw container frames, VLD
  compression other than 0 / 1, Huffman blob types other than 1 / 2, dictionary page types other than
  5 / 6, a delete-bitmap page with a compression-info record (that one needs 086's PAGE reader). Each
  is a named refusal until a fixture shows it.
- **Constants not understood** (E5§6.3) are checked against the observed values and refused when they
  differ, so a format change is loud.
- **The differential and fuzzy behaviour** of columnstore is unmeasured (D6, D7); E11 comes before
  089's columnstore path.
- Open for the owner: the order of 088 relative to 090 (R§10.4; DQ-03, accepted: 090 first); refusing or marking stale a columnstore table whose tail the
  recovery cannot cover (D6 here, 090 D7 against 023 Q4; DQ-07).

## References

- RESEARCH.md §3 ("Columnstore: FULLY DECODED"), §5 (row identity table), §6.2 (columnstore under a
  diff), §8 phase 3, §10.4.
- E5 §1 (result), §3 (format), §4 (verification), §5 (coverage), §6 (unknowns), §7 (effort), §8
  (provenance); `experiments/E5/cs_decode.py`, `csbak.py`, `truth_rows.py`, `sql/`, `blobs/`.
- research-ducklake-import.md §5 (columnstore page runs and delete bitmaps as positions).
- 085 (LOB reader, MS-XCA decoder, scan units), 086 (CD records), 089 (incremental), 090 (recovery).
