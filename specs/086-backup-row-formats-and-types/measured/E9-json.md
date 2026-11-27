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
  given as `\u0041` is stored as `\u0041` (E9b corrected this: E9's corpus had lost the escape before the
  server saw it).

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

## E9b: what PR 6 measured (2026-11-27)

The same server (SQL Server 2025 RTM-CU10, 17.0.5035), a throwaway container. The fixture `json`
(`test/backup/gen/json.sql`, `json_changes.sql`; 2025's committed under `test/data/backup/json`) holds E9's
corpora again, keys of every escape (the self-review of PR 6 found E9's had lost theirs), documents of
32,767, 32,768 and 65,535 keys around the narrow / wide boundary, and a scratch experiment of its own: 240
documents grown in place one change at a time. Ground truth: each value's server text read through
`TYPE mssql` from a RESTOREd copy, beside its stored bytes from the backup. The decoder
(`src/codec/json/json_binary.cpp`) renders every value to that text byte for byte: the 240 grown
documents and the 4,751 of an earlier cut of the fixture, then `compare_rows.sh` on the committed one --
4,966 rows of the full and 4,907 of the full + differential, 0 failures. Its VARIANT
(`src/codec/json/json_codec.cpp`) equals DuckDB's own `TRY_CAST(text AS VARIANT)` for all 10,475 rows of both
states, NULLs included.

What it found beyond E9:

- **A node's flag byte is its number of spare slots.** A container that grew in place keeps room for
  more: an object or array of count *c* and flag *f* holds *c* + *f* slots in each of its slot arrays
  (key references, values), *f* up to *c* measured (an object of 14 members grown by 6 kept 14 spare).
  E9's flag `0x01` is one spare slot. In the key table the low 7 bits are the spare count and the top bit
  (`0x80`) the sorted-hash permutation, as E9 found.
- **A key is at most 7,998 bytes** (13646 past it), so a key's length prefix is at most 2 bytes; a string
  value's goes to 3 (70,000 measured) and E9's 4.
- **A key keeps every escape as written:** `"a\"b"`, `"c\\d"`, `"a\/b"`, `"line\nbreak"`, `"\u0041"`,
  `"\u00e9t\u00e9"`, a surrogate pair `"\uD83D\uDE00"` -- all stored byte for byte as given, and the text writes them
  so. Two keys that unescape alike (`"a\/b"` and `"a/b"`) are two members to the server. A JSON -> VARIANT
  reader unescapes them, as DuckDB's parse of the text does, and keeps one child per key, the last value
  (DuckDB's `ConvertJSONObject`); a key whose escapes are not valid makes the document malformed.
- **A lone surrogate** (`"\uD800"`) is taken: in a key it is stored as written, so the text holds it, and
  DuckDB's JSON parse refuses that text ("no low surrogate"); in a string value the server stores U+FFFD.
  The JSON reading is the server's text, the key included; the VARIANT reading is NULL with a WARNING
  (`RenderJsonText(…, check_keys)`), where DuckDB's cast of the text throws.
- **A copied subtree is copied:** `JSON_MODIFY(j, '$.b', JSON_QUERY(j, '$.a'))` and `j.modify('$.b', JSON_QUERY(j,
  '$.a'))` over a 1,000-element array, and over an object, write the nodes again -- no node is named by two
  slots, so the walk's budget (size / 4 + 1 values) holds (measured on a scratch database of the same server).
- **Narrow or wide:** documents of 32,767, 32,768 and 65,535 keys render to the server's text (a narrow
  key reference's top bit is the deleted mark, so the server must widen by 32,768).
- **A string value's control characters** below 0x20 are written as `\u00XX`, upper-case; DEL (0x7F) is
  written as itself.
- **A decimal item:** a length *n*, then precision, scale, a sign byte (1 positive, 0 negative), and a
  little-endian unsigned mantissa of 2 × (*n* − 1) bytes: 4, 8, 12 or 16, as SQL Server's own `numeric`
  (38 digits in 16 bytes). `2.50` is `05 07 03 02 01 FA000000`.
- **A json column added with a default** (`ALTER TABLE … ADD jd json NOT NULL DEFAULT '…'`, metadata
  only) reads its default for the rows written before, in the same binary form: the reader's column
  default, nothing of its own.
- **A document changed in place on a page not rewritten since** decodes like any other (the old items kept,
  the trailer naming the new root).
- **Over the wire the value is always text (J1).** `SELECT j` through `mssql_scan`: without JSONSUPPORT the
  column is `varchar(max)` (TDS `A7 FFFF`, a UTF-8 collation) carrying the text; with JSONSUPPORT (`0x0D`,
  version 1) requested in LOGIN7 the server grants it in FEATUREEXTACK and still sends `A7 FFFF` text --
  no JSONTYPE `0xF4`. The binary form is a storage form only: a backup's.

So the decoder refuses (as malformed, NULL with a WARNING) only what neither E9 nor E9b saw: a header bit
beyond bit 0 of its second byte, a trailer of another shape, an item tag or constant E9 did not list, a
decimal mantissa past 16 bytes, a key reference past the key table, a key whose escapes are not valid, nesting
past 128, a walk past size / 4 + 1 values (a cycle, or a node two slots name: each value of a well-formed
document fills a 4-byte slot of its own), a text past 2^31 - 1 bytes (a key referenced often enough).
