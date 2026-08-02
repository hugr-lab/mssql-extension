# Spec 060 — Target column types and DDL options

**Status:** Implemented — PR #230. See § 9 for what shipped beyond this draft and what did not.
**Date:** 2026-07-31, revised 2026-08-01
**Depends on:** spec 049 (target's `index_kind` / `partition_count` in the catalog cache — PR #223); issue #225 (the UTF8SUPPORT ack and `mssql_utf8_collation`, which is what makes the single-byte types safe — PR #227)
**Feeds:** spec 057 (the write path's sizing policy resolves a column bound from what this spec adds)

---

## 1. The problem, measured

When the extension creates the target table itself, every string column becomes
`nvarchar(max)`. Loading into such a table measured **4.1× slower** than the same
load into a properly typed one (spec 057, "External validation": 59.29 s →
14.42 s on 2M rows × 44 columns), and `nvarchar(max)` is also what blocks a
columnstore target, which is where that article's 56× storage advantage came
from. So one default costs both speed and storage.

The obvious fix — "carry the length through from the source" — does not exist:

- **DuckDB discards the string length modifier.** `CREATE TABLE t (a VARCHAR(20))`
  reports `a VARCHAR`; `VARCHAR(20)` and `VARCHAR` are the same type. Verified.
- **Parquet has no string length either.** A string column is `BYTE_ARRAY` with
  the `UTF8` logical type. `DECIMAL(9,2)` by contrast survives both (DuckDB keeps
  precision and scale, Parquet stores `DecimalType`), which is exactly why
  numeric columns are already created correctly and string columns are not.

There is therefore nothing to plumb. The length has to be *stated* — by the
target table, by the user, or by a default.

## 2. What DuckDB gives us — verified, not assumed

`ExtensionLoader::RegisterType(name, type, bind_function)` takes a
`bind_logical_type_function_t`, which receives `BindLogicalTypeInput` with
`const vector<TypeArgument> &modifiers`. The idiom (from DuckDB's own
`loadable_extension_demo.cpp`) is:

```cpp
auto type = LogicalType(LogicalTypeId::VARCHAR);
type.SetAlias("MSSQL_NVARCHAR");
auto info = make_uniq<ExtensionTypeInfo>();
info->modifiers.emplace_back(Value::INTEGER(length));
type.SetExtensionInfo(std::move(info));
```

**This is why the approach is cheap.** The bound type is a plain `VARCHAR` at
runtime, so every codec, staging and BCP path stays untouched — the length rides
in the type's extension info and is read only where DDL is generated. No new
physical type, no new encode arm.

The one thing that must be proved before anything is built on it: that the alias
and extension info **survive the plan** as far as `copy_to_bind`'s `sql_types`
and the CTAS column list. That is D1's acceptance test, not an assumption.

## 2a. Probed — the alias is required, and the alias-free form crashes DuckDB

Three experiments, and the second one reversed the conclusion of the first.

**Registering WITHOUT an alias looks better and is wrong.** Following the demo
literally — `SetAlias("MSSQL_NVARCHAR")` on the bound type — makes it a distinct
type for binding, and every operation on it fails with "No function matches":
`upper()`, `||`, `length()`, `=`, `LIKE`, `IS NULL`, `count(DISTINCT)`. Dropping
the alias fixes all of that: the value behaves as an ordinary VARCHAR while the
modifier still reaches DDL generation.

**But an anonymous extension type crashes the binder.** With no alias:

```sql
CREATE TABLE t (a MSSQL_NVARCHAR(50));
INSERT INTO t VALUES ('x');            -- SIGSEGV, exit 139
```

No MSSQL catalog, no attached database — plain DuckDB. `INSERT ... SELECT` on
the same table is fine, and a bare cast is fine; the trigger is binding a VALUES
list against a column whose type carries `ExtensionTypeInfo` with no alias. The
signature is a stack overflow (`EXC_BAD_ACCESS` in `LogicalType::GetInternalType`
with an unwindable-past stack), i.e. unbounded recursion at bind time.

| form | `upper(col)` | `INSERT ... VALUES` |
| --- | --- | --- |
| with alias | binder error | **works** |
| without alias | works | **SIGSEGV** |

So DuckDB supports extension types only in their aliased form; the alias-free
variant passes function binding by looking like a VARCHAR and then breaks the
part of the binder that needs a real type. **Use the alias.** The ergonomic cost
lands entirely on a type that exists to *declare* a target column: it is written
in a cast that feeds the sink, and nothing is applied to it afterwards.
`upper(cust)::MSSQL_NVARCHAR(50)` — cast last — is the shape that matters and it
works.

**Report upstream.** An extension can produce a type that segfaults the binder,
with the repro above. Whether DuckDB should reject an anonymous extension type
at registration or handle it, a stack overflow is not the answer.

### The catalog must NOT report these types

The attractive version is to have the catalog surface a source column's declared
length, so a target created from an MSSQL source inherits it with no cast at all.
It was implemented and it *worked* — with one further change, narrowing the
issue-#89 divergence guard in `ResolveColumnOps`, which compares whole
`LogicalType`s and so treated an annotated VARCHAR as diverging from the plain
VARCHAR the wire produces, `COPY (SELECT * FROM d.dbo.Src) TO 'd.dbo.Dst'`
created `Dst.cust` as `nvarchar(50)`, carried from the source with no user input.

It is rejected all the same, and for a reason independent of that: the catalog
would have to report the type in its **alias-free** form to keep every string
column usable, which is exactly the form that crashes. The aliased form would
make every string column of every attached table unusable without a cast.
Reverted, with the guard change.

### CTAS uses a second translator

The same cast through `CREATE TABLE d.dbo.x AS SELECT ...` still produced
`nvarchar(max)`: that path maps types with
`MSSQLDDLTranslator::MapLogicalTypeToCTAS`, independent of
`TargetResolver::GetSQLServerTypeDeclaration`. Both must be taught, and the
duplication is itself the finding — one decision made in two places is how a
feature ends up working on one path and silently not on the other.

*(Superseded by § 4: there are four such places, not two, and the other two
decide the wire declaration and the client-side length bound.)*

### D3 is NOT already satisfied — the guard counts the wrong thing for `varchar`

An over-long value on the BCP path does fail with our own error naming the
column, the value's length and the limit, in both units:

```
MSSQL: NVARCHAR column 'cust' overflow: value is 60 UCS-2 code units
(120 UTF-16LE bytes) but column max is 50 code units (100 bytes)
```

That is the guard from spec 045 / issue #91, and it is right for `nvarchar`.
It is **wrong for `varchar`**, because the two types do not count the same unit
(§ 3). Verified by loading ten Cyrillic characters — ten UCS-2 code units,
twenty UTF-8 bytes — into a UTF-8-collated `varchar(10)`:

```
IO Error: MSSQL COPY: Failed to finalize BCP stream: ... "MSSQL: BCP failed:
String or binary data would be truncated in table 'TestDB.dbo.T060Vc',
column 'v'. Truncated value: 'Приве'."
```

The client bound passed the value and the server rejected it. The failure is at
least loud — nothing is silently truncated — but it arrives from the server at
*finalize* time, after the whole batch has been sent, wrapped in a JSON
envelope, rather than from our own guard before the first byte goes out.

## 3. What `n` means, per type — measured

The unit is not a property of the number. Four columns, one `CREATE TABLE`,
`sys.columns` immediately after:

| column | `max_length` | `collation_name` |
| --- | --- | --- |
| `varchar(10) COLLATE Latin1_General_100_CI_AS_SC_UTF8` | 10 | `..._SC_UTF8` |
| `varchar(10)` (database default) | 10 | `SQL_Latin1_General_CP1_CI_AS` |
| `nvarchar(10)` | 20 | `SQL_Latin1_General_CP1_CI_AS` |
| `char(10) COLLATE Latin1_General_100_CI_AS_SC_UTF8` | 10 | `..._SC_UTF8` |

**`max_length` alone cannot tell a UTF-8 `varchar(10)` from a code-page
`varchar(10)`** — both report 10 — yet one holds ten *bytes* (two to three
Cyrillic characters) and the other ten *characters*. Only `collation_name`
separates them. Any code that turns a declared length into a client-side bound
therefore needs the collation as an input, and today none of it has one.

So `varchar(n)` has three regimes, and only the middle one is what the current
code assumes:

- **UTF-8 collation** — `n` is UTF-8 bytes. Exactly computable on the client,
  since the value is already UTF-8 in the vector. Over-long input is **rejected**
  by the server (probe above).
- **Single-byte code page** — `n` is bytes, and bytes equal characters. Today's
  `max_length * 2` doubling is accidentally correct here. Over-long input is not
  the risk; *unrepresentable* input is: ten Cyrillic characters INSERT into a
  CP1252 `varchar(10)` as `3F3F3F3F3F3F3F3F3F3F`, which is the silent loss #225
  closed for the columns we create.
- **DBCS code page (932/936/949/950)** — `n` is bytes, and a character costs one
  or two of them. Not computable client-side without the code page tables. Stays
  a server-side error, and this spec does not pretend otherwise.

`nvarchar(n)` has one regime: `n` is UTF-16 code units, `max_length` is `2n`,
and a supplementary character costs two units. That is what the existing guard
measures, which is why it is right there and only there.

## 4. Where the type and the size are decided today — four sites

The spec previously said two translators. There are four, and they disagree:

| # | site | reached from | what it does with a `VARCHAR` |
| --- | --- | --- | --- |
| 1 | `codec::string::FormatDdlTypeName` | CTAS creates the table | `NVARCHAR(MAX)`, or `VARCHAR(MAX) COLLATE <mssql_utf8_collation>` under `mssql_ctas_text_type='VARCHAR'` (#225). Ignores any annotation. |
| 2 | `TargetResolver::GetSQLServerTypeDeclaration(LogicalType)` | COPY creates the table | reads the annotation (the § 2a probe); otherwise `nvarchar(max)` |
| 3 | `GetTDSTypeToken` + `GetMaxLength(LogicalType)`, via `GenerateColumnMetadata` | the `INSERT BULK` declaration and the client guard — **CTAS always**, COPY when `overwrite` | `TDS_TYPE_NVARCHAR` / `0xFFFF` unconditionally, so on those paths the guard never fires at all |
| 4 | `GetMaxLength(type_name, max_length, precision)`, via `GetExistingTableColumnMetadata` | COPY into a table that exists — **including one we created a moment earlier** | reads server metadata; `varchar`/`char` are doubled to UTF-16 bytes, which is where the wrong unit of § 3 enters |

`BCPColumnMetadata::GetSQLServerTypeDeclaration()` then renders the `INSERT
BULK` column text from whatever (3) or (4) produced.

Site 4 is why the `MSSQL_NVARCHAR(50)` overflow above reports our own error:
`ValidateTarget` creates the table, and COPY then re-reads the *server's*
metadata rather than trusting the types it just wrote. CTAS never does that, so
its `INSERT BULK` always declares `nvarchar(max)` no matter what the table says.

Verified on today's build (`e05c569` + this branch):

```
tbl       | col   | type_name | max_length
T060Copy  | sized | nvarchar  |        100   -- COPY honours MSSQL_NVARCHAR(50)
T060Copy  | plain | nvarchar  |         -1
T060Ctas  | sized | nvarchar  |         -1   -- CTAS ignores it
T060Ctas  | plain | nvarchar  |         -1
```

## 5. Where a column's target type comes from

Resolution order, first match wins:

1. **The existing target table.** Already the case, and already the fast path —
   the 4.1× above is precisely the gap between creating the table and inserting
   into a properly created one. Nothing to add.
2. **The column's own type**, either from an explicit cast —
   `CAST(cust AS MSSQL_NVARCHAR(50))` — or **carried from an attached MSSQL
   source by the catalog** (D2), which is the case that needs no user input at
   all: `COPY (SELECT * FROM d.dbo.Src) TO 'd.dbo.Dst'` reproduces `Src`'s
   declared lengths.
3. **A per-statement COPY option** for the maximum string length (D8).
4. **A session default**: `mssql_default_string_length` (D9), MAX by default so
   nothing changes for anyone who does not set it.
5. **Fallback `nvarchar(max)`** — today's behaviour, reached only when 2–4 are
   all silent.

**Data-derived sizing is rejected.** Scanning the first batch to take the longest
value guesses; a later batch that exceeds the guess fails the whole load, and the
failure comes from data the user never saw.

## 6. Deliverables

### D1 — two types, and only two

| type | binds to | emits | `n` counts | range |
| --- | --- | --- | --- | --- |
| `MSSQL_NVARCHAR(n)` | VARCHAR | `nvarchar(n)` | UTF-16 code units | 1–4000 |
| `MSSQL_VARCHAR(n [, collation])` | VARCHAR | `varchar(n) COLLATE <c>` | **bytes** | 1–8000 |

No `CHAR`/`NCHAR`/`BINARY`/`VARBINARY`. A type earns its place by solving a
problem, and those do not have one: `blob` already round-trips through
`varbinary(max)` without loss or surprise, and the fixed-length pair only adds
space padding. `varchar` and `nvarchar` are where the cost is — § 1's 4.1× and
§ 3's unit confusion.

One integer modifier cannot say whether it means `varchar` or `nvarchar`, so
**the alias is the type identity** and the modifier is only the size. The alias
was forced on us by the binder crash in § 2a; it turns out to be load-bearing
rather than a tax.

`n` is SQL Server's own unit for the type named: `MSSQL_VARCHAR(50)` **is**
`varchar(50)`, byte for byte, and `MSSQL_NVARCHAR(50)` is `nvarchar(50)`.
Reinterpreting `n` as characters and widening the column would make the DDL we
emit differ from the DDL the user wrote. The consequence, stated plainly because
it will surprise someone: the same `n` in the two types does not hold the same
data — 50 bytes of UTF-8 is 16 Cyrillic characters (§ 3).

**The optional second modifier is the collation**, and that is what makes the
byte count computable: `MSSQL_VARCHAR(50, 'Cyrillic_General_100_CI_AS_SC_UTF8')`.
It is what goes into the `COLLATE` clause, and it is what tells the client-side
bound which unit to count in. Omitted, it falls back to `mssql_utf8_collation`.

**The bound is informational on the DuckDB side.** The type binds to a plain
`VARCHAR`, so DuckDB neither enforces `n` nor truncates to it — a longer value
sits in the vector untouched. Enforcement exists at exactly two places: our own
guard at the write boundary (below), and the server. Anyone reading
`mssql_varchar(50)` in a `DESCRIBE` and inferring a constraint is reading it
wrong, and the documentation must say so.

**The length guard, in the right unit.** § 2a's probe showed the existing guard
is right for `nvarchar` and wrong for `varchar`. With the collation in hand:

- `nvarchar` — UTF-16 code units. Unchanged.
- `varchar` under a UTF-8 collation — UTF-8 bytes, compared against the bytes
  already in the vector. Cheaper than the UCS-2 count it replaces, not dearer.
- `varchar` under a single-byte code page — the existing doubled bound is
  already correct; leave it.
- DBCS — not computable client-side without the code page tables. Stays a
  server error, documented, not pretended away.

This needs the collation to reach `BCPColumnMetadata`, so
`GetExistingTableColumnMetadata`'s query gains `collation_name` and the metadata
gains a "length is in bytes" flag. Match the `_UTF8` suffix, **not**
`COLLATIONPROPERTY` — #225 found Fabric closes the connection rather than
answering it.

**`MSSQL_VARCHAR` requires the UTF8SUPPORT ack**, on the gate CTAS already uses
for `mssql_ctas_text_type='VARCHAR'` (`MSSQLCatalog::UTF8SupportState()` — throw
on `Declined`, proceed on `Unknown`). A single-byte column with no UTF-8
collation is the silent-`?` trap of #225 and must not come back one column at a
time.

What it buys: half the storage for ASCII-ish data, and the direct-copy read path
of #225 on every later scan.

~~What it does **not** buy: a smaller write wire.~~ It does, in the end. This
paragraph said BCP would keep sending every char type as `TDS_TYPE_NVARCHAR`
and that shrinking the write wire was PR #227's separate item. That turned out
to be one afternoon's work on top of what D1 already carries — the annotation
says the target is a UTF-8 varchar, which is exactly the fact the encoder
needed — so it shipped here. See § 9.

### D2 — the catalog reports these types, and a setting turns that off

Attached MSSQL tables report `mssql_varchar(n)` / `mssql_nvarchar(n)` for their
string columns instead of a bare `VARCHAR`. This is the resolution step that
needs no user input at all: `COPY (SELECT * FROM d.dbo.Src) TO 'd.dbo.Dst'`
reproduces `Src`'s declared lengths because they travelled in the types.

It was already built and measured behind the `MSSQL_ANNOTATE_CATALOG` env var
(§ 2a): it works, and it changes what `DESCRIBE` prints —

```
col_nvarchar  varchar              ->  col_nvarchar  mssql_nvarchar(100)
col_nchar     varchar              ->  col_nchar     mssql_nvarchar(10)
```

— which is why it stayed behind a flag pending a deliberate decision. The
decision: **on by default**, with a setting to restore the plain `VARCHAR`
output for anyone whose tooling reads type names.

### D3 — `CREATE TABLE` / `ALTER TABLE` emit the stated type

`MSSQLDDLTranslator` produces `varchar(n) COLLATE <c>` / `nvarchar(n)` where it
produces `nvarchar(max)` today. This is the path a user reaches by writing the
column type directly, and it is the one that must agree with D2: a table created
here, then read back through the catalog, reports the type it was created with.

### D4 — CTAS and COPY translate the types when they create the target

One resolver — `TryResolveTargetType(const LogicalType &)` returning
`{sql_name, length, collation}`, plus one formatter for the DDL text — feeding
**all four** sites of § 4. Teaching four independent switches the same fact is
precisely how a feature ends up working on COPY and silently not on CTAS, which
§ 4 measured.

Sites 3 and 4 are not only about DDL text: `GetMaxLength` must stop returning
`0xFFFF` unconditionally, or CTAS keeps declaring `nvarchar(max)` in `INSERT
BULK` for a column it created as `nvarchar(50)` — and the guard of D1 never
fires there. `GetTDSTypeToken` stays `TDS_TYPE_NVARCHAR` for every char type;
that is the wire form and it is unchanged here.

Coverage must include the two paths that bypass the guard entirely today — CTAS
always, and COPY with `overwrite` — asserting **our** error, not the server's.

### D5 — `CREATE TABLE ... WITH (...)` for SQL Server table properties

DuckDB already parses a `WITH` clause into `CreateTableInfo::options` and only
`Catalog::SupportsCreateTable` rejects it — a **virtual**, so `MSSQLCatalog`
overrides it and the options arrive at `MSSQLSchemaEntry::CreateTable` with no
parser work at all. Verified: plain DuckDB answers `WITH clause is not supported
for tables in a duckdb catalog`, i.e. it parsed fine and the catalog declined.

Options: the table kind — heap or **clustered columnstore**, which is where § 1's
storage advantage comes from — a clustered index key list, and
`DATA_COMPRESSION = PAGE | ROW`, measured at −51% / −41% storage for +63% / +17%
load time (spec 057). Compression must be documented together with TABLOCK:
it applies **during** a bulk load only when the load takes a table lock,
otherwise rows land uncompressed until someone rebuilds and the user silently
does not get what they asked for. `OPTIMIZE_FOR_SEQUENTIAL_KEY` is deliberately
not exposed — measured neutral under four concurrent loaders, which write
disjoint ascending ranges and leave no hot last page for it to relieve.

`PARTITIONED BY` and `SORTED BY` are rejected by the same virtual and are
adjacent to a clustered index, but they are not in scope here; spec 049 covers
reading partitioned tables, not creating them.

### D6 — the scan builds the right query for the annotated types

With D2 on, the catalog's column types carry an alias and modifiers, and
`BuildColumnExpression` in `table_scan.cpp` decides the `SELECT` expression from
them. It must keep both existing behaviours intact: the UTF-8 direct-copy path
of #225, and the non-UTF8 collation handling of spec 026. The divergence guard
in `ResolveColumnOps` already had to learn that two `VARCHAR`s never diverge
whatever they carry (§ 2a); this is the same class of change one layer up.

### D7 — one option decides the type for an unannotated `VARCHAR`

CTAS and COPY must consult the **same** option. Today `mssql_ctas_text_type`
serves CTAS only, so the two paths can disagree about a plain DuckDB `VARCHAR`.

### D8 — COPY options: target table kind, and maximum string length

Per statement, overriding the session defaults of D9: which kind of table to
create (heap / clustered), and the maximum length to give an unannotated string
column.

### D9 — session defaults for table kind and string length

`mssql_default_string_length` defaults to MAX, so a user who sets nothing sees
byte-identical DDL to today's. The default table kind likewise starts at
today's behaviour.

## 7. Acceptance criteria

1. A `COPY` that creates its target produces sized string columns, and loading
   into it is within noise of loading into a hand-created table with the same
   types — i.e. the 4.1× gap closes.
2. Every existing test passes unchanged: with no MSSQL type, no option and no
   setting, DDL generation is byte-identical to today's.
3. A too-long value fails with **our** error naming the column, on every path —
   including CTAS and COPY-with-`overwrite`, which bypass the guard today, and
   including a UTF-8 `varchar(n)`, where the bound is bytes.
4. The same cast produces the same server type on **all** of COPY, CTAS and
   `INSERT ... SELECT`, asserted from `sys.columns` (`type_name`, `max_length`,
   `collation_name`) — the § 4 table is the shape of that assertion, and today
   it fails on row 3.
5. `MSSQL_VARCHAR(n)` round-trips non-ASCII byte-for-byte, and is refused with a
   clear error when the server declined UTF8SUPPORT.
6. `DATA_COMPRESSION` produces a compressed table **after the load**, not after
   a later rebuild — asserted on `sys.dm_db_partition_stats` page counts, which
   is what caught the TABLOCK coupling in the first place.
7. Round trip: a table created through the new types reads back with the same
   declared types via the catalog, and `COPY (SELECT * FROM d.dbo.Src) TO
   'd.dbo.Dst'` gives `Dst` the same column types as `Src` with no cast written
   by the user.
8. With the D2 setting off, `DESCRIBE` and `duckdb_columns()` report exactly
   what they report today.
9. A `WITH (...)` clause naming an option we do not support is an error naming
   the option, not a silently ignored request.

## 8. Risks

- ~~**Alias survival through the plan is the whole foundation.**~~ Settled by
  the probe in § 2a: it survives for COPY, and CTAS needs its own mapper taught.
  The residual risk moved to the opposite side — an aliased type is invisible to
  every function overload, which is why § 2a confines these types to DDL.
- ~~**`MSSQL_VARCHAR(n)` and collations** may restrict D1 to the `N`-prefixed
  types in a first cut.~~ Answered by #225 (merged as `e05c569`): a `varchar`
  column the extension creates already asks for `mssql_utf8_collation`, so the
  bytes written into it are correct, and the same ack is available here through
  `MSSQLCatalog::UTF8SupportState()`. What survives is narrower and is now D3 —
  `n` counts **bytes** for these types, and every client-side bound derived from
  it must know the collation to be right (§ 3).
- **A second aliased extension type with implicit casts to and from `VARCHAR`.**
  § 2a registered one and measured it. Two of them create a cast path between
  each other — `MSSQL_VARCHAR(10)` to `MSSQL_NVARCHAR(20)` resolves through
  `VARCHAR` twice, both hops at cost 0 — and ambiguity in overload resolution is
  a plausible failure that a single registered type could not have shown. Verify
  before building on it; § 2a reversed its own conclusion once already.
- **D2 makes every attached string column carry an alias.** § 2a rejected the
  catalog annotation partly because the alias-free form crashes and the aliased
  form was thought to make columns unusable — the implicit casts of `b7a15fa`
  are what changed that. They are now on the path of every query against every
  attached MSSQL table, not just the columns someone casts. Whatever the casts
  cost, the whole extension pays it.
- **Type names are user-facing and permanent.** `MSSQL_NVARCHAR` follows the
  extension's namespace convention; picking anything shorter risks colliding
  with a future DuckDB type.


---

## 9. What shipped, and what this draft got wrong

Written after the fact, because two of the judgements above did not survive
contact with a server.

### Beyond the draft

- **The UTF-8 write path.** § D1 said this spec would not shrink the write
  wire. It does: a UTF-8 `varchar`/`char` target now receives UTF-8 bytes
  declared as `BIGVARCHAR`, instead of being transcoded to UTF-16 for the
  server to transcode back. Measured −51% client CPU on long strings and
  35 → 19 wire bytes for a 16-character value. Two things had to be learned
  the hard way: the server reads the payload by the collation in the `INSERT
  BULK` **statement text**, not the one in COLMETADATA (`Привет` arrived as
  `ÐŸÑ€Ð¸Ð²ÐµÑ‚` until the column list carried `COLLATE`), and the choice
  belongs where the encoder is resolved **once per column** — a per-value test
  cost the untouched nvarchar path ~20 ns.
- **Microsoft Fabric**, which this draft does not mention at all. BCP works
  there now, so the CTAS INSERT fallback and the COPY refusal are gone. In
  their place are guarantees, because a warehouse's limits are real: no
  `nvarchar` at all (Delta Parquet has no UTF-16 type), exactly two collations
  and both UTF-8, no indexes and no page compression. All verified against a
  live warehouse rather than read off the documentation, which was wrong about
  three of them.

### Corrections to this draft

- § 3's claim that a temp table follows the database's collation is wrong. A
  `#temp` lives in tempdb and takes **tempdb's**, which is not UTF-8 even when
  the database is. Found on Fabric, reproduced locally.
- § D1's "no MAX form" still holds, but for a reason the draft did not give:
  `mssql_default_string_length = 0` is the MAX form, and it is the default.

### Left undone

- **`TIME` disagrees between the two create paths**: CTAS emits `TIME(7)`,
  COPY's auto-create `time(6)`. Both lossless — DuckDB's `TIME` is
  microseconds — but it is the same "one decision in several places" this spec
  is about. Left because two C++ tests pin the CTAS value and changing either
  side is a behaviour change with no correctness benefit.
- **`datetimeoffset` on Fabric.** We emit it for `TIMESTAMP WITH TIME ZONE`
  and a warehouse refuses it, so such a column cannot be created there. Not
  guarded; the date/time mapping is its own piece of work.
- **`ROWS_PER_BATCH` on CTAS** (D8 in the original draft) — untouched.
- **`char` / `nchar` sources report as `mssql_varchar(n)` / `mssql_nvarchar(n)`.**
  The data is the same; the space padding is not. A consequence of not adding
  types without a problem to solve.

### Spun off

- **Spec 061** — collation-aware ORDER BY pushdown. Enabling
  `mssql_order_pushdown` by default was considered and rejected here: measured,
  it returns a different SET of rows under `LIMIT`, because the server sorts
  linguistically and DuckDB by code point. A `_BIN2` collation is the case
  where the two provably agree.
- **Spec 062** — INSERT via BCP. `INSERT INTO … SELECT` is the one write path
  still building T-SQL text; `RETURNING` is the only thing that genuinely
  requires it.
