# E20 — record forms of user tables, and page values through the codec's kernels

**Question.** PR 2d reads user rows (tasks 14–16). The committed fixtures hold only primary records
(type 0) and, in the load fixture's full under its workload, 51 ghosts (type 6). What do forwarded
records, stubs, the uniquifier, version tags, ghosts of a heap, row-overflow and LOB pointers look like
on 2022? And can the live read path's family kernels (`codec/staging`, `DecodeChunkFromStaging`) take a
page's value bytes as they are?

**Method** (2026-11-10, SQL Server 2022 16.0.4305.5, a throwaway container through a copy of
`test/backup/gen/generate.sh` with one more fixture, not committed). A database `bak_rows` with
`ALLOW_SNAPSHOT_ISOLATION` and `READ_COMMITTED_SNAPSHOT` on and trace flag 661 (no ghost cleanup):

- `fwd`: a heap of 400 `(id int, v varchar(3000))` rows, every third grown to 1,800 bytes
  (131 forwarded, per `sys.dm_db_index_physical_stats`);
- `uniq`: a non-unique clustered index on `k` (300 rows, 10 key values);
- `ver` (clustered, half the rows updated) and `ver_heap` (a fifth updated);
- `ghost` (clustered) and `ghost_heap`, a quarter / a third of their rows deleted;
- `wide`: `varchar(6000)` × 2 and `varchar(max)` / `nvarchar(max)`, rows in-row, row-overflow and
  off-row;
- `part_add`: a three-partition table with rows, then two columns added with defaults;
- `notnull` (NOT NULL fixed columns only) and `bits` (ten `bit` columns around an `int`).

Every slot of every live data page of each table's in-row unit was read with `ReadSlot` and counted
by type and status, and decoded with `DecodeRecord` through the partition's layout. Separately the
records of `sales.orders` (catalog fixture, 24 types), `dbo.items` / `dbo.events` (samples) and
`dbo.utf8_text` were staged into `ColumnStaging` and finalized by the family kernels, one
`tds::ColumnMetadata` per physical column (`type_id` = the column's xtype, `max_length` / precision /
scale from `ti`), and the values compared with the fixtures' generating expressions.

**Result.**

- **Version tag.** With row versioning on, *every* record written afterwards has statusA 0x40 and
  14 bytes after its last variable column (or its fixed part's end): an 8-byte pointer and a 6-byte
  sequence number. A row never updated has a zero pointer (`0000000000000000 7d0300000000`); an updated
  one points into tempdb's version store (`09000000 0900 3b00`, file 9, slot 0x3b). The record's
  layout before it is unchanged, so the decoder ignores it. This settles § Rules, records' "unverified".
- **Forwarding.** `fwd`: 269 records of type 0, 131 of type 1 (statusA 0x72) whose last variable
  column is the 10-byte back pointer, and 131 stubs of type 2 (statusA 0x04, 9 bytes: u32 page, u16
  file, u16 slot). `DecodeRecord` read 400 of 400 rows, each once, skipping the stubs.
- **Ghosts.** A deleted row of a clustered index is a type-6 record (statusA 0x7C), still FixedVar.
  A deleted row of a **heap** under row versioning is a **type-7** record (statusA 0x4E, a version
  ghost) that is *not* FixedVar: `ReadRecord` parses it as one and fails ("a fixed part ending at 0").
  The scan must skip a record by its type before parsing it. `ForEachPageRow`, the catalog's walk,
  parses first too.
- **Uniquifier.** The first variable column, empty when 0 (the first row of a key value), else 4 bytes;
  the table's own variable columns follow. `DecodeRecord` read 300 of 300.
- **Added columns on a partitioned table.** `sysseobjvalues` holds one default per column and per
  partition rowset (6 rows for 2 columns × 3 partitions). The model's per-rowset lookup (2c) finds
  them: 301 of 301 rows decoded, defaults included. This settles 2c's open question.
- **Bits.** `bit` columns share a byte at one offset by `bitpos` (eight at offset 4, the ninth at 9);
  NULL is the null bitmap's.
- **Complex columns.** A row-overflow pointer is 24 bytes of kind 2 (u32 length 5000 at +12, the
  root RID at +16). An off-row MAX value is an in-row root of kind 4 with 12-byte links (cumulative
  size, page, file, slot). A MAX value small enough stays in-row as plain bytes, with no complex bit.
- **`ti`.** For `decimal` / `numeric` the low byte above the xtype is the precision, the high byte the
  scale (`0x0412` = decimal(18, 4)). For `time` / `datetime2` / `datetimeoffset` it is the scale. For a
  MAX type the length is 0.
- **Values through the kernels.** Page bytes are already in the form the TDS path stages for every
  type of `sales.orders` except four: `int` family, `real` / `float`, `decimal` (sign byte + mantissa),
  `smallmoney` (i32), `date` / `time` / `datetime2` / `datetimeoffset`, `uniqueidentifier`,
  `nvarchar` / `nchar` (string kernel), `varchar` / `char` on a code page (spec 091's kernel), UTF-8
  `varchar`, `binary` / `varbinary` / `timestamp`. Every value of the 20 columns with a deterministic
expression matched it, on all 300 rows (`ref` is `NEWID()`; `ratio` / `score` / `version` were
checked on four rows by eye). The four
  are `datetime` and `smalldatetime` (time half first, the wire's day half first), `money` (a plain
  i64, the wire's high half first), and `bit` (a bit of a shared byte). For each, swapping the halves
  or taking the bit while copying into staging gave the right values through the existing kernels.
- **Types.** `TypeConverter::GetDuckDBType` (the TDS map) reads `datetime2(7)` as `TIMESTAMP`; D6's
  map (`mssql_backup_tables.duckdb_type`) reads it as `TIMESTAMP_NS`. The kernels take the target type
  (`ResolveColumnOps(meta, target)`), so the scan passes D6's.

**Used by.** PR 2d tasks 14–16: skip records by type before parsing (types 2–7); `ti`'s precision /
scale / MAX; per-column append arms for `datetime` / `smalldatetime` / `money` / `bit` feeding the
existing kernels, instead of new kernels; D6's type as the kernels' target. The fixture's shapes
(forwarding, uniquifier, version tags, both ghost kinds, bits, added columns on a partitioned table)
belong in the lane's fixtures, which hold none of them.
