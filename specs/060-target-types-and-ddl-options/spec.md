# Spec 060 — Target column types and DDL options

**Status:** Draft
**Date:** 2026-07-31
**Depends on:** spec 049 (target's `index_kind` / `partition_count` in the catalog cache — PR #223)
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

### D3 is already satisfied

An over-long value on the BCP path already fails with our own error naming the
column, the value's length and the limit, in both units:

```
MSSQL: NVARCHAR column 'cust' overflow: value is 60 UCS-2 code units
(120 UTF-16LE bytes) but column max is 50 code units (100 bytes)
```

That is the guard from spec 045 / issue #91 (character-vs-byte length). D3
reduces to covering it by a test on each path rather than writing anything new.

## 3. Where a column's target type comes from

Resolution order, first match wins:

1. **The existing target table.** Already the case, and already the fast path —
   the 4.1× above is precisely the gap between creating the table and inserting
   into a properly created one. Nothing to add.
2. **An explicit cast to an MSSQL type**: `CAST(cust AS MSSQL_NVARCHAR(50))`.
   The only mechanism available to `CREATE TABLE ... AS SELECT`, which has no
   options syntax — so it is load-bearing, not sugar.
3. **A per-column COPY option**: `column_types {'cust': 'nvarchar(50)'}`.
   Convenient when the user does not want to rewrite the SELECT list.
4. **A default**: `mssql_default_string_length` (a setting) or `string_length`
   (a COPY option). Applies to every string column with no other answer.
5. **Fallback `nvarchar(max)`** — today's behaviour, kept for compatibility but
   no longer the silent default when 4 has a value.

**Data-derived sizing is rejected.** Scanning the first batch to take the longest
value guesses; a later batch that exceeds the guess fails the whole load, and the
failure comes from data the user never saw.

## 4. Deliverables

- **D1 Parameterised MSSQL types.** `MSSQL_NVARCHAR(n)`, `MSSQL_VARCHAR(n)`,
  `MSSQL_NCHAR(n)`, `MSSQL_CHAR(n)`, `MSSQL_VARBINARY(n)`. Bound to their
  natural DuckDB physical type with the modifier in extension info.
  **Acceptance:** the modifier is readable at DDL generation for all three
  paths — `CREATE TABLE AS SELECT`, `INSERT INTO ... SELECT`, and
  `COPY ... TO ... (FORMAT 'bcp')`.
- **D2 DDL generation honours them, in BOTH translators.**
  `TargetResolver::GetSQLServerTypeDeclaration` (COPY) reads the extension info
  instead of mapping `VARCHAR` → `nvarchar(max)` unconditionally — done in the
  probe — and `MSSQLDDLTranslator::MapLogicalTypeToCTAS` must do the same, or
  the feature works on COPY and silently not on CTAS. Merging the two is
  preferable to teaching both.
- **D3 Length semantics — already implemented, needs tests only.** The BCP
  encoder's existing overflow guard (spec 045 / issue #91) reports the column,
  the value's length and the limit in both code units and bytes. Cover it on
  each path; write nothing new unless a path is found that bypasses it.
- **D4 `WITH (...)` options at creation.** `DATA_COMPRESSION = PAGE | ROW`
  measured −51% / −41% on storage for +63% / +17% on load time (spec 057). Must
  be documented together with TABLOCK, because compression is applied **during**
  a bulk load only when the load takes a table lock — otherwise rows land
  uncompressed until someone rebuilds, and the user silently does not get what
  they asked for. `OPTIMIZE_FOR_SEQUENTIAL_KEY` is deliberately **not** exposed:
  measured neutral under four concurrent loaders, because they write disjoint
  ascending ranges and there is no hot last page for it to relieve.
- **D5 A per-column type map as a COPY option** (resolution step 3).
- **D6 `mssql_default_string_length`** (resolution step 4), plus its COPY-option
  form.
- **D7 Truncate the target as an explicit COPY option.** Must be separately
  named — it destroys data the statement does not name — and must run on the
  same connection as the load, so an aborted COPY cannot leave the table both
  empty and unloaded.
- **D8 `ROWS_PER_BATCH` on the CTAS path.** COPY sends it, CTAS does not; there
  is no reason for the asymmetry.

## 5. Acceptance criteria

1. A `COPY` that creates its target produces sized string columns, and loading
   into it is within noise of loading into a hand-created table with the same
   types — i.e. the 4.1× gap closes.
2. Every existing test passes unchanged: with no MSSQL type, no option and no
   setting, DDL generation is byte-identical to today's.
3. A too-long value fails with our error naming the column, on every path.
4. `DATA_COMPRESSION` produces a compressed table **after the load**, not after
   a later rebuild — asserted on `sys.dm_db_partition_stats` page counts, which
   is what caught the TABLOCK coupling in the first place.
5. Round trip: a table created through the new types reads back with the same
   declared types via the catalog.

## 6. Risks

- ~~**Alias survival through the plan is the whole foundation.**~~ Settled by
  the probe in § 2a: it survives for COPY, and CTAS needs its own mapper taught.
  The residual risk moved to the opposite side — an aliased type is invisible to
  every function overload, which is why § 2a confines these types to DDL.
- **`MSSQL_VARCHAR(n)` and collations.** A `VARCHAR` target column is
  code-page-encoded; the extension's non-UTF8 collation handling lives in the
  scan rewrite, not the codec layer. Sizing a `VARCHAR(n)` column is therefore
  the easy half; writing correct bytes into it is not, and may restrict D1 to
  the `N`-prefixed types in a first cut.
- **Type names are user-facing and permanent.** `MSSQL_NVARCHAR` follows the
  extension's namespace convention; picking anything shorter risks colliding
  with a future DuckDB type.
