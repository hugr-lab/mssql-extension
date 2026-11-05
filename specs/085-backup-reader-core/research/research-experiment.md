# SQL Server backup reader: hands-on feasibility experiment

Date: 2026-11-05. Server: SQL Server 2025 RTM-CU8 (17.0.4075.5), Enterprise Developer, Linux container
`mssql-v2test`. Every statement went through `tools/sql.sh`; the only docker commands used were `docker ps`
and `docker cp`. Research only: nothing here is tracked by git (`design/` is in `.git/info/exclude`).

**Short answer.** With nothing but the backup file, a Python reader can:

- walk the MTF container;
- decompress both compressed formats (MS_XPRESS and the 2025 ZSTD);
- find every 8 KB page, which identifies itself by its own header;
- bootstrap the system catalog (table names, column names, types, physical layouts, allocation units);
- decode FixedVar rows of every common type, byte-exact;
- follow LOB trees and row-overflow pointers;
- apply a differential over a full backup;
- parse log blocks and log records in a log backup.

ROW and PAGE compression formats can be recognised and are known to be decodable, but were not decoded here.
The native `json` binary format is undocumented. The columnstore segment format was recognised and partly
decoded: one bit-packed segment was decoded to the exact set of values.

## Contents of this folder

| Path | What |
|---|---|
| `tools/sql.sh` / `tools/sqlh.sh` | T-SQL helpers. `sqlh.sh` is a copy of `sql.sh` with `-line` output, so the column names are shown. |
| `tools/sqlsetup/*.sql` | Every DDL/DML batch that was run, in order (01..09). |
| `tools/mtf_dump.py` | MTF walker: DBLKs, stream headers and checksums. |
| `tools/bakz.py`, `tools/xpress_huff.py` | Decompressor for the MSSQLBAK compressed container, with an LZ77+Huffman decoder. |
| `tools/try_decompressors.py` | Tries zlib, deflate, lznt1, lzxpress and lzxpress_huffman (dissect), zstd and our own decoder on every block. |
| `tools/pages.py` | Page locator, header parser, histogram, marker search, FixedVar record annotator, catalog bootstrap, table decoder, LOB follower. |
| `tools/logblocks.py` | Log block and log record walker. |
| `tools/collations.psv` | CollationId, CodePage and name for all 5540 collations (from `sys.fn_helpcollations()`). A reader would embed this table. |
| `truth/` | Ground truth captured from the live server: page allocations, `DBCC PAGE` of the boot page, system-table layouts, columnstore segment metadata. Used only to check the decoders. |
| `headers/` | `RESTORE HEADERONLY` / `FILELISTONLY` output for every backup. |
| `samples/` | The backups (`SHA1SUMS` lists their hashes). |
| `work/` | Decompressed data sets, extracted LOBs, columnstore blobs. |
| `.venv/` | venv holding `dissect.util` 3.24, used for research only. |

## 1. Test database `bak_probe` (FULL recovery)

The scripts are in `tools/sqlsetup/` and run in order:

- **01: `t_heap` (heap) and `t_pk` (clustered PK), 300 rows each.** Columns: int, bigint, decimal(18,4), money,
  datetime, datetime2(7), date, time(7), datetimeoffset(7), uniqueidentifier, bit, varchar(50), and nvarchar(50)
  holding `N'… Привет ü'`. A varchar(50) column uses `Latin1_General_100_CI_AS_SC_UTF8` and holds `'Grüße €'`.
- **02: `t_rowc` (ROW) and `t_pagec` (PAGE).** 3000 rows each, with repetitive strings.
- **03: LOBs and row-overflow.**
  - `t_lob` has vmax / nvmax / vbmax columns. Row 1 is 20 KB / 12 KB / 14 KB. Row 2 is a 324 KB varchar(max).
    Row 3 is small, so it stays in-row.
  - `t_ovf` has three varchar(5000) columns, about 4.9 KB each. The row is about 14.8 KB, which forces two
    columns into row-overflow.
- **04: other types and a view.**
  - `t_types`: xml, geometry, geography, the native `json` type, and sql_variant (nvarchar, int, datetime and
    decimal values).
  - `t_sparse`: three SPARSE columns.
  - `v_probe`: a view.
- **05: `t_vec` with a `vector(3)` column.** The type is available, and is stored as varbinary(20) with
  utype 255.
- **06: `t_cci`, clustered columnstore.** 250,000 rows were loaded with a TABLOCK insert, plus one trickle
  insert, then `REORGANIZE WITH (COMPRESS_ALL_ROW_GROUPS = ON)`. `sys.column_store_row_groups` then showed
  rowgroup 0 COMPRESSED (250000 rows, 3.67 MB), rowgroup 1 TOMBSTONE and rowgroup 2 COMPRESSED (1 row).
- **07 / 08: changes for the differential and the log backup.**
  - Before the DIFF: INSERT id 1001, UPDATE id 5, DELETE pk id 7 and heap id 8.
  - Before the LOG: INSERT 2001/2002, UPDATE id 3, DELETE pk id 9 and heap id 11.
