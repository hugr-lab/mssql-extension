# Changelog

All notable changes to the DuckDB MSSQL Extension are documented here.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- **`native_types` ATTACH option**: `mssql_catalog_native_types` for one
  catalog, over the global setting. `ATTACH '…' AS meta (TYPE mssql,
  native_types false)` reports that catalog's bounded string columns as plain
  `VARCHAR`, in its tables and its `mssql_scan` results, while every other
  attached catalog keeps `MSSQL_VARCHAR(n)` / `MSSQL_NVARCHAR(n)`. Takes a
  string too (`'false'`, `'no'`), so DuckLake's `METADATA_PARAMETERS` can set it
  for a metadata catalog. Unset, the setting decides as before.

- **`mssql_scan_unsafe` / `mssql_scan_params_unsafe`** (spec 081): `mssql_scan`
  / `mssql_scan_params` with the result's shape given as
  `columns := {'name': 'TYPE', ...}` instead of described. The bind asks the
  server nothing, so the call binds inside a transaction, on a pool of one
  connection, and for a batch the server cannot describe (a `#temp` read) like
  any other statement; the rows are checked against the shape when the
  statement runs (column count, the kind of each column as the catalog reads
  it, no conversion; a string's length and collation are not checked), and a
  mismatch fails the statement naming it. A type no SQL
  Server column is read as is refused at bind. `EXPLAIN` shows
  `Shape: given (columns :=)`.

- **Remote pushdown, first shapes (spec 079 PR B), behind
  `mssql_remote_pushdown`** (read at ATTACH; on by default since PR E2, see
  Changed). An attached catalog answers DuckDB's remote-pushdown
  rewriter, which then sends a whole single-table `SELECT` to SQL Server as
  one statement: its columns, a `WHERE` of column-vs-constant comparisons,
  `IS [NOT] NULL` and `AND` / `OR` / `NOT`, `ORDER BY` on columns, and
  `LIMIT` / `OFFSET` as `TOP` / `OFFSET … FETCH`. The statement runs through
  `mssql_scan_params` (or `mssql_scan` when it has no constants), so `EXPLAIN`
  shows the T-SQL it sends. A node is taken only when it sends the server more
  than the catalog scan would: in this PR an `ORDER BY` or a `LIMIT`. A node
  without either stays with the catalog scan, which pushes projections and
  filters itself and still takes a filter from an enclosing query. Anything
  outside that list is left to the scan path, never guessed. Not pushed in
  this PR: `$n` parameters, and an unqualified name while the session's
  search path puts another schema of the catalog first (`USE db.sales`).
  While the setting is on, the catalog's schema `main` answers as its default
  schema for every lookup (`db.main.t`, `CREATE TABLE db.main.x`), DuckDB
  skips its catalog-or-schema ambiguity check for the catalog, and the
  rewriter runs on every statement of the instance.
- **Remote pushdown: the expression vocabulary (spec 079 PR C).** A pushed
  single-table SELECT now carries computed columns and real WHERE clauses:
  arithmetic over same-type integer / decimal operands, division (floating,
  as DuckDB's `/`), widening casts, CASE, COALESCE, IN, BETWEEN, LIKE / NOT
  LIKE, IS NULL of any value, a bit column as a condition, and a column
  compared with a column of the same type and collation. The scan path's
  filter pushdown and the rewriter now render through one vocabulary
  (`pushdown/mssql_expression_vocabulary`), so a construct both paths take
  answers the same whichever takes it. What is not pushed, each with a measured reason:
  mixed-type arithmetic (the promotions differ), decimal arithmetic whose
  result passes 38 digits (the server rounds), a division inside a condition
  (a zero divisor is `inf` in DuckDB and NULL on the server, which would
  change the rows; for a selected value the NULL is the documented
  divergence), ILIKE / GLOB / LIKE … ESCAPE, a code-page `varchar` inside a
  computed value (it would arrive as its code-page bytes), a division under
  COALESCE, a string constant in a CASE / COALESCE branch wider than its
  column. A computed column's type is the server's, as for any `mssql_scan`.
- **Remote pushdown: joins, aggregates, DISTINCT (spec 079 PR D).** A pushed
  SELECT now carries joins of one catalog's tables (INNER / LEFT / RIGHT /
  FULL / CROSS with ON or USING; SEMI / ANTI as `EXISTS` / `NOT EXISTS`),
  `count` / `sum` / `avg` / `min` / `max` / `stddev` / `variance`, GROUP BY on
  columns (by name, position or select alias), HAVING, `SELECT DISTINCT` and
  ORDER BY an aggregate (`… ORDER BY count(*) DESC LIMIT 10`). Aggregates have
  DuckDB's types: an integer `sum` is HUGEINT (sent as `decimal(38,0)` and
  cast back). A floating-point aggregate (`sum` / `avg` of a float, any `avg`,
  `stddev` / `variance`) may differ from DuckDB's in its last bits, as
  DuckDB's own parallel float sum does; it is not pushed where that would
  choose rows (HAVING, ORDER BY, DISTINCT). Under D4 a string group or
  DISTINCT follows the column's collation (`a` and `A` are one value on a
  `_CI` collation); `min` / `max` of a string is not pushed. `GROUP BY ()` is
  sent without a GROUP BY clause (over no rows the server would return no row
  where DuckDB returns one). Not pushed: FILTER, `string_agg`, ROLLUP /
  CUBE / GROUPING SETS / GROUP BY ALL, expression keys, NATURAL / ASOF /
  POSITIONAL joins, a join with a subquery or a local table, a SEMI / ANTI
  join under a later RIGHT / FULL join, and a `CROSS JOIN` (or comma join)
  with nothing else to gain. `stddev` / `variance` can differ by far more
  than the last bits on large, close values (the server computes them in one
  pass). A `float` column is compared and divided when pushed, not added,
  subtracted or multiplied (an overflow is `inf` in DuckDB and an error on
  the server).
  A computed result column now has DuckDB's type or is not pushed: a decimal
  beside a constant (`v + 700`) stays with DuckDB as a result column. A
  comparison of an integer `x + c` / `x - c` / `x * c` / `-x` with a constant
  is not pushed, as DuckDB moves the constant across and never computes a
  value the server could overflow on. A repeated ORDER BY key no longer fails on the
  server (error 169, since PR B).
