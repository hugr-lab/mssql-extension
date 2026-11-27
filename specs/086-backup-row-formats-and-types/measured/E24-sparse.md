# E24 — sparse columns, measured

**Question** (spec 086 PR 3, task 7): where a record keeps its sparse columns' values, how the reader finds
them, and what `ALTER` / `DROP` leave in rows written before. Before this, spec 086 § Design had research
X§4's one line: a trailing complex variable column whose first u16 is 5, then the column ids, the end
offsets and the values. D1: no decoder source was opened; this is our own measurement.

**Method** (2026-11-24, throwaway containers of SQL Server 2019 CU32 and 2025 CU10 from
`test/backup/gen/images.txt`, synthetic values only; 2017 does not run on this host, and the oracle lane
covers it). Nine tables in a scratch database:

- `t_types`: every type a sparse column can have (bit, the integers, decimal (9, 2) and (38, 10), money,
  smallmoney, real, float, date, time (7) and (0), datetime, smalldatetime, datetime2 (7),
  datetimeoffset (7), uniqueidentifier, char / varchar / nchar / nvarchar, binary / varbinary, the three
  `(max)` types, sql_variant, xml) between two regular variable columns: a row of NULLs, a row of edge
  values, one value alone, empty strings and binaries, values of 9,000 to 10,000 bytes;
- `t_wide`: 600 sparse ints and a varchar, three of them set;
- `t_colset`: eight sparse columns behind a column set;
- `t_alter`: a sparse column added, then a regular one made sparse (`ALTER COLUMN … ADD SPARSE`);
- `t_unsparse`: a sparse column made regular (`DROP SPARSE`);
- `t_dropsp`: a sparse column dropped;
- `t_heap`: a heap whose row grows a 3,000-byte sparse value and is forwarded;
- `t_ovf`: a row of 4,000 + 4,000 + 6,000 bytes of sparse values, and updates to NULL.

