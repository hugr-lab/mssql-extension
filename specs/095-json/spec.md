# Spec 095: `json` as structured data -- `json_as`, writes from VARIANT, and pushdown of its fields

Status: **draft**, 2026-11-27, revised 2026-11-28 on oluies's review of #657. The owner's direction of
2026-11-27 is quoted under § The owner's decisions. Its first half -- a backup's `json` read as JSON or VARIANT --
is built as spec 086 PR 6, PR #681. E9b and J1's record are in that PR (`specs/086-…/measured/E9-json.md`) and
reach main with it. So this spec takes over everything else for the type: `TYPE mssql`'s reads, every write
path, and the pushdown of projections and filters on a document's fields. Line references were taken at main
`b4f146cf`.

Markers: **[spec]** a Microsoft open specification (MS-TDS); **[doc]** Microsoft Learn product
documentation; **[measure]** to be measured on a throwaway SQL Server 2025 container before the PR that
depends on it (§ Experiments); **[measured]** measured, recorded in `measured/<id>.md`; **(derived)** a rule
this spec derives from an owner decision rather than one the owner stated, open to review.

## Problem

SQL Server 2025 (and Azure SQL Database / Managed Instance) has a native `json` type. Today, by path:

| Path | Today |
|---|---|
| `TYPE mssql` read | `JSON` (spec 093 D7): the server's text, `varchar(max)` UTF-8 on the wire (`MapJson`, `mssql_column_info.cpp:365`, its table entry at 462, after #672) |
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
  A user's `mssql_scan(…, column_types := ['VARIANT'])` is allowed for a string column, converted by D2's
  kernel. That is a new arm of `ColumnTypeFits` (`mssql_functions.cpp:714`, which today takes a string into
  another string label only), PR 1's work.

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
- **What the server holds (086's E9, measured; review of #657).**
  - A json number is a `numeric` of at most 38 digits.
  - A number written with an exponent becomes `decimal(38, 10)`: `5e-324` is stored as `0.0000000000`, and
    `1e37` and `1e308` are refused.
  - A bare scalar at the top level is refused, and nesting stops at 128.
- **What DuckDB's VARIANT -> JSON cast writes** (measured on a 2.0 build, the review's run):
  - a small DOUBLE such as `1e-20` in exponent form, which the server would store as 0;
  - `1.2345678901234568e-05` in plain decimals;
  - `1e21` and above in exponent form;
  - NaN and infinity bare;
  - a 39-digit HUGEINT in full.
- **So the write kernel refuses by name, before anything is sent, what the server cannot hold as written:**
  - NaN or infinity;
  - a number of more than 38 significant digits;
  - a magnitude past `decimal(38, 0)`, and one of 1e28 or more in the exponent form DuckDB writes from 1e21
    on (the server reads an exponent form as `decimal(38, 10)`, at most 28 integer digits: why `1e37` is
    refused);
  - a non-zero number the server's `decimal(38, 10)` would round to zero or change (an exponent form below
    1e-10, or past 10 fractional digits);
  - a top-level scalar (`5::VARIANT`);
  - nesting past 128.

  Each refusal names the column and the value's path. Whether such a DOUBLE is instead written in plain
  decimals (exact when its digits fit 38) is **DQ-4**.