- **Remote pushdown: whole statements and their parts (spec 079 PR E1).**
  - A statement over one catalog now goes to the server whole when it is
    built from subqueries in FROM, subqueries in expressions (`IN`,
    `EXISTS`, a scalar subquery, correlated or not), set operations inside a
    subquery (`UNION [ALL]`, `EXCEPT`, `INTERSECT`) and `WITH` clauses (each
    CTE inlined where it is referenced).
  - A statement that cannot go whole (an `EXCLUDE`, a window, a local
    table) has each part that can go replaced by its own `mssql_scan`, and
    DuckDB runs the rest. A set operation at a statement's top always runs
    this way: DuckDB combines the children.
  - A node that would send the server nothing the catalog scan does not is
    left to the scan, which still takes filters from above it.
  - A join whose gain is uncertain (many-to-many, or `CROSS` with no key
    equality) is pushed only while every table it joins has fewer rows than
    the new setting `mssql_pushdown_join_rows_threshold` (default
    1000000, `0` turns the check off), by the cached row counts. It is not
    checked inside a transaction or on a pool of one connection.
  - `UNION` / `EXCEPT` / `INTERSECT` over strings compare under the
    column's collation when pushed, as `DISTINCT` does (D4).
  - A CTE whose body has a `LIMIT` is not inlined where the server could
    evaluate it twice. A CTE body inlined twice is read twice by the server.
  - A scalar subquery is pushed only when it returns one row (a key lookup,
    an aggregate, a `LIMIT 1`): the server evaluates it lazily and would
    return rows where DuckDB raises "More than one row returned".
  - A join inside a subquery or a CTE is gain-checked with the statement
    around it; a subquery's `GROUP BY` columns count as its key.
  - Under `USE db.sales`, `db.t` is read as DuckDB reads it (`sales.t`); it
    was pushed reading `dbo.t`.
  - A set operation's child that DuckDB sends on its own (the set operation
    also reads a local table) is pushed like a statement: a division by zero
    in it is NULL, not `inf`, before DuckDB deduplicates or filters.
  - Not pushed: `EXCEPT ALL` / `INTERSECT ALL`, `UNION BY NAME`, recursive
    CTEs, CTE column aliases, `> ANY` / `ALL`, and any scalar subquery under
    `scalar_subquery_error_on_multiple_rows = false`.
  - In a statement that also reads a local table, DuckDB's rewriter does not
    yet hand a FROM subquery or a CTE body over on its own
    ([duckdb/duckdb#26280](https://github.com/duckdb/duckdb/issues/26280)),
    so their aggregate runs in DuckDB.
- **Remote pushdown: windows, QUALIFY, FILTER, string_agg; the describe
  result shapes cached (spec 079 PR E2).**
  - Window functions go to the server: `row_number`, `rank`, `dense_rank`,
    `ntile`, `lag` / `lead` (an offset, a default), `first_value` /
    `last_value`, and aggregates `OVER (…)` with `PARTITION BY`, `ORDER BY`
    and `ROWS` frames. An integer `sum` over a window goes through
    `decimal(38,0)` (the server's `SUM(int)` overflows) and `avg` in an exact
    form (the server's `AVG(int)` truncates).
  - `QUALIFY` goes too: `QUALIFY rn = 1` filters the window's result in an
    outer query, the window evaluated once.
  - `agg(x) FILTER (WHERE c)` as `agg(CASE WHEN c THEN x END)`;
    `string_agg` / `group_concat` / `listagg` as `STRING_AGG(… nvarchar(max) …)
    WITHIN GROUP (ORDER BY …)`.
  - Not pushed: `IGNORE NULLS` (SQL Server 2022 on), `percent_rank`,
    `cume_dist`, `nth_value`, `RANGE` frames with offsets, `GROUPS`,
    `EXCLUDE`, `count(DISTINCT …) OVER`, `string_agg(DISTINCT …)`.
  - The result shape of a pushed statement is cached per statement form
    (constants are parameters), so planning it again costs no round trip and
    no connection, in a transaction too while it has changed no schema; the
    cache ends with the catalog's metadata. A table changed by another client
    fails the next pushed statement over it once, saying to run it again. Measured on a
    local server: planning 1.9 ms → 0.68 ms, a point read 4.6 ms → 2.7 ms;
    on a 1M-row table an aggregate, a TOP N, a QUALIFY and a join ran 4-15x
    faster pushed.
  - `mssql_pushdown_min_rows` (default 0): a statement whose tables hold
    fewer rows together than this is left to the plain scans.
  - The ATTACH option `remote_pushdown true/false` gives one database its own
    answer, over the setting — what DuckLake's `METADATA_PARAMETERS` can pass.
- **`column_types := [...]` on `mssql_scan` / `mssql_scan_params`**: the type
  each result column is read as, `''` for the described one, checked against
  the server's describe. The rewriter uses it so a pushed `SELECT` has the
  catalog's column types: `datetime2(7)` as `TIMESTAMP_NS` with its 100 ns
  ticks, a `varchar` under a code-page collation as its declared
  `MSSQL_VARCHAR(n)`, `geometry` as `GEOMETRY`.
- **`EXPLAIN` shows the statement of an `mssql_scan` / `mssql_scan_params`.**
- **ORDER BY … LIMIT on a nullable key is pushed** under
  `mssql_order_pushdown`. SQL Server sorts NULL lowest and has no
  `NULLS FIRST` / `LAST`, so a nullable key asking for another placement
  stopped the pushdown before. That included DuckDB's default, `NULLS LAST` on
  an ascending key. Under a LIMIT a leading `CASE WHEN key IS NULL` key now
  gets DuckDB's placement and the server returns only the N rows. A plain
  ORDER BY still sorts in DuckDB: that leading key defeats any index, so the
  server would sort the whole table where DuckDB does now.
- **`mssql_scan` reports `MSSQL_VARCHAR(n)` / `MSSQL_NVARCHAR(n)`**, as the
  catalog has since spec 060, under `mssql_catalog_native_types`, so
  `CREATE TABLE … AS SELECT * FROM mssql_scan(…)` keeps the source's lengths
  (and a UTF-8 `varchar`'s collation) instead of making `nvarchar(max)`.
  `typeof()` of such a column changes from `VARCHAR`; setting the option to
  `false` restores it. With `mssql_utf8_support = false` the column is still
  reported as the declared `MSSQL_VARCHAR(n)`, although it travels as
  nvarchar. A `char` / `varchar` under a code-page collation stays
  plain `VARCHAR`: a raw scan hands its bytes over untranscoded, and an
  annotation naming the code page would invite a re-encode. `prepared := true`
  reports the same types, asking `sp_describe_first_result_set` for the
  collations `sp_prepare` names only by id.
- **The pool opens its connections at ATTACH, in parallel; `preload` loads the
  catalog at ATTACH** ([#324](https://github.com/hugr-lab/mssql-extension/issues/324)).
  - **`mssql_min_connections` did not open anything.** It only kept idle
    connections from being closed. The pool now opens that many connections
    at ATTACH, with their logins running concurrently.
  - **ATTACH validates through the pool.** The credentials check is the
    pool's first login, and the connection stays in the pool instead of being
    closed and logged in again: a plain ATTACH is one login, not two. Pool
    refills now report the same classified reasons the ATTACH did (server error
    number and state: a paused serverless database, the 18456 login state).
    Every ATTACH validation failure is now an `Invalid Input Error` reading
    "MSSQL connection validation failed: …" -- a refused dial or an unknown
    host used to be an `IO Error`, and an Azure AD failure read "MSSQL Azure AD
    connection validation failed: …" (now "…: Azure AD authentication
    failed: …").
  - **Measured on a local server:**

    | ATTACH | before | after |
    |---|---|---|
    | plain | 0.40 s | 0.20 s |
    | `min_connections 4` | 0.54 s | 0.39 s |
    | `min_connections 8` | 0.63 s | 0.54 s |

    Opened one after another, four connections would cost about a second.
  - **Nothing opened under `lazy_validation`.** The ATTACH options `preload` and
    `min_connections` are refused beside it, and `preload` inside a transaction
    that has used an MSSQL catalog is refused — all before the ATTACH dials. A
    `min_connections` shortfall is logged as a WARNING and does not fail the
    ATTACH.
  - **New ATTACH options:**
    - `min_connections` is the ATTACH form of the setting.
    - `preload true` runs `mssql_preload_catalog()` as part of the ATTACH.

    Both exist because DuckLake's `METADATA_PARAMETERS` can forward ATTACH
    options but neither a setting nor a function call. Both accept the string
    form it sends. `preload` is refused next to `lazy_validation true` or
    `catalog false`.

- **`transaction_isolation`: the isolation level of explicit transactions**
  ([#331](https://github.com/hugr-lab/mssql-extension/issues/331)). The
  scans of one DuckDB transaction are separate statements on its pinned
  connection. Under the server's default READ COMMITTED, a row that another
  session commits between two scans is seen by one scan and not the other: 3
  rows then 4 in the same transaction. Under SNAPSHOT both reads say 3.
  - **Values.** `default` (unset) sends nothing, as before. The five T-SQL
    levels are sent as `SET TRANSACTION ISOLATION LEVEL …` before
    `BEGIN TRANSACTION`. `auto` means SNAPSHOT where the database has
    `ALLOW_SNAPSHOT_ISOLATION ON`; that state is probed at ATTACH in the same
    query as the collation, and only when `snapshot` or `auto` asks for it.
  - **Checked at ATTACH.** An explicit `snapshot` on a database with snapshot
    isolation OFF is refused there. Fabric Warehouse and Synapse get nothing.
  - **Found along the way.** SQL Server does not reset the isolation level with
    `RESET_CONNECTION`, so a level set in a transaction would follow the pooled
    connection into every later autocommit statement. The extension now puts
    READ COMMITTED back after COMMIT or ROLLBACK, including after the server
    aborted the transaction on an update conflict (3960). A `SET TRANSACTION
    ISOLATION LEVEL` sent through `mssql_exec()` has always leaked this way.
- **`default_schema`: the schema unqualified names resolve against**
  ([#322](https://github.com/hugr-lab/mssql-extension/issues/322)). It was a
  hard-coded `dbo`. It is now an option on every configuration path: the ATTACH
  option `default_schema` (which wins), `DefaultSchema=` in a connection
  string, `default_schema=` in a URI, and `default_schema` in a secret. It
  governs `db.table`, `USE db`, CTAS into `db.table`, and COPY into `db.table`
  or `mssql://db/table`. Unset is `dbo`, as before. A default that
  `schema_filter` hides is refused at ATTACH.
- **Every mssql function is documented in `duckdb_functions()`**
  ([#371](https://github.com/hugr-lab/mssql-extension/issues/371)): a
  description, runnable examples and a category on each of the 23 overloads,
  and real parameter names on the scalar functions — `mssql_exec(context,
  sql)` instead of `mssql_exec(col0, col1)`. `duckdb_functions()` is all a
  client connected to the database can read about an extension, so this is
  what an agent working through SQL sees. The scalar names are also callable,
  `mssql_exec(context := 'db', sql := '…')`, and binder errors list them.
  Table functions keep `col0`, `col1`, … for their positional parameters,
  because DuckDB's `duckdb_functions()` names those by position whatever the
  registration says; their named parameters (`prepared`) show by name.
  `mssql_preload_catalog`'s optional schema is now a second parameter instead
  of a vararg, so it is named too, and a third argument, which used to be
  accepted and ignored, is refused.

### Changed

- **Metadata queries compute a table's row count once, not once per column**
  (spec 084). Every query that returned columns carried
  `OBJECTPROPERTYEX(..., 'Cardinality')` in the same SELECT list, and the
  server evaluated it on every column row: about 110 logical reads per call on a
  catalog with keys and constraints. The loads now read the object row and the
  columns as separate result sets of one batch. A whole-catalog load without
  filters takes its row counts from one pass over `sys.dm_db_partition_stats`;
  without the `VIEW DATABASE STATE` permission, on Fabric / Synapse and inside a
  transaction, it falls back to per-object calls with the same counts. Measured
  on 200k tables in 100 schemas:

  | operation | before | after |
  |---|---:|---:|
  | `mssql_preload_catalog` of the whole catalog | 166 s | 24-29 s |
  | preload of one schema of 2,000 tables | 1.88 s | 0.53 s |
  | a table's first touch, server CPU | 1.86 ms | 0.39 ms |
  | a missing name's "did you mean" names query | 680-700 ms | 297 ms |

- **The bulk loads and `mssql_refresh_cache` carry every table's rowid key**
  (spec 084). After a preload or a listing, each transaction used to discover
  the key of every table it scanned on its pinned connection, one extra round
  trip per table per transaction. The key now comes in the same batch as the
  columns, everywhere. `mssql_refresh_cache` reloads the catalog in one batch
  instead of one columns query per table.
  - A key that cannot be read now fails the metadata load and is retried on the
    next access. It is no longer cached as a refusal naming
    `mssql_invalidate_cache`, and the "could not be read" refusal is gone.
  - A key changed behind the catalog's back (an index added by another client
    or through `mssql_exec`) is seen after an invalidation or the TTL, like a
    column. It used to be read at the table's first scan.
- **CTAS's table and schema existence checks are parameterized** (spec 084):
  one cached plan for every name instead of an ad-hoc plan per name, and
  non-ASCII names are checked correctly. A check that fails now reports its
  error instead of reading as "does not exist".

- **Parameters go as an RPC call, not a batch** (spec 083). Every
  `sp_executesql` the extension sends -- a pushed filter's constants,
  `mssql_scan_params`, `mssql_scan_params_unsafe`, `mssql_exec_params`, the
  per-table metadata queries -- is now a TDS RPC request (packet type 3) with
  the values as typed parameters, and a `prepared := true` execution an
  `sp_execute` RPC call. The batch form, `DECLARE @p0 ... = ...; EXEC
  sp_executesql ...`, made the server parse and compile the outer batch on
  every call, because its text changes with every value: measured locally, a
  scan with a pushed filter cost the server 0.26-0.29 ms of CPU that way and
  0.06-0.07 ms over RPC -- what a literal costs, while keeping one plan per
  filter shape. A declaration with no RPC encoding (a CLR type such as
  `geometry`, an alias type, `sql_variant`), or a value its declared type
  cannot take, keeps the batch form for the whole call, decided before
  anything is sent. `sp_prepare` stays a batch: it runs once per bind.
- **A scan allocates its string staging on demand** (spec 083 D4). Each
  string column's payload buffer used to be sized and zero-filled for a full
  chunk when the stream opened -- up to 2 MB for a bounded `nvarchar(n)`, 64 KB
  for a MAX column -- even when the result was empty. It now starts at 4 KB on
  the first value and doubles, stopping at the column's provable worst case,
  which used to be reserved up front.

### Fixed

- **The row count a table's storage info asks the server for always failed.**
  It read `sys.dm_db_partition_stats` through `p.rows`, but that view's column
  is `row_count`, so every call failed with error 207 from spec 008 on. The
  empty answer was taken as 0 and cached. That path is reached when DuckDB asks
  a table for its storage info in autocommit before any listing, for example
  when binding `INSERT ... ON CONFLICT`. The 0 then reached the planner for a
  table the catalog had loaded while it was empty: its scan planned with no
  estimate. The count now comes from `OBJECTPROPERTYEX(..., 'Cardinality')`,
  as the catalog's own metadata queries read it: 3 logical reads, against a
  scan of `sysrowsets` (spec 071). A failed count is no longer cached as 0.

- **In-transaction scans of different DuckDB connections ran one at a time**
  (#409): the lock that keeps a transaction's scans and sinks from using its
  pinned connection at once was one per attached catalog, so every
  transaction in the process waited for every other one's batch and drain,
  while SQL Server sat idle (measured by the mssql-ducklake side: 70 reads/s at
  16 readers). It is now the transaction's own; the catalog's is kept for
  autocommit on a pool of one connection, which every statement shares.

- **A pushed `LIMIT 0` cost a describe and an execution** (spec 081): DuckDB
  answers it with an empty result and asks the server nothing, so the rewriter
  now leaves it alone. DuckLake's attach probes every inlined-data table in one
  `UNION ALL` of `LIMIT 0` branches, and a thousand of them made each attach
  1.6-1.8x slower with pushdown on.

- **A query naming a missing table loaded every column of the catalog, and
  reloaded it every time when the database had a schema without tables**
  (#412). DuckDB walks every schema for its "did you mean" hint; that walk now
  reads table names only -- from the cache, or one names-only query for all
  schemas -- instead of building every table's entry with its columns. And a
  schema with no table or view (a new one, one holding only procedures) never
  counted as loaded, so each full listing of it ran the whole-catalog metadata
  query again; it now loads once, like any other. Measured by the
  mssql-ducklake side on a 4,724-table catalog database: 13-34 s a
  missing-table query, every time.

- **A CTE over an attached table, used more than once under a `LIMIT`, failed
  with `Table Function with name mssql_catalog_scan does not exist`.**
  DuckDB inlines such a CTE by copying its plan, and the copy looks the scan
  function up by name; the catalog scan was never registered. It is now
  (`mssql_catalog_scan`, internal: called by name it refuses), and the copy
  rebuilds the scan from the attached table, pushed filters included.

- **Spec 079 PR C follow-up review (roborev 1821): the `%` gate reached only the
  planner.** It is a case where the two pushdown walkers could still answer
  differently.
  - The `error_on_division_by_zero` gate was read in `BuildEncodeContext` (the
    planner's dry run) but **not** in `FilterEncoder::Encode`'s execution-time
    path or the optimizer's client-side-filter probe, which built their own
    `ExpressionEncodeContext` and left the flag at its default. A `%` filter
    reaching those without a matching dry run was still pushed and still failed
    with error 8134. `Encode` now takes the policy as a parameter **with no
    default**, so a new call site has to answer; all four paths (dry run,
    execution encode, optimizer probe, remote-pushdown writer) read it through
    one shared `LoadErrorOnDivisionByZero`.
  - Tests: the float case in `filter_arithmetic_types.test` could not detect a
    regression (1e200 + 1e200 and 2.0 + 2.0 are both representable, so the rows
    were the same either way) — a `1e308` row now separates them, since a pushed
    sum would raise on the server while DuckDB gives `inf` and returns the row.
    The `%` gate gained its first end-to-end case. `test_sql_writer.cpp` also
    pins the UNPARAMETERISED form of narrow-integer arithmetic, which nothing
    covered: an unparameterised literal already carries the column's type
    (`[tiny] + CAST(1 AS tinyint)`) through the cast `BindConstant` wraps it in,
    so the server overflows at 255 + 1 exactly as DuckDB overflows `UINT8`.

- **Spec 079 PR C review (roborev 1819): three divergences between the two
  pushdown paths.** All three are cases where the server and DuckDB would answer
  differently for a construct one path sent and the other did not — the
  invariant PR C's own `DATAMODEL.md` states as "a construct both paths take
  answers the same whichever takes it".
  - A **widening cast to `UTINYINT`** rendered as T-SQL `bigint`. `UTINYINT` is
    the only integer rank-1 type (SQL Server's `tinyint` is 0-255), so
    `CAST(tiny AS UTINYINT)` over a `tinyint` column passed the widening gate
    and then fell through the arm list to `bigint`, while the writer went on
    treating the operand as `UTINYINT`. `CAST(tiny AS UTINYINT) + 1` at
    `tiny = 255` therefore computed 256 on the server where DuckDB raises
    "Overflow in addition of UINT8". Now renders `tinyint`.
  - **`+` and `-` on `float` / `real` are no longer pushed**, as the product
    already was not. SQL Server's `float` cannot represent infinity, so an
    overflow is error 8115 — for every row `WHERE` passes, `TOP`'s discards
    included — where DuckDB returns `inf`; and because an encoded predicate is
    erased from the DuckDB plan, the statement fails rather than falling back to
    the client-side filter net. A product reaches the window near 1e154 and a
    sum near 1.8e308, but the divergence is the same one.
  - **`%` under `SET error_on_division_by_zero = false`** was refused by the
    remote-pushdown writer and still pushed by the scan path's filter encoder,
    so `WHERE i % j = 0` with a zero divisor failed with error 8134 through the
    scan where DuckDB alone answers `NULL` and simply omits the row. The gate
    moved into `ExpressionVocabulary::FunctionFor`, which both walkers call, so
    one answer serves both; `ExpressionEncodeContext` carries the session's
    setting to the encoder's walk.

- **LIKE pushdown is documented as the server's, and its case-insensitive
  form is gone (#392).** A simple `LIKE` is pushed as T-SQL `LIKE` and
  evaluated under the column's collation, exactly as `=` is (spec 079 D4): on
  a case-insensitive collation `LIKE '%abc%'` also returns `'ABC'`. The
  `LOWER(x) LIKE LOWER(p)` form for `iprefix` / `isuffix` / `icontains` was
  unreachable on the 2.0 pin, where `ILIKE` stays DuckDB's, and would have lost
  rows had it been reached: the server's `LOWER(N'ẞ')` stays `ẞ`, DuckDB's
  `lower` gives `ß`. It is removed so it cannot come back.

- **ORDER BY pushdown returned SQL Server's order for string keys**
  ([#362](https://github.com/hugr-lab/mssql-extension/issues/362)). With
  `mssql_order_pushdown` on, a pushed ORDER BY removes DuckDB's own sort, and
  nothing checked the key's collation. So `ORDER BY name` on a `varchar` under
  the installation default `SQL_Latin1_General_CP1_CI_AS` came back in
  linguistic order, with case and accents interleaved, instead of DuckDB's.
  - **Rule:** a key is now pushed only when the server sorts it as DuckDB
    does: numeric, `bit` and date/time types (not `datetime2(7)`, whose
    out-of-range values DuckDB reads as NULL). A `varchar` / `char` under a
    UTF-8 collation is pushed only under a LIMIT, as its bytes
    (`CAST(col AS varbinary(n))`, bounded `varchar` only — `char(n)` is read
    trimmed): as text the server pads with spaces, so `ab` + TAB would sort
    before `ab`. Not `nvarchar`, even under `_BIN2`: its
    UTF-16 order puts a character above the BMP before U+E000–U+FFFF, DuckDB
    after. Never a string-valued function (`upper(name)`) or a date part of a
    `datetimeoffset`.
  - **Never pushed:** `uniqueidentifier` (SQL Server compares its last six
    bytes first), `binary` / `varbinary` (compared zero-padded, so `0x01` =
    `0x0100`), `json`, `sql_variant`.
- **`upper()` / `lower()` in a filter could lose rows.** They were pushed to
  SQL Server, whose case mapping is not DuckDB's: `WHERE upper(name) =
  'STRAẞE'` found nothing, because the server's `UPPER('ß')` is `ß`. They are
  now applied by DuckDB, as `length()` already was; `trim`, `ltrim` and
  `rtrim` with them. A date part of a `datetimeoffset` is no longer pushed
  either: the server takes it in the value's own offset.
- **ORDER BY … LIMIT under-returned when a filter ran client-side.** With
  `mssql_order_pushdown` on, the TOP N went to the server while a filter the
  extension cannot translate (a `rowid` field of a composite key) was applied
  to what came back, so the server's N rows were filtered down, not refilled:
  `WHERE rowid.a >= 2 ORDER BY k LIMIT 2` returned 0 rows of 2. TOP N is now
  pushed only when every filter is on the server.
- **RENAME of a table or column with a dot in its name** failed: `sp_rename`
  parses the old name as a multi-part name and it was sent unbracketed.
- **Names containing `]` broke the bulk load and DELETE.** Several places put
  brackets around a table, schema or column name without doubling `]`: the
  `INSERT BULK` of COPY / INSERT / CTAS, a rowid DELETE, and the target-shape
  probe (whose `OBJECT_ID('…')` literal also broke on a `'`). Every identifier
  now goes through one quoter, which replaced eight copies of it.
- **Transactions fill the shared metadata cache again**
  ([#383](https://github.com/hugr-lab/mssql-extension/issues/383)). Since #380
  a transaction loads a missing table's metadata into a cache of its own, so a
  workload that runs everything in transactions (DuckLake) loaded every table
  once per transaction. At COMMIT or ROLLBACK what the transaction loaded and
  did not change is now published into the shared cache from memory, with no
  round trip: 100 transactions reading the same two tables load each table's
  metadata once. Nothing is published after DDL (the transaction's own through
  the catalog included, or `mssql_exec` DDL), under READ UNCOMMITTED, or when
  the shared cache was invalidated while the transaction ran. Inside a
  transaction the shared metadata cache is read too.
- **Reading the schema list no longer makes a transaction count as having used
  the catalog.** `MSSQLCatalog::SchemaListCache` asked the transaction for its
  metadata layer unconditionally, and that call CREATES this catalog's
  transaction — so any schema lookup DuckDB makes against an attached MSSQL
  catalog (search-path resolution, `duckdb_schemas()`, a "did you mean" scan)
  flipped `HasUsedAnyMSSQLCatalogInTransaction` even when it answered from the
  shared list and took no connection. `mssql_refresh_cache()` and
  `mssql_preload_catalog()` were then refused inside a transaction that had
  touched no server — the case
  [#380](https://github.com/hugr-lab/mssql-extension/issues/380)'s refusal was
  narrowed to allow. An already-loaded shared list is now read without creating
  anything; every other path creates the transaction as before, so a load still
  lands in its own cache.
- **A table created inside a transaction can be read in it; a pool of one
  connection works in a transaction**
  ([#380](https://github.com/hugr-lab/mssql-extension/issues/380)).
  - **The hang.** `BEGIN; COPY … TO 'mssql://db/dbo/new' …; SELECT … FROM
    db.dbo.new` waited out `mssql_metadata_timeout` (5 minutes by default). So
    did the same after `mssql_exec` DDL. The metadata load ran on a pool
    connection, which waits on the schema lock of the transaction's
    uncommitted CREATE. It has done so since spec 076.
  - **Pool of one.** With `mssql_connection_limit = 1`, the first table a
    transaction touched failed with `pool timed out (1 active of 1)`: its
    metadata wanted a second connection.
  - **The fix.** Inside a transaction every metadata load goes on the pinned
    connection, into a cache that belongs to the transaction. The catalog's
    shared cache is read for names the transaction did not change and is
    never written from inside a transaction. Loading into it was measured to
    leave a phantom table after ROLLBACK and to show another connection the
    uncommitted table. At COMMIT or ROLLBACK the shared cache forgets what the
    transaction changed.
  - **Refused inside a transaction that uses MSSQL.**
    `mssql_refresh_cache()` and `mssql_preload_catalog()` are bulk loads into
    the shared cache. They are refused once the DuckDB transaction has used
    any attached MSSQL catalog. Any, not just this one, because two ATTACHes
    of one database are independent catalogs (#389). A DuckDB transaction that
    touched no MSSQL catalog is not refused. `mssql_invalidate_cache()` is
    always allowed.
  - **A pool of one connection.**
    - *CTAS in a transaction* runs whole on the pinned connection: checks,
      CREATE, and rows as INSERT statements. ROLLBACK undoes it. It used to
      fail with `Failed to acquire connection to check table existence`.
    - *In autocommit*, a scan of the catalog the statement writes into is
      materialised first, whether it is a catalog scan or a raw `mssql_scan`.
      A COPY's or CTAS's bulk load then takes the one connection on its first
      chunk. COPY, CTAS and INSERT … SELECT from the same catalog used to wait
      out the acquire timeout.
  - **Extra parallel writers never wait for a connection.** For a COPY, CTAS
    or INSERT via BCP, an extra writer takes an idle connection or opens one
    under the limit. Otherwise it shares the main writer, asks again on a
    later chunk, and is not reported as a pool timeout. A connection the pool
    could not create, such as a login the server refuses, stops the asking. It used to wait
    `mssql_acquire_timeout` for a connection the statement itself held: a
    300k-row CTAS in a transaction on a pool of two took 30 s and now takes
    0.84 s. There are never more writers than `mssql_connection_limit`.
  - **Found along the way.** A CTAS on the INSERT path (`mssql_ctas_use_bcp =
    false`) reported its row count without the last batch: 1000 for 1500
    rows.
- **`mssql_preload_catalog` loads the schema list, marks only what it loaded,
  and reports it** ([#376](https://github.com/hugr-lab/mssql-extension/issues/376),
  [#375](https://github.com/hugr-lab/mssql-extension/issues/375)). Neither path
  loaded the schema list:
  - **Per-schema preload hid the other schemas.**
    `mssql_preload_catalog('db', 'dbo')` on a fresh ATTACH marked the list
    loaded while it held only `dbo`, so every other schema was gone for the
    session: `schema "test" does not exist`. It also marked every schema's table
    list and every table's columns loaded, so a schema looked up before the
    preload stayed at the one table that had been looked up.
  - **Whole-catalog preload was thrown away.** `mssql_preload_catalog('db')`
    left the list unloaded, so the first catalog access afterwards cleared the
    cache and ran the whole-catalog query again.

  Both paths now load the schema list first, which is one light query and free
  when it is already loaded, and mark only what they loaded. The schema name
  matches ignoring case when exactly one schema does, as it did when the name
  went to the server. A schema the catalog does not show, because it is absent
  or hidden by `schema_filter`, is refused by name instead of being reported as
  empty, and it gets no phantom entry. The summary counts what the call loaded,
  and the numbers are the same on every call. Before, a second preload said
  `Preloaded schema 'dbo': 0 tables, 358 columns`.
- **Boolean ATTACH options accept a string value**
  ([#325](https://github.com/hugr-lab/mssql-extension/issues/325)).
  `lazy_validation`, `catalog` and `order_pushdown` were read through an
  integer cast, so `lazy_validation 'true'` failed with `Could not convert
  string 'true' to INT8` — and a string is the only thing DuckLake's
  `METADATA_PARAMETERS` can send, since it is a `MAP(VARCHAR, VARCHAR)` whose
  values reach the inner `ATTACH` quoted. None of the boolean options were
  reachable for a `ducklake:mssql:` catalog. A string now goes through DuckDB's
  own boolean cast, the one `SET` uses (`true`/`false`, `t`/`f`, `yes`/`no`,
  `y`/`n`, `1`/`0`, any case), and anything else is refused with the option
  named.
- **A debug build no longer asserts on `SELECT k, rowid` over a string key
  column** ([#369](https://github.com/hugr-lab/mssql-extension/issues/369)).
  With `mssql_catalog_native_types` on (the default) the projected column is
  `MSSQL_VARCHAR(n)` / `MSSQL_NVARCHAR(n)` while the rowid — the scalar one and
  every child of a composite key's STRUCT — is typed from the column's plain
  type; copying the projected vector into the rowid slot was a typed copy
  across the two, which a debug build refuses (`source_p.GetType() ==
  target.GetType()`) and a release build performed on the same bytes. The
  copy now goes through a reinterpreting view of the source in the rowid's
  type. The rowid suite runs against the debug build in CI from here on, so
  the class stays caught.
- **An `UPDATE`/`DELETE` through a `varchar` key under a `SQL_` collation no
  longer changes rows it was not given.** Every rowid value is sent as an
  `N'…'` literal, so the key join compared a `char`/`varchar` key column under
  its collation's Unicode rules, where `'Straße'` equals `'Strasse'` and `'Æ'`
  equals `'AE'` — while the unique index, under the non-Unicode SQL sort
  order, holds them as distinct keys. Measured on
  `SQL_Latin1_General_CP1_CI_AS`, the installation default: deleting the
  `'Straße'` row deleted `'Strasse'` too, and updating `'Æ'` updated `'AE'`,
  both without an error. The join now converts the sent value back to the
  column's own type and collation, so the comparison is the one the index
  made. Present for a `varchar` primary key before spec 077; spec 077 would
  have extended it to every such unique index. Windows and UTF-8 collations
  apply the same rules to `varchar` and `nvarchar` and are unchanged.

- **An `INSERT` that supplies identity values works** (spec 077 W2). Naming the
  identity column — explicitly, or positionally with a value for every column —
  used to fail with the server's error 544; the statement's connection is now
  bracketed with `SET IDENTITY_INSERT … ON` before its first batch and `OFF`
  before its `COMMIT`, and the values land verbatim. `OFF` is guaranteed on
  every way out — after a failing batch, after `ROLLBACK` on a transaction's
  pinned connection, and from the destructor on an unwind, bounded by a
  timeout since that path carries no query timeout — and a connection on which
  it could not be confirmed is discarded rather than returned to the pool. A
  transaction's pinned connection cannot be discarded: there an unconfirmed
  `OFF` lasts until the transaction ends, whose session reset clears it under
  the default `mssql_reset_connection = true` and, by that setting's meaning,
  not under `false`.
  Measured both ways: with the `OFF` deliberately leaked, the next ordinary
  `INSERT` on that session fails with the server's 545. The server's refusals
  of the `ON` are explained rather than relayed: 1088 says the statement needs
  ALTER on the table (the server's own text claims the table may not exist),
  8106 that the catalog's `is_identity` is stale and `mssql_invalidate_cache()`
  is the fix, 8107 that another table is already `ON` for this session and
  where that could have come from. A NULL in a named identity column is refused
  before anything is sent — DuckDB hands `DEFAULT` and `NULL` to the extension
  identically — with the way out named, which also covers a batch mixing rows
  that supply the value with rows that do not. **This stays the statement
  path:** tens of rows, not millions; loading many rows with their identity
  values is `COPY`, which keeps a source column named like the identity column
  and lets the server assign when it is omitted.

- **`rowid`, and with it `UPDATE`/`DELETE`, no longer require a primary key**
  (spec 077 W1). A table with no primary key but a usable unique index — one
  that is not filtered, not disabled, and whose key columns are all NOT NULL —
  gets its rowid from that index; a `BIGINT IDENTITY … UNIQUE` is the common
  shape. Among several, the order is documented and unit-tested: a
  single-column identity key first, then the fewest key columns, then the
  narrowest, then the lowest `index_id`. A usable primary key still wins.

  **A primary key that cannot address a row now falls through instead of being
  taken.** A `DATETIME` key produced an `UPDATE` that reported success and
  changed nothing ([#358](https://github.com/hugr-lab/mssql-extension/issues/358):
  a `datetime` value with a 1/300-second fraction — `.003`, `.007`, most of
  them — equals no `datetime2` literal at any precision; one on a whole 10 ms
  did match, and is refused with the rest), a
  `TIME(7)` or `DATETIMEOFFSET(7)` key does the same the other way round (the
  read path keeps microseconds, so a key whose 100 ns digit is set comes back
  truncated and its literal never matches — measured: three such rows, one
  updated), and a `SQL_VARIANT` key does the same through its lossy read
  ([#354](https://github.com/hugr-lab/mssql-extension/issues/354)).
  `SMALLDATETIME` and `DATETIME2(7)` keys match their literals and stay usable. Such a
  table now uses another unique index if it has one, and otherwise refuses by
  name — a behaviour change, and a deliberate one, since what it replaces is a
  statement that did nothing and said so to nobody. Every refusal names what
  was looked for and why each candidate was rejected: the index, the column,
  and the reason. The discovery query itself lost its `sys.types` join, which
  dropped every CLR UDT key column and could return a primary key with a column
  missing — and its `sys.key_constraints` join with it. Returning every unique
  index therefore costs no more than returning the one primary key did, and on
  a small catalog less: measured server-side, 2000 executions a run, four
  interleaved runs, 151 → 38 µs on the test database (a primary key and two
  ordinary indexes; 267 → 92 µs with eight unique indexes). On a catalog of
  3000 tables and 9200 indexes the joins matter less and the extra rows a
  little more: 58 → 49 µs against the query as it shipped, and 6 µs (14%)
  more than a primary-key-only query without the joins would cost. Tens of
  microseconds either way, inside a batch that already pays a round trip.

- **`NOT IN` on a string column reaches the server**
  ([#366](https://github.com/hugr-lab/mssql-extension/issues/366)). DuckDB
  binds `v NOT IN (...)` as `NOT (v IN (...))`, and the filter encoder had no
  case for `IN` as an operator expression (only for the table filter the
  combiner builds from a bare-column `IN`), so `NOT IN` — and `IN` over an
  expression such as `n + 1 IN (2, 3)` — ran client-side after a full
  transfer, and under DuckDB's byte equality: on a `_CI` collation
  `v NOT IN ('ab', 'x')` kept `ab␣` and `AB` where `v <> 'ab'` beside it did
  not. Both now render as `[v] [NOT] IN (@p1, @p2)` with the list declared
  from the operand's column (spec 076), the server's answer like every other
  string predicate (spec 079 D4), and a seek where the column is indexed.
  `NOT IN` lists, and `IN` over an expression, longer than 256 items stay
  client-side, as they did before this change (each item is one of the
  statement's 2000 parameters; the cap is per predicate); a bare-column `IN`
  is a table filter and is not capped, as it never was.
- **A non-ASCII constant no longer costs the index seek on a `SQL_` collation**
  ([#361](https://github.com/hugr-lab/mssql-extension/issues/361)). Spec 076
  declared any non-ASCII constant of a pushed filter as an `nvarchar`
  parameter whatever the column's collation — right for a UTF-8 column on a
  Latin-1 database (#321: a `varchar` parameter takes the *database's* code
  page), and an Index Scan with `CONVERT_IMPLICIT` on the column for the
  installation default, `SQL_Latin1_General_CP1_CI_AS`, where `'ñu'`,
  `'Müller'`, `'café'` read the whole index (Windows collations seek through
  the convert and were unaffected). The declaration is now `varchar` when every
  character of the constant is representable in **both** the column's and the
  database's code page, and `nvarchar` otherwise, exactly as before. The pages
  are the server's own answer — `COLLATIONPROPERTY(collation, 'CodePage')` now
  rides with the column metadata and the database collation — and
  representability is answered from tables for the single-byte pages 874 and
  1250–1258; a double-byte or unknown page keeps the safe form. `text` columns
  follow the same rule (their `varchar(max)` parameter turned `'ы%'` into
  `'?%'` and `LIKE` matched rows it should not). **One visible consequence on
  a `SQL_` collation:** the comparison now runs under the collation's own
  (non-Unicode) sort rules instead of the Windows Unicode rules an `nvarchar`
  parameter forced — `'ß' = 'ss'` was true and is now false on
  `SQL_Latin1_General_CP1_CI_AS`, which is what a T-SQL `varchar` literal
  answers there.
- **`COPY` no longer drops a CLR UDT column of an existing target in silence**
  ([#353](https://github.com/hugr-lab/mssql-extension/issues/353)). The
  target-metadata query joined `sys.types` on `system_type_id`, which no
  `sys.types` row satisfies for a `geometry`, `geography` or `hierarchyid`
  column, so the column never reached the resolver: a source column feeding one
  was ignored and its values lost, and a NOT NULL target failed with "Cannot
  insert the value NULL into column …" about a value the user had supplied. The
  join is gone from all eight metadata queries — the name comes from
  `ISNULL(TYPE_NAME(c.system_type_id), TYPE_NAME(c.user_type_id))`, which is
  correct for UDTs and **8.7× cheaper on the query itself** (1316 µs → 152 µs
  per execution on a 101-column table, 2000 executions a run, four interleaved
  runs). The join is expensive not by itself — without the `ORDER BY` it costs
  2.7× — but because it defeats the index order on `sys.columns` and makes the
  server sort for the `ORDER BY c.column_id` every one of these queries
  carries. A source that feeds such a column is now
  refused by name, **at init, before a row is encoded** — so a COPY that is
  going to fail this way writes nothing rather than committing the batches that
  happened to precede the first value. A source that omits such a column still
  loads and the server fills it, exactly as before, and so does one that gives
  it a constant `NULL AS g`: that is how one says "leave this column alone", it
  worked before #353 (by accident — the join hid the column), and it is safe
  because DuckDB types a bare NULL as SQLNULL, which has no other value it
  could hold. Such a column is dropped from the load rather than declared;
  declaring it would make the server refuse the whole `INSERT BULK`, since the
  type mapping gives it the VARCHAR fallback. The two non-UDT types the change
  also refuses,
  `sql_variant` and `rowversion`, lose nothing: measured against the server,
  both already failed mid-stream — `Operand type clash: nvarchar(max) is
  incompatible with sql_variant` and error 273 respectively — so the refusal
  only moves the error earlier and names the column.
- **A transaction's first statement could fail with `Cannot execute: connection
  not in Idle state`** ([#356](https://github.com/hugr-lab/mssql-extension/issues/356)).
  The connection a transaction pins was published to other threads **before**
  `BEGIN TRANSACTION` had finished on it, and DuckDB initialises a plan's source
  and its sink on different threads — so the second one could execute on a
  connection that was still mid-BEGIN. Intermittent, about 4% for a statement
  that both reads from and writes to the same catalog inside a transaction, and
  it also leaked the connection out of the pool when it struck. Acquiring,
  beginning and publishing are now one critical section per transaction, so a
  second thread waits and then finds a connection that is pinned, begun and
  idle. Measured: 250 runs of the case that used to fail, in two independent
  batches, with no failures.

### Added

- **`INSERT … RETURNING` works against a table holding a column the wire cannot
  decode raw** — a spatial UDT, `sql_variant`, `hierarchyid`. It used to fail
  with `COLMETADATA parse error: Unsupported SQL Server type: UDT` **even when
  the RETURNING list did not name that column**, because the generated `OUTPUT`
  clause carries every column of the table: DuckDB's RETURNING projection sits
  above the insert and expects the table's full width. The OUTPUT list now uses
  the same expressions the read path does, `.STAsBinary()` for the spatial
  types and a CAST to NVARCHAR(MAX) for the rest, so those columns come back in
  the same shape a catalog scan gives them.

- **A `GEOMETRY` value can be written into a `geometry` / `geography` column**
  (#296). It used to go as a bare `0x…` literal, which SQL Server reads as its
  own Spatial Type Binary Format rather than as the OGC WKB a DuckDB GEOMETRY
  carries, and rejects: `24210: Geometry type with an unexpected version of 0
  received`. INSERT and UPDATE now wrap it as
  `geometry::STGeomFromWKB(0x…, srid)`, the server-side reader for that form.
  The **SRID is an assumption**, because `.STAsBinary()` does not carry one and
  a value read from SQL Server has already lost it: a `geometry` target gets 0
  (planar, undefined) and a `geography` target gets 4326 / WGS 84, since
  geography refuses 0 outright. Set another one server-side after the load. An
  INSERT naming a spatial column stays on the statement path whatever
  `mssql_insert_bcp_threshold` says, as it always has — the bulk wire would
  declare the column nvarchar and send the WKB as text.

- **`ROWVERSION` columns are readable, and the native `JSON` type of SQL
  Server 2025 is read uncast** ([#296](https://github.com/hugr-lab/mssql-extension/issues/296)).
  Neither type name was in the catalog's table, so both took the unknown-type
  route, `CAST(col AS NVARCHAR(MAX))`. For `rowversion` that is not merely
  wasteful, the server **refuses** it — `[529] Explicit conversion from data
  type timestamp to nvarchar(max) is not allowed` — so a table carrying such a
  column could not be read at all. It is `binary(8)` on the wire and now reads
  as `BLOB` with no conversion; the name to look for in `sys.types` is
  `timestamp`, which has nothing to do with time. Do not write it: SQL Server
  refuses an explicit value with error 273, so leave it out of the INSERT
  column list. The 2025 `JSON` type arrives as `varchar(max)` under a UTF-8
  collation and now reads as `VARCHAR` directly, where the CAST used to convert
  every value server-side for nothing. `COPY` into an existing table with a
  `JSON` column is accepted too: the compatibility table listed `xml` for a
  VARCHAR source but not `json`, so a COPY was refused at bind while an INSERT
  into the same column worked.

- **INSERT loads through BCP** (spec 062). An INSERT with more rows than
  `mssql_insert_bcp_threshold` (default 1000), no `RETURNING` and no
  explicitly named identity column goes through `INSERT BULK` — the wire
  COPY and CTAS use — on the transaction's pinned connection or on a pool
  connection inside a server transaction of its own, with parallel writers
  where their loads cannot block each other: a target with no nonclustered
  index on it, either a heap under TABLOCK or a clustered columnstore
  without it. SQL Server gives concurrent bulk loaders compatible BU locks
  only on a bare heap — add a nonclustered index and the same hint takes a
  Sch-M lock, and on a columnstore the extra writer fails its load outright. Measured: 1M
  rows × 3 columns into an existing table in 1.8 s on one writer and 0.5 s
  on four, where the
  statement path took 74 s. The rows are staged until the threshold decides
  the path, so the decision is exact; `RETURNING`, an identity column and
  `mssql_insert_use_bcp = false` keep the statement path. The bulk wire
  carries `CHECK_CONSTRAINTS, FIRE_TRIGGERS, KEEP_NULLS` so the INSERT still
  checks constraints, fires triggers and keeps its NULLs — a bulk load
  ignores all three by default, and COPY still does. A failed load names
  the batch and says `rolled back`. The batch size, TABLOCK policy and
  writer count are the `mssql_copy_*` settings. Reviewed by
  [@oluies](https://github.com/oluies), who found the writer rule blind to a
  nonclustered index and pushed the fix (#349, merged into #348) — a heap
  carrying one takes Sch-M rather than BU, and the extra writers stall 30 s
  behind it.

- **The catalog knows which columns are IDENTITY** (spec 062 W4, the
  metadata half of #327). `sys.columns.is_identity` rides in the four
  column-metadata queries and on `MSSQLColumnInfo`; the INSERT planner reads
  it instead of hard-coding false. It is what routes an INSERT that names an
  identity column onto the statement path. The other half of #327 — omitting
  the column from a column-list-less INSERT — is not reachable from an
  extension: DuckDB's binder compares the value count against the columns the
  catalog reports, before any extension code runs. See spec 077.

- **Pushed filters are parameterised** (spec 076). The constants of a pushed
  filter travel as `sp_executesql` parameters declared from the column they
  are compared with, so the statement text is one fixed string per filter
  shape and SQL Server keeps one plan for it — 40 scans with 40 distinct
  two-predicate filters left 40 ad-hoc plans (2.3 MB) before and leave 1
  (57 KB) now. Declared from the column, not the constant: varchar stays
  varchar and an index on it stays seekable, the width is never narrower
  than the constant, a non-ASCII constant goes as nvarchar whatever the
  column's collation (a varchar variable takes the database's code page). `IN` lists and `IS NULL` stay literal.
  `mssql_scan_parameterize_filters = false` restores literal SQL, the
  escape hatch for parameter sniffing.
- **A fresh table's first touch is one round trip** (spec 076 W2). The
  catalog loaded a table's metadata and columns in one query and its primary
  key in a second, on a second connection; the discovery statement now rides
  in the same `sp_executesql` batch as the table's metadata (a second result
  set off the same `@s` / `@t`) and the entry is created with its key already
  known. Against a remote server that is the difference between two login
  round trips and one before the first row.
- **ATTACH initialised the catalog twice** (spec 076 W2). The storage
  extension's attach callback called `MSSQLCatalog::Initialize` and DuckDB's
  `AttachedDatabase::Initialize` called it again right after: the second
  call built a second connection pool (the first, with its freshly logged-in
  connection, was thrown away), logged in again and asked the database
  collation again — two of the three logins an ATTACH cost (#324), and the
  "collation query twice" the spec measured. `Initialize` is now a no-op
  once the pool exists.

- **`mssql_scan_params` and `mssql_exec_params`** (spec 075): the raw-SQL
  functions with parameters. `mssql_scan_params(ctx, statement, {'id': 42,
  'since': TIMESTAMP '2024-01-02'})` sends `@id` and `@since` through
  `sp_executesql`, declared from the DuckDB types (`VARCHAR` →
  `nvarchar(4000)`, or `nvarchar(max)` past 4000 bytes; `MSSQL_VARCHAR(20)` →
  `varchar(20)`; `TIMESTAMP` → `datetime2(6)`; `DECIMAL(10,2)` →
  `decimal(10,2)`; the full table is in `docs/query-execution.md`) or from
  the caller's own list as a fourth argument (`'@ts datetime, @c
  varchar(8)'`). The server keys its plan on the statement text and the
  declarations, so every call — from any session — reuses one compiled plan
  instead of compiling per distinct value; a statement `mssql_exec_params`
  runs for a thousand rows compiles once. A bare `NULL`, a LIST or STRUCT
  value and a key that is not a T-SQL identifier are refused at bind with
  the fix named.
- **`mssql_scan(..., prepared := true)`** (spec 075): compile once via
  `sp_prepare` — the shape comes from the prepare's answer and execution is
  `sp_execute` by handle, on the session that holds it. The default path
  describes at bind and compiles again at execution; the server's ad hoc
  cache makes that second compile cheap, but a statement the plan cache
  will not keep (`optimize for ad hoc workloads`, a large batch) pays it in
  full. A statement the server prepares without a shape — a batch of more
  than one statement — degrades to the default path.

### Changed

- **A pushed statement whose every column the extension types is planned
  without a round trip** (spec 081): the remote-pushdown rewriter sends it
  through `mssql_scan_params_unsafe` with the catalog's types, so its first
  run takes one connection instead of two and its bind needs no connection
  (77% of the pushdown suite's statements). A statement with a column the
  server types (`g + 1`) or one cast back after the read (an integer `sum`) is
  described as before, through
  the shape cache. Over a table changed outside the catalog such a statement
  now fails at execution rather than at bind, naming the statement and
  `mssql_invalidate_cache()`.

- **`mssql_remote_pushdown` is on by default** (spec 079 PR E2). A SELECT
  over one attached database with a join, an aggregate, `DISTINCT`, `ORDER
  BY` or `LIMIT` runs on SQL Server as one statement. **String
  `GROUP BY`, `DISTINCT`, `count(DISTINCT …)`, join keys, `PARTITION BY` and
  a nested `UNION`'s deduplication now follow the column's collation**, as `WHERE`
  already did: on a case-insensitive collation (`SQL_Latin1_General_CP1_CI_AS`
  is SQL Server's installation default) `'a'` and `'A'` are one group —
  `count(DISTINCT legacy)` over `'a', 'A', 'b', 'B'` is 2 where it was 4.
  String orders stay DuckDB's. `SET mssql_remote_pushdown = false;` before
  `ATTACH` (or the ATTACH option `remote_pushdown false`) restores the
  previous behaviour. While on, the database's schema
  `main` answers as its default schema, for DDL too (`DROP TABLE
  db.main.x` drops `dbo.x`), and DuckDB skips its catalog-or-schema ambiguity
  check for it.
- **An error names its cause.** When the server answers a metadata query, an
  `mssql_exec` or a result-shape describe with several errors, the first one
  is reported (it was the last), as a result stream already did: a pushed
  statement over a dropped table says `Invalid object name 'dbo.t'` instead
  of `sp_describe_first_result_set`'s "see previous error".
- **No INSERT statement carries more than 1000 constants** (spec 062 W1b).
  SQL Server auto-parameterises a multi-row `VALUES` INSERT — one cached
  plan per (table, column list, row count), compiled once — only up to 1000
  constants; past that every statement compiles its own ad-hoc plan and
  leaves it in the cache. The default of 1000 rows per statement put every
  table with two or more columns past the line: a 1M-row `INSERT … SELECT`
  into a 3-column table cost 74 s, 70 µs a row, all of it compile. Rows per
  statement are now `min(mssql_insert_batch_size, 1000 / columns)` — 333 for
  three columns, at 14 µs a row (measured 5.5× at the boundary). This is the
  statement path only; the bulk path is spec 062's main work.

- **One bulk-load session type for every writer** (spec 062 W0). COPY's and
  CTAS's shared writer — the one on the operator's own connection — ran their
  own copies of the INSERT BULK / COLMETADATA / flush-and-reopen / DONE
  sequence; both now run it through `BulkLoadSession`, which the parallel
  writers already used, via a second entry point that adopts a connection the
  operator holds (the transaction's pinned one, or a pool connection). No
  wire change. One release path changed: a COPY or CTAS returning its pool
  connection after a successful load now sets the reset flag from
  `mssql_reset_connection`, as every other release path did — before, the
  success path returned it without. And a CTAS whose SELECT yields no rows
  no longer opens a bulk stream at all (it used to send `INSERT BULK` and a
  zero-row `DONE`); the table is created the same.

- **`mssql_scan` no longer executes its query at bind** (spec 075, #336).
  Bind asks `sp_describe_first_result_set` for the result's shape; the query
  runs when the scan initialises. A `DESCRIBE` or `EXPLAIN` of a batch with
  an `INSERT` ahead of its `SELECT` therefore inserts nothing, and a query
  with a side effect runs exactly once. A statement the server cannot
  describe — a batch that reads a `#temp` table it creates, a procedure — is
  still run at bind, as before. Should the shape at execution differ from
  the one bound, the scan fails loudly rather than serving rows of another
  shape.
- **The per-table metadata queries are parameterised** (spec 075, #334).
  The table list, a table's columns, its primary key and its row-count
  estimate used to carry the schema and table names inside the query text,
  so the server compiled a plan on every first touch of a table — measured
  at ~30 ms per table before, 0–1 ms after, because the text is now one
  fixed string per shape with `@s` / `@t` as `sp_executesql` parameters.
- **A DML batch whose response cannot be parsed is an error, not a silent
  success** (#344). In autocommit UPDATE and DELETE run in batches of
  `mssql_dml_batch_size` and INSERT in statements of `mssql_insert_batch_size`,
  each committed on its own; a desync in batch K therefore leaves batches 1..K
  applied and K+1.. never sent, and the error says exactly that. Chosen over
  the previous behaviour, which applied everything and reported success with
  the server's own errors after the desync dropped, and as an interim: spec
  062 and the UPDATE/DELETE rework move DML to one server-side statement per
  DuckDB statement, and the batch boundary goes with them.

- **Breaking: the server certificate is verified by default** (spec 074).
  `Encrypt`, `TrustServerCertificate` and the new `HostNameInCertificate` now
  mean what they mean in the Microsoft drivers (ODBC 18, SqlClient 4.0, JDBC
  10.2, go-mssqldb): `Encrypt` (default `true`) says whether the session is
  encrypted; `TrustServerCertificate` (default `false`) whether the server's
  certificate chain is checked against the platform trust store (Windows
  ROOT + CA, macOS keychain, OpenSSL's default paths; `SSL_CERT_FILE` /
  `SSL_CERT_DIR` honoured) and its subject against the host connected to;
  `HostNameInCertificate` names the subject when it differs from the address
  dialled (an IP, an SSH tunnel, an alias), and after a login-time routing
  hop the routed host is checked unless it was given. `TrustServerCertificate`
  used to be an alias of `Encrypt`, and giving both with different values was
  an ATTACH error; `Encrypt=true;TrustServerCertificate=false` is now simply
  the default. **A connection to a server on a self-signed certificate** --
  the docker image, an on-prem instance with none installed -- **now fails**
  with `certificate verification failed for <host>: self-signed certificate`
  and needs `TrustServerCertificate=yes`, exactly as `sqlcmd -C` does. The
  message names that, and `HostNameInCertificate=<name>` for the case where
  the certificate is valid but issued for another name. URI spellings
  `trustservercertificate` / `hostnameincertificate`; secret fields
  `trust_server_certificate` / `host_name_in_certificate`.

### Removed

- **`mssql_open()` / `mssql_close()` / `mssql_ping()` / `mssql_close_all()`**,
  the standalone diagnostic-handle API, and the `MSSQLConnectionHandleManager`
  singleton behind it. Marked `[DEPRECATED]` in spec 047 (v0.2.1) for removal
  at the next major boundary, which the first release on the duckdb 2.0 line
  is; it was the last extension-internal process-wide state. `ATTACH` plus
  `mssql_scan` / `mssql_exec` / `mssql_pool_stats` cover every use it had,
  inside the catalog lifecycle and the per-catalog pool.

### Fixed

- **`INSERT … RETURNING` returned only the last statement's rows** once the
  insert spanned several statements: the executor kept the last result
  chunk "for simplicity", so a RETURNING insert of more than one
  statement's worth of rows (1000 before, `1000 / columns` after the cap
  above) silently lost the earlier rows from its RETURNING output while
  inserting all of them. Every statement's rows come back now
  (`insert_returning_batches.test`).

- **A failed INSERT, UPDATE or DELETE leaves the table as it was** (spec 062
  W1c, closes #344). The three statement executors took a pool connection
  per batch in autocommit, and each batch committed on its own, so a failure
  in batch K left batches 1..K-1 applied with no way back; the interim of
  #344 could only say so in the message. A statement now runs on ONE
  connection — the pinned one inside a DuckDB transaction — and in
  autocommit brackets its batches in a server transaction of its own:
  `BEGIN TRANSACTION` before the first, `COMMIT` after the last, `ROLLBACK`
  on any failure. The message says what happened to the rows: `rolled back`
  in autocommit, or that they sit in the open transaction until its
  `ROLLBACK`.

- **Two reads of one catalog inside a transaction** (spec 075, #329). Two
  `mssql_scan` calls in one statement, or an `mssql_scan` beside a catalog
  scan, collided on the transaction's pinned connection (`Cannot execute:
  connection not in Idle state`) because the raw scan held it from bind to
  the last row. Inside a transaction every scan of a catalog now drains at
  initialisation under the catalog's materialise lock, in whatever order
  DuckDB initialises them.
- **A sink reading from the catalog it writes to, inside a transaction**
  (spec 075, #239). `COPY (SELECT ... FROM srv.dbo.t) TO 'mssql://srv/...'`
  and `INSERT INTO srv.dbo.t2 SELECT ... FROM srv.dbo.t` past one batch
  failed with `Connection is busy executing another query`: the bulk load or
  the second INSERT batch went down the pinned connection the source scan
  was still streaming from. The planner now materialises the scans of any
  catalog the plan also sinks into, and the bulk load opens its stream
  (`INSERT BULK` + COLMETADATA) on the first chunk rather than when the sink
  is created -- DuckDB creates the sink before it initialises the source, so
  an open stream at that point would have collided with the source's drain.
  Documented as a limitation until now.
- **A TDS desync in a DML response is reported, names what the server did,
  and never hangs** (#323 follow-up, #344; the four response loops of the
  INSERT batch, `INSERT ... RETURNING`, UPDATE and DELETE). A parser that
  desynced mid-response was fed the whole remaining response for nothing and
  then reported success — every token after the desync dropped, including a
  SQL Server error following a procedure call. `INSERT ... RETURNING` did
  worse: its loop had no end-of-message exit, so it blocked for its full 30 s
  and then blamed the socket, leaving the connection `Executing`. Each loop
  now stops feeding a parser in Error, drains to end-of-message, and fails
  with the parse error; the message says the server executed the batch (a
  desync is a client-side framing failure, noticed after the batch ran) and
  how many rows the batches before it applied. A parse error no longer
  renders as `[0]`, which read as a server error code. Test lever:
  `mssql_test_fail_parse_after_tokens`. Found on the way: an INSERT error
  named the statement after the failing one and an empty row range
  (`statement 1 (rows 1000-999)`), because the batch builder had already
  moved past the batch by the time it ran; the batch's own index and range
  are reported now.
- **`mssql_pool_stats.last_create_error` carries its age.** A pool at its
  limit recovers by reuse and never reaches the creation-success path that
  clears the recorded error, so the column could show a reason the pool had
  outlived for hours — or, had a reuse cleared it, blank out a live failure
  (an expired Azure AD token beside warm connections). The new
  `last_create_error_age_ms` says which; the creation backoff of #302 is
  untouched by reuse.
- **CTAS names why it could not get a connection.** Its four `Acquire` calls
  (existence checks, the bulk load, the cleanup DROP) threw without the
  reason the pool now records; an expired token read as "Failed to acquire
  connection to check table existence" and nothing more.

- **An attached Azure AD catalog can open new connections after its token has
  expired** ([#302](https://github.com/hugr-lab/mssql-extension/issues/302),
  spec 073). The pool factory captured the FEDAUTH token bytes at `ATTACH` and
  presented them for every connection it ever created; an Azure AD token lives
  **60 minutes**, so a pool refill hours later — a second scan in a join, an
  `UPDATE` after the idle connection was reaped — was dropped by the gateway
  and reported as `Failed to acquire connection from pool (timeout)` after the
  full `mssql_acquire_timeout` (measured: 600 s, then `DETACH`/`ATTACH` and it
  worked). The token is now resolved when a *connection* is created: the
  factory holds the secret's name and the `DatabaseInstance`, `TokenCache`
  answers while the token is good and the secret is re-read and a new token
  minted past the refresh margin. A fixed `access_token` secret, and an
  interactive credential chain that cannot mint silently from a worker thread,
  fail **by name** — *"expired at …; DETACH and ATTACH …"* — before any socket
  is opened, because Azure SQL will not say "expired" itself: it hangs up.

- **A connection the pool cannot create no longer looks like a full pool.**
  `Acquire` treated a factory failure as exhaustion and waited for a `Release`
  that, with nothing active, could not come — then said "(timeout)" and threw
  the reason away. Now: nothing active → fail at once, every time; others
  active → keep waiting for a release, but retry creation on a backoff
  (250 ms doubling to 4 s), not on every wakeup; and the message carries the
  reason of **this** call's attempt — `could not create a connection: Login
  failed for user 'sa'.` — while a timeout on a pool that has since recovered
  is reported as the timeout it is. A wrong password used to take
  `mssql_acquire_timeout` seconds to say nothing.

- **`mssql_connection_timeout` now bounds a pool refill.** Every factory passed
  no timeout to `Connect` (compiled-in 30 s), and every login-phase read —
  PRELOGIN, TLS, LOGIN7 and FEDAUTH responses, on all three auth paths — had
  `DEFAULT_CONNECTION_TIMEOUT` spelled out. The setting governed `ATTACH`-time
  validation and nothing after it. This is the reporter's "31 seconds": one
  30 s login read on a connection the gateway had dropped.

- **Stored-procedure calls desynced the TDS parser**
  ([#323](https://github.com/hugr-lab/mssql-extension/issues/323), spec 072).
  RETURNSTATUS (0x79) is a fixed five-byte token with no length field, and the
  parser skipped it as if it carried one: a procedure returning 0 left two bytes
  behind, one returning *n* ate *n* bytes of the DONEPROC that follows. Through
  `mssql_scan` that was `TDS parse error: Unknown token type: 0x0` and a
  discarded connection. Through `mssql_exec` it was **worse and silent**: the
  batch reported success with every token after the procedure dropped —
  including a SQL Server ERROR raised after `EXEC`, so `EXEC p; RAISERROR(…)`
  returned 0 with no error. Three more in the same group, found by the survey:
  TABNAME was registered as 0x04 (the wire says 0xA4, so any `FOR BROWSE`
  result failed); RETURNVALUE has no length field either and now fails by name
  instead of mis-skipping (it is unreachable — sent only for RPC, which the
  extension does not issue); and the unknown-token message printed decimal
  after `0x`, so 0xA4 read as `0x164`. `mssql_exec` now reports a parser error
  as an error rather than as end-of-batch, after draining to EOM so the
  connection stays reusable.

- **A metadata load that fails mid-query no longer mutates the cache**
  ([#317](https://github.com/hugr-lab/mssql-extension/issues/317), reported by
  [@oluies](https://github.com/oluies)). `LoadAllSchemasMetadata` cleared each
  schema's table map from inside the row callback and only re-published
  `tables_load_state` after the query returned, so a non-retryable throw —
  metadata timeout, reset connection, killed session — left every schema the
  callback had touched **emptied while still carrying its previous load
  state**. The reset lambda covered the 1205 retry path only. It is the shape
  the #178 review fixed for `Refresh()`, and spec 071 widened the blast radius
  from one schema to the whole catalog by replacing the per-schema loop with a
  single query. Nothing is written into the cache now until the query has
  returned.

  The same defect was in the sibling loader — `BulkLoadAll`'s per-schema branch,
  where `mssql_preload_catalog(ctx, 'schema')` routes — and **there it was
  observable** (found by @oluies reviewing the first fix). That loop cleared the
  columns of a table already cached and already marked `LOADED`, so a throw
  before publication left it claiming to be complete with only the columns that
  had arrived: a two-column table came back from `SELECT *` with one column, for
  the rest of the session. Both loaders are staged now.

### Added

- **`mssql_test_fail_metadata_after_rows`** (test-only, default 0 = off): make
  a metadata query throw after N rows. Every cause of a mid-query metadata
  failure in the wild arrives from outside and cannot be asked for from SQL, so
  without it the invariant above is untestable — which is how it came to be
  broken twice. Off costs nothing: the row callback is passed through unwrapped.

- **`MSSQL_VARCHAR('MAX', 'collation')` — a per-column MAX target**
  ([#321](https://github.com/hugr-lab/mssql-extension/issues/321)). The
  annotation was capped at 8000/4000, so the MAX form could only be reached by
  leaving a column *unannotated* — where which of `varchar(max)` /
  `nvarchar(max)` it becomes, and with which collation, is decided by two
  GLOBAL settings. Library code creating tables through CTAS or
  `COPY … (CREATE_TABLE true)` cannot set those: it would change the type of
  every other unannotated string column in the caller's session. `'MAX'`, `0`
  and `-1` are now all accepted in the length position and mean the same thing
  (`0` matches `mssql_default_string_length`, `-1` matches
  `sys.columns.max_length`). A bare `MAX` keyword cannot be supported — DuckDB's
  parser rejects a non-constant type modifier before the extension is consulted
  — note that this is also the form the type *prints* as (`MSSQL_VARCHAR(MAX)`
  in `DESCRIBE` / `duckdb_columns()`), so pasting it back into a cast needs the
  quotes added.

- **A non-UTF-8 text column now says so, once per query**
  ([#224](https://github.com/hugr-lab/mssql-extension/issues/224)). A
  `CHAR`/`VARCHAR`/`TEXT` column whose collation is not a UTF-8 one is handed
  back as code-page bytes in a DuckDB `VARCHAR`, which is UTF-8 by contract.
  That stays **documented rather than enforced** — validating costs the scan's
  hot path in order to make a hand-written `mssql_scan()` fail loudly, and it
  bills hardest the UTF-8 configuration #225 optimised for. Instead the check
  runs over COLMETADATA — a handful of columns, never a per-row cost — as soon
  as the stream has its column types *and* again when it is drained, and reaches
  `duckdb_logs` at `WARNING` naming the column, its LCID and its SortId. Both
  calls matter: warning only at the drain says nothing for any query that stops
  early, which is the `LIMIT` that a code-page column is most likely to be met
  by first. The predicate is the TDS fUTF8 flag, verified against a live SQL
  Server rather than read off MS-TDS. `mssql_warn_non_utf8_collation` (default
  `true`) turns it off: the trigger is the majority configuration, since
  `SQL_Latin1_General_CP1_CI_AS` is the installation default, so anyone who has
  read the warning once and decided their data is fine needs a way to stop
  hearing it.

### Fixed

- **Forward-port of the v0.2.5 fixes to the duckdb 2.0 line** (#314). The
  released v0.2.5 sits on the `duckdb-v1.5.5` maintenance branch; these are the
  same two fixes against duckdb `main`.
  - **An MSSQL catalog's default schema is `dbo`** (#129). `Catalog::GetDefaultSchema()`
    answered `main` from the base class, which no SQL Server database has, so
    anything resolving an unqualified name through the catalog default failed
    with `Schema 'main' not found` — ducklake hits it on ATTACH, before any
    table exists. On the 2.0 API the signature is `optional<Identifier>`, where
    `nullopt` means "this catalog has no default, do not try"; we return
    `Identifier("dbo")`.
  - **Two scans of one catalog inside a transaction** (#239). A transaction pins
    one connection and routes every read of that catalog to it, but a scan holds
    that connection from `InitGlobal` until its last row, and DuckDB does not
    promise to drain one source before starting the next. Such plans are now
    materialized into a buffer-managed collection as each scan starts, freeing
    the connection immediately. Autocommit is untouched.

- **`mssql_scan()` inside a transaction took the pinned connection and kept it**
  (#316). The #239 gate runs on the optimized plan and sees only
  `mssql_catalog_scan`. `mssql_scan()` is unreachable from there by construction:
  it executes its query during **binding** — that is how it reads the result's
  column types — so a second scan of the catalog failed inside its own Bind,
  before any `InitGlobal` and long before the gate. Reported shapes were
  `Connection closed unexpectedly` (catalog scan + `mssql_scan`) and
  `Cannot execute: connection not in Idle state` (two `mssql_scan`). It now
  drains at Bind whenever the session is not in autocommit — the trigger is "in
  a transaction", not "more than one scan", because Bind cannot see the rest of
  the plan. Autocommit still streams.

- **The materialized scan could not spill** (#318, thanks @oluies). It was built
  from `Allocator::Get(context)`, and that overload is duckdb's
  `IN_MEMORY_ALLOCATOR` one: unaccounted against the buffer manager and unable to
  spill, so a large materialized scan grew in process memory until it OOMed while
  the docs promised the opposite. Built from the `ClientContext` overload, which
  defaults to `BUFFER_MANAGER_ALLOCATOR`.

- **SQL Server INFO messages were collected and thrown away.**
  `SurfaceWarnings` walked them and did nothing, on the grounds that "DuckDB
  doesn't have a built-in warning API" — true when it was written, not true
  now. `mssql_exec()` was worse: its token loop read
  `case Info: // Ignore informational messages`, so running a procedure that
  `PRINT`s dropped every line. `PRINT` output, `RAISERROR` at severity 10 or
  below and procedure progress notices now reach `duckdb_logs` on both paths,
  at `INFO` (`SET logging_level = 'INFO'` to see them), carrying the message
  number and severity. They are logged **before** a failing batch raises — the
  notices of a batch that failed are the ones worth reading — including when a
  scan fails mid-stream.

- **The 5th collation byte was parsed and discarded** (`offset += 5; // we only
  store 4`), thanks [@oluies](https://github.com/oluies) —
  [#305](https://github.com/hugr-lab/mssql-extension/pull/305). It is the
  SortId, and for the `SQL_*` collations it is the *only* thing that names the
  code page: `SQL_Latin1_General_CP1_CI_AS` (CP1252) and
  `SQL_Latin1_General_CP1251_CI_AS` (CP1251) both report LCID `0x0409` and
  differ only as SortId 52 vs 106.

## [0.2.4] - 2026-08-17

### Fixed

- **Azure Synapse Serverless login** (#254; closes
  [discussion #88](https://github.com/hugr-lab/mssql-extension/discussions/88),
  [#164](https://github.com/hugr-lab/mssql-extension/issues/164)).
  `ParseLoginResponse` skipped DONE/DONEPROC/DONEINPROC as 8 bytes after
  the token byte — the pre-7.2 layout with a 4-byte row count. We negotiate
  TDS 7.4, where `DoneRowCount` is 8 bytes ([MS-TDS] 2.2.7.9), so every
  DONE token desynced the parse by 4 bytes. Harmless against regular SQL
  Server, which sends LOGINACK first and DONE last; fatal against Synapse
  Serverless, which runs internal procs during login and fronts the
  LOGINACK with a run of DONEINPROC tokens — the parser never reached the
  LOGINACK, and every auth path failed with `No LOGINACK token in
  response`. `ParseDoneForAttentionAck` had the same 8-byte skip; fixed
  alike. The unit-test DONE helper itself emitted the pre-7.2 layout —
  which is why the fixtures never caught this — and now emits 7.2+; two
  regressions replay the Synapse token pattern, and #255 adds a fuzz corpus
  over login-response token streams plus tests for the quiet casualties of
  the same skip. Confirmed against a reporter's `MSSQL_DEBUG=2` hex dump of
  the exact stream.

- **Login errors are translated from the server's error number, not our own
  wrapper text** (#262, via #263). Every login-time server error used to
  surface as "check username and password", because the translator
  pattern-matched the wrapper message it had itself produced.
  Classification now reads the server's error number: 40613/40197/40501/
  49918 are named as retryable (Azure serverless resume, reconfiguration,
  throttling), 18456 keeps and explains its State byte, and the server's
  own message text is always appended.

- **Kerberos SPN derivation from an IP literal** (#259, via #263). An SPN
  built from an IP can never match an Active Directory registration. IP
  literals are now reverse-resolved to the host's name (definitive answers
  only, memoized); hostnames pass through unresolved. The derivation is
  shared with the `mssql_kerberos_auth_test` diagnostics, so they cannot
  report an SPN the connection path would not request.

- **Windows build**: `winsock2.h` is included before anything that reaches
  `windows.h` in the auth strategy factory (d5408e5).

- **Login-time routing is now followed on every authentication path**
  (spec 068, #258). A server can answer LOGIN7 with a ROUTING ENVCHANGE (type 20)
  meaning "log in over there instead" — Azure SQL Managed Instance, Azure SQL
  under the **Redirect** connection policy (the default for clients connecting
  from inside Azure), Fabric/Synapse gateways, and on-prem AlwaysOn read-only
  routing all do it. Only the Azure AD path honoured it; SQL authentication and
  integrated (Kerberos/SSPI) authentication did not.
  - **SQL auth**: a routed login used to fail as `Authentication failed` when
    the redirect arrived without a LOGINACK, and — worse, because it was silent
    — appear to *succeed* when a LOGINACK came with it, leaving the session
    bound to a gateway that had just told the client to leave. Both now follow
    the hop.
  - **Azure AD**: a redirect with no LOGINACK (legal per [MS-TDS]) died as
    `Azure AD authentication failed` because the success check ran before the
    routing fields were read. Behaviour against gateways that send LOGINACK is
    unchanged.
  - **Integrated auth**: hops now acquire a service ticket for the **routed**
    host's SPN instead of replaying the gateway's, which could not validate. An
    explicit `service_principal_name=` is still used verbatim on every hop. See
    `Kerberos.md`.
  - The hop budget (5) and the routed-target parsing (`host\instance:port`,
    no SQL Browser lookup on a hop) are unchanged; exceeding the budget now
    names the last routed target in the error.

  **Behaviour change**: SQL authentication against an endpoint that answers with
  ROUTING *and* a LOGINACK now redirects instead of transacting against the
  gateway. That is the correct behaviour per [MS-TDS] — a routed session is not
  usable — but it is a visible change for anyone who was unknowingly relying on
  the old one.

  **Internal API change**: `TdsConnection::AuthenticateIntegrated` takes an
  `AuthenticatorFactory` (`(host, port) -> IAuthenticator`) instead of a
  pre-built authenticator. Affects embedders calling the TDS layer directly.

### Added

- **`azure_tenant_id` on ATTACH and MSSQL secrets now works** (#264). The
  parameter has been registered on the MSSQL secret since spec 032 and was
  read by nothing: `credential_chain`/`interactive` authentication always
  fell back to the `/common/` endpoint, with no way to pick a tenant in a
  single-tenant org (`TENANT_ID` on the *azure* secret is not an
  alternative — duckdb-azure rejects it on `provider='credential_chain'`).
  The value now flows into token acquisition (ATTACH option
  `azure_tenant_id` / `azure_tenant` overrides the secret's value) and into
  the token-cache key, so two tenants never share a token.
- **A runnable Azure test lane** (#264, #265). `require azure` was
  unsatisfiable in every environment the suite ran in, so the whole
  `[azure]` group silently skipped. `make azure-test` now self-provisions
  the azure extension into a local extension repository and runs the group
  against live Azure SQL (serverless warm-up, assertion floor); CI gains a
  manually dispatched job (`run_azure_tests`) that mints the
  service-principal token in-job and unsets absent secrets, so files skip
  instead of failing on empty values. The two tests the skip had hidden —
  never-executed WIP syntax — are fixed.
- **Windows SSPI CI coverage** (#260): a context/token test runs on every
  PR in an ungated job, needing no domain and no SQL Server. **Kerberos
  routing-hop e2e** (#261): `test/kerberos` gained a routing gateway, so CI
  shows a real MIT KDC issuing a second service ticket for the routed host.
- Multi-column read coverage the one-column spec-058 cases could not reach
  (#251).

### CI / Build

- The DuckDB-compat matrix could pass without comparing and broke its own
  `--json` output (#250); the LOB detector is excluded from the TruffleHog
  secret scan (#252); the Security Scan can be dispatched manually (#253).
- Linux CI fetches the simdutf amalgamation directly instead of a distro
  package; the unit-test link is wrapped in `--start-group` so GNU ld's
  single-pass archive scan cannot drop the extension loader (via #264).

> The three sections below were backfilled on 2026-08-17: v0.2.1–v0.2.3
> shipped without CHANGELOG sections. The spec 052 and credential-wipe
> entries under 0.2.1 were written at the time but sat under [Unreleased].

## [0.2.3] - 2026-08-07

The performance release. Full v0.2.2 → v0.2.3 report:
`test/bench/bench_results_v023_report.md` (44 columns × 38M rows,
interleaved same-session A/B).

### Changed

- **Write path rebuilt** — columnar encode, parallel writers, TABLOCK and
  flush policy sized to columnstore rowgroups (specs 057/063/064; #234,
  #240, #241). Like-for-like 1.34×, out of the box ≈6× (parallel writers),
  sized strings ≈9.7×.
- **Read path staged** — column-major staging + batch decode (spec 055,
  #213: read −14…−47%), framing (spec 058, #244), materialization quick
  wins (spec 054, #209), uniform column chunks published as CONSTANT
  vectors (spec 056, #221). Reads 1.22–1.40× wall vs 0.2.2.
- **UTF8SUPPORT advertised in LOGIN7** (#225, #227) — UTF-8-collated
  columns arrive as UTF-8 instead of being transcoded to UTF-16.
- Default TDS packet size 4096 → 16384 (`mssql_tds_packet_size`, spec 055).
- **Target column types and table shape for CTAS/COPY** (spec 060, #230):
  `MSSQL_VARCHAR(n)` / `MSSQL_NVARCHAR(n)` annotations, `table_kind`
  (heap/columnstore), UTF-8 collation for created VARCHAR columns.

### Fixed

- Partitioned tables were unreadable, and their row estimate came from one
  arbitrary partition (spec 049, #85, #223).
- DATETIME day count widened before epoch conversion (#222).
- Catalog-scan bind data is serialized, so common-subplan optimization
  keeps subplans distinct (#211).
- 0.2.2 segfaulted on dictionary vectors from any real columnar source —
  found while sizing this release's report, pinned by
  `test/sql/copy/vector_encodings_bcp.test` (#245).

### Added

- Versioned documentation site (Docusaurus, #243).
- Staged-read-path test pack — and the four defects it found (spec 059,
  #220).
- Local SQL Server version × charset compat matrix (#232).

### CI / Build

- POSIX vcpkg clones pinned to vcpkg.json's builtin-baseline (#249).

## [0.2.2] - 2026-07-28

### Changed

- DuckDB bumped to v1.5.5 (#207); OpenSSL 3.4.1 → 3.5.4 LTS.

### Fixed

- The named-instance resolver (spec 045) is actually wired into ATTACH —
  `Server=host\instance` resolves through the SQL Server Browser (#205,
  #206).
- Reentrant `gmtime_r`/`gmtime_s` in Azure token timestamp formatting
  (#194).
- COPY connection leak (#191, #193).

### CI / Build

- The SQLLogicTest suite actually runs in CI (#192); vcpkg binary caching
  repaired after GitHub removed `x-gha` (#198); CodeQL filters vendored
  dependencies (#195).

## [0.2.1] - 2026-06-07

### Changed

- DuckDB submodule pinned to v1.5.3 (#148); spec 051 API-compat shim
  (`mssql_compat.hpp`) so one source tree compiles against both the pinned
  SHA and rolling DuckDB main (#124).
- `DATAMODEL.md` — layered architecture reference with diagrams (#130).

### Fixed

- **Integrated-auth LOGIN7 is fragmented across TDS packets** (#138) — an
  AD-sized Kerberos PAC overflowed a single packet and broke login. Adds
  the test-only `mssql_login7_max_packet` setting so the multi-packet path
  is exercisable without a real AD.
- `mssql_query_timeout` is honored in `mssql_exec()`; long-running
  statements are no longer dropped (#90, #145).
- Clean error for DELETE on tables without a primary key (#141).
- **dbt segfault with `threads >= 2`** (spec 052, closes
  [#126](https://github.com/hugr-lab/mssql-extension/issues/126)).
  Catalog entries (`MSSQLTableEntry`, `MSSQLSchemaEntry`) switched from
  `unique_ptr` to `shared_ptr` ownership; concurrent first-load of the
  same table is coordinated via per-table singleflight so only one
  thread issues the SQL Server round trip (waiters re-check the cache).
  Lifetime extension across `Invalidate()` / `OnDetach` is done via
  per-`ClientContext` bind-time anchors registered as
  `ClientContextState`: every `MSSQLSchemaEntry::LookupEntry` /
  `MSSQLCatalog::LookupSchema` / catalog `Scan` callback path stashes
  the `shared_ptr` into the per-context `MSSQLBindAnchors` holder, which
  DuckDB releases via the `QueryEnd` hook. In-flight binders therefore
  hold every entry they touched alive for the full bind+execute span;
  release happens naturally at end of query, with no catalog-lifetime
  accumulation. Audit of `MSSQLMetadataCache::GetTableMetadata` confirms
  its only caller (`MSSQLTableSet::LoadSingleEntry`) copies fields
  immediately; contract pinned in the header.
  `MSSQLStatisticsProvider` returns by value, no pointer-handout surface.
- **`MSSQLTableEntry::EnsurePKLoaded` double-free under thread stress**
  (spec 052). Two threads both saw the load flag false, both fetched, and
  both move-assigned to `pk_info_` — the second move freed the loser's
  previous `vector<PKColumnInfo>` while the first thread still held it.
  Caught by AddressSanitizer during the spec 052 invalidation-race soak.
  Fixed with a `pk_load_mutex_` + `std::atomic<bool> pk_loaded_`
  acquire/release publication so the lock-free fast path stays correct
  under the C++ memory model.
- **`MSSQLCatalog::RefreshCache` connection leak on TDS hiccup** (spec
  052). Under thread stress (~318 invalidations / 30 s), SQL Server
  occasionally returns a transient TDS error mid-Refresh. The exception
  propagated past `connection_pool_->Release`, leaving one connection
  checked out and tripping `~ConnectionPool`'s quiescence-contract
  assert at catalog teardown. Wrapped the `Refresh` call in try/release.
- **`MSSQLTableEntry::EnsurePKLoaded` / `GetStorageInfo` connection
  leak on TDS hiccup** (spec 052). Both functions intentionally swallow
  exceptions to fall back to `pk_info_.exists = false` / cached
  `approx_row_count_`, but the outer `catch (...)` never released the
  pool-owned connection acquired inside the try. Every SELECT bind
  calls `EnsurePKLoaded`, so under scenario 5/8 stress a single TDS
  hiccup inside `PrimaryKeyInfo::Discover` stranded one connection in
  `active_connections_` for the lifetime of the pool. Nested an inner
  try/release/throw around the SQL Server I/O so the connection is
  returned BEFORE the outer fallback catch runs.
- **`ConnectionPool::Shutdown` cleanup-thread strand on quiescence
  violation** (spec 052). `D_ASSERT` in DuckDB-debug builds throws
  `InternalException` rather than calling `abort()`. The throw fired
  while `cleanup_thread_` was still joinable; `~ConnectionPool noexcept`
  caught the exception, but `~std::thread` on the joinable thread then
  called `std::terminate()`. Reordered: signal + join the cleanup thread
  and close pooled connections FIRST, then emit the warning and assert
  at the end. The warning is the operator-visibility signal; the
  trailing assert preserves the debug invariant without stranding
  resources on its unwind path.

### Added

- `test/cpp/test_concurrent_reads.cpp` scenarios 5-8 (spec 052
  US2/US3 + concurrent-write acceptance): 30 s soak runs that exercise
  every spec 052 lifetime guarantee under AddressSanitizer/UBSan.
  Scenario 5 — 4 readers + invalidator at 50 ms cadence
  (~2500 reads × ~300 invalidations). Scenario 6 — scenario 5 plus a
  `duckdb_schemas()` / `duckdb_tables()` walker exercising the bulk-scan
  anchor path (~1200 reads × ~260 invalidations × ~200 schema walks).
  Scenario 7 — 4 writers + 1 reader on one shared table with disjoint PK
  ranges (~550 INSERT/UPDATE/DELETE cycles + ~500 reads). Scenario 8 —
  4 pure-write threads (~2700 INSERTs / 30 s).
- `.github/workflows/concurrency-tests.yml` job that rebuilds the
  extension with ASan/UBSan and runs `test-concurrent-reads` on every
  PR that touches the catalog or singleflight surfaces. Includes an
  8 GB swap on `/mnt` to absorb the link-time RAM peak, a
  `TestDB`-creation step for scenarios 4-8, and `LD_PRELOAD`-ed
  `libasan`/`libubsan` on the test binary run only (so the preload
  doesn't leak into vcpkg's compiler probe).
### Security

- **Wipe bearer credentials on destruction.** `MSSQLConnectionInfo` gains a
  user-declared destructor that `OPENSSL_cleanse`s `password` and
  `access_token`; `~MSSQLCatalog` wipes the cached `fedauth_token_utf16le_`
  byte vector. `OPENSSL_cleanse` defeats dead-store elimination, so secrets
  do not linger in heap-recycled memory after the owning
  `shared_ptr<MSSQLConnectionInfo>` or `MSSQLCatalog` is destroyed.
  Rule-of-five compliance: copy/move ctors and assigns on
  `MSSQLConnectionInfo` are explicitly `= default`-ed so that user-declaring
  the destructor doesn't silently disable move generation.

## [0.2.0] - 2026-05-20

Major release: integrated authentication (Kerberos + Windows SSPI),
process-wide singleton cleanup, security hardening, and a deep codec
refactor. Closes [#82](https://github.com/hugr-lab/mssql-extension/issues/82)
(custom Application Name) and [#96](https://github.com/hugr-lab/mssql-extension/issues/96)
(ATTACH/DETACH-in-Python-loop crash class).

### Added

- **Custom Application Name in connection string** (spec 047 FR-014,
  closes [#82](https://github.com/hugr-lab/mssql-extension/issues/82)).
  ADO.NET keys `Application Name` / `ApplicationName` / `App Name` /
  `application_name` (case-insensitive), URI query parameter
  `applicationname`, and MSSQL secret fields `application_name`
  (canonical) / `applicationname` (fallback) propagate to LOGIN7
  `program_name` — visible as `APP_NAME()` /
  `sys.dm_exec_sessions.program_name`. Empty falls back to the
  extension default (`"DuckDB MSSQL Extension"`); values exceeding
  128 UTF-16 code units are clamped client-side so what the user sees
  in `APP_NAME()` equals what we sent.
- **`lazy_validation` ATTACH option** + **`mssql_attach_validation_timeout`
  setting** (spec 047 FR-011). Eager ATTACH validation is on by default
  (wrong password / unreachable host fail ATTACH instead of being
  deferred to the first query); opt out per ATTACH with
  `lazy_validation true` to preserve the pre-047 lazy behaviour
  (useful for container / orchestrator startup where the SQL Server
  may not yet be reachable). The setting bounds the eager round-trip;
  `0` (default) inherits `mssql_connection_timeout`.
- **`mssql_close_all()`** scalar function (spec 047 FR-013). Closes every
  diagnostic-API connection opened via `mssql_open()` in one call; returns
  the count of handles closed. Idempotent — a second call after a full
  close returns 0. Recommended shutdown hook for hosts that use the
  diagnostic API but do not track individual handles. Marked `[DEPRECATED]`
  from registration: lives in the same group as `mssql_open` / `mssql_close`
  / `mssql_ping` and will be removed alongside them in a future major
  release once the catalog-bound API covers all diagnostic needs (FR-010).

### Changed

- **LOGIN7 default `program_name` unified to `"DuckDB MSSQL Extension"`**
  (spec 047 FR-014 side-effect). Pre-047 SQL auth sent `"DuckDB"` and
  integrated auth sent `"DuckDB MSSQL Extension"`. The single resolution
  point (`ResolveAppName` helper, called by every auth path) now sends
  `"DuckDB MSSQL Extension"` uniformly when no `application_name` is
  supplied. Observable change for SQL-auth users who previously saw
  `APP_NAME() = 'DuckDB'`; they will now see `'DuckDB MSSQL Extension'`
  unless they explicitly set `Application Name=DuckDB`.
- **`mssql_open` / `mssql_close` / `mssql_ping` are now documented as
  `[DEPRECATED]`** (spec 047 FR-010). They remain functional and are kept
  for backward compatibility. Prefer ATTACH + the catalog-bound functions
  (`mssql_scan`, `mssql_exec`, `mssql_pool_stats`) which integrate with
  DuckDB's catalog lifecycle and the per-catalog pool ownership introduced
  in spec 047. The handle manager singleton these three functions share is
  the last extension-internal process-wide state and will be removed
  together with the functions themselves.

### Security

- **Azure TokenCache cross-instance aliasing** fixed (spec 047 FR-012).
  Pre-047, two DuckDB instances in the same process that each defined a
  secret with the same name (e.g. `mssql_secret`) shared a single
  TokenCache row keyed by `secret_name` alone — instance B could silently
  authenticate with instance A's already-acquired token even when the two
  secrets resolved to different Azure principals. The cache key is now
  namespaced by `(DatabaseInstance address, cache_key)`; tokens from
  different instances are independent. The `OnDetach` invalidation path
  is scoped to the calling instance's namespace so a sibling instance
  sharing the secret name keeps its token.

- **ATTACH credentials are now validated eagerly** by default (spec 047
  FR-011). Wrong passwords / unreachable hosts surface as ATTACH errors
  instead of being deferred to the first query. Error messages never
  contain the password (audited via sentinel substring assertion in
  `test/sql/attach/attach_validates_credentials.test`). Opt out with
  `lazy_validation true` for container/orchestration scenarios; ceiling
  controlled by the new `mssql_attach_validation_timeout` setting.

### Internal

- **Process-wide singletons removed** (spec 047, closes [issue #96](https://github.com/hugr-lab/mssql-extension/issues/96)):
  `MssqlPoolManager`, `MSSQLContextManager`, and `MSSQLResultStreamRegistry`
  are gone. Connection pools are now owned per-`MSSQLCatalog` via
  `unique_ptr`; result streams live on the catalog as
  `RegisterStream` / `RetrieveStream` methods. Two attached MSSQL
  databases under different ATTACH aliases no longer alias to the same
  pool, and ATTACH/DETACH cycles in a Python-style loop (the issue #96
  repro) succeed indefinitely.

- **Windows SSPI** integrated authentication (spec 042 Phase 4). `WinSspiAuthenticator`
  via `secur32.dll`'s Negotiate package. Uses the current Windows logon session — no
  `kinit` needed. Same connection-string surface as POSIX (`Trusted_Connection=yes` /
  `authenticator=winsspi`). Mirrors the structure of `Krb5Authenticator`; shares the
  `IAuthenticator` interface and the SPNEGO continuation loop in
  `TdsConnection::AuthenticateIntegrated`. Linked against `secur32.lib` from the
  Windows SDK — no third-party dependency.

- **Integrated Authentication (Kerberos)** for POSIX hosts (spec 042, phases 1-3).
  Adds the `IAuthenticator` multi-round interface, parser support for the
  `microsoft/go-mssqldb` connection-string surface, LOGIN7 `fIntSecurity`
  wiring, `0xED` SSPI continuation tokens, and a POSIX Kerberos backend via
  system GSSAPI. Self-contained `test/kerberos/` docker-compose stack (KDC +
  SQL Server + test-client) — no real Active Directory required.
  - New connection-string keys (verbatim from `go-mssqldb`): `authenticator`,
    `krb5-configfile`, `krb5-keytabfile`, `krb5-credcachefile`, `krb5-realm`,
    `service_principal_name`.
  - Aliases: `Trusted_Connection=yes`, `Integrated Security=SSPI/true` —
    resolve to `krb5` on POSIX, `winsspi` on Windows.
  - Three credential modes on POSIX (Linux only for keytab + raw):
    credential cache (default, uses `kinit` ticket), keytab, raw credentials
    (secret-only).
  - macOS supports credential-cache mode (uses `GSS.framework`); keytab and
    raw modes are rejected at construction time with a clear error pointing
    at the Linux container path.
  - Verbatim GSSAPI status text in errors plus actionable hints (no
    ccache → run kinit; clock skew → ntp/chrony; SPN not registered →
    setspn -L; etc.) per spec 042 R8.
  - New end-user documentation: `Kerberos.md` (mirrors `AZURE.md`).
  - Windows SSPI (`winsspi` authenticator) is Phase 4 — pending. WSL2 Ubuntu
    is the supported testing path on Windows in the meantime.

### Fixed

- Linux build with Kerberos enabled now links `libkrb5` explicitly. Previous
  builds failed at the link step with `undefined reference to symbol
  'krb5_free_error_message'` on distros where `krb5-gssapi.pc` doesn't
  transitively pull in `libkrb5` (Ubuntu 24.04 is the documented case).
  Affects spec 042 raw-credentials mode users on Linux only — macOS uses
  `GSS.framework` which bundles all symbols. Configure-time warnings now
  cite both Debian (`libkrb5-dev`) and RHEL (`krb5-devel`) package names.

### Security

- Hardened FEDAUTH JWT debug logging: `tds_connection.cpp` previously
  hex-dumped the first 20 bytes of the access token at debug level 2.
  Replaced with size-only logging plus `(contents redacted)`.
- Raw-credentials Kerberos mode is SECRET-ONLY by design — cleartext
  `Password` is rejected in any connection string when integrated auth is
  selected. Defends against cleartext passwords in connection-string logs.
- Per-connection krb5 overrides (`krb5-configfile`, `krb5-credcachefile`)
  apply through `gss_acquire_cred_from` `cred_store` elements per instance,
  not via process-global `setenv()`. Thread-safe vs concurrent `getenv` on
  pool worker threads.
- Raw-mode `MEMORY:` ccache is destroyed after `gss_acquire_cred_from`
  copies credentials internally, so cleartext credentials don't linger in
  MIT's process-global ccache registry.

### Changed

- README's stale "Windows Authentication: Only SQL Server authentication is
  supported" limitation removed. Windows SSPI is now scoped as "Phase 4
  pending" with WSL2 documented as the interim testing path.
- README's Secret Fields and Key Aliases tables expanded with Kerberos rows.
- `docs/architecture.md` Authentication Strategy Pattern section updated to
  document the new `IAuthenticator` layered interface and the
  `IntegratedAuthStrategy` adapter.
- `docs/TESTING.md` gained a Kerberos Tests section covering the docker-compose
  stack and WSL2 testing.

### Internal

- New `src/include/tds/auth/iauthenticator.hpp` — three-method multi-round
  interface (`InitialBytes` / `NextBytes` / `Free`), modeled on
  `microsoft/go-mssqldb`'s `integratedauth.IntegratedAuthenticator`. No
  DuckDB headers — the TDS auth layer is reusable outside DuckDB.
- New `src/tds/auth/krb5_authenticator.{hpp,cpp}` — POSIX GSSAPI
  implementation. SPNEGO mechanism. Inline GSS OID literals to work around
  macOS GSS.framework not exporting the well-known OID symbols.
- New `src/include/tds/auth/integrated_auth_strategy.hpp` — adapter wrapping
  `IAuthenticator` in the existing `AuthenticationStrategy` interface.
- `src/tds/tds_protocol.cpp` gains `BuildLogin7WithSSPI` (sets
  `OptionFlags2.fIntSecurity`, writes SPNEGO blob into LOGIN7's SSPI field;
  `cbSSPILong` fallback for blobs > 65 535 bytes) and `BuildSSPIMessage`
  (continuation packet, type `0x11`).
- `src/tds/tds_token_parser.cpp` recognizes `TokenType::SSPI = 0xED`.
- `src/tds/tds_connection.cpp` gains `AuthenticateIntegrated()` — drives the
  full SPNEGO continuation loop on `0xED` tokens, with an 8-round cap to
  detect cross-realm misconfiguration.
- `src/connection/mssql_pool_manager.cpp` gains
  `GetOrCreatePoolWithIntegratedAuth` — each pool connection builds a fresh
  `Krb5Authenticator` so kinit-refreshed tickets are picked up on the next
  fill. Logs verbatim GSSAPI errors to stderr on pool refill failures.
- `CMakeLists.txt` adds `ENABLE_KRB5` option (default ON on POSIX),
  pkg-config GSSAPI discovery on Linux, `find_library(GSS_FRAMEWORK GSS)` on
  macOS, `secur32` linkage hook for Windows (Phase 4).

- **Type codec consolidation** (spec 045). Per-type encoding/decoding/literal/DDL
  logic consolidated into 9 family modules under `src/codec/`:
  boolean/integer/float/decimal/money/string/binary/datetime/uuid. Each
  `<family>_codec.cpp` owns `EncodeToBcp` / `DecodeFromTds` /
  `FormatSqlLiteral` / `FormatDdlTypeName` for its types. Dispatch via
  `FamilyFromLogicalType` switch in `literal_format.cpp` + `type_family.cpp`.
  5 LogicalType-side dispatch sites collapsed; net −762 LOC across dispatch
  sites (3243→2481, −23.5%). Bonus: TIMESTAMP_MS/NS/S/TZ now round-trip
  losslessly through SQL Server DATETIME2(3/7/0/7) with full
  type-transparency. Closes [issue #91](https://github.com/hugr-lab/mssql-extension/issues/91)
  (BCP nvarchar character-vs-byte length) and [#89](https://github.com/hugr-lab/mssql-extension/issues/89)
  (VIEW catalog-vs-runtime type divergence). No new vcpkg deps. Per-row bench
  (1M rows): within 5% gate vs spec-044 baseline.

- **Named instance resolver** (spec 045 phases 0-2). SQL Server Browser
  (UDP 1434) discovery for named instances. Mock-browser test stack under
  `test/named-instance/`.

- **UTF-16 codec consolidation** (spec 044). Finishes the simdutf migration
  started in spec 043 — every legacy `Utf16LE*` call site moves to the
  simdutf-backed wrapper. simdutf becomes the production UTF-16 codec; the
  legacy hand-rolled converter survives only as a private invalid-input
  fallback. Includes microbenchmark (`make bench-utf16`) and an end-to-end
  before/after benchmark (`test/bench/bench_codec_e2e.sh`).

- **LOGIN7 non-ASCII fix + simdutf foundation** (spec 043). Non-ASCII bytes
  in LOGIN7 username/password/database fields no longer get corrupted by the
  hand-rolled UTF-16 converter. Adds simdutf as a vcpkg dependency
  (statically linked, MIT). Foundation for spec 044's full migration.

### CI / Build

- **Tier-1 lint and security checks** added: CodeQL (C++), gitleaks,
  shellcheck, hadolint, yamllint, codespell. Dependabot updates enabled.
  PR prompt-injection scanner for review descriptions.
- **CodeQL speedup** (3-part): target restriction + vcpkg cache + submodule
  trim. Cuts CodeQL job runtime substantially on PR triggers.
- **Kerberos integration job** in CI: spins up the
  `test/kerberos/docker-compose.yml` KDC + SQL Server + test-client stack
  for every PR touching the integrated-auth path.
- **Drive-by fix**: 9 codec headers (spec 045) had `class ColumnMetadata` /
  `class BCPColumnMetadata` forward declarations while the real
  definitions are `struct`. MSVC mangles `class` and `struct`
  differently (clang/gcc don't), producing 16 unresolved-external LNK2019
  errors at link time. Latent regression — last successful MSVC build on
  main was 2026-05-15, BEFORE spec 045 merged. Fixed all 9 forward
  declarations.

## [0.1.18] - 2026-02-24

### Added

- XML data type support (spec 041). XML columns read as VARCHAR; BCP write
  path; clear errors for INSERT-with-RETURNING / UPDATE on XML columns.

### Fixed

- UDT type alias crash in catalog metadata queries (issue #81).

## Earlier history

Earlier versions are tracked in git history under `specs/NNN-*/` directories.
Notable recent specs:

- **041-xml-type-support** — XML column read/write (TDS type 0xF1).
- **040-fix-datetimeoffset-nbc** — DATETIMEOFFSET in NBC row reader.
- **039-order-pushdown** — ORDER BY pushdown to SQL Server (experimental).
- **037-replace-libcurl-httplib** — Replaced libcurl with bundled cpp-httplib
  for Azure OAuth2.
- **036-azure-token-docs** — Azure AD documentation expansion.
- **034-duckdb-v15-upgrade** — DuckDB v1.5 upgrade.
- **033-fix-catalog-scan** — Catalog metadata cache fix.
- **032-fedauth-token-provider** — Manual access token support for Azure AD.
- **031-connection-fedauth-refactor** — Auth strategy pattern introduction.
- **027-ctas-bcp-integration** — CTAS via BCP protocol.
- **024-mssql-copy-bcp** — COPY TO via BCP.
- **020-multi-statement-scan** — Multi-statement support in `mssql_scan`.
- **001-azure-token-infrastructure** — Initial Azure AD support.

See `specs/` for the full feature design history.

[Unreleased]: https://github.com/hugr-lab/mssql-extension/compare/v0.2.4...HEAD
[0.2.4]: https://github.com/hugr-lab/mssql-extension/compare/v0.2.3...v0.2.4
[0.2.3]: https://github.com/hugr-lab/mssql-extension/compare/v0.2.2...v0.2.3
[0.2.2]: https://github.com/hugr-lab/mssql-extension/compare/v0.2.1...v0.2.2
[0.2.1]: https://github.com/hugr-lab/mssql-extension/compare/v0.2.0...v0.2.1
[0.2.0]: https://github.com/hugr-lab/mssql-extension/compare/v0.1.18...v0.2.0
[0.1.18]: https://github.com/hugr-lab/mssql-extension/releases/tag/v0.1.18
