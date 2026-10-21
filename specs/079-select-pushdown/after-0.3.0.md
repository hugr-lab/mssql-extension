# Spec 079 — remote pushdown: what is left for after v0.3.0

The things spec 079 records as divergences, vetoes or later steps, gathered in
one place to plan from once v0.3.0 is out. Each names where it was found and
why it was not done in the PR that found it. Not a commitment, and not in any
order of priority yet.

## Correctness edges (recorded, not vetoed)

- **Division in a set operation's child that DuckDB sends on its own.** A set
  operation that also reads a local table or another catalog has each remote
  child finished by DuckDB's rewriter, and `RemoteExecute` cannot tell that
  child from a statement. A division there is NULL at a zero divisor where
  DuckDB says inf, before DuckDB deduplicates or filters it: `SELECT n / 0 ...
  UNION SELECT NULL::DOUBLE FROM loc` gives {NULL} pushed, {inf, NULL}
  locally. Vetoing it would take division from every statement's top (E1
  full review). The way forward is the next item.
- **A divisor that cannot be zero.** Division is rendered as
  `CAST(a AS float) / NULLIF(CAST(b AS float), 0)` and vetoed in conditions
  because of the zero divisor alone. A divisor the writer can prove non-zero
  -- a non-zero constant, `CASE WHEN b = 0 THEN ... ELSE a / b END`, a
  `WHERE b <> 0` on the same node, a column with a CHECK constraint -- needs
  neither the NULLIF nor the veto, and could be compared, ordered and kept in
  a set operation's child (owner's suggestion, E1).
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
  many-to-many pass at the default and the server may produce ~10^11 rows
  where the scans would ship 1.8M (raised on #399). For large statements
  the answer is the server's own estimate: a SHOWPLAN on a separate
  connection (or an `mssql_estimate` function) read before deciding (owner's
  plan, E1). Owner's call on #399: not before v0.3.0 unless the rest is done
  first -- no interim heuristic. A cheaper step on the way, if wanted: a
  product bound (largest x second-largest input against an output-row
  threshold of its own), from the same cached counts, no round trip.
- **Keys through a wrapper.** A derived table's key is its GROUP BY / DISTINCT
  columns only; a plain join of keyed tables inside it carries no key up, so
  a GROUP BY over the wrapper's columns is treated as not reducing (E1 full
  review, conservative).
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
  widenings at the check.

## Shapes not pushed yet

- **E2** (planned): windows and QUALIFY, FILTER as CASE, `string_agg`, the
  default flip with the D4-for-sets documentation.
- **Decomposition of a window over `*`, and of a join with a local table.** A
  window over named columns decomposes; over `*` it does not (the `*` over a
  join guard). A FROM subquery or a CTE body in a statement that also reads a
  local table is not finished by DuckDB's rewriter on its own --
  duckdb/duckdb#26280; nothing to change here when DuckDB does.
- **A user's own `mssql_scan` inside a pushed statement.** A raw scan on the
  same catalog inside a CTE or a subquery could be inlined as a derived table
  (its describe gives the types, its parameters merge with ours; refuse T-SQL
  that cannot nest: ORDER BY without TOP, several statements, DECLARE).
- **`IGNORE NULLS` in a window (`last_value(x IGNORE NULLS)`, the
  forward-fill idiom).** The server takes it from SQL Server 2022 and refuses
  it as a syntax error before; the extension keeps only the TDS version, which
  is the same from 2012 on. Vetoed in E2; the product version LOGINACK already
  carries, kept on the connection, would let a 2022+ server take it (owner,
  E2).
- **A correlated scalar subquery on a string key against an outer column.**
  Vetoed unless the key meets a constant (an outer varchar against an
  nvarchar compares under other rules); a same-type, same-collation outer
  column could be allowed.
- **A set operation at a statement's top.** Never pushed whole (owner's
  call): DuckDB combines the children. Pushing a whole `UNION` / `EXCEPT` /
  `INTERSECT` would move the deduplication to the server.
- **Vetoed constructs with a possible T-SQL form**: EXCLUDE / REPLACE /
  COLUMNS (the explicit list could be expanded once the columns are known),
  `> ANY` / `ALL` quantified comparisons, CTE column alias lists, `EXCEPT
  ALL` / `INTERSECT ALL` (no T-SQL form; a ROW_NUMBER rewrite), `float`
  `+ - *` (inf in DuckDB, an error there), `min` / `max` of a string (an
  order under D4), mixed-type set operation columns with an explicit cast.

## Measured on TPC-H (test/bench/tpch, 2026-10-20)

