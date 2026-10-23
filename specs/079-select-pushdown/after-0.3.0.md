# Spec 079 — remote pushdown: what is left for after v0.3.0

The things spec 079 records as divergences, vetoes or later steps, gathered in
one place to plan from once v0.3.0 is out. Each names where it was found and
why it was not done in the PR that found it. Not a commitment, and not in any
order of priority yet.

**Checked against main `6df11bc` on 2026-10-23.** Every item below was
re-read against the code; what had shipped is removed, what was wrong is
corrected (the TPC-H section after #508). v0.3.0 is out, so "not before
v0.3.0" no longer holds anywhere here. The umbrella issue #515
tracks this file together with the type work (sql_variant, geometry, vector).

## Correctness edges (recorded, not vetoed)

- **Division in a set operation's child that DuckDB sends on its own.** A set
  operation that also reads a local table or another catalog has each remote
  child finished by DuckDB's rewriter, and `RemoteExecute` cannot tell that
  child from a statement. A division there is NULL at a zero divisor where
  DuckDB says inf, before DuckDB deduplicates or filters it: `SELECT n / 0 ...
  UNION SELECT NULL::DOUBLE FROM loc` gives {NULL} pushed, {inf, NULL}
  locally. Vetoing it would take division from every statement's top (E1
  full review). The way forward is the next item. Since E1 a child that
  sends no more than the scan would (`PushesMoreThanScan`) goes back to the
  scan path, so it takes an ORDER BY, a LIMIT or an aggregate in the child;
  a floating-point aggregate's last bits have the same exposure.
- **A divisor that cannot be zero.** Division is rendered as
  `CAST(a AS float) / NULLIF(CAST(b AS float), 0)`, and because of the zero
  divisor alone it is refused or flagged in many places: in a condition,
  under an aggregate, under COALESCE / NULLIF, in DISTINCT, in a set
  operation that compares rows, in ORDER BY and in a DML SET. A divisor the
  writer can prove non-zero -- a non-zero constant, `CASE WHEN b = 0 THEN ...
  ELSE a / b END`, a `WHERE b <> 0` on the same node, a column with a CHECK
  constraint (the catalog loads no CHECK constraints today) -- needs neither
  the NULLIF nor the veto, and could be compared, ordered and kept in a set
  operation's child (owner's suggestion, E1). A non-zero constant first.
- **A CTE body inlined twice is read twice.** DuckDB evaluates a CTE once; the
  server reads an inlined body at each reference, which under concurrent
  writes can see different committed rows. A body that picks rows (a LIMIT)
  is already vetoed when inlined twice; the rest is recorded. Options: a
  snapshot / repeatable-read isolation for the statement, or materialising
  the body once (`#temp`) when it is referenced more than once.
- **Function and type names the rewriter strips.** DuckDB's rewriter strips
  the catalog from function, window and type names too, and only table names
  are restored on a handed-back node: `SELECT rc_on.main.lower(name) FROM
  (...) s` binds the system `lower` where pushdown off gives a catalog error.
- **The duplicate-name guard is untested.** A handed-back node would dedupe
  `n, n` into `n, n_1`; `MayRepeatOutputNames` and the `rowid` / alias rule
  keep such statements out of the hand-back, but sqllogictest cannot see
  result names (DESCRIBE is not rewritten, CTAS dedupes on both paths). A C++
  test through the client API would pin it.

## Gain decision

