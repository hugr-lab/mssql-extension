# Spec 076 — catalog scans on one plan: parameterised filters, one round trip per first touch

**Status:** in progress on `spec/076-catalog-scan-parameters` from `main`
`d5d2346` (spec 075 merged as #341 on 2026-09-14); reconnaissance done on
2026-09-11 against the local docker server (SQL Server 2022) with the spec 075
build. Spec and implementation in one PR.
**Builds on:** spec 075 — `query/mssql_sql_params.{hpp,cpp}` (the
`sp_executesql` batch builder and the declaration table) and
`MSSQLSimpleQuery`'s per-result-set COLMETADATA.
**Out of scope, by decision:** the DML paths. INSERT moves to BCP (spec 062),
UPDATE and DELETE to a BCP-loaded `#temp` table joined on the server (spec 065
recon); their VALUES-list compiles go away with them, so nothing here touches
`src/dml/`.

## 0. Measured ground

Everything below was measured, not reasoned. The server is local, so a round
trip costs ~0.3 ms here and 20–50 ms against Azure SQL; compile time is the
server's own number from the cached plan's `CompileTime`.

### 0.1 What a catalog scan sends, and what the server keeps

Forty scans of one table with forty different filter values, plan cache
cleared first (`DBCC FREEPROCCACHE`), then
`sys.dm_exec_cached_plans` for texts naming the table:

| Shape, 40 scans with distinct values | Plans in cache | Bytes |
|---|---|---|
| catalog scan, one equality on the PK: `WHERE ([id] = 5)` | 40 Adhoc shells + 1 Prepared | 655 K + 49 K |
| catalog scan, two predicates: `WHERE ([v] LIKE N'v%') AND ([id] > 5) AND ([n] = 5)` | 40 Adhoc, full plans | 2.3 M |
| the same two predicates through `mssql_scan_params` (`@a`, `@b`) | 1 Prepared | 57 K |

The single-equality case is caught by SQL Server's **simple
parameterization** — the plan is shared, but every distinct text still leaves
a 16 KB shell entry. Two predicates do not qualify (simple parameterization
covers only a narrow set of "safe" trivial plans) and every value set compiles
and caches a full plan. A server running many DuckDB clients against many
distinct predicates accumulates them linearly; this is the plan-cache
pollution `optimize for ad hoc workloads` exists to blunt, and the standard
complaint DBAs have about tools that emit literal SQL.

Wall time is **not** where the gain is. 300 distinct-literal scans took
0.88–1.35 s, 300 parameterised ones 1.18–1.21 s — a trivial plan compiles in
well under a millisecond and the numbers are inside the noise. The gain is
what the server keeps, and what compiles once the predicates stop being
trivial (a pushed join, spec 065, is where compile time would show).

### 0.2 What a fresh table costs before its first row

`SELECT count(*) FROM t.dbo.x` on a table the catalog has not seen, batches
counted from `MSSQL_DEBUG=1`:

| Batch | Bytes | Issued by |
|---|---|---|
| database collation (`DATABASEPROPERTYEX(DB_NAME(), 'Collation')`) | 88 | `MSSQLCatalog::QueryDatabaseCollation`, **twice per ATTACH** |
| single-table metadata (`SINGLE_TABLE_METADATA_SQL_TEMPLATE`) | 385 | `MSSQLMetadataCache::GetTableMetadata` |
| columns (`COLUMN_DISCOVERY_SQL_TEMPLATE`, via `sp_executesql`) | 1132 | `MSSQLMetadataCache::LoadColumns` |
| primary key (`PK_DISCOVERY_SQL_TEMPLATE`, via `sp_executesql`) | 808 | `MSSQLTableEntry::EnsurePKLoaded` → `mssql_primary_key.cpp` |
| the scan itself | 31 | `TableScanInitGlobal` |

Three metadata round trips per fresh table, plus two identical collation
queries per ATTACH. (Found in W2: the collation is asked twice because the
catalog is *initialised* twice — by the storage extension's attach callback
and again by DuckDB's `AttachedDatabase::Initialize` — and the second pass
builds a second pool and logs in again; that, not the query, is the cost.) Spec 075 took the compile out of each (30 ms → 0–1 ms on
this server); what remains is the round trip, which is the whole cost against
a remote server: ~100 ms before the first row on Azure, three times what one
batch would cost.

### 0.3 Where a constant becomes text

Five sites in `src/table_scan/filter_encoder.cpp`, all through
`FilterEncoder::ValueToSQLLiteral` →
`codec::FormatSqlLiteral(value, type, LiteralContext::Filter)`:

| Site | Filter |
|---|---|
| `EncodeConstantComparison` (409) | `col OP value` |
| `EncodeInFilter` (428) | `col IN (v1, v2, ...)` |
| `EncodeConstant` (967) | a constant inside an `EXPRESSION_FILTER` (LIKE patterns, arithmetic, BETWEEN, CASE) |
| `EncodeRowidEquality` (1159, 1168) | `rowid = value` against the PK columns |

Two callers assemble the text: `TableScanInitGlobal` encodes the
`TableFilterSet` at init (`table_scan.cpp:432`), and
`pushdown_complex_filter` encodes the expressions it accepts **at plan time**
into `bind_data.complex_filter_where_clause` (`table_scan.cpp:1258`). A
parameter set therefore has to live on the bind data, filled from both.

The encoder's context carries the DuckDB column types only
(`ExpressionEncodeContext::column_types`); the bind data beside it carries
`mssql_columns` — `MSSQLColumnInfo` with `sql_type_name`, `max_length`,
`precision`, `scale`, `collation_name`, `is_unicode`. That is the fact this
spec is built on: **the catalog scan knows the column's declared SQL Server
type, `mssql_scan_params` does not.**

## 1. Findings

**F1 — a distinct filter value is a distinct plan.** § 0.1. Every catalog scan
with non-trivial predicates compiles and caches its own plan per value set.

**F2 — a parameter declared from the DuckDB type can cost the index seek.**
`mssql_scan_params` declares a string as `nvarchar(4000)` (W5's table). Against
a `varchar(20)` column, SQL Server converts the **column** to the parameter's
type (nvarchar has the higher precedence), and an index on that column is no
longer seekable — a scan where the literal form seeks. The same applies to
`datetime` columns against a `datetime2` parameter and to `decimal(p,s)`
mismatches. The catalog scan has the column's type and must declare from it.

**F3 — a parameter declared from the column type can be too narrow for the
constant.** `id = 3000000000` against an `int` column: DuckDB types the
constant BIGINT; `@p int` would fail at the DECLARE. A 25-character constant
against `varchar(20)` declared as `varchar(20)` would be **silently truncated**
and then compare equal to a 20-character row. The declaration must be the
column's kind (varchar vs nvarchar, int family vs decimal) with the
constant's width where the two disagree.

**F4 — three round trips per fresh table, two identical ones per ATTACH.**
§ 0.2. `MSSQLSimpleQuery` returns every result set of a batch since spec 075
(`result_sets`), so three queries can be one batch with three result sets;
the collation is a per-database constant that is queried twice.

**F5 — `IN` lists and NULL tests are not parameters.** `sp_executesql` takes at
most 2100 parameters and a list of N values is N of them; `IS NULL` /
`IS NOT NULL` carry no value. Both stay text. A list can travel as **one**
`nvarchar(max)` JSON parameter through `OPENJSON`, but that is a different
plan shape (a nested loop over the parsed list) and is not proposed here.

## 2. Work

### W1 — parameterised filter pushdown

The encoder gets an optional sink: when `ExpressionEncodeContext` carries a
`mssql::SqlParamSet *`, every constant at the five sites of § 0.3 becomes
`@pN` and the literal it would have rendered goes into the set as the
parameter's initialiser (the DECLARE line accepts any expression, which is why
the CAST literals of the datetime codec need no special case — spec 075). The
scan's text becomes `params.ExecuteSqlBatch(select)`; with no parameters it is
the bare statement it is today, byte for byte.

**Declaration rule (F2, F3):** from the column the constant is compared with,
found through the column reference on the other side of the comparison:

| Column (`sql_type_name`) | Declaration |
|---|---|
| `int`, `bigint`, `smallint`, `tinyint`, `bit` | the column's type, unless the DuckDB constant is wider (a BIGINT constant against `int`) — then the constant's type from W5's table |
| `decimal(p,s)` / `numeric(p,s)`, `money`, `smallmoney` | the column's type |
| `float`, `real` | the column's type |
| `varchar(n)`, `char(n)` | `varchar(k)` with k = max(n, constant bytes), `varchar(max)` past 8000 — never narrower than the constant; an ASCII constant only. A non-ASCII constant goes as `nvarchar(k)`: a varchar *variable* takes the database's code page, not the column's, so `@p varchar(max) = N'ы'` against a UTF-8 column arrives as `?` (found by `annotated_max_string.test`) |
| `nvarchar(n)`, `nchar(n)` | `nvarchar(k)` with k = max(n, constant UTF-16 units), `nvarchar(max)` past 4000 |
| `date`, `time(s)`, `datetime`, `smalldatetime`, `datetime2(s)`, `datetimeoffset(s)` | the column's type, precision included |
| `uniqueidentifier`, `varbinary(n)`, `binary(n)` | the column's type (`varbinary(max)` past 8000) |
| anything else (`xml`, `sql_variant`, UDTs, `is_cast_required`) | not parameterised: the literal stays in the text, as today |

A constant compared with an **expression** rather than a column (`col + 1 >
5`, `LEN(col) = 3`) is declared from the DuckDB type by W5's table — there is
no column to take the type from — and a `rowid` equality takes the PK
column's type. `IN` lists stay literal (F5). `IS NULL` stays literal.

**Setting:** `mssql_scan_parameterize_filters`, default `true`, read at
`TableScanInitGlobal`. The switch is the escape hatch for parameter sniffing
— a plan compiled for a selective value and reused for a non-selective one,
which is a server-side property of every parameterised client and the reason
DBAs sometimes prefer literal SQL for reporting queries. It is also what a
reviewer turns off to compare plans.

**Cardinality (`MSSQLCatalogScanCardinality`)** is unaffected: it estimates
from the filter set, not from the text.

### W2 — one round trip per first touch, one collation per ATTACH

- `MSSQLTableSet::LoadSingleEntry` sends the single-table metadata query, the
  column query and the PK query as **one batch** and reads three result sets
  from `SimpleQueryResult::result_sets` / `rows` — the parser already
  separates them (spec 075). `EnsurePKLoaded` then finds the PK already
  cached on the entry and sends nothing. The bulk path
  (`mssql_preload_catalog`) is untouched.
- `MSSQLCatalog::Initialize` is a no-op once the pool exists: the second
  call (DuckDB's `AttachedDatabase::Initialize`, after the attach callback's
  own) used to rebuild the pool, log in again and ask the collation again.

Measured target: a fresh table costs one metadata round trip before its scan
(§ 0.2 counted three), ATTACH one collation query (counted two).

### W3 — tests

- **`test/sql/catalog/scan_parameterized_filters.test`**: every declaration
  row of W1 (a filter against a column of that type returns the right rows —
  the same rows the literal form returns, asserted by running both with the
  setting on and off); a BIGINT constant against an `int` column (F3, no
  DECLARE error, right rows); a 25-character constant against `varchar(20)`
  (F3, no false match); a `varchar` column with a non-UTF-8 collation keeps
  its collation semantics (the comparison happens in the column's collation
  either way); an `IN` list and `IS NULL` beside a parameterised predicate;
  a rowid equality; the complex-filter path (a LIKE with a pattern, a
  BETWEEN); `EXPLAIN` shows the parameterised text. Plan reuse is measured in
  the PR with § 0.1's query, not asserted.
- **`test/cpp/test_filter_encoder.cpp`**: the encoder with a sink — one
  parameter per constant in order, the declaration per W1's table, the text
  with `@pN` in place; without a sink, the text unchanged from today.
- **`test/sql/catalog/first_touch_round_trips.test`**: a fresh table's
  columns, PK (rowid) and row count all present after one scan; the count of
  metadata batches is measured in the PR with `MSSQL_DEBUG=1` (§ 0.2), not
  asserted.
- Existing: `filter_pushdown*.test`, `rowid/*`, `catalog/statistics_pushdown`
  stay green with the setting on (the default) — those are the files that
  compare rows through pushed filters.

### W4 — docs

`CLAUDE.md` (the setting, the read-path concept), `DATAMODEL.md` (the
parameter set on the bind data, the first-touch batch), `docs/query-execution.md`
(filter pushdown section: what the server receives now), the website's
`reading/` page on filter pushdown and `reference/settings.md`, CHANGELOG.

## 3. Not proposed

- **`PARAMETERIZATION FORCED`** on the database: the DBA's server-side
  alternative, and it would parameterise every literal of every client. Not
  ours to set, and it declares from the literal, not from the column (F2).
- **`sp_prepare` handles for catalog scans.** A handle lives in one session;
  the pool rotates sessions and `sp_executesql` already shares the plan across
  them. Nothing to gain.
- **`OPENJSON` for `IN` lists** (F5): one text for any list length, but a
  different plan shape and a different cost model; measure before proposing.
- **Parameterising `TOP N` / `ORDER BY`**: `TOP @n` is legal, but N is part of
  what the optimizer decides on, and the text differs by `ORDER BY` anyway.
- **DML**: by decision, spec 062 / 065. Noted for an issue, not for this
  spec: an `UPDATE` of 5000 rows through the rowid path today spends 209 ms of
  its 519 ms in server compile (11 `VALUES` batches, 5.4 MB of plans), and
  `mssql_dml_use_prepared` is read into `DMLConfig` and consulted by nothing.

## 4. Risks

- **Parameter sniffing** — the setting (W1) is the way out; the CHANGELOG
  names it.
- **A declaration that changes the comparison.** F2 and F3 are the two ways
  a parameter can be worse than a literal: seekability and truncation. W1's
  rule is built around both, and W3's first test runs every row of the table
  through both forms.
- **The 2100-parameter cap** cannot be reached: `IN` lists stay literal and a
  filter set with more than 2100 scalar constants does not occur in practice;
  the encoder falls back to the literal form past the cap rather than fail.
- **Plan-time vs init-time**: the complex-filter parameters are collected at
  plan time into the bind data, the simple-filter ones at init; the DECLARE
  line is rendered at init from both. A bind data copied by DuckDB copies the
  set (same rule as `complex_filter_where_clause`).

## 5. Acceptance

- § 0.1's two-predicate shape: 40 scans with distinct values leave **one**
  Prepared plan in `sys.dm_exec_cached_plans`, and the same rows come back as
  with `mssql_scan_parameterize_filters = false`.
- A `varchar(20)` column with an index: `EXPLAIN`'s text declares
  `varchar(k)`, and the server's plan for it is an index seek (checked once in
  the PR with `SET STATISTICS XML`, not asserted).
- § 0.2's fresh table: one metadata batch before the scan; ATTACH sends the
  collation query once.
- All existing pushdown, rowid and statistics tests green with the default.
