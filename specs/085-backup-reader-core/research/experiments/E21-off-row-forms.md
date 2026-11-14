# E21 — off-row forms: text pointers, text in row, out-of-row MAX values, row-overflow, deep trees

**Question.** PR 3 reads values stored off row (tasks 17–20). E20 met two forms: the row-overflow pointer
(kind 2) and the in-row root of a MAX value (kind 4). What do `text` / `ntext` / `image` hold in row,
with `text in row` off and on? What does `large value types out of row` change for a MAX column? How
is a NULL text value stored, and an empty one? Which allocation unit does each pointer name? How deep
do the trees get, and do fragment boundaries split characters? (PR 3's recon: M1–M5, M7, M9, M11, M12;
M14 in passing.)

**Method** (2026-11-13, SQL Server 2022 16.0.4305.5, two throwaway containers through a copy of
`test/backup/gen/generate.sh` with one more fixture each, not committed). Database `bak_lobforms2`
(round 2 repeats round 1's tables and adds the rest):

- `legacy`: `text`, `ntext`, `image` with text in row off; values NULL, empty, 1 B, 200 B, 8,000,
  8,041, 100 KB, 5 MB, and one `text` of 40 MB;
- `legacy_upd`: values updated to NULL (100 KB and 200 B), rewritten small (`'x'`), grown to 9,000;
- `legacy_small` / `maxout_small`: 60, 63, 64, 65, 70, 100 B (the small root's threshold);
- `inrow256` (the option set after four rows, one of them then updated), `inrow7000` (set first),
  `inrow24`, `inrow_off` (256, rows, then OFF, more rows);
- `maxout` (`large value types out of row` set after rows; two old rows then updated), `maxout_first`
  (set first), `maxnull` (pointer values updated to NULL);
- `ov_charmax` (`char(6000)` beside `varchar(max)`), `ov_wide` (0 to 4 of `varchar(8000)`,
  `nvarchar(4000)`, `varbinary(8000)`, `sql_variant`, `varchar(5000)` pushed off), `ov_exact` (rows of
  8,060, 8,061 and 8,062 bytes);
- `deep`: `varchar(max)` of 8,001, 40,200, 4.0 MB, 4.1 MB, 25 MB, 64 MB; `deepn`: `nvarchar(max)` of
  64 MB;
- `gs`: values grown off row and shrunk back, and regrown smaller (MAX and row-overflow);
- `dbcs_lob`: `varchar(max)` under Japanese / Chinese PRC / Korean / Chinese Taiwan collations, a
  `text` under Japanese, an `nvarchar(max)` and a UTF-8 `varchar(max)`, with a double-byte character,
  a surrogate pair or a 4-byte UTF-8 character across the first and second fragment boundaries;
- `part_lob`: three partitions, MAX and row-overflow values in each, then `REBUILD PARTITION = 2`.

A probe (`OpenBackupCatalog` + `ReadSlot`, ad hoc) read every record of every partition's in-row unit,
classified each variable column's bytes, walked the tree behind every pointer through
`PageStore::ReadLive` (so only live pages), recorded the unit of every node's page, assembled the
value, and printed its SHA-256. The server printed `HASHBYTES('SHA2_256', …)` of the same 231 values
(text, ntext and image cast through `varchar(max)` / `nvarchar(max)` to `varbinary(max)`).

**Result.** All 229 variable values the probe reads match the server's hash, bit for bit: 153 read off
row (every form below, four NULL roots among them, the 64 MB trees, the boundary-straddling
characters, the rebuilt partition), 18 NULL by their null bit and 58 inline or empty. The two values left over are a fixed `char(6000)`,
outside the probe; `sql_variant` was not hashed (the server hashes its base value, not its stored
bytes), so `ov_wide`'s 7,008-byte overflowed `sql_variant` is classified but not verified. No node
read failed the liveness check. PR 3's walker (`WalkLob`, task S1), run over the same backup assembled
and map-only, reads the same 153 values to the same hashes.

- **Three in-row forms of an off-row value.** The complex bit of the variable offset is set on all
  three. 16 bytes is always the root pointer; a row-overflow pointer and an in-row root of one link are
  both 24 bytes, told apart by their first byte (kind 2 or 4):

  | in-row bytes | form | target |
  |---|---|---|
  | 16 | **pointer to a root record**: u64 blob id, u32 page, u16 file, u16 slot. The id equals the root record's own u64 at +4 | the rowset's LOB_DATA unit |
  | 24, kind byte 2 | **row-overflow**: u32 length at +12, the fragment's RID at +16 (E20) | the ROW_OVERFLOW_DATA unit, one DATA fragment (type 3), no root |
  | 12 + 12n, kind byte 4 | **in-row root** (E20): byte 2 is the level (0: the links name DATA fragments; above: INTERNAL nodes), then n × 12-byte links | the LOB_DATA unit |

  The 16-byte pointer's first byte is the blob id's low byte (the id's low 16 bits were 0 in every
  value here), so it cannot be told from the others by its first byte: its length decides. Kind 5
  never appeared in row (the reader accepts it, as the catalog's does). Byte 3 of an in-row root (0x85
  / 0x88 on roots of two or more links) and the u32 at +8 of both 12-byte headers are not explained.
- **Which form is used.**
  - `text` / `ntext` / `image` with text in row **off**: always the 16-byte pointer, whatever the size,
    an empty value too (a small root of size 0).
  - With text in row **on** (limit L): a value of up to L bytes is stored inline, without the complex
    bit (as a plain variable column); a longer one is an **in-row root** (kind 4), not the 16-byte
    pointer, and so is a short one the row has no room for: in `inrow7000` a 6,000-byte `text` stayed
    inline while the 6,000-byte `image` beside it became a root. L = 24 behaves the same (24 inline, 25 a
    root). An empty value is the absent trailing column (empty, as 085 § Rules, records).
  - Turning the option on leaves old values as 16-byte pointers until that column is updated (in
    `inrow256` id 2 only `t` was updated and came back inline; `n` and `i` stayed pointers). Turning it
    **off** converts at once to 16-byte pointers both the inline values and the in-row roots.
  - MAX columns with `large value types out of row` = 1: every value, 10 B and empty included, is the
    16-byte pointer. Rows written before the option keep their inline / in-row-root forms until
    updated, so one table holds all three forms.
  - Row-overflow is used for bounded types only (`varchar(8000)`, `nvarchar(4000)`, `varbinary(8000)`,
    `sql_variant`): a MAX value that fits nowhere becomes an in-row root (`ov_charmax`). Which column
    goes is consistent with the documented widest-first rule; on a tie the later column goes (`gs`,
    `part_lob`). An 8,060-byte row stays whole, 8,061 overflows.
- **Root records** (on text-mix pages, type 3; record type 4, length 84). The fragment type is the u16
  at +12:
  - 0 **SMALL_ROOT**: size u16 at +14, bytes from +20. It holds up to **64 bytes**; 65 and more is a
    LARGE_ROOT (both the 16-byte-pointer forms).
  - 5 **LARGE_ROOT**: max links u16 at +14 (5 in every root), current links at +16, level at +18,
    12-byte links from +24 (u32 cumulative size, u32 page, u16 file, u16 slot).
  - 8 **NULL**: a value updated to NULL.
  - After an update in place: a SMALL_ROOT's u16 at +16 is 1 and bytes past its size are stale (`'x'`
    over `'abcdef…'`); a LARGE_ROOT's u32 at +20 is 1. Neither changes the reading.
- **A NULL `text` / `image` is not NULL in the null bitmap.** After `SET t = NULL` the row keeps its
  16-byte pointer, the column's null bit stays clear, and the root becomes type 8 (stale size and link
  bytes after it, so the fragment type is read before any size). So NULL is only known from the root
  record. Seen for `text` and `image` with text in row off; `ntext` (presumably alike), text in row on
  and a MAX value in in-row-root form were not tried. A value NULL at INSERT has its null bit set. A
  MAX value behind a 16-byte pointer updated to NULL has its null bit set (`maxnull`; whether the
  pointer bytes remain was not looked at).
