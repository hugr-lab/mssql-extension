# E22 — the CD record (ROW compression), measured

**Question** (spec 086 PR 1, tasks 1–3): the byte layout of a ROW-compressed record and of each
type's value in it. Spec 086 § Design lists what E1 verified (integers, strings, the CD codes up to
10) and what DbReader documents (codes 11 / 12, clusters of 30, vardecimal, SCSU; W§6), the latter
"to verify". D1: no decoder source is opened; this is our own measurement.

**Method** (2026-11-16, SQL Server 2022 RTM-CU27-GDR 16.0.4305.5, a throwaway container on the
image `images.txt` pins; synthetic values only; a third batch the same day, after the review: row
versioning, ANSI_PADDING OFF, rowversion, the types kept as stored, smalldatetime's edges). Two batches of tables `WITH (DATA_COMPRESSION =
ROW)`: one table per type, `(id int PRIMARY KEY, v <type> NULL)`, with edge values (NULL, zero,
minimum, maximum, negatives, trailing spaces / zeros, Unicode in several scripts, surrogate pairs,
long values); tables of 31, 60, 129 and 300 columns; a key that is not the first column; a heap with
forwarded rows; a non-unique clustered index; ghosts (trace flag 661); columns added after rows. Every
data page read by `DBCC PAGE (…, 3) WITH TABLERESULTS`, which gives each record's bytes, its CD array
as the server reads it, and each column's offset, physical length and value. A prototype walk and
prototype value decoders (Python, kept out of the repository) were checked against those offsets and
values: 1,858 column values, every one placed where the server places it (the key-order and
uniquifier tables aside, whose physical order is `sysrscols`'), and every value of the types below
marked *checked* equal to the server's text.

## The record

| part | layout |
|---|---|
| header | u8. Bit 0 = CD. Bit 1 (`0x02`): a 14-byte version tag ends the record (row versioning: RCSI, snapshot; every record written while it is on). Bits 2–4 = the record type, CD's own numbering: 0 primary (`0x01`, `0x03`, `0x21`, `0x23`), 1 version ghost, 2 forwarding stub (`0x09`), 3 ghost (`0x0d`, `0x0f`), 4 forwarded (`0x31`, `0x33`). Bit 5 (`0x20`): a long data region follows. Bits 6–7 not seen. |
| forwarding stub | not a CD record: 9 bytes, header `0x09`, then the RID it forwards to (page u32, file u16, slot u16); the same under row versioning. |
| version ghost | a heap row deleted under row versioning: the header (`0x07`) and its 14-byte version tag, nothing else (15 bytes). A deleted row of a clustered table is a ghost (type 3) keeping its columns. |
| forwarded record | the record as a primary one, then its back pointer (RID: page u32, file u16, slot u16), then the version tag when bit 1 is set. |
| column count | u8; when its bit 7 is set, u16 big-endian with bit 15 masked off (`80 82` = 130, `81 2d` = 301). A record written before a column was added counts fewer columns (the added ones take their default or NULL, as 085 reads FixedVar). |
| CD array | ceil(n / 2) bytes, 4 bits a column, low nibble first. 0 NULL, 1 EMPTY (a zero, an empty string, a value trimmed to nothing), 2–9 a value of 1–8 bytes in the short region, 10 a value in the long region, 11 bit = 1. |
| cluster array | with more than 30 columns: one byte per cluster of 30 columns but the last, each the short-region bytes of that cluster (31 columns: `19` = 25 bytes of 25 one-byte values). |
| short region | the 1–8-byte values back to back, in CD order. |
| long region | (header bit 5) u8 flags (`01` seen), u16 count, count × u16 end offsets from the start of the long data (bit 15: a complex column — an off-row pointer as 085 reads it, e.g. a 36-byte in-row root); with more than one cluster, then one byte per cluster but the last, each the number of long values that cluster holds (long values in columns 6, 7, 41, 42 and 66 of 70: `02 02`); then the data. |
| minimum size | 9 bytes: a shorter record is padded, with whatever the bytes held (`0e 00 00 00 00`, `… 00 80`). The parts above say where the record ends. |

Columns are in physical order (`sysrscols`): the clustered key first, a uniquifier as a column of
its own (its value an integer as below), whatever order the table declares. Every column of a
ROW-compressed rowset has a negative `sysrscols.offset`, its CD index being `-offset - 1` (086 D4),
fixed types included; and a decimal's precision in `sysrscols.ti` carries `0x80` when it is above 18
(`decimal(19, 0)` 147, `(28, 6)` 156, `(38, x)` 166; 9 and 18 as they are), which an uncompressed
rowset's does not.

## The values

| type | stored | checked |
|---|---|---|
| `smallint`, `int`, `bigint` | two's complement, big-endian, in the fewest bytes that hold it, the top bit inverted (`1` → `81`, `-1` → `7f`, `128` → `80 80`, `-129` → `7f 7f`); 0 is EMPTY | yes |
| `tinyint` | the byte as it is (`128` → `80`); 0 is EMPTY | yes |
| `bit` | 0 EMPTY, 1 code 11 | yes |
| `money`, `smallmoney` | the value in units of 1/10000 as an integer above (`$1` → `a7 10`) | yes |
| `decimal`, `numeric` | vardecimal: a byte with bit 7 the sign (set: positive) and bits 0–6 a decimal exponent biased by 64; then the decimal digits from the first non-zero one, three to 10 bits, the last group's trailing zero bits not stored (`1` → `c0 19`: exponent 0, `0001100100` = 100, i.e. 1.00 × 10⁰; `123.45` → `c2 1edc20`: 123 450 × 10²⁻⁵); 0 is EMPTY | yes, 63 values, 38 digits and scale 38 included |
| `real`, `float` | the IEEE value's little-endian bytes with the low-order zero bytes left out (`1.0` → `f0 3f`, `0.5` real → `3f`); 0 is EMPTY | bytes seen |
| `date`, `time(n)`, `datetime2(n)`, `datetimeoffset(n)` | the uncompressed form, in full (3; 3–5; 6–8; 8–10 bytes) | bytes seen |
| `datetime` | one 8-byte integer, days since 1900-01-01 × 2³² + 1/300 s ticks, as an integer above (1900-01-01 → EMPTY, + 1 tick → `81`) | bytes seen |
| `smalldatetime` | 2 bytes: the day count (u16 LE), the minutes being 0 (day 256: `00 01`, not one byte); else the 4 uncompressed bytes (minutes, days; 04:16 is `00 01 00 00`) | yes |
| `char(n)`, `binary(n)` | trailing spaces / zero bytes left out (all of them: EMPTY); the reader pads to n -- but a nullable one declared under `ANSI_PADDING OFF` (`syscolpars.status` without `0x2`, `is_ansi_padded` 0) is read as stored: the server returns it trimmed (`DATALENGTH` 1 for `0x0100` in a `binary(8)`); `nchar` and NOT NULL columns are padded whatever the setting. (An uncompressed rowset stores such a column at a fixed offset, padded, and the server trims it on read: the reader does the same.) | yes |
| `timestamp` (`rowversion`) | its 8 bytes big-endian with the leading zero bytes left out (`0x7D1` → `07 d1`; not an integer above) | yes |
| `sql_variant`, `xml`, the CLR types (`hierarchyid`, `geometry`) | as stored uncompressed: a `sql_variant`'s base value in its uncompressed form (`38 01 01000000` for int 1), binary XML, the CLR bytes (`hierarchyid` `/1/` → `58`, `/` EMPTY); a CLR column described in `sysrscols` as `varbinary` (xtype 165), as it is too in an uncompressed rowset made by `SELECT INTO` | bytes, against an uncompressed twin |
| `varchar(n)`, `varbinary(n)`, the MAX types | as they are (`varchar` keeps its trailing spaces); a value of 8 bytes or fewer in the short region even for a MAX type; a MAX value off row a complex long column | yes |
| `uniqueidentifier` | the 16 bytes, always in the long region | bytes seen |
| `nchar(n)`, `nvarchar(n)` | Unicode compression: an odd length is SCSU (UTS #6; `жук` → `12 b6 c3 ba`, SC2 then window 2 = U+0400; quotes SQ*n* and SQU, a surrogate pair as two SQU), with `0x10` (SC0, no effect) appended when the encoding came out even; an even length is the UTF-16LE text uncompressed (`𝄞𝄞`, `日本語テキスト`); `nchar` trailing spaces left out | yes, 50 values in 9 scripts |
| `nvarchar(max)` | as it is (UTF-16LE), not compressed | yes |

"Bytes seen" values follow from the uncompressed layouts 085 already decodes; the oracle lane's row
comparison (RESTORE + `TYPE mssql` against `mssql_backup_scan`) checks them value by value.

## Not covered here

Code 12 (a page-dictionary symbol) belongs to PAGE compression (PR 2). The SCSU tags SCU / the
Unicode mode and the dynamic-window definitions (SD*n*, SDX) were not produced by these values; the
decoder takes them as UTS #6 defines them, and the fuzz harness and the oracle fixture's corpus
exercise them. Versions other than 2022 are the oracle lane's (2017–2025).
