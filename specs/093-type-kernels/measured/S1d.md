# S1d — `sql_variant` values as a record stores them

**Question** (spec 093 T8b): the disk form of a `sql_variant` value -- what a backup's record holds --
for every base type S1 measured over TDS, so the backup reads it with T1's kernel.

**Method** (2026-11-25, SQL Server 2022 and 2025 from `test/backup/gen/images.txt`, throwaway
containers; `scripts/s1d.sql`, `scripts/s1d.sh`). S1's 50 values in `dbo.s1 (id int PRIMARY KEY, v
sql_variant)`, a full backup, and the value's stored bytes as the backup reader returns them before
T8b (`mssql_backup_scan`, a `sql_variant` its record bytes), beside the server's
`SQL_VARIANT_PROPERTY` and `COLLATIONPROPERTY(…, 'CollationId')`. 2022 and 2025 store every value byte
for byte the same.

**Correction to S1.** S1 said the server's `CAST(v AS varbinary(max))` is the disk form. It is not: it
is the value converted to binary (an int `-2147483648` is `80000000`, big-endian), with a decimal's
precision, scale and sign in front. The record's bytes below are the disk form.

**Result.** A value is a u8 base type, a u8 version (1), the properties, then the value:

| | TDS (S1) | disk |
|---|---|---|
| header | u32 length, BASETYPE, PROPBYTES | base type, version `01` |
| bit's base type | `0x32` (BITTYPE) | `0x68` (its xtype, 104) |
| decimal / numeric | precision, scale; a sign byte and a **16-byte** mantissa | precision, scale; a sign byte and a mantissa of 4, 8, 12 or 16 bytes: the fewest that hold the value, not the precision's width (numeric(10,3) 12.500 and decimal(38,38) 1e-38 take 4, decimal(18,0) and (19,9) 8, (28,8) 12, (38,0) 16) |
| money | 8 bytes, the high half first | 8 bytes, an int64 little-endian |
| datetime | the u32 days, then the u32 1/300 s ticks | the ticks, then the days |
| smalldatetime | the u16 days, then the u16 minutes | the minutes, then the days |
| char / varchar / nchar / nvarchar | a 5-byte collation, then a u16 maximum length | a u16 maximum length, then a u32 CollationId (`COLLATIONPROPERTY(…, 'CollationId')`) |
| every other base type | | the same properties and value bytes as TDS |

The same in both: tinyint / smallint / int / bigint / real / float / smallmoney (little-endian),
uniqueidentifier, date, time / datetime2 / datetimeoffset (a scale, then the column form at it), binary /
varbinary (a u16 maximum length, then the value, a binary(n) padded), a char(n) / nchar(n) value padded
with spaces to n, an empty string or binary a value of no bytes. NULL is the row's NULL (no bytes).

| id | base type | stored bytes (record) | collation (CollationId) |
|---|---|---|---|
| 1 | bit | `680101` |  |
| 2 | bit | `680100` |  |
| 3 | tinyint | `3001ff` |  |
| 4 | smallint | `34010080` |  |
| 5 | int | `380100000080` |  |
| 6 | bigint | `7f010000000000000080` |  |
| 7 | real | `3b010000c03f` |  |
| 8 | float | `3e01039300aa4bdd3dfe` |  |
| 9 | decimal(1,1) | `6a0101010105000000` |  |
| 10 | decimal(9,2) | `6a0109020015cd5b07` |  |
| 11 | decimal(18,0) | `6a011200014ef330a64b9bb601` |  |
| 12 | decimal(19,9) | `6a011309011581e97df4102211` |  |
| 13 | decimal(28,8) | `6a011c08004ef338be917a796deb35fd03` |  |
| 14 | decimal(38,0) | `6a01260001ffffffff3f228a097ac4865aa84c3b4b` |  |
| 15 | decimal(38,38) | `6a0126260001000000` |  |
| 16 | numeric(10,3) | `6c010a0301d4300000` |  |
| 17 | money | `3c010000000000000080` |  |
| 18 | smallmoney | `7a01ffffff7f` |  |
| 19 | date | `2801000000` |  |
| 20 | date | `2801dab937` |  |
| 21 | time(0) | `2901007f5101` |  |
| 22 | time(3) | `2901037b512502` |  |
| 23 | time(6) | `290106404ac66108` |  |
| 24 | time(7) | `29010787e6bed153` |  |
| 25 | datetime2(0) | `2a0100a08c0020490b` |  |
| 26 | datetime2(3) | `2a01037b51250220490b` |  |
| 27 | datetime2(6) | `2a0106404ac6610820490b` |  |
| 28 | datetime2(7) | `2a010787e6bed15320490b` |  |
| 29 | datetime2(7) | `2a0107ffbf692ac9dab937` |  |
| 30 | datetime2(7) | `2a01070000000000000000` |  |
| 31 | datetime | `3d017198750001b50000` |  |
| 32 | smalldatetime | `3a01ac0101b5` |  |
| 33 | datetimeoffset(0) | `2b01004019011f490b4803` |  |
| 34 | datetimeoffset(6) | `2b010640e201000021490bb8fc` |  |
| 35 | datetimeoffset(7) | `2b010787aa06b82520490b4a01` |  |
| 36 | datetimeoffset(7) | `2b0107ffbf692ac9dab9370000` |  |
| 37 | uniqueidentifier | `2401ff19966f868b11d0b42d00c04fc964ff` |  |
| 38 | binary(4) | `ad01040001020000` |  |
| 39 | varbinary(10) | `a5010a00010203` |  |
| 40 | char(5) | `af01050008d000346162202020` | SQL_Latin1_General_CP1_CI_AS (872468488) |
| 41 | varchar(10) | `a7010a0008d00034636166e9` | SQL_Latin1_General_CP1_CI_AS (872468488) |
| 42 | varchar(10) | `a7010a0015d00000e4eeec` | Cyrillic_General_CI_AS (53269) |
| 43 | varchar(10) | `a7010a0010d000008abf8e9a` | Japanese_CI_AS (53264) |
| 44 | varchar(20) | `a701140088d10400f09f9880c3a9` | Latin1_General_100_CI_AS_SC_UTF8 (315784) |
| 45 | nchar(10) | `ef01140008d000346100620020002000200020002000200020002000` | SQL_Latin1_General_CP1_CI_AS (872468488) |
| 46 | nvarchar(20) | `e701280008d104003dd800de7800` | Latin1_General_100_CI_AS_SC (315656) |
| 47 | char(6) | `af01060015d00000e4eeec202020` | Cyrillic_General_CI_AS (53269) |
| 48 | NULL | `null` |  |
| 49 | varchar(5) | `a701050008d00034` | SQL_Latin1_General_CP1_CI_AS (872468488) |
| 50 | varbinary(5) | `a5010500` |  |

**Used by** T8b: `SplitDiskVariant` (`codec/variant/variant_envelope`) puts a disk value in the TDS form
where they differ -- bit's base type, money's halves,
datetime's and smalldatetime's parts, the collation from its CollationId -- and T1's `EncodeVariant`
reads it unchanged (a decimal's mantissa at the width it is stored at, which it takes as it is). The bytes above are its unit test's vectors.