- **Trees.** DATA fragments hold up to 8,040 bytes (8,041 needs two). An in-row root or a LARGE_ROOT
  holds up to 5 links, so level 0 reaches 40,200 bytes (5 × 8,040, measured). INTERNAL nodes (type 2, on
  text-tree pages, type 4) hold about 500 16-byte links: 4.0 MB is level 1 with one INTERNAL of 498
  DATA, 4.1 MB two INTERNAL; 25 MB and 64 MB are level 2 (64 MB: 1, then 16 INTERNAL, 7,961 DATA), for
  `varchar(max)` and `nvarchar(max)` alike, and a 40 MB `text` is level 2 under its LARGE_ROOT. 2 GiB
  (about 267k DATA under about 535 INTERNAL of level 0, under 2 of level 1) still fits level 2, so the
  reader's depth bound is never near.
- **Units.** Every node behind a 16-byte pointer or an in-row root was on the partition's own LOB_DATA
  unit, and every row-overflow fragment on its own ROW_OVERFLOW_DATA unit: the strict check (a kind-2
  pointer names the overflow unit, the others the LOB unit) holds. After `REBUILD PARTITION = 2` that
  partition's rowset owns a new IN_ROW and a new ROW_OVERFLOW unit and keeps its LOB unit, and every pointer of its rows
  names those (`part_lob`).
- **Grown and shrunk** (`gs`): a value shrunk back is plain bytes in row again (MAX and row-overflow
  alike), and an overflowed value comes back in row untouched when a neighbour shrinks (`gs` id 1's
  `p`). Every page the live pointers reached was live; whether old trees' pages linger was not
  looked for.
- **Fragment boundaries split characters.** In `dbcs_lob` a double-byte character, a UTF-16 surrogate
  pair and a 4-byte UTF-8 sequence each straddle the 8,040-byte boundary, and the assembled bytes are
  the server's. So a value is decoded after it is assembled, never fragment by fragment.
- **`sys.tables`** (M14): `text_in_row_limit` (0 when off) and `large_value_types_out_of_row` give the
  two options; their base-table field was not looked for.

**Not covered.** Heaps and forwarded records holding pointers (every table here is clustered), other
versions than 2022 (the oracle lane's 2017–2025 runs PR 3's fixture), `ALTER COLUMN` between types.

**Used by.** PR 3 tasks 17–20:
- the pointer is classified by its length (16) and kind byte (2 at 24 bytes; 4 or 5 at 12 + 12n), not
  by the column's type (an `ALTER COLUMN` to a wider type is metadata only and was not tried, so a
  form may outlive its type);
- a `text` / `ntext` / `image` value behind a 16-byte pointer needs its root read even to know it is
  NULL, so projecting such a column, or `_mssql_lob`, costs one root read per row;
- SMALL_ROOT, LARGE_ROOT and NULL are the root records, INTERNAL and DATA the nodes below, each
  level agreeing with its parent's;
- the unit check is strict per form, and a partition's units are looked up by its own rowset;
- values are assembled before they are decoded.

The fixture's shapes (each form, the mixed tables, NULL roots, a straddling character per page, a deep
tree, a rebuilt partition) go into PR 3's `lob.sql`, sized for the lane (the 64 MB values were for the
measurement only).
