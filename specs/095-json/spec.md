# Spec 095: `json` as structured data -- `json_as`, writes from VARIANT, and pushdown of its fields

Status: **draft**, 2026-11-27. The owner's direction of 2026-11-27 is quoted under § The owner's decisions.
Its first half -- a backup's `json` read as JSON or VARIANT -- is built as spec 086 PR 6 (committed; its PR
opens after #655, which it builds on), so this spec takes over everything else for the type: `TYPE mssql`'s reads, every write path, and the
pushdown of projections and filters on a document's fields. Line references were taken at main `15e9b20d`.

Markers: **[spec]** a Microsoft open specification (MS-TDS); **[doc]** Microsoft Learn product
documentation; **[measure]** to be measured on a throwaway SQL Server 2025 container before the PR that
depends on it (§ Experiments); **[measured]** measured, recorded in `measured/<id>.md`; **(derived)** a rule
this spec derives from an owner decision rather than one the owner stated, open to review.

## Problem

SQL Server 2025 (and Azure SQL Database / Managed Instance) has a native `json` type. Today, by path:

| Path | Today |
|---|---|
| `TYPE mssql` read | `JSON` (spec 093 D7): the server's text, `varchar(max)` UTF-8 on the wire (`mssql_column_info.cpp:410-417`) |
| raw `mssql_scan` | `VARCHAR`, the describe's type |
| backup read (086 PR 6) | `JSON`, or `VARIANT` under `json_as 'variant'`, from the stored binary form |
| write (INSERT, COPY, CTAS target, UPDATE, MERGE) | a string only (`target_resolver.cpp:779` maps VARCHAR to `json`); a `VARIANT` value is refused unless the column is `sql_variant` (`type_family.cpp:126-131`) |
| CTAS / COPY creating a table | a `JSON` column becomes `nvarchar(max)` / `varchar(max)` like any VARCHAR; a `VARIANT` column `sql_variant` (`mssql_ddl_translator.cpp:450`) |
| filter on a field | none: a JSON's functions (`->`, `->>`, `json_extract`) and a VARIANT's (`variant_extract`, `.field`) run in DuckDB over the whole document, read whole |
| projection of a field | none: the whole document crosses the wire to give one member |

Three consequences:
- A document is text to `TYPE mssql`: a user who wants `t.j.a.b` writes `json_extract` over text, or
  `::VARIANT` per query, and the backup's `json_as 'variant'` has no counterpart on the live path.
- A VARIANT cannot be written back into the `json` column it was read from.
- Selecting one field of a 1 MB document moves the megabyte; filtering on a field reads every row.

## Scope

In scope:
- **R1:** `json_as` (`'json'` default, `'variant'`) for `TYPE mssql`, the same option the backup reader
  takes, read once per catalog at ATTACH.
- **R2:** writes of `VARIANT` and `JSON` into a `json` column on every write path, and the type a CTAS /
  COPY creates for them.
- **R3:** projection pushdown of a VARIANT's fields (`j.a.b`, `variant_extract(j, 'a')`) into the scan.
- **R4:** filter pushdown on those fields, as a server-side superset that DuckDB re-checks.
- **R5:** the remote-pushdown writer (spec 079): a field in a pushed SELECT's select list.

Out of scope:
- `xml` (the same treatment, a separate spec; MS-BINXML from a backup is spec 086 PR 4).
- The json extension's functions over a `JSON` column (`->>`, `json_extract_string`, `json_exists`) in the
  079 writer: their text output differs from the server's in ways J7 is to measure first. A later step of
  this spec or its own.
- A json document stored in `nvarchar(max)` (pre-2025): it is a VARCHAR; nothing here applies to it.
- The server's JSON index as a planner input (its seek is a gain § D5 measures, not a decision input).

## The owner's decisions

Owner, 2026-11-27, on spec 086 PR 6, widened to the whole extension:

- **Two read forms:** "два режима varchar (json) и variant -- что бы читать и писать напрямую в json, с
  последующим пушдауном фильтров и запросов на сервер".