- **A plan-based estimate.** The join gain check compares the largest input
  table with `mssql_pushdown_join_rows_threshold`, from cached counts: it
  bounds the inputs, not the output -- two 900k-row tables joined
  many-to-many pass at the default and the server may produce ~8×10^11 rows
  where the scans would ship 1.8M (raised on #399). For large statements
  the answer is the server's own estimate: a SHOWPLAN on a separate
  connection (or an `mssql_estimate` function) read before deciding (owner's
  plan, E1). Owner's call on #399: not before v0.3.0 unless the rest is done
  first -- no interim heuristic (v0.3.0 is out). A cheaper step on the way, if wanted: a
  product bound (largest x second-largest input against an output-row
  threshold of its own), from the same cached counts, no round trip.
- **Keys through a wrapper.** A derived table's key is its GROUP BY / DISTINCT
  columns only: not even a single keyed table's key carries up through a
  plain projection, let alone a join's. Two effects, not all of them
  conservative: a join onto the wrapper's would-be key counts as unbounded,
  and the wrapper's own uncertain join counts as reduced only by a LIMIT or
  an aggregate without GROUP BY; but in the outer node a keyless relation
  makes a GROUP BY count as reducing (`CoversEveryKey` is false), which is
  optimistic. Carrying unique keys up (a keyed table's projection,
  key-preserving joins) fixes both.
- **Derived table size.** A derived table is sized by its largest input, not
  by what it returns (an aggregate's group count).

## Before spec 080: a vehicle that trusts the shape it is given

- **The shape passed in, not asked for: `mssql_scan_unsafe` /
  `mssql_scan_params_unsafe`, and the writer's shape for pushed statements.**
  Since spec 075 a scan describes at bind (`sp_describe_first_result_set`) and
  runs at init. Running at bind is not the way out: DuckLake runs its reads in
  transactions, and several scans of one statement bound there contend for the
  one pinned connection (the reason 075 moved execution to init). Instead the
  bind trusts a shape given to it: it returns those names and types without
  asking the server (no round trip, no connection, so inside a transaction and
  on a pool of one too), and execution, at init as always, returns the rows in
  that shape. A stream of another shape is a loud error at init, never a silent
  conversion (lossless widenings, int into bigint, could be allowed). Hence
  "unsafe": the caller vouches for the shape and the check waits for execution.
  Two users: the remote-pushdown writer, which knows the shape of table columns
  and aggregates and passes it (a statement with a computed column whose type
  is the server's keeps the describe cache, else the describe); and users'
  own T-SQL whose shape they know (mssql-ducklake's own statements pay a
  describe today). The describe cost measured in E2: 1.5-3 ms a statement
  locally, a network round trip remotely; the E2 cache covers repeated shapes,
  this covers the first one too. Owner's call (E2): its own small spec after
  #406, before spec 080, which builds its DML pushdown on the same vehicle.
  **Done in spec 081** (`specs/081-shape-vehicle/`), for statements whose
  every column the writer types. Left: a cast-back column (the integer `sum`
  as decimal(38,0) is exact; decimal arithmetic over cast-back operands is
  widened by the server in ways the writer does not model) and lossless
  widenings at the check (`ColumnTypeFits` takes equal types only, so a
  smallint sent for a declared int fails loudly; allowing it needs a
  widening decode at read, not only a looser check). Also left from 081: the
  rewriter's dry run costs 0.63 s of CPU against 0.016 s on 1000 `LIMIT 0`
  branches (spec 081, "Not done").

## Shapes not pushed yet

- **Decomposition of a window over `*`, and of a join with a local table.** A
  window over named columns decomposes; over `*` it does not (the `*` over a
  join guard). A FROM subquery or a CTE body in a statement that also reads a
  local table is not finished by DuckDB's rewriter on its own --
  duckdb/duckdb#26280 (still open); nothing to change here when DuckDB does.
  The guard (`MayRepeatOutputNames`) is wider than "a `*` over a join": a
  `*` beside any other select item refuses too. Expanding `*` to the names
  DuckDB binds (`n`, `n_1`) would let such a node be handed back.
- **A query over a user's own `mssql_scan` / `mssql_scan_params` (owner,
  2026-10-21).** Today nothing over a raw scan goes to the server.
  `mssql_scan` has neither `filter_pushdown` nor `projection_pushdown` (only
  the catalog scan has them), and the remote-pushdown rewriter sees catalog
  tables only. So `SELECT a FROM mssql_scan(db, '<T-SQL>') WHERE b > 5 ORDER
  BY a LIMIT 10` brings every row of the T-SQL over and DuckDB filters,
  projects, sorts and cuts. The T-SQL already runs on the server, so it can be
  the inside of a derived table.
  - **Step 1, the operators over the scan.** Wrap the text as `SELECT … FROM
    (<T-SQL>) AS [r1]([c1], [c2], …)` and push the enclosing projection,
    filters, aggregates, ORDER BY and LIMIT into it. The column alias list
    names every column, so an unnamed one (`SELECT count(*) …`) or a
    duplicate does not fail the derived table (error 8155 / 8156). The
    types come from the scan's describe (spec 075), or from `columns` for
    `mssql_scan_unsafe` (spec 081). A filter alone could go through
    `filter_pushdown` on the raw scan with the same FilterEncoder as the
    catalog scan. The wider shapes (aggregates, ORDER BY, LIMIT) need the
    writer, through `MSSQLOptimizer`, which already knows the raw scans
    (`IsRawScanName`).
  - **Step 2, inlined with the same catalog's tables.** When the enclosing
    statement also reads tables of the same catalog (a join, IN / EXISTS, a
    CTE), the scan is inlined into the pushed statement as a derived table
    and the whole statement goes as one. Another catalog's scan, or a local
    table, keeps the scan as it is.
  - **Text that cannot nest:** several statements, DECLARE / SET, a `#temp`
    the batch creates, EXEC, `SELECT … INTO`, `OPTION (…)`, `FOR XML` /
    `FOR JSON`. A leading `WITH` cannot sit inside a derived table either
    (T-SQL has no CTE there); it would have to be hoisted to the top of the
    pushed statement, renamed against ours. Telling these apart needs a
    lexer over the user's text (literals, comments, `[…]` skipped, as
    `SqlMayChangeSchema` does). Whatever it does not recognise as a single
    SELECT keeps the scan as it is. `mssql_scan_params` parameters merge
    with the writer's, renamed where they clash.
  - **Sorting (the open part):**
    - *An outer ORDER BY* follows the ORDER BY rule below, keyed on the
      described type and collation of the scan's column
      (`sp_describe_first_result_set` returns `collation_name`). A string
      under a collation is sorted by DuckDB; a number or a date goes with
      the rest, a bounded UTF-8 varchar through its bytes. A computed column
      whose collation the describe reports from an expression should be
      checked before it is trusted. `mssql_scan_unsafe`'s `columns` carry no
      collation, so its string keys are always sorted by DuckDB.
    - *Filters on the raw scan* (step 1): on 2.0 a `filter_pushdown` scan must
      apply every TableFilter it is given, so the raw scan needs the same
      client-side net as the catalog scan (`table_scan.cpp`, the refused
      filters armed client-side).
    - *An ORDER BY inside the user's text.* At the top level of their batch
      it is legal; inside a derived table the server refuses it (1033)
      unless TOP / OFFSET / FOR XML is there. The written order must not be
      dropped (owner's rule), and the extension does not rewrite the user's
      T-SQL. So such a text is not wrapped: the scan stays as it is.
    - *Order the user relies on today.* A raw scan is one stream, so
      `SELECT * FROM mssql_scan(db, '… ORDER BY x') LIMIT 10` returns the
      first ten by `x`. Wrapped as `SELECT TOP (10) … FROM (… ORDER BY x
      OFFSET 0 ROWS) r`, the server guarantees no order through the derived
      table, so the result would be any ten rows. The same holds for any
      operator over the scan that DuckDB runs in order of arrival. This is
      one more reason not to wrap a text with an ORDER BY. A text without
      one carries no order to keep.
- **`IGNORE NULLS` in a window (`last_value(x IGNORE NULLS)`, the
  forward-fill idiom).** The server takes it from SQL Server 2022 (and Azure
  SQL Database / Managed Instance) and refuses it as a syntax error before.
  Vetoed in E2. No LOGINACK work is needed: since spec 080 D0 the catalog
  reads `SERVERPROPERTY('ProductMajorVersion')` and `EngineEdition` at ATTACH
  (`product_major_version_`, used by `DmlCapabilities`), so the writer can
  take it when the major version is 16+ or the engine is Azure SQL.
- **A correlated scalar subquery on a string key against an outer column.**
  Vetoed unless the key meets a constant (an outer varchar against an
  nvarchar compares under other rules); a same-type, same-collation outer
  column could be allowed.
- **A set operation at a statement's top.** Never pushed whole (owner's
  call): DuckDB combines the children. Pushing a whole `UNION` / `EXCEPT` /
  `INTERSECT` would move the deduplication to the server.
- **Vetoed constructs with a possible T-SQL form**: EXCLUDE / REPLACE /
  COLUMNS (the explicit list could be expanded once the columns are known),
  `> ANY` / `ALL` quantified comparisons, CTE and subquery column alias
  lists (both a derived column list), `EXCEPT
  ALL` / `INTERSECT ALL` (no T-SQL form; a ROW_NUMBER rewrite), `float`
  `+ - *` (inf in DuckDB, an error there), `min` / `max` of a string (an
  order under D4), mixed-type set operation columns with an explicit cast.

## Measured on TPC-H (test/bench/tpch, 2026-10-20)

TPC-H SF1 in a local SQL Server, with clustered columnstore and string columns typed as the specification says (`CHAR(n)` / `VARCHAR(n)`). Client B is DuckDB 2.0 + 0.3.0. Column C ("native") is SQL Server 2022 in docker under x86 emulation on a Mac, so it compares clients with one another and is not native throughput on real hardware (`test/bench/tpch/RESULTS-local.md`).
- **Pushed whole:** only q18. That one query ran 35× faster than 0.2.5: 0.057 s against 2.03 s, close to column C's 0.084 s.
- **Pushed partly:** q13. Only its inner `customer LEFT JOIN orders … GROUP BY c_custkey` goes: 150000 rows come back, and the server spends 1024 ms CPU on that half against 618 ms for the whole query in column C. Why the outer GROUP BY / ORDER BY was handed back is not yet known; look at the gain check.
- **Not pushed:** the other 20.

Over the 22 queries: 0.2.5 took 20.7 s, 0.3.0 16.6 s, and column C 1.55 s. That gap is the size of what follows. Each query reports only its first veto (from `MSSQL_DEBUG=1` on an EXPLAIN), so removing one may uncover the next.

- **Decimal arithmetic over different types** stops about half of TPC-H. The cases:

  | operation | queries | DuckDB's type |
  |---|---|---|
  | `DECIMAL(15,2)` with `DECIMAL(16,2)`, from `l_extendedprice * (1 - l_discount)` | q01, q03, q05, q10, q14, q15, q19 | the product is capped at `DECIMAL(18,4)` |
  | `DECIMAL(15,2) * BIGINT` | q11 | `DECIMAL(34,2)`, not capped |
  | a product | q06 | `DECIMAL(18)` (capped) |
  | a `DECIMAL(38,2)` product | q20 | past 38 digits |

  Corrected after #508. Two vetoes stand in the way, and lifting the first alone unblocks nothing:
  - the type-equality veto in `mssql_node_writer_values.cpp` (operands of different types are refused before `FunctionFor` is asked);
  - DuckDB's cap: a product of two decimals that both fit 18 digits is typed `DECIMAL(18, s1+s2)`, not `(w1+w2, s1+s2)`, when `w1+w2 > 18` and `s1+s2 < 18`, and it raises an overflow past ±(10^18−1) where the server's wider type holds the value (`mssql_expression_vocabulary.cpp` vetoes it for that reason).

  The prescription:
  - `+` / `−`: DuckDB's result type equals the server's below 38 digits, even for operands of different types; relax the equality veto for decimal and integer operands, compute DuckDB's type by DuckDB's own formulas, and cast back wherever the server's type differs.
  - A capped product: render `CAST(a * b AS decimal(18, s1+s2))`. Values match, and both sides raise an overflow at the same bound (8115 needs ANSI_WARNINGS, which LOGIN7's fODBC sets). One difference stays: the server may raise it on a row DuckDB never computes; the owner's call.
  - q11's `DECIMAL(15,2) * BIGINT` is exact as `DECIMAL(34,2)`; its HAVING (`sum(…) * 0.0001000000`) goes past 38 digits and stays vetoed, as does q20.
- **`date_part('year', DATE)`**, i.e. `extract(year …)`, stops q07, q08 and q09. `YEAR(x)` is exact for DATE and datetime2, but the writer renders no scalar function but arithmetic: it sees DuckDB's parsed `date_part('year', x)`, and the scan path's `year → YEAR` mapping never reaches it. The prescription: map `date_part` / `extract` with a constant `year` / `month` / `day`, and `year()` and its siblings, to `CAST(YEAR(x) AS bigint)` (DuckDB returns BIGINT); refuse datetime2(7) (read as TIMESTAMP_NS) and datetimeoffset. Those three queries then meet the decimal veto above.
- **ORDER BY: send it whenever the server orders the same way, sort in DuckDB otherwise (owner's decision, 2026-10-21).**
  - **The keys order on the server as in DuckDB** (`OrdersLikeDuckDB`: numbers, dates; a bounded UTF-8 `varchar` only through its bytes, `OrdersLikeDuckDBAsBytes`): the ORDER BY goes to the server always, at the top and in a subquery or CTE alike.
    - The query asked for that order, and it is not the extension's call whether the order matters: the SQL Server optimizer decides.
    - In a derived table or CTE, T-SQL takes an ORDER BY only with TOP / OFFSET, so it is written `ORDER BY … OFFSET 0 ROWS`. Without one the server refuses it with error 1033, "The ORDER BY clause is invalid in views, inline functions, derived tables, subqueries, and common table expressions, unless TOP, OFFSET or FOR XML is also specified" (checked on SQL Server 2022). At the top level an ORDER BY needs neither. Measure whether the server pays for a sort it does not need there.
    - Today the bytes form and the emulated NULL placement are allowed only under a LIMIT (and the bytes form not under DISTINCT); the rule drops the LIMIT condition for both, outside DISTINCT.
    - A nullable key: DuckDB puts NULLs last on an ascending key, the server first. Today such a key is vetoed without a LIMIT, which covers every nullable ascending key; the emulated placement (a leading `CASE WHEN k IS NULL …` key) is what lets it go.
  - **The server would order differently** (a string under a collation, D4): DuckDB sorts it afterwards.
    - At the top level, everything else goes to the server and DuckDB sorts the pushed result. The order is DuckDB's whatever the collation; no varbinary form is needed.
    - In a subquery or CTE, the subquery goes to the server without that ORDER BY. DuckDB sorts its result, and the enclosing query stays in DuckDB and is not rewritten further.
  - Today an ORDER BY the writer cannot render vetoes the whole statement, and a renderable one inside a derived table is not written whole. Measured on TestDB:

    | statement | what goes to the server today |
    |---|---|
    | `SELECT count(*), sum(k) FROM (SELECT id, k FROM t ORDER BY k) s` | only the inner SELECT (with its `ORDER BY [k]`); every row travels and DuckDB aggregates. Under the rule: the whole statement, with `ORDER BY [k] OFFSET 0 ROWS` inside |
    | the same with `ORDER BY name` (nvarchar) | nothing; D4's "the server does not order nvarchar as DuckDB does" vetoes the whole statement. Under the rule: the subquery without its ORDER BY, the sort and the aggregate in DuckDB |
    | `WITH c AS (SELECT … ORDER BY name) SELECT name, count(*) FROM c GROUP BY name` | nothing, the same veto |
    | `SELECT k, count(*) FROM t GROUP BY k` (no ORDER BY) | the whole statement |

    Subqueries and CTEs with an ORDER BY are common in generated SQL (BI tools, dbt, ORMs).
  - Under a LIMIT / OFFSET (TOP N) the server must order before it cuts if the LIMIT is to go with the statement. A key it orders differently can still be sorted and cut by DuckDB, but then the LIMIT stays in DuckDB and every row travels. The next item widens which string keys the server orders like DuckDB.
- **More string keys the server orders like DuckDB** (q02 and q21 here, under their LIMIT; q04 and q16, which have none, are covered by the rule above): **order by the UTF-8 bytes** for `nvarchar(n)`, `char(n)` / `nchar(n)` and code-page `varchar(n)`, not only for a UTF-8 `varchar`, the `OrdersLikeDuckDBAsBytes` rule of today. Corrected T-SQL (`COLLATE` is not part of `CAST`'s type: `CAST(nvarchar AS varchar)` converts in the column's code page first, turning what it cannot hold into `?`):
  - `nvarchar(n)`: `CAST(CAST(col COLLATE Latin1_General_100_BIN2_UTF8 AS varchar(3*n)) AS varbinary(3*n))`;
  - code-page `varchar(n)`: through `CAST(col AS nvarchar(n))` first, then as above;
  - `char(n)` / `nchar(n)`: `RTRIM` first (stored padded, read trimmed).
  - Comparing varbinary does not consult a collation, and UTF-8 byte order is code point order, which is how DuckDB orders strings. One UTF-16 unit is at most 3 UTF-8 bytes, so `varchar(3n)` is enough, MAX past 8000. UTF-8 collations need SQL Server 2019+.
  - Residual differences: a trailing NUL ties with no NUL; unpaired surrogates may convert differently.
  - The owner's caveat (2026-10-20, "not sure where the database's collation orders strings its own way") is answered for the comparison, which ignores collation; what needs testing on CI, BIN2 and `SQL_` sources is the conversion step.
  - It leaves D4 for GROUP BY / DISTINCT / join keys as it is.
- **BIN2 does NOT order like DuckDB (withdrawn after #508).** The server pads trailing spaces under every collation, BIN2 included (`'ab'` = `'ab '`, `ab` + TAB before `ab`); a code-page `_BIN2` column orders by code-page bytes and an nvarchar `_BIN2` one by UTF-16 code units. `order_pushdown_collation.test` pins all three as not pushed, and `IsBinary2Collation` has no callers. Pushing a GROUP BY on such a column already follows D4 and folds `'ab'` and `'ab '` into one group; BIN2 does not change that. What stays from the measurement is the cost: BIN2 makes LIKE cheap on the server (q13: 20 s of server CPU against 56–63 s under NVARCHAR or a CI UTF-8 collation).
- **The rest:**
  - q12, "an expression of constants only": `Unify` (`mssql_node_writer_values.cpp`) finds no non-constant branch to type the constants from, reached from `WriteCase`: q12's `sum(CASE … THEN 1 ELSE 0 END)` has only literal branches. Type an all-integer-literal CASE / COALESCE as INTEGER (`CAST(1 AS int)`); the integer SUM path does the rest. q12's next veto is its `ORDER BY l_shipmode` (the ORDER BY rule).
  - q17, `DOUBLE` arithmetic (`0.2 * avg(…)`): the known float veto, and behind it a floating-point aggregate in a condition is vetoed too, so lifting the product alone does nothing. Low priority.
  - q22, `substring`: no string function is pushed, by design. q22 also meets the avg-in-a-condition veto.

## Runtime join filters (checked 2026-10-23)

A join of a DuckDB table (local, Parquet, DuckLake) with an SQL Server table is not pushed whole -- the rewriter takes only statements that read one catalog -- so the SQL Server side goes through the catalog scan. DuckDB already hands that scan the join's keys; the extension drops them. Found by the `duckdb-4d` session, checked against DuckDB v2.0.0 (`d54cbbe3df1`) and on a live server.

- **What DuckDB sends** (`physical_hash_join.cpp`, `JoinFilterPushdownInfo`). After the build side finalizes, per equality key, into the probe Get's `DynamicTableFilterSet` -- only for a Get with `filter_pushdown` (`join_filter_pushdown_optimizer.cpp`; a LIMIT / TOP N above the scan blocks it):
  - min == max: a plain, unwrapped `k = v`;
  - an IN list when 1 < build rows ≤ `dynamic_or_filter_threshold` (default 50), not dense and without NULL, wrapped `__internal_tablefilter_optional`;
  - a prefix-range filter for small-span keys (when registered, no min/max and no bloom follow);
  - min/max only when no IN list was pushed, as `k >= min AND k <= max`, each wrapped `__internal_tablefilter_optional` (equality joins) or `__internal_tablefilter_selectivity_optional`, ANDed with the scan's static filter on that column;
  - a bloom filter, deferred, inside the selectivity-optional wrapper.

  The wrapper is an `ExpressionFilter` holding a `BoundFunctionExpression` over `BoundReference(0)`; the real predicate is in its bind data (`OptionalFilterFunctionData::child_filter_expr`). They are merged into `TableFunctionInitInput.filters` when the probe pipeline's source is initialised, after the build (`physical_table_scan.cpp`): they DO reach `TableScanInitGlobal`. (Spec 070 said otherwise; corrected there.)
- **What the extension does with them.** FilterEncoder has no mapping for the wrapper, so the column's filter is refused and armed client-side -- a no-op, since DuckDB evaluates the optional wrapper as all-true -- and under the AND the static part is re-run client-side as well. Measured on SQL Server 2025, a local table ⋈ `dbo.LargeTable` (150k rows) `USING (id)`:

  | build keys | sent | rows from the server |
  |---|---|---|
  | 1 | `SELECT [id] FROM [dbo].[LargeTable] WHERE ([id] = @p0)` | 1 |
  | 3 | `SELECT [id] FROM [dbo].[LargeTable]` | 150000 |
  | 40 | `SELECT [id] FROM [dbo].[LargeTable]` | 150000 |
- **Level 0** (the first tier of `specs/065-dml-pushdown-recon/join-agg-pushdown.md`):
  1. FilterEncoder recognises the two optional wrappers at the top level or as an AND child and renders their bind-data child through the existing encoder, so the per-type rules (datetime ticks, the time(7) / datetimeoffset(7) truncated read, parameters typed from the column, spec 076) apply as they do to a written filter.
  2. An IN list is rendered on any type the encoder takes: it can only widen the result, and the join re-checks every match. A `>=` / `<=` bound only where `MSSQLColumnInfo::OrdersLikeDuckDB()` holds: a range on a string under a non-binary collation can EXCLUDE matching rows (build `{'B','a'}` gives `'B'..'a'`, empty under `_CI_AS`). This is not D4, which is about filters the user writes; a join filter must stay a superset.
  3. Any other wrapped filter (bloom, prefix-range, a string range, anything the encoder refuses) is dropped, not armed client-side. The unwrapped min == max comparison keeps today's path.
  4. Caveats: IN lists are literals today, so each distinct key set compiles its own plan (parameters would fix that); a registered prefix-range filter leaves nothing to render (small-span integer keys past 50 build rows).
- **The vehicles get none.** `mssql_scan` / `mssql_scan_params` (and `_unsafe`) set no `filter_pushdown`, so DuckDB never offers them join filters. Once the rewriter sends a nested FROM subquery or CTE inside a mixed statement (after duckdb/duckdb#26280), the join becomes `local ⋈ <vehicle>`; the vehicle could take filters on its output columns and wrap the pushed SQL at init (`SELECT * FROM (<pushed>) AS v WHERE …`), IN only, since a derived column's collation is unknown. Same net as the raw-scan item above.
- **More than 50 keys (tiers 1 / 2a).** DuckDB enumerates keys only as the IN list, and the threshold is one global setting; a bloom filter cannot be enumerated or hashed in T-SQL (a home-made bloom over `HASHBYTES` is not sargable -- not recommended). The way that needs no DuckDB patch: `MSSQLOptimizer` rewrites `HashJoin(build X, probe mssql Get)` with a small estimated build into `X ⋈ mssql_keyed_scan(DISTINCT keys of X)`, an in-out table function that collects the keys, loads them by INSERT BULK into `#keys` (the stage of spec 080) and runs `… WHERE k IN (SELECT k FROM #keys)`; the join stays in DuckDB, so the server returns a superset. Its own spec, after level 0 is measured. Raising `dynamic_or_filter_threshold` per session would hit every scan; an upstream per-scan threshold is out (no DuckDB patches).
- **The other direction already works:** with SQL Server as the build side, DuckDB pushes its bloom / prefix-range / IN filters into the local probe scan, given the MSSQL scan's cardinality estimate (statistics).
