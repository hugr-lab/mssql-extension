# E9 — the stored `json` value (SQL Server 2025), the spike of spec 086 PR 6

**Question** (spec 086 D8, § `json`: the research spike). SQL Server 2025's `json` type (xtype 244) is
stored in a binary form that nothing documents. Can a decoder written from the bytes alone give the
server's own text (`CAST(j AS nvarchar(max))`) for every value of a controlled corpus? The corpus covers
every JSON type, nesting to the server's limit, Unicode, escapes, numbers at every edge, documents that
go off row, compressed tables and values modified in place. That is D8's gate.

**Method** (2026-11-20, a throwaway container of SQL Server 2025 RTM-CU10, 17.0.5035, the image of
`test/backup/gen/images.txt`; database compatibility level 170; synthetic values only; the raw output
is kept out of the repository).
- **The server's text.** `CONVERT(varchar(max), CAST(CAST(j AS nvarchar(max)) AS varbinary(max)), 2)`, so
  the text is compared byte for byte, not through a client's rendering.
- **The stored bytes.** The server refuses `CAST(j AS varbinary(max))` (Msg 529). So the database was
  backed up, and the bytes were read by this repository's backup reader (`mssql_backup_scan`), which
  returns a `json` column as its stored bytes (085 D13). Values past 8,000 bytes come through its off-row
  path (PR 3).
- **The decoder.** A prototype in Python, kept out of the repository, written from the bytes (D1: no
  source opened).
- **Four corpora:**

| corpus | values | what |
|---|---|---|
| 1 | 73 | objects, arrays, every scalar in every container; empty object, array, string and key; duplicate keys; key order; strings from 0 to 70,000 bytes (each length-prefix width); BMP, astral and escaped characters, control characters; integers at every width and sign edge; decimals and exponents; nesting 32 to 128; a 10,000-element array, a 2,000-key object, 3,000 strings |
| 2 | 30 | objects of 16 to 1,000 and 70,000 keys; keys shared by nested objects; 1,600 nested keys; decimals at 38 digits; a 7 MB document; long and escaped keys |
| 3 | 321 | `\uXXXX` keys; the same rows in an uncompressed, a ROW and a PAGE table (300 more rows to fill PAGE pages); values changed by `JSON_MODIFY` (replace, add, delete, grow a string, append to an array, nested) and `j.modify()` |
| 4 | 8 | ten adds, ten appends, a run of deletes, nested changes, a value's type changed twice, add / delete / re-add, a 20,000-character string, six `j.modify()` rounds |

**Result.** The prototype gives the server's text for **432 of 432** values, in every corpus.

## What the server accepts and how it writes the text