TPC-H SF1 in a local SQL Server, with clustered columnstore and string columns typed as the specification says (`CHAR(n)` / `VARCHAR(n)`). Client B is DuckDB 2.0 + 0.3.0.
- **Pushed whole:** only q18. That one query ran 35× faster than 0.2.5: 0.057 s against 2.03 s, close to native SQL Server's 0.084 s.
- **Pushed partly:** q13. Only its inner `customer LEFT JOIN orders … GROUP BY c_custkey` goes: 150000 rows come back, and the server spends 1024 ms CPU on that half against 618 ms for the whole query natively. Why the outer GROUP BY / ORDER BY was handed back is not yet known; look at the gain check.
- **Not pushed:** the other 20.

Over the 22 queries: 0.2.5 took 20.7 s, 0.3.0 16.6 s, and native SQL Server 1.55 s. That gap is the size of what follows. Each query reports only its first veto (from `MSSQL_DEBUG=1` on an EXPLAIN), so removing one may uncover the next.

- **Decimal arithmetic over different types** stops about half of TPC-H. The cases:

  | operation | queries | DuckDB's type |
  |---|---|---|
  | `DECIMAL(15,2)` with `DECIMAL(16,2)`, from `l_extendedprice * (1 - l_discount)` | q01, q03, q05, q10, q14, q15, q19 | `1 - l_discount` widens to (16,2) |
  | `DECIMAL(15,2) * BIGINT` | q11 | |
  | a product | q06 | `DECIMAL(18)`, the veto taking DuckDB's int64 overflow into account |
  | a `DECIMAL(38,2)` product | q20 | |

  For `+ - *`, while the result stays within 38 digits, the result type is the same on both sides: precision p1 + p2 (+1), scale s1 + s2 for a product; max scale for a sum or difference. Both sides are then exact, and both raise an error on overflow. Allow these, and keep division and anything past 38 digits vetoed.
- **`date_part('year', DATE)`**, i.e. `extract(year …)`, stops q07, q08 and q09. `YEAR(x)` is exact for DATE and datetime2. Those three then meet the decimal veto above.
- **ORDER BY a string** stops q02, q04, q16 and q21, because the server orders by collation (D4). Two ways to remove it:
  1. **Order by the UTF-8 bytes** for `nvarchar(n)`, `char(n)` (after `RTRIM`) and code-page `varchar(n)`, not only for a UTF-8 `varchar` under a LIMIT, the `OrdersLikeDuckDBAsBytes` rule of today:
     ```sql
     ORDER BY CAST(CAST(col AS varchar(3*n) COLLATE Latin1_General_100_BIN2_UTF8) AS varbinary(3*n))
     ```
     - Comparing varbinary does not consult a collation. UTF-8 byte order is code point order, which is how DuckDB orders strings; the UTF-8 collation only serves to get the UTF-8 encoding.
     - One UTF-16 unit is at most 3 UTF-8 bytes, so `varchar(3n)` is enough, MAX past 8000.
     - UTF-8 collations need SQL Server 2019+.
     - `varbinary` of the nvarchar itself would be UTF-16LE, whose byte order is not code point order.
     - In the writer, unlike `mssql_order_pushdown`, the stake is the whole statement, so the LIMIT-only rule need not apply.
     - **Owner's caveat (2026-10-20):** not sure this is right where the database's collation orders strings its own way. Verify on CI, BIN2, UTF-8 and `SQL_` collations against DuckDB's order before relying on it.
  2. **Push everything but the top-level ORDER BY** and let DuckDB sort the (small) result. This is exact for every type, CHAR padding included, and independent of collation.

  Either leaves D4 for GROUP BY / DISTINCT / join keys as it is.
- **A BIN2 column orders like DuckDB.**
  - `…_BIN2` / `…_BIN2_UTF8` compare and order by code point, as DuckDB does, so an ORDER BY / GROUP BY / DISTINCT on such a column could be pushed as is, without the varbinary form and without D4's caveat.
  - Measured on the same TPC-H data with four string schemas (`test/bench/tpch/RESULTS-local.md`):
    - UTF-8 `varchar` already sends q02 and q21 whole, through the bytes order under their LIMIT 100 (q21: 20.7 s → 0.47 s);
    - BIN2 also makes LIKE cheap on the server: q13 costs 20 s of server CPU, against 56–63 s under NVARCHAR or a CI UTF-8 collation.
- **The rest:**
  - q12, "an expression of constants only": not understood yet.
  - q17, `DOUBLE` arithmetic (`0.2 * avg(…)`): the known float veto.
  - q22, `substring`: no string function is pushed, by design.
