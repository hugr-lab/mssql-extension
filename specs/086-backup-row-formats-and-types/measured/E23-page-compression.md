# E23 — PAGE compression, measured

**Question** (spec 086 PR 2, tasks 5–6): the layout of a PAGE-compressed page's compression-information
record — its anchor record and its dictionary — and how a record's values refer to them. Before this,
spec 086 § Design had only what research X§4 recognised: a header flag `0x80`, a record at offset 96 with
flags `0x06`, an anchor record and a dictionary. D1: no decoder source was opened; this is our own
measurement.

**Method** (2026-11-20, SQL Server 2025 RTM 17.0, a scratch database on the shared test server, dropped
afterwards; synthetic values only). Six tables `WITH (DATA_COMPRESSION = PAGE)`, each filled and then
rebuilt PAGE:

- `t_prefix`: values sharing a prefix;
- `t_dict`: five columns of a few repeating values, NULLs and empty strings;
- `t_long`: a 200-byte shared prefix, beyond one byte's length;
- `t_types`: bigint, datetime2, decimal, varbinary, nchar, money, uniqueidentifier;
- a heap;
- a two-row table.

Every data page (25 pages) was read by `DBCC PAGE (…, 3) WITH TABLERESULTS`. That gives the
compression-information record's bytes, the server's own reading of its anchor and dictionary, each
record's bytes and CD array, and each column's value.

A prototype (Python, kept out of the repository) rebuilt each value from the bytes alone. It checked
30,204 integer, string and binary values against the server's text, and every one was equal. Every
type's value was then checked by the reader itself: the oracle fixture (`test/backup/gen/page.sql`, the
shapes of `cd.sql` under PAGE) restored and compared through `TYPE mssql`.

A ghost forwarded record was measured afterwards, in a throwaway container on 2025 with trace flag 661
(no ghost cleanup): a PAGE heap under row versioning, its forwarded rows deleted.

## The compression-information record

| part | layout |
|---|---|
| where | the page's header `typeFlagBits` (+2) carries `0x80`; the record begins at offset 96, and the first row record follows it. A page of a PAGE rowset without the flag holds no such record and is a ROW page: a page that never filled (4 of 25 here, the last of a table and a two-row table's) |
| header | u8 flags: `0x02` an anchor record, `0x04` a dictionary (`0x02` and `0x06` seen). u16 a modification count (0 seen). With a dictionary, u16 its offset from the record's start. Then u16 the record's size |
| anchor record | right after the header (at 5, or 7 with a dictionary): a CD record (E22's layout) of every column, each column's value the prefix its values share, or NULL for none. It is **not** padded to 9 bytes as a row record is (5 bytes seen) |
| dictionary | at its offset: u16 the entry count, then one u16 per entry, its end offset from the dictionary's start, then the entries back to back (the first starts at 2 + 2 × count). An entry is a value in its CD form; an anchored column's entry starts with its prefix length, as its values do (`00 alpha`: prefix 0, then `alpha`) |

## A record's values

A value of a PAGE record is resolved in this order:

1. **Code 12 (`ONE_BYTE_PAGE_SYMBOL`)**: one byte in the short region, the index of a dictionary entry. The entry is the value. Only the one-byte form was seen; a dictionary held at most 16 entries here.
2. **A column with an anchor value**: the value starts with its prefix length, then holds the suffix. The length is one byte; when its top bit is set, it is two bytes, the length in the low 15 bits, big-endian (`80 ca` = 202). The value is the first *length* bytes of the anchor's value, then the suffix. This applies to the long region as well, and to a dictionary entry of the column.
3. **EMPTY (code 1) in an anchored column**: the anchor's value whole (`charlie`, `zebra`). In a column with no anchor value, EMPTY is the empty value, as under ROW.
4. **The bytes so built are the value's ROW form** (E22): an integer sign-flipped big-endian, a string as stored, Unicode SCSU, vardecimal, and so on, put in the uncompressed form as ROW values are.

The prefix is taken over those compressed bytes, not over the values: the integers of a column share
their high bytes (`1000000000000 + i`).

## A ghost forwarded record

A heap's forwarded record deleted under row versioning has header `0x17`:

- bit 0 is CD;
- bit 1 marks a version tag;
- record type 5 in bits 2–4, which E22 did not see.

Its column count is 0. Then come its back pointer (page u32, file u16, slot u16) and its 14-byte version tag: 24 bytes in all. It holds no row: a ghost. It appears under ROW compression the same way.

## Not covered here

- **Larger dictionaries.** No page held more than 16 dictionary entries. A symbol past 255 would need a code this experiment did not produce. Codes 13–15 are refused by name, as before.
- **A modification count other than 0** was not seen.
- **Other versions.** 2017–2022 are the oracle lane's: the `page` fixture on every version.