- **09 (added on request, after the first backups).**
  - `t_nuci`: a non-unique clustered index with duplicate keys.
  - `t_fwd`: a heap where an UPDATE grows id 5 from 375 to 3021 bytes, which forwards the row.
  - `t_ghost`: a clustered PK. Five deletes (ids 10..14) ran in the same batch as the backup, so their ghost
    records were not yet cleaned up. This went into a second full backup, `bak_probe_full2.bak`
    (COPY_ONLY, uncompressed).

Every value carries a `ZZMARK_*` marker.

## 2. Backups

```sql
BACKUP DATABASE bak_probe TO DISK='/var/opt/mssql/data/bak_probe_full.bak'      WITH INIT, FORMAT, NO_COMPRESSION
BACKUP DATABASE bak_probe TO DISK='/var/opt/mssql/data/bak_probe_full_comp.bak' WITH INIT, FORMAT, COPY_ONLY, COMPRESSION (ALGORITHM = MS_XPRESS)
BACKUP DATABASE bak_probe TO DISK='/var/opt/mssql/data/bak_probe_full_zstd.bak' WITH INIT, FORMAT, COPY_ONLY, COMPRESSION (ALGORITHM = ZSTD)   -- new in 2025, accepted
-- 07_diff_changes.sql
BACKUP DATABASE bak_probe TO DISK='/var/opt/mssql/data/bak_probe_diff.bak' WITH DIFFERENTIAL, INIT, FORMAT, NO_COMPRESSION
-- 08_log_changes.sql
BACKUP LOG bak_probe TO DISK='/var/opt/mssql/data/bak_probe_log.trn' WITH INIT, FORMAT, NO_COMPRESSION
-- 09_uniq_fwd_ghost.sql, then in one batch: DELETE t_ghost 10..14; BACKUP … bak_probe_full2.bak WITH COPY_ONLY, NO_COMPRESSION
```

The compressed backups were COPY_ONLY so that the differential base stays the uncompressed full backup.
`RESTORE HEADERONLY` / `FILELISTONLY` both work through `mssql_scan`; the outputs are in `headers/`.

| File | Size | Type | Compressed | FirstLSN | LastLSN | BackupSize | CompressedBackupSize |
|---|---:|---|---|---|---|---:|---:|
| bak_probe_full.bak | 11,653,120 | 1 Database | 0 | 50:1800:1 | 50:1824:1 | 11,624,448 | 11,624,448 |
| bak_probe_full_comp.bak | 3,563,520 | 1 (copy-only) | MS_XPRESS | 50:1904:1 | 50:1928:1 | 11,624,448 | 3,544,412 (3.28x) |
| bak_probe_full_zstd.bak | 3,158,016 | 1 (copy-only) | ZSTD | 50:1976:1 | 50:2000:1 | 11,624,448 | 3,139,948 (3.70x) |
| bak_probe_diff.bak | 3,264,512 | 5 Differential | 0 | 50:2216:1 | 50:2240:1 | 3,235,840 | — |
| bak_probe_log.trn | 307,200 | 2 Log | 0 | 50:1800:1 | 50:2288:1 | 278,528 | — |
| bak_probe_full2.bak | 12,701,696 | 1 (copy-only) | 0 | 50:2912:1 | 50:2936:1 | 12,673,024 | — |

Two more points from the headers and the file list:

- The DIFF's `DifferentialBaseLSN` is 50:1800:1 and its `DifferentialBaseGUID` is the full's `BackupSetGUID`.
  The log backup has `BeginsLogChain = true` and starts at the full backup's FirstLSN.
- `FILELISTONLY`: the data file is 75,497,472 bytes and `BackupSizeInBytes` is 10,092,544, which is 1232 pages.
  The log file shows `BackupSizeInBytes = 0`.

## 3. MTF layout (`tools/mtf_dump.py`)

### Format

Every descriptor block (DBLK) starts with the 52-byte MTF common header:

- 4-char type;
- `attributes` u32;
- `offset to first event` u16;
- OS id and version;
- displayable size;
- format logical address;
- control block id;
- OS data;
- string type;
- checksum: the XOR of the 25 preceding 16-bit words.

Streams are 22-byte headers: 4-char id, two attribute words, u64 length, encryption, compression, and a checksum
(the XOR of the 10 preceding words). Stream data is padded to 4 bytes. Every block is padded to the next block
boundary by a final `SPAD` stream. The 1024-byte format logical block is declared in TAPE.

Both checksums verified on every header. Strings are UTF-16LE, referenced by (size, offset) relative to the block.

### Uncompressed full backup (`bak_probe_full.bak`)