Every data page was read by `DBCC PAGE (…, 2)` (the bytes) and `(…, 3)` (the server's reading: each sparse
column's offset and length). A prototype (Python, kept out of the repository) parsed every record and
placed every sparse value of every row where the server says it is: no mismatch, on both versions, and the
two versions' records are alike byte for byte apart from the null bitmap's unused bits. `sysrscols` was
read from the database's backup by the reader itself (`mssql_backup_catalog(…, 'sysrscols')`).

## The catalog

| fact | measured |
|---|---|
| a sparse column in `sysrscols` | status `0x100`, offset 0, null bit 0: it has no place in the fixed part, no variable column and no null bit |
| a dropped sparse column | status `0x102` |
| the record's column count and null bitmap | count only the columns that are not sparse (`t_types`: 3 of 33) |
| a sparse table compressed | refused: ROW by 10622 ("A compressed index is not supported on table that contains sparse columns"), PAGE by 11418. A sparse table's records are FixedVar records |
| a sparse column as a clustered key | refused (1919) |

## The vector

| part | layout |
|---|---|
| where | a variable column, complex (bit 15 of its end offset): the **last** one of the record, or the one before a forwarded record's back pointer (`t_heap`: pad, vector, back pointer). Not at a fixed index: the record's NULL trailing variable columns are left out before it, so with `r1` and `r2` NULL the vector is variable column 0, and with both set it is column 2 |
| header | u16 `0x0005`, u16 n (≥ 1) |
| ids | n × u16, ascending, each a `sysrscols.hbcolid` (not `rscolid`, not `syscolpars.colid`: they differ after an `ALTER`) |
| ends | n × u16, each value's end from the vector's start, ascending; the first value starts at 4 + 4n. Bit 15 marks a complex value: an off-row pointer |
| values | each in its FixedVar form: a fixed type's bytes (a bit is one byte, 0 or 1), char / nchar / binary padded to their width, a variable type's bytes, sql_variant's envelope, xml's binary XML |
| a NULL | no entry. A row whose every sparse value is NULL has no vector at all. An update to NULL removes the entry |
| an empty value | an entry of length 0 (`''`, `N''`, `0x`), told from NULL |

Off-row values, inside the vector, with bit 15 set on the entry's end:

- a `(max)` value of 9,000 or 10,000 bytes: a 36-byte in-row LOB root (kind 4, two links) on the LOB_DATA
  unit;
- `t_ovf`, a vector that would pass the row's size: the 24-byte row-overflow pointer (kind 2) of the
  values pushed out, on the ROW_OVERFLOW_DATA unit; the vector itself stays in row.

So kind 5 is the sparse vector. It is the kind E21 never saw in row and the reader accepted as an in-row
root.

## After `ALTER`

| change | the rows written before |
|---|---|
| a sparse column added | unchanged: no entry, NULL |
| `ALTER COLUMN x ADD SPARSE` | rewritten: x's value moved into the vector under a **new** hbcolid (5); the old column is dropped (`67108865`, the old fixed offset) |
| `ALTER COLUMN y DROP SPARSE` | rewritten into the fixed part (the new column, hbcolid 4); the vector keeps the old entry under the dropped column's hbcolid (2) |
| a sparse column dropped | its entries stay, under the dropped column's hbcolid |

So an entry is read for the live sparse column whose hbcolid it carries, and an entry of an hbcolid no live
sparse column has (a dropped one) is passed over.

## The column set

Not stored: no `sysrscols` row, no entry. It is computed from the sparse values, like a computed column
that is not persisted, and read as one (owner, 2026-11-24: decision A).

## Found by the fixture

`test/backup/gen/sparse.sql`, against RESTORE (`compare_rows`), added these:

- **The uniquifier.** A non-unique clustered index's uniquifier is a variable column, not stored when it is 0
  (its null bit says nothing of that). The vector then takes its index. A real uniquifier is 4 bytes and not
  complex, so the vector's tag tells them apart.
- **An empty value** (review of #626). An empty regular variable column (`''`, `N''`, `0x`, its null bit
  clear) closing the regular ones is not stored, as a trailing NULL is not, and the vector takes its index
  (rows 8-11 of `types_sparse`, the empty pads of `heap_sparse` -- forwarded ones too -- and the empty `v` of
  `uniq_sparse`; measured on 2019, 2022 and 2025). So the null bitmap cannot say where the vector is: its tag does.
- **No DEFAULT.** A sparse column takes no DEFAULT constraint (1791). One added after rows were written is
  NULL in them, as an absent entry is.
- **ANSI_PADDING OFF, sparse or not.** A nullable char / binary holding `''` / `0x` is one pad byte: the server
  reads `DATALENGTH` 1 (one space, one zero byte). The reader trimmed it to none.

## What the reader does with this

- **The vector:** taken from the last variable column (before a forwarded record's back pointer) when
  the layout has a sparse column (live or dropped) and that column is complex and starts with `0x0005`.
  Whatever the null bitmap says (an empty regular column and a uniquifier of 0 are not stored): a regular
  column's complex form starts with kind 2 or 4, and a text / ntext / image root pointer with its blob id,
  whose low 16 bits were 0 in every value (E21). Past every regular column, the last one must be the vector.
  Every part is checked: n, the ids ascending, the ends ascending and within the vector. Where the bitmap says
  the regular column at that index holds a value, the vector must also have every id a sparse column's of the
  layout (live or dropped) and its last value end at its end; a tagged column that fails is that column, not
  an error. The vector is then not counted among the
  record's regular variable columns, so a regular one at its index is not stored.
- **The change listing:** a sparse table's rows are compared by their values, not their bytes. `ALTER`
  rewrites every row with its values the same, and a vector keeps a dropped column's entries.
- **A sparse column's value:** the entry of its hbcolid. An entry with bit 15 is an off-row pointer and is
  walked as any.
- **Everything after the value** (the kernels, `_mssql_lob`, the change listing's comparisons) is the
  regular columns' path.

## Against the published descriptions (2026-11-25)

Read after the review of #626: Mark Rasmussen's posts on the sparse vector and on complex columns
(improve.dk, 2011; the OrcaMDF reader, `SparseVectorParser.cs`, `Records/Record.cs`,
`Records/Parsers/PrimaryRecordEntityParser.cs`) and Qingsong Yao's post on sparse column storage
(Microsoft, 2008). They agree with the layout above: tag 5, the count, the ids, the end offsets from the
vector's start, the values, and no vector when every sparse value is NULL. Where they differ from what was
measured here:

- **How the vector is found.** OrcaMDF scans every complex variable column for id 5, whatever its place
  or the null bitmap; the reader here takes the tag too, at the last variable column (before a back
  pointer), which both posts describe as the vector's place.
- **A 16-byte vector.** OrcaMDF takes every 16-byte complex column for a text / ntext / image root
  pointer before it reads an id, and calls this a known bug in its own comments: a vector of one 8-byte
  value (4 + 2 + 2 + 8 bytes) is lost. The tag decides here (`test_backup_rows`, a bigint entry beside a
  NULL and beside an empty column).
- **The ids.** Rasmussen names them `sys.columns` column ids. They are `sysrscols.hbcolid`: after `ALTER
  COLUMN … ADD SPARSE` the value is under a new hbcolid, and after `DROP SPARSE` the old entry keeps the
  dropped column's.
- **An empty value before the vector.** Neither source describes it; Yao says only that trailing NULL
  columns take no offsets and that the vector does not change that. Measured here (rows 8-11 of
  `types_sparse`, 2019 / 2022 / 2025): an empty value closing the regular columns is not stored either.
- **Checks.** OrcaMDF checks the tag alone; the reader checks every part (above).