- **The top level is an object or an array.** A bare scalar is refused ("Unexpected character … at
  position 0").
- **Nesting is limited to 128.** At 129: "Nested level of JSON document exceeds limit 128".
- **A number is a `numeric` of precision at most 38.**
  - `1e308` and `1e37` are refused: "out of the range for numeric representation (maximum precision 38)".
  - An integer that fits 64 bits is kept as an integer.
  - Any other number is a decimal, its precision and scale from the literal (`2.50` is `2.50`).
  - **A number written with an exponent becomes `decimal(38, 10)`:** `1E2` is `100.0000000000`,
    `1e-10` is `0.0000000001`, and `5e-324` is `0.0000000000`, so the value is lost there. `-0` is `0`.
- **Duplicate keys: the first one wins** (`{"a":1,"a":2}` is `{"a":1}`). Keys keep their insertion order.
- **The text has no whitespace.**
- **A string value is written by the server's rules:**
  - `"` → `\"`, `\` → `\\`, `\n` `\t` `\r` `\b` `\f` as such;
  - any other control character as `\u` and four upper-case hex digits;
  - `/` and everything else, non-ASCII included, as itself.
- **A key is written as it was given**, its escapes included: `{"a\"b":1}` stays `{"a\"b":1}`. A key
  given as `A` is stored as `A`.

## The stored form

Little-endian throughout. A document is a header, a pool of items, the container nodes, a key table, and
a 16-byte trailer.

- **Header.** 2 bytes. `10 00` for most documents; `20 00` for the 2,000-key object. When the second
  byte's bit 0 is set (`20 01`: the 70,000-key object, the 7 MB document), the document is **wide**: a
  node's count and a key reference are u32 instead of u16. The first byte's meaning beyond that is
  not known; the decoder does not need it.
- **Trailer.** The last 16 bytes:
  - u8 `08`;
  - a u32 seen 0 in written documents, and non-zero in documents modified in place (5, 6, 24, 7);
  - u32 the root value (below);
  - u32 the key table's offset, `FFFFFFFF` when there is none;
  - 3 zero bytes.
- **A value** is a u32. Its top two bits:
  - `10`: an integer in the low 30 bits, two's complement (`FFFFFFBF` is −1);
  - `11`: a constant: 0 false, 1 true, 2 null, 4 `{}`, 5 `[]`, 6 `""`;
  - `00`: the offset of an item or a node.
- **Variable lengths.** A length is 1 to 4 bytes, sized by its low bits:
  - `x1`: one byte, `len >> 1` (up to 127);
  - `x10`: two bytes, `>> 2`;
  - `x100`: three, `>> 3` (70,000 measured);
  - `x1000`: four, `>> 4`.
- **Items:**
  - **`03` a string:** a length, then the UTF-8 bytes, unescaped;
  - **`07` a key:** a length, then the UTF-8 bytes as written. The empty key is not an item: the key
    table names it by the constant `C0000006`;
  - **`04` an integer** that does not fit 30 bits: an int64;
  - **`05` a decimal:** a length *n*, then precision, scale, a sign byte (1 positive, 0 negative), then
    a mantissa of `2 × (n − 1)` bytes. The text is the mantissa with *scale* digits after the point,
    padded with zeros.
- **Nodes.** A tag, a count (u16, or u32 when wide), and a flag byte. Flag `0x01` (spare slots)
  appears after an in-place modification. The node then keeps one more slot than its count, past the
  count, in every array of slots.
  - **`02` an array:** the values, a u32 each.
  - **`01` an object:** the key references (u16 or u32: an index into the key table), then the values
    (u32). A member deleted in place keeps its slot, its reference marked by the top bit (`0x8000`, or
    `0x80000000` when wide), and is left out of the text.
- **The key table.** One per document, for every object in it:
  - tag `06`, a count, a flag byte;
  - the keys' hashes, u32 each;
  - when flag `0x80` is set (the 2,000- and 70,000-key objects, not 1,000: the hashes sorted), a
    permutation, u16 a key (u32 when wide);
  - the keys' offsets, u32 each, in key order;
  - with flag `0x01`, a spare slot in each array.

  The hash function is not known. It is used for lookup only, so the decoder does not need it.
- **A document modified in place** keeps its old items and nodes. It appends new ones, and its trailer
  names the new root and key table. A replaced value's old item stays, unreferenced. A document
  rewritten by `JSON_MODIFY` and one changed by `j.modify()` decode the same way.
- **Compression.** A `json` column in a ROW- or PAGE-compressed table reads the same bytes (corpus 3,
  the same rows in all three tables). The value is not compressed by the row format (E22's "kept as
  stored" arm).

## What it means for PR 6

- **D8's gate passes on this corpus.** PR 6 builds the decoder.
- **The decoder:**
  - DuckDB-free, every offset and count bounded;
  - the type `JSON`, the text as the server writes it;
  - refused by name: a tag, flag or constant the corpus did not show (an unknown flag bit beyond `0x01`
    / `0x80`, a constant other than 0, 1, 2, 4, 5 and 6).
- **Not seen, so refused rather than guessed:**
  - a spare count other than 1 in a flag;
  - a header whose second byte has bits beyond bit 0.
- **The fixture** for the oracle lane is these corpora on 2025, compared with `CAST(j AS
  nvarchar(max))` through `TYPE mssql`, which reads a `json` column as its text.
- **To measure in PR 6:**
  - a document on a page not yet rewritten since an in-place change;
  - a `json` column added with a default (metadata-only ADD);
  - a document past 2 GB is out of scope (the backup reader's 2^31 − 1 value limit).