```
      0 DBLK TAPE  software="Microsoft SQL Server", vendor 4608, FLBS 1024, media family 0xa010f2f3
            stream RAID 32 B, SPAD
   4096 DBLK SFMB  soft filemark block (4096 B, 503 entries)
   8192 DBLK SSET  data set 1, name "ZZMARK_backupset_full", description, user "sa", sw 17.0, attr 0x4
   9216 DBLK VOLB  machine "886fa337de39"
  10240 DBLK MSCI  (SQL config info)
            stream MQCI 2920 B   -- nested SQL records SCIN (db name, server), SFGI (filegroup PRIMARY),
                                     SFIN (logical names + physical paths): the FILELISTONLY data
  13312 DBLK MSDA  (data file, pass 1)
            stream APAD 2966 B (alignment pad, media attr 0x80)
            stream MQDA 10,485,762 B  @16382 = 2 B lead-in + 1280 x 8 KB units, page-aligned at file offset 16384
            stream MQDA 0 B (media attr 0x6: end-of-data marker)
            stream SPAD
10503168 DBLK MSDA  (pass 2: the first two extents, pages 0..15, copied again at the end)
            stream APAD, MQDA 1,048,578 B (2 + 128 x 8 KB), MQDA 0 B, SPAD
11555840 DBLK MSTL  (log), block-specific header 544 B
            stream APAD 2430 B, MQTL 65,538 B (2 + 64 KB of log blocks), SPAD
11625472 DBLK MSTL x3 (no data, SPAD only)
11628544 DBLK MSLS  stream MQCI 2920 B (trailer copy of the config info)
11632640 DBLK SFMB
11636736 DBLK ESET  streams OTCP 3986 B (zeros), TSMP 278 B (tape set map: set name, description, user, machine), SPAD
11644928 DBLK ESET  SPAD
11649024 DBLK SFMB  (+ 3584 B tail)
```

### Differential, log and full2

**Differential.** Identical skeleton:

- SSET attribute 0x8.
- MSDA pass 1 holds an MQDA of 2,097,154 B (256 units).
- MSDA pass 2 holds 1 MB.
- MSTL / MQTL holds 64 KB.

**Log backup.** There is no MSDA: TAPE, SFMB, SSET, VOLB, MSCI, then MSTL with MQTL 262,146 B (256 KB of log
blocks), three empty MSTL blocks, MSLS, SFMB, ESET (OTCP, TSMP), ESET, SFMB.

**full2.** Same skeleton as the full backup: an MQDA of 11,534,338 B, then 1 MB.

### Data stream contents (`pages.py hist`)

An MQDA stream holds whole extents, in increasing extent order, and nothing else:

- Never-formatted pages inside an allocated extent are all-zero units: 267 in the full backup.
- After the last real extent, the stream is padded to a multiple of 1 MB with units of `m_type = 101`. These have
  pageId 0, slotCnt 1 and an 0x21 fill pattern at the end, but their body is **stale buffer memory**: the pass-2
  pad units in the full backup contain full copies of the `t_heap` / `t_pk` page bodies. A reader must skip
  them, and they leak deleted data.
- In pass 1, units 0..1231 are real pages (= `BackupSizeInBytes` / 8192) and units 1232..1279 are type 101.
- No per-page map exists. Each page names itself in its header: `m_pageId` at offset 32, `m_fileId` at 36.

## 4. Pages (`tools/pages.py`, uncompressed full)

### Header layout

The header fields are verified against `DBCC PAGE (…,1,9,3) WITH TABLERESULTS`, which works through
`mssql_scan`:

- `headerVersion` @0, `m_type` @1, `typeFlagBits` @2, `m_level` @3, `m_flagBits` @4;
- `m_indexId` @6 (AllocUnitId.idInd);
- prev page @8 (page u32, file u16), `pminlen` @14, next page @16;
- `m_slotCnt` @22, `m_objId` @24 (AllocUnitId.idObj), `m_freeCnt` @28, `m_freeData` @30;
- `m_pageId` @32, `m_fileId` @36;
- `m_lsn` @40 (u32 vlf, u32 block, u16 slot), `xdesId` @52, `m_ghostRecCnt` @58, `m_tornBits` @60.

The allocation unit id is `idInd << 48 | idObj << 16`. Examples:

- `t_heap` pages carry objId 245 and indexId 256, giving AU 72057594053984256, the same as `sys.allocation_units`.
- The boot page carries objId 99, giving AU 6488064, as DBCC shows.

### Page-type histogram (full backup)

| m_type | pass 1 | pass 2 | distinct pages |
|---|---:|---:|---:|
| 1 DATA | 199 | 2 | 199 |
| 2 INDEX | 133 | 1 | 133 |
| 3 TEXT_MIX | 518 | – | 518 |
| 4 TEXT_TREE | 5 | – | 5 |
| 10 IAM | 81 | 4 | 81 |
| 20 BULK_OP_PREALLOC (columnstore load) | 21 | – | 21 |
| 11 PFS (1:1, 1:8088) | 2 | 1 | 2 |
| 8 GAM / 9 SGAM / 16 DCM / 17 BCM | 1 each | 1 each | 1 each |
| 13 BOOT (1:9) | 1 | 1 | 1 |
| 15 FILE_HEADER (1:0) | 1 | 1 | 1 |
| 101 backup pad (stale memory) | 48 | 112 | – |
| all-zero units | 267 | 2 | – |

