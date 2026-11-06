# E5 — Decoding COLUMNSTORE data from a backup, without SQL Server

Date: 2026-11-05. Server: SQL Server 2025 RTM-CU8 (17.0.4075.5), container `mssql-v2test`. Research only
(nothing goes to git). Every statement went through `tools/sql.sh`; docker was used only for `docker ps` and
`docker cp`. Clean room: no code from Internals Viewer or any GPL/AGPL project was looked at; see PROVENANCE.

## 1. Result

**Columnstore is fully decodable from a backup file.** A Python decoder (`E5/cs_decode.py`, ~1100 lines on top
of `tools/pages.py`) reads every column of **21 test tables in three backups — 3.74 million rows, 168 column
checks — and every one equals the server's `SELECT` as a multiset**, byte for byte in a canonical form
(`E5/verify_bk_e5_*.txt`). Covered:

- all five segment encodings (value-based with base/magnitude, value-hash dictionary, string-hash dictionary,
  store-by-value, string store-by-value);
- RLE + bit-packing at every bit width the engine uses;
- NULLs in each encoding;
- primary (global) and secondary (local) dictionaries;
- plain and Huffman-coded string dictionaries;
- the by-value ("VLD") string store with its own LZ77+Huffman pages;
- COLUMNSTORE_ARCHIVE;
- off-row `(max)` values;
- every data type a CCI accepts on 2025 (including `json` bytes and `vector`);
- rowgroup states COMPRESSED / OPEN / TOMBSTONE;
- delete bitmaps (up to 120,300 entries), delta stores, NCCI delete buffers, NCCI locators;
- a partitioned CCI and an ordered CCI.

Only two pieces of the whole format were not fully reverse engineered:

- the meaning of a few constant header words;
- the native `json` binary, which is a separate format shared with rowstore.

A handful of states could not be produced on this server; see §6.

It took four ideas beyond what the earlier experiment saw:

1. RLE runs address the bit-pack array by `~index`.
2. Encoding-4 segments use 16-byte RLE entries.
3. String dictionaries are canonical Huffman over bytes, with an MS-XCA bit reader.
4. By-value string segments are pages of MS-XCA LZ77+Huffman (the same codec as MS_XPRESS backups), addressed
   by (page, index).

## 2. What was built

| Backup (local copy, `E5/bak/`) | Tables | Purpose |
|---|---|---|
| `bk_e5_full.bak` (77.7 MB) | `cs_int` (18 int columns), `cs_num` (decimal/numeric/money/float/real, 19), `cs_dt` (all date/time types, 17), `cs_str` (17 string columns, REBUILD → global dictionaries), `cs_str_tm` (same, bulk path → local dictionaries), `cs_misc` (guid, bit, binary, varbinary(max), json, vector), `cs_rg` (3 compressed rowgroups + deletes + updates + open delta store), `cs_tomb` (TOMBSTONE), `cs_arch` (COLUMNSTORE_ARCHIVE), `cs_ord` (ordered CCI), `cs_dict2` (1.2 M rows, 2 rowgroups, VLD), `nc_heap`, `nc_pk` (NCCI on heap / on clustered PK with deletes, updates, delta) | round 1 |
| `bk_e5_b_full.bak` (29.5 MB) | `e2_dto` (datetimeoffset(0..7), offsets −14:00..+14:00), `e2_mag` (magnitudes, negative bases, wide value ranges), `e2_vld` (by-value nvarchar / UTF-8 / char / decimal(38) / varbinary with NULLs), `e2_runs` (850 k rows, 34 k distinct GUIDs in runs), `e2_part` (2 partitions, deletes, delta), `e2_ncr` (NCCI after REORGANIZE: deletes in bitmap and buffer) | round 2: edges |
| `bk_e5_c_full.bak` (12.7 MB) | `e3_del` (120,300 deleted rows → 226 delete-bitmap pages), `e3_arch` (by-value columns under ARCHIVE) | round 3 |

Scripts: `E5/sql/*.sql` (every batch, in order), `E5/truth_meta.sh` / `truth_meta_b.sh` (live metadata oracle
into `E5/truth/*.psv`), `E5/truth_rows.py` (canonical export of every table from the server),
`E5/csindex.sh` (DBCC CSINDEX via trace flag 3605 → error log → table), `E5/csbak.py` (system tables and blobs
from the backup), `E5/cs_decode.py` (the decoder and verifier). SHA-1 of the backups in `E5/bak/SHA1SUMS`.

## 3. The format, precise enough to implement

All integers are little-endian unless stated. "Data id" = the integer a segment stores per row.

### 3.1 Finding the pieces (backup catalog)