- **DuckDB's semantics for VARIANT (option (a)):** a VARIANT is the one DuckDB's own `CAST(text AS
  JSON)::VARIANT` gives -- an integer `UBIGINT` / `BIGINT`, any other number `DOUBLE`, keys unescaped,
  one child per key (the last). The backup builds it directly from the binary form and equals the cast for
  every row of its fixture (086 PR 6, 10,469 rows).
- **Binary over the wire where the server speaks it, else DuckDB:** "чтения по проводу (если умеет, если
  нет то тоже duckdb)"; "запись из вариант в json в бинарном формате если BCP это понимает, если нет то
  используем функциональность duckdb что бы преобразовать из variant в json (varchar) перед записью".
  J1 answered the read half: the server sends text (below). The write half is J3.
- **Pushdown of projections and filters on a VARIANT's fields:** "pushdown projection и pushdown фильтров по
  полям json из variant".

### J1: the wire carries text, JSONSUPPORT or not [measured]

SQL Server 2025 RTM-CU10 (17.0.5035.5), 2026-11-27 (recorded in 086's `measured/E9-json.md`, E9b):
- Without JSONSUPPORT, a `json` column is `varchar(max)` (TDS `A7 FFFF`, a UTF-8 collation) carrying the
  document's text.
- With JSONSUPPORT (`0x0D`, version 1) requested in LOGIN7, the server grants it in FEATUREEXTACK and still
  sends `A7 FFFF` text. No JSONTYPE `0xF4` appears.
- [MS-TDS] 2.2.5.4.3 says the same for 0xF4: the value is a PLP of RFC 8259 text, not the storage form.

So the binary form is a storage form only. A live read is text, converted by DuckDB (option (a) as stated).

## Design

### D1: `json_as` for `TYPE mssql`

- **The option.** `mssql_json_as` (GLOBAL, `'json'` | `'variant'`), the ATTACH option `json_as` (wins),
  and the secret / connection-string / URI key `json_as`, as `datetime2_7_as` (spec 093 D8). Read once per
  catalog at ATTACH: a catalog never mixes the two. `ATTACH … (TYPE mssql_backup)` reads the same setting
  where its own option and the call's are absent (086 PR 6 takes the option and the parameter today).
- **The type.** A `json` column reports `JSON` or `VARIANT` (`MapSQLServerTypeToDuckDB(…, json_as)`); the
  entry's own `columns_` carry it to the scan, INSERT column types, the 079 vehicle's `column_types` and the
  describe-cache key, as `native_types` does (issue #415's rule: never read the setting again).
- **A raw scan** keeps `VARCHAR`: the describe says `varchar(max)` and cannot tell a json column from text.
  A user's `mssql_scan(…, column_types := ['VARIANT'])` is allowed for a string column (`ColumnTypeFits`: a
  string into VARIANT), converted by D2's kernel.

### D2: the read kernel -- text to VARIANT on the client

- `FinalizeKernel::Json` (086 PR 6, `src/codec/json/json_codec.cpp`) gains its text source: the staged
  UTF-8 text of a chunk is converted by DuckDB's own JSON -> VARIANT cast
  (`duckdb/src/include/duckdb/function/cast/variant/json_to_variant.hpp`), called once per chunk on the
  column's staged strings, not per value. So the TDS path and the backup path give one VARIANT: the backup
  builds it from the binary form (086), the wire's text goes through the cast it is proved equal to.
- `ColumnMetadata::json_text` marks the column (set by whoever reads COLMETADATA for a catalog, as
  `geometry_as`); `json_disk_form` stays the backup's.
- A value DuckDB's parse refuses is NULL with one WARNING per column, as the backup's: the cast is a
  `TryCast`, never a throw. The server can hand one over: it takes a lone surrogate (`"\uD800"`) in a key and
  keeps it as written (086 E9b, measured), and DuckDB's parse refuses it ("no low surrogate"). Under
  `json_as 'json'` the text is the server's, that key included.
- **J2** measures the cost: VARIANT vs JSON over 1M documents of 086's corpora, wall and client CPU.

### D3: writes into a `json` column

- **A `JSON` / `VARCHAR` value** is written as today: text, validated by the server.
- **A `VARIANT` value** is converted to JSON text by DuckDB's VARIANT -> JSON cast
  (`duckdb/src/function/cast/variant/to_json.cpp`), then takes the string path: INSERT BULK (COPY, INSERT
  past the threshold, CTAS, `#stage`, `#src`), the VALUES literal (`FormatSqlLiteral`, an `N'…'` or UTF-8
  literal), the UPDATE / DELETE VALUES join, and `mssql_exec_params` / `mssql_scan_params` parameters.
  `IsTypeCompatible` admits VARIANT into `json` beside `sql_variant` (T2). The choice is the target
  column's: `sql_variant` takes T2's envelope, `json` the text.
- **A binary form on the wire.** J1's text-only read and [MS-TDS]'s definition of 0xF4 say no client
  sends the storage form. J3 measures whether INSERT BULK takes a column declared 0xF4 at all (with
  JSONSUPPORT); either way the value it carries is text, so the write is DuckDB's text (owner: "если нет
  то используем функциональность duckdb").
- **Fidelity (derived).** VARIANT -> JSON -> the server's decimal -> JSON -> VARIANT round-trips: an integer
  as UBIGINT / BIGINT, a DOUBLE as its shortest round-trip text (read back as the same DOUBLE), a DECIMAL as
  its digits (read back as a DOUBLE, DuckDB's typing of the text). A VARIANT's DATE / TIMESTAMP / BLOB
  become JSON strings by DuckDB's cast and read back as VARCHAR: documented, not hidden.
- **The pushed DML (spec 080).** A VARIANT value in a pushed UPDATE / INSERT … SELECT has no T-SQL form: it
  is vetoed and takes the row path (the existing "a SET value not already of the column's type" veto).
- **CTAS / COPY creating a table:** **DQ-1**.

### D4: projection pushdown of a VARIANT's fields

- **DuckDB's lever.** `TableFunction::supports_pushdown_extract` (`duckdb/src/include/duckdb/function/
  table_function.hpp:378, 534`): for a VARIANT column the optimizer (`remove_unused_columns.cpp:1606-1650`,
  `HandleVariantExtract`) hands the scan a `ColumnIndex` path of keys -- `j.a.b` is child `a`, child `b` --
  and keeps the extract above it only when the scan says no. By key only: an array index is not pushed
  (`BY_KEY`), nor is a cast to JSON (`ExtractCastCanBePushedDown`). Parquet's shredded VARIANT is the
  precedent (`parquet_multi_file_info.cpp:120-126`).
- **The server's form.** Each pushed path is one select-list item. **J4:** `JSON_QUERY(j, '$."a"."b"' WITH
  ARRAY WRAPPER)` (SQL Server 2025, preview [doc]) returns the member as a one-element array whatever its
  type -- a scalar included, which a bare `JSON_QUERY` returns as NULL and `JSON_VALUE` returns without its
  type (`5` and `"5"` both `5`). The kernel converts the text and takes the array's element: the VARIANT
  `variant_extract(VARIANT(j), 'a')` would give, if J4 holds for every value of 086's corpora.
- **What the server must agree on (J5, J6).** A missing member (lax mode: NULL; DuckDB: NULL), a member
  whose value is `null` (DuckDB: a VARIANT null, not a SQL NULL -- to measure), key matching (the server
  matches the path against keys **stored as written**, E9b: `"\u0041"` is a stored key, DuckDB's key is `A`),
  duplicate keys (DuckDB keeps the last; the server's path returns the first?), and the path literal's
  escaping. A key the server would match differently than DuckDB is not pushed; if a document can hold a
  key that differs only by its escapes, J5 decides whether any key is safe.
- **The gain.** Only the members cross the wire. A scan of `SELECT j.a FROM t` over 1 MB documents moves
  bytes per row, not the megabyte (J9).

### D5: filter pushdown on a field -- a superset DuckDB re-checks

- **The rule (derived, spec 094's for spatial predicates).** A pushed field filter is a condition every row
  DuckDB keeps satisfies; DuckDB keeps its own filter on top. So the server may let rows through, never drop
  one. This sidesteps the semantic gaps exactness would have to close: the server's collations (D4 of 079),
  `JSON_VALUE`'s text typing, its 4,000-character lax NULL.
- **Forms, each a superset [measure J7]:**
  - `j.a = c` (number): `TRY_CAST(JSON_VALUE(j, '$."a"') AS float) BETWEEN lo AND hi`, `[lo, hi]` the
    doubles one ulp around `c` (a decimal member rounds to the same double DuckDB parses, or a neighbour).
  - `j.a = 'c'` (string, `c` at most 4,000 characters): `JSON_VALUE(j, '$."a"') = @c`. A binary-equal
    value is equal under any collation; a longer value cannot equal `c`.
  - `j.a IN (…)`: the OR of those; `j.a IS NOT NULL`: `JSON_PATH_EXISTS(j, '$."a"') = 1` (if J5 shows a
    `null` member is NULL in DuckDB, the superset still holds).
  - A range on a number: the same `float` bounds widened by an ulp. A range on a string: not pushed (order
    is the collation's).
- **Not pushed:** a filter under a cast DuckDB applies differently than `TRY_CAST` (a string to a number in
  DuckDB's terms), a field of an array element, anything over two fields. `JSON_CONTAINS` (2025, preview
  [doc]) is not used while it is in preview.
- **The gain.** The server returns the matching rows only, and a JSON index on `j` (2025 [doc]) can seek
  `JSON_VALUE` -- measured, not assumed (J9).

### D6: the remote-pushdown writer (spec 079)

- A pushed SELECT names a field in its select list as D4's `JSON_QUERY … WITH ARRAY WRAPPER`, its vehicle
  column typed VARIANT and unwrapped by the kernel.
- A pushed statement has no DuckDB filter above its vehicle, so D5's supersets cannot go there: a condition
  on a field vetoes the node (the rewriter's existing fallback hands it back with the parts that gain
  pushed). An exact form, if J7 finds one, is a later step.
- A JSON column in a pushed statement keeps D7's typing (`JSON` named as the core's type, 093 D7).

## What changes for a user

- `json_as 'variant'` makes `t.j.a.b` a field access with no cast, on a live catalog as on a backup.
- A VARIANT computed in DuckDB can be written into a `json` column (INSERT, COPY, UPDATE, MERGE).
- `SELECT j.a FROM t WHERE j.b = 5` reads members, not documents, and filters on the server.
- Breaking only by opt-in: the default stays `JSON`.

## Experiments

All on a throwaway SQL Server 2025 container (images.txt's digest), recorded in `measured/J<n>.md`.

| Id | Question | Decides |
|---|---|---|
| J1 | Does the wire ever carry the binary form? **[measured]**: no | D2 |
| J2 | Cost of VARIANT vs JSON on read, 1M documents | D2's default stays `json` either way; the number is documented |
| J3 | INSERT BULK into `json`: which wire types it takes (varchar(max) UTF-8, nvarchar(max), 0xF4 under JSONSUPPORT), the error and its effect on a batch for invalid text | D3 |
| J4 | For every document and member path of 086's corpora: is the converted, unwrapped `JSON_QUERY(… WITH ARRAY WRAPPER)` the VARIANT `variant_extract` gives? Numbers as stored, strings, nested containers, `null` | D4 |
| J5 | Path matching: case, keys stored with escapes (`"\u0041"`, `"a\/b"`), `"` and `\` in a key, the empty key, the path literal's escaping | D4, D5 |
| J6 | Duplicate keys: what a `json` column stores, which one a path returns | D4 |
| J7 | Each D5 form over 086's corpora: the server's rows a superset of DuckDB's, and how many more | D5 |
| J8 | Lax / strict mode, members past 4,000 characters, nesting at 128 | D4, D5 |
| J9 | Gain: bytes and wall for a field over 1 KB / 100 KB / 1 MB documents; a JSON index's seek | D4, D5 (documented) |

## Owner's questions

- **DQ-1: what a CTAS / COPY creates.** Today a `JSON` column becomes `nvarchar(max)` / `varchar(max)` and a
  `VARIANT` `sql_variant`. Proposal: a `JSON` column becomes `json` on a server that has the type (2025,
  Azure SQL; probed at ATTACH), else as today; a `VARIANT` stays `sql_variant` (T2), since a VARIANT holding
  an object or array then fails by name, and `x::JSON` creates a `json` column. The alternative is `json`
  for every VARIANT on such a server (a scalar VARIANT then loses its base type, which sql_variant keeps).
- **DQ-2: duplicate keys.** If J6 shows the server keeps both and its path returns the first while DuckDB's
  VARIANT keeps the last, a field of such a document differs pushed and not. Proposal: push anyway and
  document it (a document with duplicate keys is outside RFC 8259's interoperable subset); else no key is
  pushed at all.
- **DQ-3: preview features.** `WITH ARRAY WRAPPER` is a 2025 preview [doc]; D4 rests on it. Proposal: use it
  where the server accepts it (probed once per catalog), and push no projection where it does not.

## Tests and acceptance

- R1: `TYPE mssql` reads 086's corpora (restored into the integration server) as JSON and VARIANT; each
  VARIANT equals the text's `::JSON::VARIANT` and the backup's `json_as 'variant'` read of the same
  database (one fixture, three paths).
- R2: every write path writes a VARIANT into `json` and reads it back equal (D3's fidelity cases listed);
  `sql_variant` targets unchanged (T2's tests pass).
- R3 / R4: for each J4 / J7 case, the pushed query's answer equals the unpushed one's (`SET
  mssql_json_pushdown = false`, a test-only lever, or EXPLAIN showing what went); EXPLAIN shows the
  server's expression.
- R5: the 079 writer's corpus gains field projections; conditions on fields hand the node back.
- On servers without the type (2019, 2022) nothing changes: the tests skip by version.

## Tasks

| PR | Content | Depends on |
|---|---|---|
| 1 | J2, J3; D1 + D2: `json_as` for `TYPE mssql`, the kernel's text source | 086 PR 6 merged |
| 2 | D3: writes of VARIANT into `json` on every path; DQ-1's CTAS / COPY types | PR 1, DQ-1 |
| 3 | J4 - J6, J8; D4: `supports_pushdown_extract`, the path expressions, the unwrap | PR 1, DQ-2, DQ-3 |
| 4 | J7, J9; D5: field filters as supersets | PR 3 |
| 5 | D6: fields in the 079 writer's select list | PR 3 |

## Risks

- `WITH ARRAY WRAPPER` and `JSON_CONTAINS` are previews: their behaviour may change in a CU. Probed per
  catalog; the tests pin the measured behaviour.
- DuckDB's VARIANT STRUCT names are case-insensitive: a VARIANT whose keys differ only in case
  (`{"z":1,"Z":2}`) reads and compares, but its cast to VARCHAR fails in DuckDB itself ("duplicate name
  "z" in struct"; measured on the pin, 086 PR 6's row 27). Not ours to fix (no upstream patches); documented.
- A pushed superset that is not one loses rows silently. Every D5 form needs J7's corpus check, and the
  tests compare pushed and unpushed answers.

## References

- 086's `measured/E9-json.md` (E9, E9b, J1); 086 PR 6 (`src/codec/json/`).
- Spec 093 D7 (`JSON` for `TYPE mssql`), D8 (`datetime2_7_as`, the per-catalog option pattern), T2
  (VARIANT into `sql_variant`).
- Spec 094 (supersets DuckDB re-checks, for spatial predicates).
- DuckDB: `json_to_variant.hpp`, `to_json.cpp`, `remove_unused_columns.cpp` (`HandleVariantExtract`),
  `table_function.hpp` (`supports_pushdown_extract`).
- [JSON_QUERY (Transact-SQL)](https://learn.microsoft.com/sql/t-sql/functions/json-query-transact-sql)
  (`WITH ARRAY WRAPPER`, 2025 preview); [JSON_CONTAINS](https://learn.microsoft.com/en-us/sql/t-sql/functions/json-contains-transact-sql)
  (2025 preview); [JSON data in SQL Server](https://learn.microsoft.com/en-us/sql/relational-databases/json/json-data-sql-server)
  (the `json` type: GA on Azure SQL, preview on SQL Server 2025).