Distinct pages: 965. The live `sys.dm_db_database_page_allocations` reported DATA 196, INDEX 133, IAM 81,
TEXT_MIX 518 and TEXT_TREE 5, so the counts match except for 3 DATA pages.

### Boot page and file header

Boot page 1:9 holds a single record. The fields were found by value, then hard-coded at these record offsets:

- `dbi_version` @+4 = 998;
- `dbi_dbname` @+52 (nvarchar(128), padded with 0x2020) = `bak_probe`;
- `dbi_cmptlevel` @+314 = 170;
- `dbi_collation` @+392 = 872468488;
- `dbi_familyGUID` @+408;
- `dbi_firstSysIndexes` @+516 = **1:20**, the first page of `sysallocunits`.

File header 1:0 is in both passes; the pass-2 copy is the later one. Markers were found on 12 TEXT_MIX pages and
on every heap, PK, LOB, overflow, types, sparse and vector page. They were also found in:

- `sysobjvalues` (the view text);
- an internal table (query texts with the markers, probably Query Store);
- `syscscolsegments` (columnstore min/max strings);
- the columnstore delta-store B-tree (`ZZMARK_cs_cat_TRICKLE`).

### One heap row decoded by hand (`pages.py … row 1:352:0`, `t_heap` id 1)

```
[0]  statusA 0x30 = type 0 PRIMARY | 0x10 null bitmap | 0x20 var columns     [1] statusB 0x00
[2]  0x0054 = 84: end of fixed part
fixed @4  01 00 00 00                     id   = 1
      @8  cb 04 fb 71 1f 01 00 00          big  = 0x11F71FB04CB = 1234567890123
      @16 01 | 15 cd 5b 07 00 00 00 00     dec  = sign 1, 123456789 / 10^4 = 12345.6789
      @25 bf 8b c1 25 02 00 00 00          mon  = int64 9223375807 / 10^4 = 922337.5807
      @33 1d bb e2 00 | f9 b4 00 00        dt   = 14859037 ticks/300 s = 13:45:30.123, day 46329 since 1900 = 2026-11-05
      @41 87 0f 41 52 73 | 54 4a 0b        dt2  = 495301234567 x 100 ns = 13:45:30.1234567, day 739924 since 0001 = 2026-11-05
      @49 54 4a 0b                         d    = 2026-11-05
      @52 87 0f 41 52 73                   tm   = 13:45:30.1234567
      @57 87 d7 f3 2c 5a | 54 4a 0b | b4 00 dto = 10:45:30.1234567 UTC, offset +180 min = 13:45:30.1234567 +03:00
      @67 ff 19 96 6f 86 8b 11 d0 b4 2d 00 c0 4f c9 64 ff   g = 6f9619ff-8b86-d011-b42d-00c04fc964ff
      @83 c3 (bit 0)                       b    = 1   (the other bits of the byte are not zeroed)
[84] column count 14 | null bitmap 00 00 | var count 3 | end offsets at +90
var[0] 15 B  'ZZMARK_heap_001'                         (cp1252)
var[1] 50 B  'ZZMARK_nheap_001 Привет ü'               (UTF-16LE)
var[2] 29 B  'ZZMARK_u8heap_001 Grüße €'  c3 bc / c3 9f / e2 82 ac   (UTF-8, from the _UTF8 collation)
```

All 14 values match `SELECT … FROM t_heap`. `pages.py table t_heap` and `pages.py table t_pk` then decode all
300 rows, using only the layout recovered from the backup.

### System tables, without SQL Server (`pages.py … catalog`)

Pages self-identify by (m_objId, m_indexId), so the reader does not walk any page chain. It scans the leaf DATA
pages of each system table directly:

- `sysallocunits`: obj 7, ind 0;
- `sysrowsets`: obj 5, ind 0;
- `sysschobjs`: obj 34, ind 1;
- `syscolpars`: obj 41, ind 1;
- `sysrscols`: obj 3, ind 0.

The layouts are hard-coded once; the live `sys.system_internals_partition_columns` confirmed them, and they match
the OrcaMDF definitions. The decoded catalog has 2925 objects, 211 rowsets and 251 allocation units. It gives
table names, column names, types, length, precision, scale and collation, the rowset → allocation unit mapping
with first, root and IAM pages, and the physical layout of every rowset.

`syscolpars` also lists the columns of the system base tables themselves, for example the 22 columns of
`syscscolsegments`. So after a 5-table bootstrap, every other system table decodes generically: `sysobjvalues`,
`syscscolsegments`, `syscsdictionaries`.

Things learnt on the way:

- In `sysrscols`, the field at @32 that OrcaMDF calls `cid` holds the **collation id**. The column id is
  `rscolid` (= partition_column_id; 0 = uniquifier).
- `offset` is a signed 16-bit value: > 0 is a fixed offset, -n is the n-th variable column.
- `nullbit` is the low 16 bits; the high 16 bits are the key ordinal.
- The `ti` packs the type: low byte = xtype, then max length, or precision and scale.

