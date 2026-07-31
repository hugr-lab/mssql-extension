# UTF-8 on the wire: when to ask the server for it

Design note for [#225](https://github.com/hugr-lab/mssql-extension/issues/225) (advertise
UTF8SUPPORT) and [#224](https://github.com/hugr-lab/mssql-extension/issues/224) (invalid
UTF-8 from non-UTF8 collations). Measured on SQL Server 2022 Developer in docker,
1,000,000 rows, ~41 characters per value.

The shipped path — ask the server for UTF-16 and decode it — stays the fallback. Everything
below is an *acceleration* that must degrade to it, and every condition is something the
extension can observe at runtime rather than infer from a product name.

## What the wire actually does

`wire_in` is the extension's own byte counter (`MSSQL_DEBUG=2`, "stream close").

| column | UTF8SUPPORT off | UTF8SUPPORT on |
| --- | --- | --- |
| `varchar(100)` with a UTF-8 collation | 85.8 MB, arrives 0xE7 (server transcodes) | **43.9 MB**, arrives 0xA7 |
| `nvarchar(100)` | 85.8 MB | 85.8 MB — the feature does not touch it |
| `varchar(100)` with a code page | 85.8 MB — *we* cast it to NVARCHAR | 85.8 MB — same |

End to end, `SELECT sum(length(x))` over 1M rows, wall clock, three passes:

| | as today | via UTF-8 |
| --- | --- | --- |
| UTF-8-collation column | 0.44–0.45 s | **0.25–0.27 s** |
| `nvarchar`, ASCII-ish content | 0.43–0.47 s | **0.29 s** (server-side cast) |
| `nvarchar`, CJK content | **0.21–0.23 s** | 0.29 s (server-side cast) — *a loss* |

The cast costs about the same regardless of content; what varies is the baseline. So a
blanket "always cast unicode columns to UTF-8" is wrong — hence the setting below.

The client side needs no new code: `BIGCHAR`/`BIGVARCHAR` already resolve to
`P2StageBinary` / `PlpStageBinary` in `column_ops.cpp`, i.e. one allocation and one copy per
column, no conversion. The UTF-16 batch decode (spec 055, ~2.1 ns/value) is skipped entirely
the moment the server sends 0xA7.

## Inputs

**Per connection, once at login:**

- `utf8_support_acked` — from FEATUREEXTACK (0xAE). The server acks `feature=0x0A len=1
  first=0x01`; a server that does not support it simply omits the entry, and it never acks a
  feature that was not requested.
- database default collation — already cached (`MSSQLMetadataCache::GetDatabaseCollation`).
- whether a UTF-8 collation can be named in a `COLLATE` clause.

**Per column, from catalog metadata:**

- `sql_type_name`, `is_unicode`, `max_length`, `collation_name`.
- **proposed addition**: `COLLATIONPROPERTY(c.collation_name, 'CodePage')` as one more
  column in the existing metadata queries — no extra round trip. It answers both questions
  that matter and is more robust than matching `_UTF8` on the name:
  - `65001` → the column is already UTF-8;
  - `932 / 936 / 949 / 950` → a double-byte East Asian code page, where UTF-8 *inflates*;
  - anything else → single-byte, where UTF-8 is never larger than UTF-16.

**Settings:**

- `mssql_utf8_support` (**landed**, default **on**) — advertise UTF8SUPPORT in LOGIN7. The
  request goes out on every connection; the setting exists to turn it off, because asking
  costs nothing on a server that does not support it.
- `mssql_server_side_utf8` (proposed, default **off**) — permit rewriting `nchar`/`nvarchar`/
  `ntext` to UTF-8 on the server. This is the content-dependent bet from the table above.
- `mssql_convert_varchar_max` (existing) — unchanged.

## The algorithm

### A. What to put in the SELECT list, per projected string column

```
collation code page 65001 (column already UTF-8)
    → project as-is.
      feature on  : arrives 0xA7 UTF-8   → direct copy
      feature off : server transcodes to 0xE7 → UTF-16 decode (today's behaviour)

char / varchar / text with a code page
    → if utf8_support_acked and a UTF-8 target is available and the code page is single-byte:
          CAST(col COLLATE <utf8> AS VARCHAR(m))
      else:
          CAST(col AS NVARCHAR(n))                       ← today
    A single-byte source is at most one character per byte, and a character is at most
    2 bytes in UTF-8 for every code page in this class, versus always 2 in UTF-16. So this
    branch is never worse on the wire. (Exception worth knowing: a handful of CP1252
    punctuation code points — € ‰ † — are 3 bytes in UTF-8. Rare enough not to gate on.)

nchar / nvarchar / ntext
    → if utf8_support_acked and mssql_server_side_utf8 and a UTF-8 target is available:
          CAST(col COLLATE <utf8> AS VARCHAR(m))
      else:
          as-is for nchar/nvarchar, CAST(col AS NVARCHAR(MAX)) for ntext   ← today

not a string type
    → unchanged.
```

### B. The cast width `m`

One UTF-16 code unit is **at most 3 bytes** of UTF-8, and the bound is tight: 100 CJK
characters produce exactly 300 bytes. A supplementary character is 4 bytes across 2 units,
i.e. 2 bytes per unit, so 3 per unit dominates.

