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

## Execution

- **A vehicle that runs at bind: `mssql_scan_unsafe` / `mssql_scan_params_unsafe`.**
  Since spec 075 a scan describes at bind (`sp_describe_first_result_set`) and
  runs at init, because a query started at bind holds its connection from bind
  to execution, in an order DuckDB does not define: inside a transaction, or
  on a pool short of connections, two scans of one statement would contend for
  the one connection and could not be materialised in turn. A variant that
  starts the query at bind and takes the shape from the stream's own
  COLMETADATA saves the describe's round trip (measured E2: 1.5-3 ms a
  statement on a local server; a network round trip on a remote one); it
  would be refused inside a transaction and on a pool of one, and remote
  pushdown would use it only outside them (in autocommit with more than one
  connection), keeping the describing vehicle there. EXPLAIN would then run
  the statement (the cost spec 075 removed) unless the vehicle defers when
  only planning. E2 caches the describe of pushed statements instead (owner,
  E2: after spec 079).

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