**Collations.** The 4-byte collation id needs a table to map to a code page; `tools/collations.psv` has all 5540.
One rule held over all 5540: **a collation is UTF-8 iff `id >> 24 == 0` and `id & 0x80` is set.** SQL_
collations keep their sort id in the top byte.

**View definitions** are in `sysobjvalues.imageval` as plain text:
`CREATE VIEW dbo.v_probe AS SELECT id, vc, 'ZZMARK_view_def' AS tag …`.

### LOB and row-overflow

**`t_lob` row 1, `vmax`.** The in-row value is a complex column type 4 (BLOB_INLINE_ROOT, 48 B): 12 B header,
then links of (u32 cumulative size, RID): `8040@(1:521:0), 16080@(1:522:0), 20042@(1:520:0)`. The fragments are
TEXT_MIX records:

- statusA 0x08 (blob fragment);
- u64 blob id;
- u16 type 3 = DATA.

The rebuilt value is **20042 B, byte-exact**.

**`t_lob` row 2, 324 KB `vmax`.** The inline root points to one INTERNAL node at TEXT_TREE page 1:527:

- type 2, maxlinks 501, curlinks 41;
- links at +20 are 16 bytes each: u64 cumulative size, page u32, file u16, slot u16.

The 41 DATA fragments rebuild the value: **324,042 B, byte-exact**. The nvarchar(max) is 12,088 B and the
varbinary(max) is 14,000 B; both rebuild correctly too.

**`t_ovf` row 1.** `c1` stays in-row. `c2` and `c3` are 24-byte complex type-2 pointers:
`ROW_OVERFLOW length=4914 -> (1:592:0)` and `(1:593:0)`. Each points to one DATA fragment with the 4914-byte
value, on pages of the table's ROW_OVERFLOW allocation unit.

### Other types

| Type | On disk (observed) | Decodable |
|---|---|---|
| sql_variant | `[base type][version 1][props: u16 maxlen + u32 collation id / prec, scale / scale][value]` | yes, done |
| sparse columns | trailing complex var column `0x0005`: u16 n, n x u16 column id, n x u16 end offset, values | yes, done |
| vector(3) | varbinary(20), utype 255: `a9 01`, u16 dims, u32 0 (float32), dims x float32 | yes, done (`[1.5, -2.25, 3.0]`) |
| xml | binary XML: `df ff 01 b0 04 …`, the [MS-BINXML] signature, UTF-16 names | yes (documented spec); names and text extracted |
| geometry / geography | CLR serialization: SRID int32, version 1, properties byte, doubles ([MS-SSCLRT]) | yes (documented); SRID 0 / 4326, points visible |
| json (2025 native, xtype 244) | binary tree, `10 00 07 0d 'marker' 03 1f 'ZZMARK_json_001' …`. Keys and strings in UTF-8 with length prefixes; numbers and arrays in a compact form | **undocumented**: needs reverse engineering |
| ROW compression | CD record: header 0x21, column count, 4-bit CD array (0 NULL, 1 empty, 2-9 = 1-8 B short, 0xA long), short-data region (ints with the sign bit flipped: `81 83 87` = 1, 3, 7), long-data region with u16 end offsets | known format (OrcaMDF, Delaney); not decoded |
| PAGE compression | `typeFlagBits = 0x80`; compression info record at offset 96 (flags 0x06, anchor record with the prefix values, dictionary of 4 entries), then CD records with prefix references | known format, more work; not decoded |

### Uniquifier, forwarded records, ghosts (`bak_probe_full2.bak`)

**Uniquifier (`t_nuci`, clustered index on `k`, no PK).**

- `sysrscols` has a hidden **rscolid 0**: int, status 0x90, offset -1 (= the **first variable-length column**),
  key ordinal 2.
- The first row of each key stores a zero-length entry, meaning 0. Duplicates store 4 bytes: `01 00 00 00`,
  `02 00 00 00`.
- Records on page 1:1248 give (k, uniquifier) = (1,0) (1,1) (1,2) (2,0) (2,1) (3,0), which is unique.
- Raw bytes of the second record:
  `30 00 08 00 | 01 00 00 00 | 03 00 | 00 | 02 00 | 15 00 25 00 | 01 00 00 00 | 'ZZMARK_nuci_k1_b'`.

**Forwarded record (`t_fwd` heap).**

- Page 1:1256, slot 4 is a **forwarding stub**, 9 bytes: statusA 0x04 (type 2), then the RID of the new location
  (`04 | e9 04 00 00 | 01 00 | 00 00`), which is **1:1257:0**.
- Page 1:1257, slot 0 is the **forwarded record**: statusA 0x32 (type 1). Its last variable column is complex,
  10 bytes: `00 04 | e8 04 00 00 01 00 04 00`, i.e. header 0x0400 followed by the back pointer **1:1256:4**.
- The live server's `sys.fn_PhysLocFormatter(%%physloc%%)` returns `(1:1256:4)` for id 5.