```
units = nchar/nvarchar(n)   : n
        char/varchar(n)     : n        (single-byte: 1 byte = 1 char; double-byte: ≤ n chars)
        MAX / text / ntext  : unbounded

m = 3 * units,  capped by VARCHAR's 8000-byte limit
    → units ≤ 2666  : VARCHAR(3 * units)
    → otherwise     : VARCHAR(MAX)
```

`VARCHAR(MAX)` is PLP-framed, which costs about 14 bytes per value: the same 1M-row column
measured 43.9 MB as `VARCHAR(300)` and 57.9 MB as `VARCHAR(MAX)`. Above 2666 units the
comparison is PLP against PLP (`NVARCHAR(MAX)` is PLP too), so it is a plain byte count.

**Getting `m` wrong is silent.** `CAST(x COLLATE <utf8> AS VARCHAR(299))` on 100 CJK
characters returns 297 bytes — truncated on a character boundary, so no error, no invalid
UTF-8, just short data. The width must be derived, never guessed.

### C. Which collation to name

```
database default collation is UTF-8 (code page 65001)
    → no COLLATE clause at all: CAST(col AS VARCHAR(m)) already yields UTF-8
otherwise
    → CAST(col COLLATE Latin1_General_100_BIN2_UTF8 AS VARCHAR(m))
```

Two things this gets right:

- **The COLLATE goes on the source expression, not the result.** `CAST(x AS VARCHAR(m))
  COLLATE <utf8>` converts through the *database's* code page first and only then relabels:
  with a CP1252 database, Cyrillic, CJK and emoji all come back as `0x3F` — `?`. Silent,
  and invisible to any ASCII test. Verified byte-for-byte.
- **BIN2 is the right pick.** Output bytes are identical across UTF-8 collations (verified),
  so the choice only affects comparison semantics, which a projection never uses — and the
  binary collation avoids the linguistic tables. It is also the collation Fabric standardises
  on, so it is the most likely to exist.

Availability is a fact to observe, not assume: `COLLATIONPROPERTY('Latin1_General_100_BIN2_UTF8',
'CodePage')` returns 65001 where it exists and NULL where it does not, and folds into an
existing metadata query.

### D. How the decode path is chosen

Not a decision — a consequence. The client routes on what actually arrived:

```
0xA7 / 0xAF with the collation's fUTF8 bit  → direct copy into the vector (already implemented)
0xA7 / 0xAF without fUTF8                   → issue #224: fail, naming the column and collation
0xE7 / 0xEF                                 → UTF-16 batch decode (spec 055)
```

This is why the request side can be optimistic: if the server declines the feature, or
ignores our `COLLATE`, the bytes still describe themselves and the client still reads them
correctly — it simply does not get the speed-up.

## Platform notes

The algorithm deliberately never asks "is this Fabric?". It asks whether the feature was
acked, what the database collation is, and whether a UTF-8 collation exists — all observable.
The notes below are context, and the two marked *unverified* have not been tested against a
live endpoint.

- **Fabric Warehouse** stores strings as UTF-8 and its database collation is a UTF-8 one, so
  its columns report code page 65001 and take the first branch: no cast at all, and the
  LOGIN7 feature alone delivers the win. *Unverified against a live warehouse.*
- **Synapse dedicated SQL pool** — UTF-8 collation support needs checking. If UTF8SUPPORT is
  not acked, or the collation probe comes back NULL, every branch above falls through to
  today's path with no special-casing. *Unverified.*
- **Azure SQL Database** supports UTF-8 collations on the 2019+ engine.

## Guards

- **Do not let the rewrite leak into `WHERE` or `ORDER BY`.** The cast belongs to the
  projection only. `Latin1_General_100_BIN2_UTF8` is case- and accent-sensitive; pushing it
  into a predicate or a sort (spec 039 pushes ORDER BY) would silently change results.
- **`is_utf8` is currently derived from the collation name's `_UTF8` suffix.** Moving it to
  the code page removes a string-matching dependency and gives the single/double-byte
  distinction the algorithm needs.
- The FEATUREEXTACK token is **not** length-prefixed. The generic "skip a USHORT length"
  fallback in `ParseLoginResponse` read the UTF8 ack as a 266-byte length; it had been
  misparsing the FEDAUTH ack the same way since Azure AD support shipped. Fixed — the token
  now has its own walk.
- **All four LOGIN7 builders advertise the feature** (SQL auth, SSPI — which is also the
  Kerberos path — FedAuth and ADAL). Three of them cannot be reached from the local test
  server, so their byte layout is pinned by `test/cpp/test_login7_encoding.cpp` instead: the
  record's declared Length must equal the bytes produced, and the feature list must parse the
  way a server would.
- **The client's UTF8_SUPPORT request carries NO data — `FeatureDataLen` is 0.** Only the
  server's acknowledgement has a byte. Sending a data byte is accepted by SQL Server 2022,
  which acks and switches the wire form anyway, but makes **Azure SQL reject the login** with
  error 18456, "Authentication failed for user ..." — indistinguishable from a wrong
  password. Isolated by probing: an empty feature list connects, `AZURESQLSUPPORT` with a
  data byte connects, `UTF8_SUPPORT` with a data byte does not. No local server reproduces
  it; only running the Azure suite found it.