- **Rowsets** (`sysrowsets`, decoded generically from its own `syscolpars`/`sysrscols`): a columnstore index
  (`idmajor` = object, `idminor` = index id, `numpart` = partition) owns:

  | `ownertype` | rowset | `cmprlevel` | layout |
  |---|---|---|---|
  | 1 | the columnstore itself | 3 COLUMNSTORE, 4 COLUMNSTORE_ARCHIVE | no pages of its own; segments and dictionaries live in its LOB allocation unit |
  | 2 | delete bitmap | 2 (PAGE) | 2 int columns: (rowgroup id, tuple id); CD-format records (§3.9) |
  | 3 | delta store, one per OPEN/CLOSED rowgroup (a TOMBSTONE's old one may still exist) | 0 | B-tree; CCI: data records; NCCI: index records |
  | 4 | delete buffer (NCCI only; two of them) | 0 | (locator, bigint) |

- **Rowgroups**: `syscsrowgroups` (obj 35) columns: `hobt_id, segment_id` (= rowgroup id), `version`,
  `ds_hobtid` (delta-store rowset of an OPEN/TOMBSTONE rowgroup), `row_count` (0 for a delta rowgroup),
  `status`, `flags`, `compressed_reason`, `generation`, `created_time`, `closed_time`, `container_id`,
  `blob_id`, `metadata_offset`, `metadata_size`. **`status & 0xFF` = state**:
  - 1 OPEN
  - 2 CLOSED
  - 3 COMPRESSED
  - 4 TOMBSTONE

  This matches `sys.dm_db_column_store_row_group_physical_stats` for all 30 rowgroups. Deleted-row counts are
  not stored here; they come from the delete bitmap.
- **Segments**: `syscscolsegments` (obj 62): `hobt_id, column_id, segment_id, version, encoding_type,
  row_count, status, base_id, magnitude (float), primary_dictionary_id, secondary_dictionary_id, min_data_id,
  max_data_id, null_value, on_disk_size, data_ptr binary(16), container_id, bloom_filter_md,
  bloom_filter_data_ptr, collation_id, min_deep_data, max_deep_data`.
  - `status` bit 0 = has_nulls (172 of 172 segments).
  - `column_id` here = **hbcolid** = the view's `column_id + 1` (DBCC CSINDEX calls it "HoBt column id").
- **Dictionaries**: `syscsdictionaries` (obj 63): `hobt_id, column_id (hbcolid), dictionary_id, version, type,
  flags, last_id, entry_count, on_disk_size, data_ptr, container_id`.
- **`data_ptr`** = u64 blob id + RID (u32 page, u16 file, u16 slot) of an ordinary LOB root (type-5
  LARGE_ROOT); `pages.read_lob` returns exactly `on_disk_size` bytes for all 279 blobs of the three backups.
- **Column mapping**: `sysrscols` of the columnstore rowset.
  - CCI: `rscolid 0` is a hidden int with no segment. Table column *k* has `rscolid` *k* and `hbcolid` *k+1*.
  - NCCI: `rscolid` 1..n are the index columns; `sysiscols` maps `subid` → table column (`intprop`). Then come
    the **locator** columns: a binary(8) heap RID, or the clustering key columns. Their `hbcolid` comes first
    (1), and they are segments like any other.
  - Types come from `syscolpars`; the locator's type comes from the `ti` of `sysrscols`.
- **Blob container**: `version` bit 31 set (seen as `-2147483647`) = the blob is wrapped. It is wrapped for
  every segment and dictionary of a COLUMNSTORE_ARCHIVE index, and also for the `(max)` and `json` columns of
  ordinary CCIs. The wrapped form is `u32 0`, then frames `(u32 uncompressed size ≤ 65536, u32 compressed size,
  MS-XCA LZ77+Huffman bytes)` until the end. Frames are concatenated. A frame with compressed = uncompressed
  is taken raw (assumed; not observed). The low bits of `version` are 1 everywhere.

### 3.2 Segment, encodings 1–4 ("lob type 3")

```
+0   u32 1, u32 1, u32 0, u32 10            constants (meaning unknown)
+16  u32 3                                   lob type
+20  u32 B   bookmark count  = floor(rows / D) + 1
+24  u32 D   bookmark distance (64, 256, 512, 16384 seen)
+28  u32 R   RLE array size in 8-byte units
+32  u16 8,  u16 W   bit-pack width (0..64)
+36  u32 U   bit-pack 64-bit words
+40  i64 M   bit-pack MinId
+48  B x 8   bookmarks (seek aids; not needed to decode)
     R x 8   RLE array
     U x 8   bit-pack array          total = 48 + 8(B + R + U) = on_disk_size (unwrapped)
```

- **RLE array**: entries in row order, ending in an all-zero entry.
  - **Narrow** (encodings 1, 2, 3): `(u32 value, u32 count)`. If bit 31 of value is set, this is a
    *bit-packed run* of `count` values starting at bit-pack index `~value` (0xFFFFFFFF = index 0,
    0xFFFFEC77 = index 5000). Otherwise the next `count` rows have data id `value`.
  - **Wide** (encoding 4, whose data ids are 64-bit): two units per entry, `(u64 value, u32 count, u32 flag)`.
    `flag` = 1 means a bit-packed run at index `~value`; this is needed because −1 is a legal value.
    `flag` = 0 means `count` rows of data id `value`.
  - Bit-packed runs consume the array contiguously (checked on every segment).
  - The previous experiment's "pairs repeated 18 times" were the bookmarks.
- **Bit-pack array**: `floor(64/W)` values per little-endian u64, least significant first, never straddling a
  word. Data id = `M + raw` (int64 wrap). W is always one of
  **1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 12, 16, 21, 32, 64**: for each count of values per word, the engine takes
  the widest width that fits that many (21 where 18 would do). All 15 occurred and decoded.
- **Data id → value**, where NULL ⇔ `has_nulls` and `id == null_value`:

  | encoding | meaning (MS docs) | value |
  |---|---|---|
  | 1 VALUE_BASED | non-string, no dictionary | integer domains: `canonical = (id + base_id) × magnitude` (magnitude = 10^k, k ≥ 0: values divisible by it). FLOAT/REAL: `value = (id + base_id) / magnitude` (the value was *multiplied* by 10^k to become an integer: −0.5·i → magnitude 10, 0.25·i → 100). Usually `base_id = min/magnitude − 3`, so ids start at 3; NULL id = 2 |
  | 2 VALUE_HASH_BASED | non-string, dictionary | ids 3..`last_id` index the primary dictionary at `id − 3`; NULL id = 2 |
  | 3 STRING_HASH_BASED | string/binary, dictionary | id 2 = NULL, **id 3 = empty string** (not in the dictionary), ids 4..`primary.last_id` → primary entry `id − 4`; larger ids → secondary (local) dictionary entry `(id − primary.last_id) − 4`; with no primary, ids index the secondary directly |
  | 4 STORE_BY_VALUE_BASED | non-string, no dictionary | `canonical = id + base_id` (base 0, or = min once); FLOAT/REAL: canonical = IEEE-754 bits; `null_value` is a free id (e.g. min − 1, or 0) |
  | 5 STRING_STORE_BY_VALUE_BASED | string/binary, no dictionary | different body, §3.6 |

### 3.3 Canonical value per type

This is what dictionaries hold and encodings 1/2/4 produce. Encoding 5 and delta stores hold the on-disk row
bytes instead.

| SQL type | canonical integer / value | encoding-5 / delta-store bytes |
|---|---|---|
| tinyint, smallint, int, bigint, bit | the integer | row format |
| decimal/numeric, precision ≤ 18 | unscaled integer (value × 10^scale) | — |
| decimal/numeric, precision > 18 | (always encoding 5) | sign byte (1 = +), magnitude little-endian |
| money, smallmoney | integer in 1/10000 | row format |
| float | value (enc 1), IEEE-754 bits as int64 (enc 2/4; dictionary type 4 holds doubles) | 8-byte double |
| real | as float, **widened to double** | 4-byte float |
| date | days since 0001-01-01 | 3 bytes |
| datetime | `days_since_1900 · 2^32 + ticks(1/300 s)` (days negative before 1900) | ticks u32, days i32 |
| smalldatetime | `days << 16 \| minutes` | u16 minutes, u16 days |
| datetime2(p), every p | `days_since_0001 << 40 \| time in 100 ns` | time (3–5 B, 10^−p s), 3-byte date |
| time(p) | time in 100 ns (enc 1 magnitude = 10^(7−p)) | 3–5 bytes |
| datetimeoffset(p ≤ 2) | `offset_minutes << 48 \| UTC days << 24 \| UTC time in 10 ms (24 bits)`, signed | — |
| datetimeoffset(p ≥ 3) | (always encoding 5) | UTC time (3–5 B), 3-byte date, i16 offset minutes |
| uniqueidentifier | string family: enc 3 or 5, the 16 on-disk bytes | 16 bytes |
| char/varchar, nchar/nvarchar, binary/varbinary | string family: the stored bytes (code page, UTF-8 or UTF-16LE). char/nchar/binary are **stored padded** to n. Case and trailing-space variants are distinct entries | as stored |
| varchar/nvarchar/varbinary(max), json | string family; each value prefixed (§3.5) | in-row or LOB pointer |
| vector(n) (2025) | encoding 5 seen: `a9 01, u16 dims, u32 0, dims × float32` | same |
| json (2025) | the native json binary, same as rowstore (decoding it is a separate item) | same |

A CCI on 2025 accepts `json`, `vector`, all `(max)` types. It refuses `sql_variant`, `xml`, `rowversion`, CLR
types (geometry, geography, hierarchyid), `text`, `ntext` and `image` (error 35343).

### 3.4 Non-string dictionaries (type 1 int, type 4 float)

```
+0  u32 1, u32 2, u32 0, u32 2, u32 64, u32 0, u32 (6 | 3), u32 (8 | 16), u64 0, i32 -1, u32 1
+48 u32 entry size (4 | 8), u32 count N, then N entries: int32 / int64 canonical, or double
```

Entries are in hash order, not sorted. Entry *k* ↔ data id *k + 3*.

### 3.5 String dictionaries (type 3)

DBCC CSINDEX's names are given in quotes.

```
+0x00 'Dictionary Header'          u32 1 (version), u32 3 (lob type), u32 0
+0x0c 'String Store Header'        u32 4 (sub-lob type), u32 last index (N-1), u32 8192 (max string size), u32 2, zeros
+0x38 'String Handle Array Header' u32 1, u32 8 (element size), u32 N
+0x44 'Page Size Array Header'     u32 1, u32 4 (element size), u32 P (pages)
+0x50 N handles: (u32 offset, u32 page)   offset = BYTE offset in a plain page, BIT offset in a Huffman page
      P x u32 page sizes, then the P pages back to back
```

Every string is length-prefixed, `[len][bytes]`, with `len` the byte count:
- if `b0 < 0x80`, `len = b0`;
- otherwise `len = (b0 & 0x7F) | (b1 & 0x80) | ((b1 & 0x7F) << 8)` (two bytes; checked against true lengths up
  to 1098).

**Pages**:
- **sub-lob type 5, plain**: `u32 5, u32 flags 0, u32 count, u32 0, u32 0, u32 data size`, then the strings.
  Used for small dictionaries.
- **sub-lob type 6, Huffman**:
  - Header: `u32 6, u32 flags 0, u32 count, u32 Huffman blob type, u32 bit count, u32 decoder bit size
    (lookup width; not needed), u32 data size`.
  - Then `u8 character set code`, 128 bytes of 4-bit code lengths for byte values 0..255 (low nibble first),
    3 bytes padding (always 0), then the bit stream.
  - The code is a canonical Huffman code: codes assigned in (length, byte value) order, Kraft sum exactly 1.
    Bits are read from 16-bit little-endian words, most significant bit first (the MS-XCA bit reader).
  - **Blob type 2**: the `[len][bytes]` strings are coded byte by byte.
  - **Blob type 1**: UTF-16 text whose characters all share one high byte = the character set code (0x30 for
    U+30xx kana). The prefix bytes and only the low byte of each character are coded.
  - Pages hold up to ~315 KB. A 10 MB dictionary had 33 pages.

**`(max)` / json entries** (also in encoding 5) carry one prefix byte:
- `0x21`: the value follows inline.
- `0x11`: a 22-byte pointer `11 01 | u32 length | 8 bytes (id) | RID (u32 page, u16 file, u16 slot)` to an
  ordinary LOB tree in the index's LOB allocation unit. The tree's content is the §3.1 container (Xpress
  frames), so a 10,000-byte value took 275 bytes. Values over 8000 bytes went off-row.

When a dictionary fills, the engine adds a secondary dictionary for that rowgroup. Here it happened at ≈10 MB:
20,870 primary + 29,130 secondary entries. REBUILD produced global (primary) dictionaries. Bulk-load and
tuple-mover rowgroups had only local (secondary, id 1) dictionaries.

### 3.6 Encoding 5: string store by value ("VLD", lob type 7)

The structure is 2-byte aligned:

```
+0   u32 1, 1, 0, 10, 7 (lob type), B (bookmarks), D, R (RLE units); +32 u16 8
+34  B x 8 bookmarks, then R x (u32 value, u32 count) RLE entries (ending 0,0)
     'VLD Header'             u32 8, u32 string count S, u32 8192 (max string size)
     'Page Size Array Header' u32 1, u32 4, u32 P, P x u32 page sizes
     pages: u32 9, u8 compression, u8 flags 0, u16 common size, u32 string count, then
       compression 0: raw bytes;  compression 1: u16 (uncompressed size - 1), MS-XCA LZ77+Huffman stream
```

- **Uncompressed page**:
  - If `common size != 0xFFFF` (fixed-width values): `count × common` bytes.
  - Otherwise: the strings back to back, then `count × u16` start offsets **in reverse order** (the last u16
    is string 0). Offset `0xFFFE` = NULL, and a string ends where the next non-NULL one starts.
  - Pages hold ≤ 64 KB uncompressed (3,449–6,553 values).
- **RLE value** in encoding 5:
  - bits 31..30: kind. 2 = the next `count` rows are consecutive strings starting at the addressed one,
    continuing across pages. 1 = the next `count` rows all hold the addressed string.
  - bits 29..15: index inside the page.
  - bits 14..0: page number.
  - Example: `0x4359000e` = string 1714 of page 14 = a NULL (S = 50002).
  - The repeated strings (runs, NULLs) are stored once, usually first.
- **Values** are the on-disk bytes of §3.3. A NULL is a NULL string (offset 0xFFFE) referenced by a kind-1
  run. `null_value` is 0.
- **Which encoding is chosen**:
  - by value: decimal(>18), datetimeoffset(≥3), heap RIDs, and usually uniqueidentifier / binary / vector;
  - strings: only at a very high distinct ratio (e.g. 524 k distinct in 1 M rows);
  - otherwise the column gets a dictionary.

### 3.7 Rows: assembling a table

Per index and per partition (the columnstore rowset with that `numpart`):

1. Take the rowgroups of `syscsrowgroups` with `hobt_id` = the columnstore rowset, in `segment_id` order.
2. **COMPRESSED** (state 3): decode every segment of the rowgroup. Row *t* (the **tuple id**, 0-based) is
   position *t* in every segment. VertiPaq reorders rows inside a rowgroup, but the same way in all columns.
   - **Drop** tuple *t* if `(rowgroup, t)` is in the **delete bitmap** (ownertype 2).
   - For an NCCI, also drop it if its locator value is in a **delete buffer** (ownertype 4). Delete-buffer
     rows are `(locator, bigint)`; the locator is the heap RID (8 bytes) or the clustering key, compared with
     the NCCI's locator column.
3. **OPEN / CLOSED** (1, 2): read the delta store `ds_hobtid` as a rowstore B-tree. Its `sysrscols` carry the
   same `hbcolid`s; `rscolid 0` is the hidden key. Use the live leaf records and skip ghosts.
   - CCI delta stores: DATA pages with FixedVar data records.
   - NCCI delta stores: INDEX pages with **index records** (status byte 0x36, fixed data from offset 1, then
     column count / null bitmap / variable part). `sysrscols` offsets index the record directly in both cases.
   - Deletes of delta rows are physical; nothing to subtract.
4. **TOMBSTONE** (4): skip. Its rows live in the compressed rowgroup that replaced it, even though its old
   delta store may still have pages. INVISIBLE (0) rowgroups are presumably to be skipped too (not seen).
5. **Delete-bitmap records** are ROW-compressed (CD) records:
   - `01`, column count `02`, CD array of two nibbles, then the short data.
   - Integers are big-endian with the sign bit flipped (`83 46` → 838). CD code 1 = EMPTY = 0; codes 2..9 = 1..8
     bytes.
   - 4 more bytes per record are unexplained and ignored.
   - No page carried a compression-info (anchor/dictionary) record, even with 226 pages and 120,300 rows.

Updates are delete-bitmap entries plus delta rows. REORGANIZE moves an NCCI's delete buffer into its bitmap.
`e2_ncr` checks both at once: 120 deletes in the bitmap, 120 in the buffer.

## 4. Verification (decoded from the backup vs. the server's SELECT, as multisets)

| table | rows | columns | state exercised | result |
|---|---:|---:|---|---|
| cs_int | 50,000 | 18 | enc 1/2/4, NULL runs, min/max bigint, all-NULL, constant | ✓ |
| cs_num | 50,000 | 19 | decimal ≤18 (1/2/4), decimal >18 (5), money, smallmoney, float 1/2/4, real 2/4, ±1e300 | ✓ |
| cs_dt | 50,000 | 17 | every date/time type, pre-1900 datetime, 0001/9999, dto(0) and dto(7) | ✓ |
| cs_str | 50,000 | 17 | plain + Huffman (type 1 and 2) dictionaries, primary + secondary, UTF-8, cp1251, char/nchar, '', NULL, case/space variants, (max) in-row and off-row | ✓ |
| cs_str_tm | 30,000 | 17 | the same through local dictionaries | ✓ |
| cs_misc | 20,000 | 11 | guid (3, 5), bit, binary, varbinary, varbinary(max) off-row, json bytes, vector | ✓ |
| cs_rg | 330,660 | 3 | 3 compressed rowgroups, 363 bitmap deletes (deletes + updates), open delta | ✓ |
| cs_tomb | 3,001 | 2 | TOMBSTONE + REORG-compressed + open delta | ✓ |
| cs_arch | 120,000 | 7 | COLUMNSTORE_ARCHIVE, multi-frame container | ✓ |
| cs_ord | 200,000 | 3 | ordered CCI (`ORDER (k)`) | ✓ |
| cs_dict2 | 1,200,000 | 4 | 2 rowgroups sharing a global dictionary; 95-page VLD with 50 repeated + 473,576 unique strings | ✓ |
| nc_heap (NCCI) | 120,380 | 3 + RID | delete buffer by RID, delta index records | ✓ |
| nc_pk (NCCI) | 119,880 | 2 + key | delete buffer by clustering key | ✓ |
| e2_dto | 40,000 | 10 | datetimeoffset(0..7): integer form ≤2, VLD ≥3, negative offsets, NULL runs | ✓ |
| e2_mag | 40,000 | 13 | magnitudes 10/100/1000/10^4/10^6, float magnitude, negative bases, a 36-bit-range bigint (store by value, 64-bit packing) | ✓ |
| e2_vld | 60,000 | 7 | VLD char(20), decimal(38) with NULLs in two pages; nvarchar/UTF-8 dictionaries | ✓ |
| e2_runs | 850,000 | 2 | 34,000-entry GUID dictionary in runs | ✓ |
| e2_part | 59,942 | 2 | 2 partitions, per-partition bitmap + delta | ✓ |
| e2_ncr (NCCI) | 119,760 | 3 + RID | deletes split between bitmap and buffer | ✓ |
| e3_del | 180,000 | 3 | 120,300-row delete bitmap on 226 pages | ✓ |
| e3_arch | 50,000 | 5 | VLD (guid, decimal(38), dto(7), varbinary) under ARCHIVE | ✓ |

The canonical form is defined in `truth_rows.py`:
- floats as IEEE hex;
- strings and binaries as their stored bytes (SHA-256 when over 64 bytes);
- dates in styles 23 / 120 / 121.

`json` values were checked through a shape-specific reader of this test's objects only (§6). Pure-Python run
time is about 20 s for round 1 (2.3 M rows).

## 5. Coverage: type × encoding

Counts are segments decoded and verified; "A" = inside the compressed container (ARCHIVE or `(max)`).

| type | 1 value | 2 value-dict | 3 string-dict | 4 by value | 5 VLD | notes |
|---|---|---|---|---|---|---|
| tinyint | | 2 ✓ | | | | |
| smallint | 1 ✓ | 1 ✓ | | | | |
| int | 26 + 2A ✓ | 12 ✓ | | | | NCCI clustering-key locator 1 ✓ |
| bigint | 5 ✓ | 1 ✓ | | 5 + 1A ✓ | | min/max int64 |
| bit | | 2 ✓ | | | | |
| decimal/numeric ≤ 18 | 5 + 1A ✓ | 2 ✓ | | 1 ✓ | | magnitudes up to 10^6 |
| decimal > 18 | | | | | 3 + 1A ✓ | |
| money, smallmoney | 3 ✓ | 2 ✓ | | | | |
| float | 2 + 1A ✓ | 3 ✓ | | 2 ✓ | | dictionary type 4 |
| real | | 2 ✓ | | 1 ✓ | | |
| date | 3 ✓ | 2 ✓ | | | | |
| datetime | 1 ✓ | | | 1 ✓ | | |
| smalldatetime | 1 ✓ | | | | | |
| datetime2 | | 1 ✓ | | 4 + 1A ✓ | | scales 0/3/7 |
| time | 2 ✓ | 1 ✓ | | 1 ✓ | | scales 0/3/7 |
| datetimeoffset(0–2) | 1 ✓ | 1 ✓ | | 3 ✓ | | |
| datetimeoffset(3–7) | | | | | 6 + 1A ✓ | |
| uniqueidentifier | | | 2 ✓ | | 1 + 1A ✓ | |
| char, nchar | | | 4 ✓ | | 1 ✓ | stored padded |
| varchar | | | 33 + 1A ✓ | | 2 ✓ | code page, UTF-8 |
| nvarchar | | | 7 + 1A ✓ | | | Huffman type 1 and 2 |
| binary, varbinary | | | 2 ✓ | | 2 + 1A ✓ | heap-RID locator 2 ✓ |
| varchar/nvarchar/varbinary(max) | | | 6A ✓ | | | in-row and off-row (compressed LOB) |
| vector | | | | | 1 ✓ | |
| json | | | 1A partial | | | bytes complete; text needs the json-binary decoder |

Bit widths seen: all 15 the engine uses. RLE seen: narrow and wide entries, value runs and bit-packed runs.
VLD pages seen: compression 0 and 1, fixed and variable width.

Not produced on this server:
- nvarchar, UTF-8 varchar, `(max)` types and json under encoding 5;
- a non-string secondary dictionary with entries;
- RLE value runs in a 64-bit segment other than a NULL run.

All of these use the structures above.

## 6. Remaining unknowns and limits

1. **Other SQL Server versions.** Only 2025 was tested; `version` = 1 in every segment and dictionary. Run the
   same scripts on 2016 / 2017 / 2019 / 2022: catalog columns differ (`collation_id` and `min/max_deep_data`
   only exist from 2022), and older builds may lack the Huffman pages. This is the main residual risk.
2. **Native `json`** in a CCI is the same binary as rowstore. The bytes come out complete, but a text decoder
   is a separate work item. The checker in `cs_decode.json_text` reads only this test's two-key objects.
   Observed so far: strings and keys stored as `(len << 1) | 1`; integers as u32 with bit 31 set.
3. **Meaning of constants**:
   - segment header words 1, 1, 0, 10;
   - non-string dictionary header words (6 | 3, 8 | 16, 2, 64);
   - the `u32 2` in the string-store header;
   - the 4 trailing bytes of delete-bitmap CD records;
   - the delete-buffer bigint;
   - the `syscsrowgroups` flags.

   None is needed to decode.
4. **Not observed:**
   - CLOSED rowgroups (structurally a delta store, read the same way) and INVISIBLE ones;
   - container frames stored raw;
   - VLD page compression other than 0 / 1;
   - Huffman blob types other than 1 / 2;
   - dictionary page sub-lob types other than 5 / 6;
   - delete-bitmap pages with a PAGE-compression info record. If one appears, the general PAGE-compression
     reader (planned for phase 1) is needed;
   - composite clustering keys as NCCI locators;
   - filtered NCCIs;
   - the mapping index of a CCI with nonclustered B-tree indexes (not needed to read the CCI);
   - bloom filters (2025 columns; zero here).
5. **VLD addressing limits**: 15 bits each for page number and in-page index were inferred from one non-zero
   page number (14). A segment with more than 32,767 pages is impossible at ≤ 1 M rows, so the inference is
   safe.
6. **Fuzzy backups**: the tuple mover running during BACKUP (a rowgroup compressed while its delta store is
   copied) was not tested. It falls under E3 / the log-replay phase.
7. Compressed backups (MS_XPRESS / ZSTD) of columnstore databases were not re-tested; the container works at
   page level, so the risk is low.

## 7. Effort for the C++ implementation

This assumes phase 0/1 exist: page store, catalog bootstrap, FixedVar and CD rows, LOB reader, and an MS-XCA
decoder (needed for MS_XPRESS backups anyway).

| item | days |
|---|---:|
| columnstore system tables, rowset roles, column mapping (CCI / NCCI / partitions), blob container | 3 |
| segment decoder: header, narrow/wide RLE, bit-unpack (all 15 widths, vectorised), validation | 3 |
| value reconstruction, encodings 1/2/4 → DuckDB vectors for ~25 types (dictionary vectors for 2/3) | 5 |
| numeric dictionaries; string dictionaries (plain, table-driven Huffman, blob types 1/2, length prefix, `(max)` prefix, off-row LOB) | 5 |
| VLD (encoding 5): pages, per-page MS-XCA, fixed/variable/NULL, page/index addressing | 3 |
| row assembly: rowgroup states, delete bitmap, delta stores (data + index records), delete buffers, locators, partitions | 5 |
| scan integration: per-rowgroup parallelism, column projection, rowgroup elimination from min/max data ids, filters | 4 |
| oracle test matrix (2016–2025 containers, scripts reused from `E5/sql`) and fuzz corpora for segment / dictionary / VLD parsers | 6 |
| **total** | **≈ 34 days ≈ 7 weeks** (range 5–8) |

This replaces the 6–10 weeks / high risk of RESEARCH.md phase 3 with 5–8 weeks / medium risk. The remaining
risk is version drift (§6.1).

## 8. PROVENANCE

Sources:
- **E** = our experiments on bk_e5 / bk_e5b / bk_e5c (backup bytes compared with the live server);
- **D** = DBCC CSINDEX / DBCC PAGE output on the live server (Microsoft's own read-only dump, used as an
  oracle for field names and boundaries);
- **M** = Microsoft documentation (learn.microsoft.com, fetched 2026-11-05: sys.column_store_segments,
  sys.column_store_dictionaries; sys.internal_partitions; [MS-XCA] for LZ77+Huffman);
- **P** = Larson et al., "SQL Server Column Store Indexes" (SIGMOD 2011) and "Enhancements to SQL Server
  Column Stores" (SIGMOD 2013), for concepts only: value encoding with base and exponent, primary/secondary
  dictionaries, RLE plus bit packing, row reordering, delete bitmap, delta stores, archival compression.

No source code of any columnstore decoder (Internals Viewer or other) was opened.

| fact | source |
|---|---|
| `syscsrowgroups` / `syscscolsegments` / `syscsdictionaries` column lists | E (their own `syscolpars` rows in the backup) |
| hbcolid = view `column_id` + 1; hidden rscolid 0; NCCI locator columns | E (backup `sysrscols` vs `truth/segments.psv`), D ("HoBt column id") |
| `sysrowsets.ownertype` 1/2/3/4 roles | E (rowset ids matched `sys.internal_partitions` in `truth/internal_partitions.psv`), M (internal object types) |
| rowgroup state = `status & 0xFF` | E (matched `dm_db_column_store_row_group_physical_stats` for 30 rowgroups) |
| `status` bit 0 = has_nulls | E (172 segments vs the view) |
| data_ptr → LOB root, blob length = on_disk_size | E (279 blobs) |
| version bit 31 = Xpress frame container; frame layout | E (xpress_huff.py decoded every frame; sizes add up to the blob) |
| segment header layout, bookmarks, RLE "native units", bit-pack header | D (field names: RLE header, Bookmark header, Bitpack header) + E (byte offsets, sizes sum to on_disk_size) |
| bit-packed run value = ~index | E (DBCC "Bitpack Array Index = 5000" vs bytes `77 ec ff ff`) |
| wide RLE entries with flag for encoding 4 | E + D ("RLE Array Count (In terms of Native Units) = 4" for 2 entries) |
| bit packing LSB-first, floor(64/W) per word; the 15 widths | E (all segments; the width set derived from the observed widths) |
| encoding_type names 1–5 | M; their exact value semantics: E |
| base_id / magnitude formulas, float magnitude multiplies | P (concept) + E (fits on all segments, e.g. −0.5·i → magnitude 10) |
| reserved data ids (2 NULL, 3 empty string / first entry) | E (min_data_id, null_value, dictionary last_id) |
| canonical forms of every type (datetime2 `<<40`, dto `<<48/<<24`, datetime `<<32`, …) | E (min/max_data_id of the view decoded by hand, then whole columns) |
| dictionary type 1/3/4 meanings | M; layouts: E |
| string dictionary structure names | D (Dictionary / String Store / String Handle Array / Page Size Array headers, page sub-lob types 5/6, "Huffman Blob Type", "Bit Count", "Decoder Bit Size", "Character Set Code") |
| Huffman: 4-bit lengths, canonical codes, MS-XCA bit order | E (Kraft sum = 1 exactly; decoding produced `v16927_737`, consistent with the generator) + M (MS-XCA bit reader) |
| 2-byte length prefix rule | E (true lengths of 8 entries recovered from the data, then all 50,000 entries) |
| blob type 1 = UTF-16 with a shared high byte | E (table byte 0 = 0x30, Kraft = 1 without it, decoded entries matched the generator) + D (name "Character Set Code") |
| `(max)` prefix 0x21 / 0x11 and off-row pointer; off-row LOB in the container | E |
| VLD structure names | D (VLD Header, Page Size Array, VLD Page Header: Compression / Common Size / String Count) |
| VLD page compression 1 = u16 (size − 1) + MS-XCA LZ77+Huffman | E (Kraft = 1.0 for a 256-byte table at page offset 14; decompressed to the id order) + M (MS-XCA) |
| VLD offsets reversed, 0xFFFE = NULL | E |
| VLD RLE (page, index) addressing | E (`0x4359000e` → page 14 index 1714 = the second NULL; page 14 holds 1715 strings) |
| delete bitmap = (rowgroup, tuple), CD integers | D (DBCC PAGE: "CD array entry", "EMPTY", "TWO_BYTE_SHORT", decoded column values) + E |
| delta stores: data records (CCI), index records (NCCI) | E |
| delete buffer = (locator, bigint), applied by locator | E (counts and multiset equality) |
| TOMBSTONE skipped | M (state meaning) + E (multiset equality) |
| CCI accepts json/vector on 2025; refuses sql_variant, xml, rowversion, CLR, LOB-legacy | E (CREATE TABLE probes, error 35343) |
| global dictionaries from REBUILD, local ones from bulk / tuple mover; secondary at ≈10 MB | E, P (concept) |

## 9. Side findings (tools, extension)

- `mssql_scan` raises a DuckDB **InternalException** ("Table function must return at least one column")
  instead of a clean error when a statement describes as zero columns: `DBCC … WITH TABLERESULTS` that only
  prints messages, `EXEC xp_readerrorlog`. Worth an issue.
- A multi-column `INSERT … SELECT` into `cs_misc` (json + vector casts) made `mssql_exec` return 0 rows with no
  error. The same values went in through `UPDATE`. The error appears to be swallowed; worth a look.
- `mssql_exec`'s 30 s query timeout cut a 1.2 M-row load; loads were chunked.
- `DBCC CSINDEX` output was routed through trace flag 3605 (session-scoped). Its text therefore remains in the
  server's current error log, which other experiments share. Nothing else was left behind: databases
  bk_e5 / bk_e5b / bk_e5c were dropped, and `xp_delete_files` removed `/var/opt/mssql/data/bk_e5_*`.

## 10. Files (all under `experiments/`)

| path | what |
|---|---|
| `E5/cs_decode.py` | the decoder: `tables`, `segment`, `dict`, `rows`, `verify` |
| `E5/csbak.py` | backup → columnstore system tables and blobs (`systables`, `dump`) |
| `E5/truth_rows.py`, `E5/truth_meta.sh`, `E5/truth_meta_b.sh`, `E5/csindex.sh` | oracle capture |
| `E5/sql/` | every T-SQL batch run |
| `E5/bak/` | the three backups + SHA1SUMS |
| `E5/truth/` | live metadata, canonical row exports, DBCC CSINDEX / DBCC PAGE dumps |
| `E5/blobs/` | round-1 segment and dictionary blobs as extracted from the backup |
| `E5/verify_bk_e5_full.txt`, `E5/verify_bk_e5_b_full.txt`, `E5/verify_bk_e5_c_full.txt` | verification logs |