So the original RID (file:page:slot) is the row's stable identifier. It survives the update that moved the row,
and the reader recovers it from the back pointer. It is not stable across a rebuild or shrink, which renumbers
pages.

**Ghost records.** After the delete, `t_ghost` page 1:1264 shows:

- `m_ghostRecCnt = 5` in the header;
- PFS byte 0x48 = allocated plus the "has ghost" bit 0x08;
- slots 9..13 with statusA **0x3c** = record type **6 (GHOST_DATA)**, plus null bitmap and var columns, with the
  full values `ZZMARK_ghost_010..014`.

The differential shows the same: page 1:360 holds `t_pk` id 7 as statusA 0x3c, with `m_ghostRecCnt = 1`.

A reader must skip record types 2, 5, 6 and 7. Deleted **heap** rows instead get slot offset 0, but their bytes
stay on the page: `ZZMARK_heap_008` is still on 1:352 in the differential.

## 5. Compressed backups

**The MTF structure does not survive at the file level; it survives inside the compressed stream.**

### Container (MSSQLBAK)

- @0, 4096 B: the "MSSQLBAK" container header. Word @12 is 3 for MS_XPRESS and 0x83 for ZSTD. In the XPRESS file
  it embeds a compressed copy of the TAPE block at offset 480.
- @4096: SFMB, uncompressed.
- @8192: a chain of compressed blocks. Each has a 28-byte header:
  - u8 kind: 0x21 = compressed, 0x23 = stored (incompressible);
  - u8 uncompressed size in 512-byte units: 0x80 = 64 KB, 0x10 = 8 KB, 0x08 = 4 KB, 0x18 = 12 KB;
  - u16 compressed length, where 0 means 65536;
  - u64, u64, u32 0;
  - u32 checksum.
- The payloads are packed back to back and not aligned.
- After the chain:
  - a seek index (pairs of uncompressed MTF offset → compressed offset, at the data-stream boundaries);
  - an SFMB;
  - a compressed 12 KB block holding ESET, OTCP and TSMP;
  - a final SFMB.

### Results

| Decompressor | MS_XPRESS (181 blocks) | ZSTD (163 compressed + 18 stored) |
|---|---|---|
| zlib / raw deflate | fails, or garbage | fails |
| dissect lznt1, dissect lzxpress (plain LZ77) | run without error, wrong size: garbage | garbage |
| dissect lzxpress_huffman | 51 of 181 blocks right; others throw "Not enough data for Huffman code tree" or come out 1 byte short | n/a |
| **own LZ77+Huffman** ([MS-XCA] 2.2, 140 lines, ends at output size = header byte x 512) | **181/181, 1.0 s** | n/a |
| **Python stdlib `compression.zstd`** | n/a | **163/163** (frames start `28 b5 2f fd`) |

**MS_XPRESS is LZ77+Huffman** (MS-XCA "LZXPRESS Huffman"): each block's payload starts with the 256-byte
Huffman table, one 64 KB chunk per block. The dissect decoder stops on input exhaustion rather than output size,
and its 4-byte prefetch breaks that.

Both compressed files decompress to **exactly 11,624,448 B (= BackupSize)**. That data set runs from SSET to
MSLS with the same block and stream offsets as the uncompressed file. 1403 of the 1419 8-KB units are
byte-identical to the uncompressed backup taken minutes earlier; the differences are in the boot, file-header and
allocation pages, the log tail and the headers. `pages.py` decodes `t_heap` and `t_lob` from the decompressed
data unchanged. So compression is a thin container over the same MTF stream.

## 6. Differential backup

- The DIFF holds 18 extents: 0, 1, 2, 3, 5, 8, 15, 17, 18, 19, 20, 23, 24, 44, 45, 148, 150 and 276. They are
  **exactly the bits set in the DCM page (1:6)** of the differential. The full backup's DCM had only 0, 1, 2
  and 8. (Bitmap: one bit per extent, read from page offset 194, as for GAM.)
- That is 122 distinct pages plus the same 1 MB "pass 2" (extents 0-1) plus pad units. The pages are identified
  by their own headers, as in the full backup.
- Against the full backup, 31 pages have a newer `m_lsn`, 3 are new and 88 are the same, because whole extents
  are copied.
- The changed user pages, after the INSERT / UPDATE / DELETE before the diff:

| Page | Status | m_lsn: full → diff |
|---|---|---|
| `t_heap` 1:352 | NEWER | 47:320:50 → 50:2104:2 |
| `t_heap` 1:358 | NEW page (id 1001) | — → 50:2064:9 |
| `t_pk` 1:360 | NEWER | 47:496:78 → 50:2096:2 |
| `t_pk` 1:366 | NEWER | 47:616:80 → 50:2072:3 |

- The other newer pages belong to system tables (`sysrscols`, `sysrowsets`, `sysallocunits`, `sysobjvalues`,
  `syscsrowgroups`) and to `t_cci`.
- **Restore by overlay works:** `pages.py "full.bak+diff.bak" table t_heap|t_pk` gives 300 rows each, with
  id 5 updated (`ZZMARK_diff_upd_*`), id 1001 present, heap id 8 gone and pk id 7 gone (its ghost is skipped).