- **What reads back as it went (J10, [measure]).** Integers, a DOUBLE whose text the server keeps, a DECIMAL's
  digits (read back as a DOUBLE, DuckDB's typing), strings and containers. A VARIANT's DATE / TIMESTAMP / BLOB
  become JSON strings by DuckDB's cast and read back as VARCHAR: documented, not hidden. J10 runs each edge
  above, VARIANT -> `json` -> VARIANT, on every write path.
- **The pushed DML (spec 080).** A VARIANT value in a pushed UPDATE / INSERT … SELECT has no T-SQL form: it
  is vetoed and takes the row path. Under `json_as 'variant'` the existing veto ("a SET value not already of
  the column's type") does not fire, because the value *is* the column's type. So PR 2 adds a rule of its own
  to `NodeWriter`: a VARIANT-typed constant or expression assigned to a `json` column vetoes the node (review
  of #657).
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
  whose value is `null` (DuckDB: `j.a IS NULL` is true for both, and `variant_typeof` says `VARIANT_NULL` for
  both -- measured by the review on a 2.0 build -- so D4 need not tell them apart), key matching (the server
  matches the path against keys **stored as written**, E9b: `"\u0041"` is a stored key, DuckDB's key is `A`),
  keys that are equal only after unescaping (DQ-2: the server keeps plain duplicates out on store, E9), and the
  path literal's escaping. A key the server would match differently than DuckDB is not pushed; if a document can hold a
  key that differs only by its escapes, J5 decides whether any key is safe.
- **Casts are pushed into the scan too** (review of #657, `remove_unused_columns.cpp:1694-1701`,
  `multi_file_function.hpp:917`).
  - For any target but JSON, the optimizer puts the cast's type on the path, so `j.a::INTEGER` asks the scan
    for an INTEGER.
  - The kernel builds the member's VARIANT and applies DuckDB's own cast, errors included. Today `j.a::INTEGER`
    over a member `"C"` throws "Can't convert VARIANT(VARCHAR) value 'C' to 'INTEGER'", and so will a pushed
    read.
- **The callback is per column, not per path** (`supports_pushdown_extract(const FunctionData &, const
  LogicalIndex &)`). So "a key the server would match differently is not pushed" cannot be answered there.
  - The scan accepts the extract for the column.
  - For each path whose keys are not safe (J5), it reads the whole document and extracts on the client, through
    the same kernel.
  - The pruner already turns extract pushdown off for a function with `statistics` (line 1238), and the
    catalog scan sets none.
- **The gain.** Only the members cross the wire. A scan of `SELECT j.a FROM t` over 1 MB documents moves
  bytes per row, not the megabyte (J9).

### D5: filter pushdown on a field -- a superset DuckDB re-checks

- **The rule (derived, spec 094 D0's for spatial predicates).** A pushed field filter is a condition every row
  DuckDB keeps satisfies, and DuckDB keeps its own filter on top. So the server may let rows through, never
  drop one. It rests on spec 094's D0 (#622), the superset kind of encoded filter: today encoding a filter
  erases it (`table_scan_pushdown.cpp:332`). **PR 4 is not built before D0 is merged.**
- **D5 needs D4's exactness for the same path** (review of #657). With D4 on, the pruner pushes the extract for
  the kept filter too. DuckDB's re-check then evaluates the member the server's `JSON_QUERY` returned, not the
  document's. A key the server resolves differently (E9b's escaped keys) would give the re-check the wrong
  member. So a path is filtered on the server only where D4 pushes it exactly; any other path under a pushed
  filter reads the whole document (D4's client-side extract).
- **How DuckDB compares a VARIANT at the pin** (measured by the review). `j.a > 4` is planned as
  `variant_comparator(j.a) > <BLOB>`, a total order over tagged values, not a numeric comparison:

  | Query | Members DuckDB keeps |
  |---|---|
  | `j.a > 4` | `5`, `"5"`, `4.6`, `{"x":1}`, `" 5 "`, `5.0`, `"5.0"`, `[5]`, `"c"`, `"C"` |
  | `j.a < 4` | `true`, `1` |
  | `j.a BETWEEN 4 AND 6` | integer `5` only (not `4.6`, not `5.0`) |

  **So no range on a field is pushed**: `TRY_CAST(JSON_VALUE(…) AS float)` against bounds drops the objects,
  arrays, booleans and strings DuckDB keeps. Equality is stricter in DuckDB than the forms below, so they stay
  supersets: `j.a = 5` matches an integer `5` only (not `"5"`, not `5.0`), and `j.a = '5'` matches the string
  only.
- **Forms, each a superset [measure J7]:**
  - `j.a = c` (an integer or a number): `TRY_CAST(JSON_VALUE(j, '$."a"') AS float) BETWEEN lo AND hi`, `[lo, hi]`
    the doubles one ulp around `c`. On 2022, 2 of 3,000 random decimal strings came back one ulp from the
    correctly rounded double and none further (the review), so the window holds them. J7 checks it on 2025.
  - A number is never long enough for the lax mode's 4,000-character NULL: a `numeric(38)` member's text is
    about 41 characters at most (the review's question, closed).
  - `j.a = 'c'` (a string, `c` at most 4,000 characters): `JSON_VALUE(j, '$."a"') = @c`. A binary-equal
    value is equal under any collation, and a longer value cannot equal `c`. Under a CI collation the server's
    `=` also matches `C` and `c  `, so the form is a superset as claimed (the review, on 2022).
  - `j.a IN (…)`: the OR of those.
- **The encoder's arm.** It recognises `variant_comparator(variant_extract(j, 'a')) = <BLOB>` and reads the
  constant's VARIANT tag from the blob to choose the number or the string form.
- **Never pushed:** `<>`, NOT, `IS NULL` / `IS NOT NULL` on a field, and any range. Only AND and OR propagate a
  superset (094 D0's allow-list). Also never pushed:
  - a filter under a cast DuckDB applies differently than `TRY_CAST`;
  - a field of an array element;
  - anything over two fields.

  `JSON_CONTAINS` (2025, preview [doc]) is not used while it is in preview.
- **The gain.** The server returns the matching rows only, and a JSON index on `j` (2025 [doc]) can seek
  `JSON_VALUE`: measured, not assumed (J9).
- **The off switch** (review of #657): `mssql_json_pushdown` (default true), read once per catalog at ATTACH like
  `mssql_remote_pushdown`, with an ATTACH option `json_pushdown` that wins. Off, no field projection (D4) and
  no field filter (D5) is pushed. It is the escape hatch for DQ-2's answer, and what a reviewer flips to
  compare.

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
| J6 | Keys equal only after unescaping ("\u0041" beside `"A"`): what the server stores, which a path returns, what DuckDB's VARIANT keeps (plain duplicates: answered by E9, the first kept on store) | D4, DQ-2 |
| J7 | Each D5 form over 086's corpora: the server's rows a superset of DuckDB's, and how many more | D5 |
| J8 | Lax / strict mode, members past 4,000 characters, nesting at 128 | D4, D5 |
| J9 | Gain: bytes and wall for a field over 1 KB / 100 KB / 1 MB documents; a JSON index's seek | D4, D5 (documented) |
| J10 | Write fidelity: numbers at each edge of D3 (tiny, 1e21 / 1e28 / 1e30, 38 / 39 digits, NaN, a top-level scalar, depth 128 / 129), VARIANT -> `json` -> VARIANT on every write path | D3 |
| J11 | Detecting the type: what `TYPE_ID('json')` and `sys.types` answer by compatibility level, on Azure SQL, Fabric Warehouse and Synapse; whether the probe can ride the ATTACH collation query | DQ-1, D1 |

## Owner's questions

- **DQ-1: what a CTAS / COPY creates.** Today a `JSON` column becomes `nvarchar(max)` / `varchar(max)` and a
  `VARIANT` `sql_variant`.
  - **Proposal:** a `JSON` column becomes `json` on a server that has the type (2025, Azure SQL; J11's probe at
    ATTACH), else as today. A `VARIANT` stays `sql_variant` (T2), since a VARIANT holding an object or array
    then fails by name, and `x::JSON` creates a `json` column.
  - **The alternative** is `json` for every VARIANT on such a server. A scalar VARIANT (`5::VARIANT`) cannot be
    stored in `json` at all, since a top-level scalar is refused (E9), and D3 refuses it by name.
- **DQ-2: keys equal only after unescaping.** E9 (on main) already answers plain duplicates: the server keeps
  the first on store (`{"a":1,"a":2}` is `{"a":1}`), so a `json` column holds none, and DuckDB's "last wins"
  never sees one (review of #657). What is left is two stored keys the server holds as distinct, an escaped one
  and its plain twin, which DuckDB's VARIANT unescapes into one key, keeping the last. A field of such a
  document differs pushed and not. Proposal: such a key is unsafe (D4 reads the document on the client for
  it); J5 decides whether any key can be told safe, and else no key is pushed at all.
- **DQ-3: preview features.** `WITH ARRAY WRAPPER` is a 2025 preview [doc]; D4 rests on it. Proposal: use it
  where the server accepts it, and push no projection where it does not.
  - **The probe.** A `SELECT JSON_QUERY(N'{"a":1}', '$."a"' WITH ARRAY WRAPPER)` once per catalog at ATTACH,
    beside the collation query.
  - **If it goes stale** (a preview switched off at runtime, which is not checked), a pushed scan fails with
    the server's error. The error then names the setting that turns D4 off (`mssql_json_pushdown`), and the
    catalog marks the feature absent for the rest of its life, so the next scan reads documents.
- **DQ-4: a DOUBLE the server would change** (D3). Refuse it by name (as proposed), or write it in plain decimals
  where its digits fit 38 (exact), refusing only the rest? Plain decimals would also take an exponent form's
  1e28 .. 1e38, which `decimal(38, 10)` refuses.

## Tests and acceptance

- R1: `TYPE mssql` reads 086's corpora (restored into the integration server) as JSON and VARIANT; each
  VARIANT equals the text's `::JSON::VARIANT` and the backup's `json_as 'variant'` read of the same
  database (one fixture, three paths).
- R2: every write path writes a VARIANT into `json` and reads it back as J10 records, or refuses it by name
  (D3's list);
  `sql_variant` targets unchanged (T2's tests pass).
- R3 / R4: for each J4 / J7 case, the pushed query's answer equals the unpushed one's (`SET
  mssql_json_pushdown = false` before the ATTACH, D5's off switch); EXPLAIN shows the server's expression. A
  static filter on one field and the join's on another, and a cast pushed with the path (`j.a::INTEGER` over
  a member `"C"`: DuckDB's error, pushed or not).
- R5: the 079 writer's corpus gains field projections; conditions on fields hand the node back.
- On servers without the type (2019, 2022) nothing changes: the tests skip by version.

## Tasks

| PR | Content | Depends on |
|---|---|---|
| 1 | J2, J3; D1 + D2: `json_as` for `TYPE mssql`, the kernel's text source, `ColumnTypeFits`' string -> VARIANT arm | 086 PR 6 (#681) merged |
| 2 | J10; D3: writes of VARIANT into `json` on every path, the refusals by name, the pushed DML's veto; DQ-1's CTAS / COPY types | PR 1, DQ-1, DQ-4 |
| 3 | J4 - J6, J8, J11; D4: `supports_pushdown_extract`, the path expressions, the unwrap, the client-side fallback per path, `mssql_json_pushdown` | PR 1, DQ-2, DQ-3 |
| 4 | J7, J9; D5: field filters as supersets | PR 3, spec 094's D0 (#622) merged |
| 5 | D6: fields in the 079 writer's select list | PR 3 |

PRs 1 and 2 stand on 086 PR 6 alone. **PRs 3 - 5 are not schedulable until J4 - J8 and J11 are recorded**, and
PR 4 also waits for spec 094's D0 (review of #657: the pushdown rests on a preview feature, on 094's D0 and on
experiments not yet run).

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