## 7. Log backup (`tools/logblocks.py`)

### Where the log is

- `MSTL/MQTL`: 2 B lead-in, then 256 KB of raw log blocks, from LSN 50:1800 to 50:2288.
- A block's id is its 512-byte sector number in the log file: stream sector = block id - 1800. For example,
  block 0x8C0 = 2240 sits at stream sector 440.
- The full and diff backups carry 64 KB of MQTL each. That is only the 3 checkpoint records from their FirstLSN
  (LOP_BEGIN_CKPT 0x96, LOP_XACT_CKPT 0x98, LOP_END_CKPT 0x99).

### Log block header (observed)

- +0: parity/flags byte. Every 512-B sector starts with 0x40, 0x48 or 0x50.
- +2: u16 number of records.
- +4: u16 end of the slot array.
- +6: u16 block size (4096 here).
- +12: u32 VLF sequence number (50).
- +16: u32 block id.
- +72: first record.
- The slot array follows the records: u16 offsets, the last record first. The bytes after it are stale.

### Log record header (observed)

- +2: u16 fixed length (62 for row operations);
- +4: previous LSN (10 B);
- +14: flags;
- +16: transaction id (6 B);
- +22: operation (2 INSERT_ROWS, 3 DELETE_ROWS, 4 MODIFY_ROW, 5 MODIFY_COLUMNS, 0x80 BEGIN_XACT,
  0x81 COMMIT_XACT, …);
- +23: context (1 HEAP, 2 CLUSTERED, 19 MARK_AS_GHOST);
- +24: page (u32 page, u16 file);
- +30: u16 slot.

### Results

There are 48 blocks and 352 records. Against `fn_dblog` on the live database, every overlapping record matched
on LSN, operation and context: BEGIN/COMMIT x6, INSERT, MODIFY and DELETE in both heap and clustered.

All twelve marker changes were found, with page and slot:

```
32:810:9  INSERT_ROWS LCX_HEAP       1:358 slot 0  ZZMARK_diff_ins_heap_1001 (+ nvarchar)
32:818:3  INSERT_ROWS LCX_CLUSTERED  1:366 slot 20 ZZMARK_diff_ins_pk_1001
32:820:2  MODIFY_ROW  LCX_CLUSTERED  1:360 slot 4  'ZZMARK_diff_upd_'   (only the changed byte range is logged)
32:828:2  MODIFY_ROW  LCX_HEAP       1:352 slot 4
32:830:2  DELETE_ROWS ctx 19 (ghost) 1:360 slot 6  ZZMARK_pk_007   (full before-image)
32:838:2  DELETE_ROWS LCX_HEAP       1:352 slot 7  ZZMARK_heap_008
32:8c0:2  INSERT_ROWS LCX_CLUSTERED  1:366 slot 21 ZZMARK_log_ins_pk_2001   (= fn_dblog page 0001:0000016e slot 21)
32:8c8:2 … 32:8e8:2   heap insert 2002, both updates of id 3, deletes pk 9 / heap 11
```

Inserts and deletes carry the full row image in the page's own record format. `MODIFY_ROW` carries the before
and after byte ranges at an offset in the row. In fn_dblog terms: offset 90, size 13, the old
`6f00 8f00 9300 'ZZMARK_'` and the new `… 'ZZMARK_log_upd_'`.

## 8. Columnstore

**Where the data is.** Segments and dictionaries are LOBs in the CCI's LOB_DATA allocation unit
(AU 72057594055098368, about 470 TEXT_MIX pages). Two system tables point to them, and both were decoded from the
backup:

- `syscscolsegments` (obj 62): hobt, column, segment, encoding_type, row_count, base_id, magnitude, min/max data
  id, on_disk_size, min/max_deep_data (holding the min/max strings, e.g. `0f00 'ZZMARK_cs_cat_A'`), and
  **`data_ptr` binary(16) = u64 blob id + RID of the LOB root**.
- `syscsdictionaries` (obj 63), with the same kind of `data_ptr`.

Our LOB reader followed every `data_ptr`. The root is a type-5 LARGE_ROOT fragment, with links at +24 of
12 bytes each. Every blob came out with **length = on_disk_size exactly**: 667,720 / 1,096 / 667,720 /
2,001,064 / 334,384 B for the 5 columns of rowgroup 0, and 276 / 18,226 / 159 B for the dictionaries.

**Segment header.** The first 24 B are a common prefix (`01 00 00 00 01 00 00 00 00 00 00 00 0a 00 00 00 …`).
Then come the encoding kind, the bit width and the 64-bit word count:

| Column | Kind | Bit width | Words | Note |
|---|---|---|---|---|
| id | 2 | 21 | 83,334 | 250000 / 3 values per word |
| note | 2 | 10 | 41,667 | 6 values per word |
| ts | 4 | 64 | 250,000 | — |
| cat | 8 | 3 | 0 | RLE |

Bit widths are chosen to fill 64-bit words: 21 x 3 = 63, which is why 21 is used where 18 would do.

**id segment, decoded.** After the 1048-byte header come 83,334 words. The 21-bit values are read LSB-first,
3 per word. The decoded values + 1 are **exactly {1..250000}**, in VertiPaq-reordered row order, grouped by `cat`
(7, 14, 21 … then 1, 8, 15 …).

**cat segment (RLE).** A run table of (u32 value, u32 end row) pairs: 0 → 35715, 2 → 71430, 4 → 107144, …
That is 7 runs of about 35714 rows. Each pair is repeated 18 times, for a reason not understood.

**Dictionaries.** A header, a table of u64 offsets, then length-prefixed strings in plain text:
`0f 'ZZMARK_cs_cat_B' 0f 'ZZMARK_cs_cat_C' … 'ZZMARK_cs_cat_A'`. The 1000-entry `note` dictionary is 18 KB.

**Delta store.** The trickle row's delta store (rowgroup 1, TOMBSTONE) is still an ordinary B-tree page, 1:1144,
whose row decodes with the normal FixedVar reader.

**Assessment.** A columnstore reader is feasible. The layout is recognisable: header, bit-packing and RLE run
tables, plain dictionaries, and every pointer is resolvable from the backup. It is a real reverse-engineering
project, though, because the format is undocumented.

## 9. Cleanup

- `ALTER DATABASE bak_probe SET SINGLE_USER WITH ROLLBACK IMMEDIATE; DROP DATABASE bak_probe;`
- `EXEC master.sys.xp_delete_files N'/var/opt/mssql/data/bak_probe*'` (available on 2025).
- `sys.dm_os_enumerate_filesystem('/var/opt/mssql/data/','bak_probe*')` now returns nothing.
- The copies remain in `samples/`.

## 10. What worked, what did not, unknowns

### Worked, verified against ground truth

- **MTF walk** of all 6 files, with both header checksums.
- **Both compression formats decoded:**
  - ZSTD with the stdlib;
  - MS_XPRESS with our own LZ77+Huffman decoder (dissect's decoder is unreliable here).
  - Both produce the exact uncompressed data set.
- **Pages self-identify.** No external map is needed. Pass 1 holds the extents and pass 2 re-copies extents 0-1.
  The last copy of a page wins.
- **Catalog bootstrap from the backup alone**, through the boot page and 5 hard-coded system-table layouts. This
  gives every table's name, columns, types, collations, physical layout and allocation units, and `syscolpars`
  then describes every other system table.
- **FixedVar rows decoded, byte-exact**, for all the listed types, including UTF-8 varchar, decimal, money and the
  datetime family. Also: sql_variant, sparse vectors, vector, the uniquifier, forwarded records (back pointer =
  stable RID) and ghost skipping.
- **LOB trees** (inline root, INTERNAL node, LARGE_ROOT) and **row-overflow** pointers, byte-exact.
- **Differential:** the extents are exactly the DCM bits, and overlaying it on the full backup reproduces the
  post-change table contents.
- **Log backup:** log blocks and record headers parse. Op, context, page, slot and xid match fn_dblog, and the
  row images and markers of every change are visible.

### Did not work, or not attempted

- `dissect.util` lzxpress_huffman decodes only 51 of the 181 blocks: it terminates on input, not output size.
  lznt1, plain lzxpress and zlib/deflate do not apply at all.
- ROW and PAGE compressed records: the formats were recognised, not decoded.
- Native json binary: undocumented; only keys and strings are recognisable.
- Columnstore: not fully decoded.
  - VALUE_BASED: the exact use of base_id and magnitude is unclear (here the value is stored + 1, with base_id -2).
  - The RLE / bit-pack hybrid has an unexplained 18-fold repetition.
  - Delete bitmaps and secondary dictionaries were not examined.
- Applying log records (redo) was not attempted. MODIFY_ROW logs only byte ranges, so redo needs the page image.

### Unknowns, and what was not tested

- Header fields of the MSSQLBAK container and of the compressed blocks: two u64s, the checksum algorithm, the
  seek-index layout. Not needed to decompress.
- What exactly the pad units (`m_type 101`) are. Their bodies are stale memory, which is a data-leak concern in
  backups.
- Log sector parity: every 512-B sector's first byte is overwritten with a parity byte. The original bytes are
  presumably kept at the block end. This matters for records that span sectors and was not verified.
- The meaning of the MSTL block-specific 544 B, and the full opcode table beyond the codes validated here.
- Not tested:
  - backups WITH CHECKSUM, ENCRYPTION, or TDE databases;
  - striped or multi-file backup sets, appended backup sets (several SSETs), tape-style multiple volumes;
  - non-default BLOCKSIZE / MAXTRANSFERSIZE;
  - multi-file and multi-filegroup databases (several MSDA blocks with file ids);
  - FILESTREAM and in-memory OLTP containers (the server has `$HKv2` / xtp directories, unused here);
  - very large databases;
  - page compression on 2025 and later builds, and the format stability of the `json` and `vector` types.
