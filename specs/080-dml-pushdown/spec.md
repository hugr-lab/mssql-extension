# Spec 080 — UPDATE / DELETE / INSERT … SELECT through the writer, and the staged fallback

**Status:** Draft, 2026-09-17, on `spec/065-067-revalidation` (PR #364),
after spec 079 (the writer it uses). Absorbs and closes the 065
reconnaissance's DML half: spec 065 (direct UPDATE/DELETE — its goal, on
the core mechanism instead of our own plan hooks), the 066 remainder
(scans feeding a DML materialise in a transaction), and spec 067 (the
match-key ladder, so DML works without a primary key). The research is
`../065-dml-pushdown-recon/revalidation-2026-09-17.md` § 3, § 4, § 9.4 and
`research.md` there; `../067-dml-staging/spec.md` § 1–3 hold the ladder's
argument and are pointed at, not repeated.
**Goal:** a DML statement the writer can express runs on the server as
**one statement** — no rowids travel; one that it cannot express runs on a
staged JOIN keyed by the best key the table has — every column the server
can compare if it has no key, and a named refusal if those do not
distinguish its rows. **Closes #140 fully** (065 closed it for pushable
statements only; 067 for the rest).
**Not the goal:** RETURNING through the rewriter (an `OUTPUT` mapping is a
later row), MERGE (later), strict string semantics (owner, 2026-09-17: a
SQL Server user expects SQL Server's DELETE).
**Depends on:** spec 079 (the writer, `mssql_remote_pushdown`, the agreement
suite pattern); #350 (spec 077 — **open at the time of writing**, this spec
follows its merge: `ChooseRowIdKey` becomes rung 2 of the ladder and the
`IDENTITY_INSERT` bracket is reused); spec 062 as shipped in #348 —
`BulkLoadSession::Adopt` and the autocommit bracket `mssql::LoadTransaction`
(CLAUDE.md: "every DML statement is atomic in autocommit"); 062's text
predates W-numbered headings, so the code symbols are the citation.
**Revised 2026-10-08** against spec 079's **2026-09-23** revision — five
sequential PRs, and `mssql_remote_pushdown` read **once per catalog at
ATTACH** (079 D6), which is why the agreement suite here attaches twice
rather than `SET`ting it (W5) and why the DML half gets its own switch
(D5) — and against the design review of this text: the ladder's
non-comparable-type base case, the rung-3 anti-rematch rule, the stage
threshold, the join-form capability probe, the client-visible DML
properties, the CTAS type source, and a PR split of its own (§ 2.7).

---

## 0. Measured ground (in the research record)

| what | where | the fact |
|---|---|---|
| the rewriter and DML | § 5, § 9.1 | `GetNodeFromStatement` yields a node for INSERT, DELETE, UPDATE and MERGE INTO; the INSERT path pushes the SELECT alone when the target is local or the INSERT is vetoed; `RemoteExecute(QueryNode)` returns a table ref whose result stands in for the statement's |
| today's DML | § 3 (065 claims table) | UPDATE/DELETE scan rowids to the client and ship them back as `VALUES`-join batches of 500; in a transaction they defer to Finalize with the rows buffered; the scan's pushed WHERE is already the server's (native) |
| the sink set | § 3 | `CollectSinkCatalogs` counts INSERT and COPY, not UPDATE/DELETE — the 066 remainder |
| the key ladder | § 4, 067 § 1–2, 077 | rung 2 is `ChooseRowIdKey` (PK, else a usable unique index; datetime / sql_variant keys unusable, #358); rung 3 is value matching over all columns, exact for deterministic statements |
| strings in DML | § 8.3 → § 9.3 | native: the rowid path pushes the WHERE already and DuckDB does not re-check it, so `DELETE … WHERE a = 'ab'` removes the rows `SELECT … WHERE a = 'ab'` shows; a strict rewrite would remove fewer than the SELECT displays |
| autocommit atomicity | spec 062 (`mssql::LoadTransaction`, #348) | one statement connection, batches bracketed in a server transaction, the ENVCHANGE descriptor carried |

## 1. Design

### D1 — whole-statement DML through the rewriter

`Supports(EXECUTE_QUERY_NODE)` already claimed by 079 covers the DML nodes;
`SupportsPushdown(const QueryNode &)` grows `UpdateQueryNode`,
`DeleteQueryNode` and `InsertQueryNode` cases, and the writer renders:

| DuckDB | T-SQL | rule |
|---|---|---|
| `UPDATE t SET c = e, … [FROM u …] WHERE p` | `UPDATE t SET [c] = e, … FROM [s].[t] AS t [JOIN …] WHERE p` | every SET expression and the WHERE through 079's expression writer under the same rules; SET of a rowid-key column is refused as today; `DEFAULT` in SET → veto (server DEFAULT ≠ DuckDB's NULL default, 065 D2.5) |
| `DELETE FROM t [USING u …] WHERE p` | `DELETE t FROM [s].[t] AS t [JOIN …] WHERE p` | same |
| `INSERT INTO t [(cols)] SELECT …` (both sides remote) | `INSERT INTO [s].[t] ([cols]) SELECT …` | the SELECT through 079; an explicitly named identity column brackets the statement in `SET IDENTITY_INSERT` (077 W2); a column list that **omits** a column carrying a server DEFAULT → veto, the same divergence the `DEFAULT`-in-SET veto refuses (the server would apply its default where the shipped path inserts DuckDB's NULL, and a NOT NULL such column makes the two paths differ on whether the statement fails at all); `has_default` comes from `sys.columns.default_object_id <> 0` riding the column metadata query, no extra round trip; `INSERT … VALUES` stays on the shipped path (it carries no remote SELECT to win by) |
| `INSERT INTO local SELECT … FROM remote` | the SELECT alone | the rewriter does this by itself (`push_select_only`); nothing to add |
| a **vetoed** `INSERT INTO remote SELECT … FROM remote` | the SELECT alone; the rows land through the shipped INSERT | the same core behaviour (§ 0: the INSERT path pushes the SELECT alone when the target is local **or the INSERT is vetoed**) — not ours to suppress, and correct: a SELECT reads. D2's invariant is about a DML's own predicate, never about its SELECT |
| `CREATE TABLE remote AS SELECT … FROM remote` | our CREATE (table kind, collation, lengths — the WITH options keep meaning), then the pushed `INSERT … SELECT` | not `SELECT INTO`; needs `EXECUTE_STATEMENT`, which is a **catalog-wide** claim — so `SupportsPushdown(const SQLStatement &)` must veto every statement shape but CREATE TABLE AS. Column types and the DDL that keeps the shipped path: **CTAS** below |
| `RETURNING`, `MERGE INTO`, `ON CONFLICT` | — | veto; the shipped path handles what it handles today |

Execution: `RemoteExecute` returns a ref to the **count form** of the
vehicle — a distinct table function whose result shape is statically one
BIGINT from the DONE token. It **must not** reuse `MSSQLScanBind`'s shape
discovery: `DescribeFirstResultSet` answers `ok == false` for a statement
with no result set and the bind then **runs the statement** (spec 075's F1
fallback, `executed_at_bind`), and the rewriter descends into `EXPLAIN` and
`PREPARE` — an inherited bind would make `EXPLAIN UPDATE ms.t …` perform the
update. The count form needs no describe and executes only when the plan
runs (W5 asserts `EXPLAIN` / `PREPARE` of a pushed DML change nothing). The
statement runs through `MSSQLStatementConnection` as every DML does (the
pinned connection inside a transaction; in autocommit one connection under
the `mssql::LoadTransaction` bracket, so the statement is atomic); the
affected count is returned as the single BIGINT row DuckDB expects of a DML,
in a column named **`Count`** — the name DuckDB's own DML binders give it
(`result.names = {"Count"}` in `bind_update.cpp` / `bind_delete.cpp` /
`bind_insert.cpp`), so the printed result is byte-identical to the shipped
path's; errors surface with the server's message and number, as `mssql_exec`
reports them. The plan is one statement; nothing is buffered on the client; in
a transaction nothing defers. After the statement the target table's row
count and statistics cache entries are invalidated (as COPY and CTAS do
today — a pushed DML passes through no plan hook, so nothing else would).

**Exactly once.** A pushed DML executes once per plan execution: the count
form has **one global init and no parallel local state**, and it is not a
scan — there is no range to split and nothing to re-initialise. The
invariant is load-bearing in a way a SELECT vehicle's is not: a table
function DuckDB initialised twice would run the UPDATE twice. A `PREPARE`d
pushed DML executed twice must affect its rows twice, as the shipped path
does. W5 pins both ends (`threads = 4`, and `PREPARE` + two `EXECUTE`s).

**What stops being a DML to DuckDB.** The rewriter replaces the whole
statement with a SELECT over the vehicle's ref (`FinishPushdown` /
`WrapRemoteRef`, research § 5), so for a pushed statement `StatementType` is
`SELECT`, `StatementProperties::return_type` is `QUERY_RESULT` rather than
`CHANGED_ROWS`, and `modified_databases` is empty. The `Count` column keeps
the shell's output the same, but a client binding that reads the statement
type rather than the result — `cursor.rowcount`, `executeUpdate()` — and
DuckDB's own modified-catalog bookkeeping see a SELECT. This is the DML peer
of 079's "result types change for a pushed statement": enumerated in W6's
docs, carried as a risk in § 4, pinned by W5, and the reason the DML half
has a switch of its own (D5).

**Read-only attach.** DuckDB's read-only enforcement is bind-time
(`modified_databases`, filled only by the DML binders) and the extension's
`CheckWriteAccess` runs in the `Plan*` hooks — a rewritten DML is a SELECT
before either runs, so both are skipped and `ATTACH … (READ_ONLY)` would stop
protecting the catalog. Two guards, as duckdb-mysql's vehicle has:
`SupportsPushdown` refuses every DML and CTAS node when the catalog
`IsReadOnly()` (the statement then takes the shipped path, whose hook
refuses it as today), **and** the count form's bind refuses on a read-only
catalog, so no route around the first guard executes a write. Both guards go
through `MSSQLCatalog::CheckWriteAccess` (`catalog/mssql_catalog.cpp`), so the
class and the wording are the shipped path's —
`CatalogException("Cannot execute %s: MSSQL catalog '%s' is attached in
read-only mode")` — and the same refusal does not grow a second contract. A
read-only **database** (`SET access_mode = read_only`, `duckdb --readonly`)
is DuckDB's own bind-time check and is skipped for the same reason, so the
count form's bind refuses that too, with DuckDB's wording. W5 exercises all
three refusals.

**CTAS.** `RemoteExecute(SQLStatement)` for the CREATE TABLE AS shape
returns a **lazy** ref too — nothing runs at optimize time, so `EXPLAIN` /
`PREPARE` create nothing. At execution the CREATE and the pushed
`INSERT … SELECT` run on one connection in **one server transaction** (the
autocommit bracket; the pinned transaction otherwise), so a failed load
leaves no table behind. The catalog cache is invalidated as today's CTAS
invalidates it.

The target's **column types** cannot come from the bound query: the rewriter
runs on the parsed statement, before binding (research § 5), so there are no
result types to read — and `SELECT INTO` is rejected precisely because it
would let the server pick them and ignore every CTAS setting. They come from
`sp_describe_first_result_set` on the **rendered** SELECT, issued at
execution on the statement's own connection immediately before the CREATE —
the describe 079 PR A already reads native types and collations from for
`mssql_scan`. Those types then go through the shipped CTAS type mapping, so
`mssql_ctas_text_type`, `mssql_default_string_length`,
`mssql_utf8_collation`, `mssql_default_table_kind` and
`WITH (table_kind = …)` keep their meaning, and `OR REPLACE` /
`IF NOT EXISTS` behave as they do today. A describe the server cannot answer
(`ok == false`) is a **veto at execution**: the ref reports the refusal and
nothing is created. It must not fall back to running the SELECT for its
shape — that is spec 075's F1 fallback, and inheriting it is what would make
`EXPLAIN` dangerous. This round trip before the decision is why pushed CTAS
is the **last** PR of § 2.7 and the one droppable piece of this spec.

`EXECUTE_STATEMENT` is a **per-catalog boolean**, not a per-shape claim:
claiming it routes every DDL statement against the catalog — `CREATE TABLE`,
`DROP`, `ALTER`, `CREATE SCHEMA`, `CREATE VIEW` — through
`SupportsPushdown(const SQLStatement &)`, which must therefore answer
**false for every shape but CREATE TABLE AS** so the rest stays on the
shipped path untouched (W5 asserts DROP / ALTER / CREATE SCHEMA after the
claim). When the CREATE is vetoed but its query is pushable, 079 D3's
behaviour stands and nothing here changes it: the rewriter pushes the query
alone (`RewriteCreateInfo`) and the shipped CTAS executor loads the rows it
returns. Nothing is handled twice — the veto is what hands the statement
back.

Strings: **native** (D4 of 079 applies unchanged). What the pushed
statement's WHERE selects is what a pushed SELECT with that WHERE shows.

### D2 — what is vetoed, and what happens then

Everything 079's writer vetoes (an unmapped function, a parameter, a local
table in FROM / USING, a construct outside the table) vetoes the statement,
and the rewriter leaves it to the binder: the statement plans as today,
through `PlanUpdate` / `PlanDelete` / `PlanInsert`, on the path D3 describes.
**No DML predicate is ever partially pushed**: a DML's own WHERE and SET
either run on the server as one statement or run on the client whole
(research § 2: extra rows *written* are not harmless, so no relaxed pushdown
of a DML predicate, ever). That is a narrower claim than "nothing is
half-pushed", and deliberately so — a vetoed `INSERT INTO remote SELECT …
FROM remote` does have its **SELECT** pushed and its rows written by the
shipped INSERT (D1's table, `push_select_only`). That is core behaviour, it
is not ours to suppress, and it is safe: the half that is pushed only reads.

### D3 — the fallback: one path, keyed by the ladder

The shipped rowid path generalised by 067's ladder, resolved per table at
plan time:

1. **Primary key** — today's join key.
2. **A usable unique index** — spec 077's `ChooseRowIdKey` (PR #350, open at
   the time of writing), which becomes the rowid source; its refusals (`RowIdRefusal`) name why a key
   is unusable (datetime / sql_variant, #358; a cast-required type, #354).
3. **Every comparable column, NULL-safe** — the keyless base case (067 § 1's
   argument: for a deterministic WHERE and SET, matching by value updates
   exactly the set DuckDB would). The key is every column the **server** can
   compare, and the ones it cannot are excluded **by type, with a refusal —
   not by cost**: a column marked `MSSQLColumnInfo::is_cast_required` (the
   catalog's own marker for a type it reads as NVARCHAR(MAX): `xml`,
   `sql_variant`, `hierarchyid`, CLR UDT), one marked `is_geometry` /
   `MSSQLColumnInfo::IsSpatialType` (`geometry`, `geography`), and the
   deprecated LOBs `text` / `ntext` / `image` are usable in no `=`, no
   `IS NOT DISTINCT FROM`, no `INTERSECT` and no `DISTINCT`. They are left
   out of the key, and if the remaining columns are **not unique in the
   staged set** the statement is **refused by name**, naming the column and
   its type ("`t.payload` is `xml`: the server cannot compare it, and the
   other columns do not distinguish these rows — add a unique index"). 067
   § 2 anticipated half of this as a cost note; here it is a refusal, because
   there is no key to fall back to. MAX-length columns (`varchar(max)`,
   `nvarchar(max)`, `varbinary(max)`) **are** comparable and stay in the key
   unless the cost exclusion below drops them.
   The join form is `IS NOT DISTINCT FROM` where the server has it, else
   `EXISTS (SELECT t.c1, t.c2, … INTERSECT SELECT s.c1, s.c2, …)` — NULL-safe
   on every version (067 § 2), and the **default**: see the capability probe
   below. The stage is DISTINCT over the key.
   LOB-sized columns may be dropped from the key **for cost** when the rest
   is unique in the staged set — but **never when any SET expression or the
   WHERE references a dropped column**. Two staged rows then share the
   reduced key while carrying different new values, and SQL Server's
   `UPDATE … FROM … JOIN` with several matching source rows picks one
   arbitrarily and raises nothing: the target silently takes whichever row
   the server chose. The rule is that the staged set must be unique on the
   reduced key **and** on the computed new values, or the column stays in the
   key.
   A VOLATILE function in WHERE or SET on rung 3 is refused by name ("add a
   unique index, or make the expression deterministic"). So is a rung-3
   statement whose WHERE is **not fully
   pushed to the scan**: 067 § 1's equivalence argument needs both sides to
   compare under the same semantics, and a predicate the scan's pushdown
   refuses is evaluated client-side under DuckDB's binary equality while the
   stage JOIN matches under the column's collation and padding
   (`DELETE FROM keyless WHERE regexp_matches(v, '^ab$')` selects `ab`
   client-side, the JOIN also removes `AB` and `ab␣` — § 8.5). The refusal
   names the unpushed predicate; rungs 1–2 are unaffected, a key identifies
   its row.

**The capability probe (which join form).** `IS NOT DISTINCT FROM` is SQL
Server 2022 (16.x) and newer; the `EXISTS … INTERSECT` form is correct on
every version, 2022 included. So the INTERSECT form is the **default** and
the operator is an optimisation taken only when a cached answer positively
says it is there — nothing in the codebase answers this today (the LOGINACK
version is the protocol's, not the product's). The answer rides the **ATTACH
collation query**: one more `SERVERPROPERTY` pair appended to the statement
`QueryDatabaseCollation` already sends, exactly as issue #331 appended
`SNAPSHOT_STATE_COLUMN` (`catalog/mssql_catalog.cpp`) — no extra round trip, and
no probe on the statement path — cached on the catalog beside
`snapshot_isolation_state_`. The operator is taken when
`SERVERPROPERTY('ProductMajorVersion') >= 16`, **or** when
`SERVERPROPERTY('EngineEdition') IN (5, 8)`: Azure SQL Database and Managed
Instance are evergreen and have the operator while **reporting
`ProductMajorVersion` 12**, so a version test alone would send the slow form
there forever. Everything else takes the INTERSECT form — a lower version, a
NULL or unreadable property, a Fabric or Synapse endpoint
(`mssql::IsFabricEndpoint`, `src/include/mssql_platform.hpp`), an edition
nobody has seen yet. The fallback errs in the only direction that cannot be
wrong.

Delivery: rungs 1–2 keep the `VALUES`-join statements up to
**`mssql_dml_stage_threshold`** and stage above it; rung 3 always stages.
The threshold is a new setting (BIGINT, default **1000 rows**, the shape and
the default of `mssql_insert_bcp_threshold`): **rows**, counted as they
arrive in the stage buffer, **never estimated** — the planner's cardinality
is not an answer here. 1000 is the starting default because it is where the
`VALUES`-join path is at most two statements; W3 measures the crossover on a
local server and at a 20 ms RTT, as spec 062 § 6.2 did, and the default moves
if the measurement says so. The stage is a **session-local `#stage_<uuid>`** —
066 D5 chose `##` because the stage was filled from a second connection; here
it is filled on the statement's own connection (`BulkLoadSession::Adopt`, the
pinned one in a transaction), so the cross-session visibility is neither
needed nor wanted, and a `#` name also stays clear of spec 063 D1's refusal
of a second bulk-load writer against a session-scoped target. The connection
is taken at the **first `Sink`**, never at init (W2's first consequence). The
DML is `UPDATE t SET t.c = s.c__new … FROM target t JOIN #stage s ON <key>` /
`DELETE t FROM target t JOIN #stage s ON <key>`, and the stage is dropped on
the way out (067 D2).

**Batching, and why a rung-3 UPDATE is one statement.** A DELETE batches at
~100k staged rows: a deleted row cannot match a later batch. An UPDATE on
**rungs 1–2** batches too — SET of a rowid-key column is refused, so no batch
can move a row into another batch's key. An UPDATE on **rung 3** is a
**single statement over the whole stage**, because there the key is every
comparable column and any SET necessarily rewrites part of it. Batched, it
corrupts silently on exactly the keyless case #140 is about: batch 1 commits,
and a row it rewrote matches batch 2's old-value key and is updated a second
time with another row's new values — keyless `(a, b)` holding `(1, 1)` and
`(2, 2)`, `UPDATE t SET a = a + 1`, the stage split in two, and the `(2, 1)`
batch 1 produced is matched again by batch 2's `(2, …)` key. One statement is
immune: the server matches it against the target as it stood before the
statement and updates each target row at most once (several matching staged
rows pick one arbitrarily, which the key's DISTINCT and the LOB restriction
above already handle). If one statement ever proves too much for the log or
escalates further than a user will accept, the way back is a key-stable
generation guard carried in the stage — not batching as it stands. Named
here so nobody re-derives the hazard.

**Stage fully, then join**: the
JOIN batches start after the feeding scan has finished — 067 D3's pipelined
autocommit mode (scan ∥ stage-fill ∥ DML on separate connections) is **not**
used, because a rung-3 JOIN cannot seek (`IS NOT DISTINCT FROM` / the
INTERSECT form over every column is not SARGable), each batch scans the
target and escalates toward a table X lock, and a scan still reading the
same table from a second session while that happens is a blocking pattern
by construction (a 1205 cycle is plausible, unconfirmed — W5 forces the
case). The `vector<vector<Value>>` buffer and the defer machinery go (no
per-value path).

The **066 remainder** lands here because this is where it stops being
theoretical: `CollectSinkCatalogs` counts `LOGICAL_UPDATE` and
`LOGICAL_DELETE` beside INSERT and COPY, so the scans feeding a DML
materialise at init inside a transaction (spec 075 W3) and the executors'
`defer_execution_` retires — the sink and the scan feeding it share the one
pinned connection by the same rule INSERT already follows.

`BindUpdateConstraints` / `GetRowIdColumns` stop refusing a keyless table at
bind (065 D4; the two `RowIdRefusal` throws in `catalog/mssql_table_entry.cpp`,
`BindUpdateConstraints` and `GetRowIdColumns`): the refusal moves to
plan time, where rung 3 names what is left — the volatile guard, the
unpushed predicate, the non-comparable-column case. Two things those
bind-time refusals **also** covered, and that must stay refused by name at
bind rather than fall through to a path that cannot serve them:

- **`UPDATE … RETURNING` / `DELETE … RETURNING` against a table that resolves
  to rung 3.** RETURNING through the rewriter is a non-goal (§ 3) and the
  staged JOIN has no `OUTPUT` mapping, so nothing would execute it. Refused
  naming the table and the reason ("no usable unique key, and RETURNING needs
  one"). Rungs 1–2 keep today's RETURNING behaviour unchanged.
- **The `rowid` pseudo-column** stays refused on a keyless table (067 § 5:
  the pseudo-column is the key's, and rung 3 has no key to expose) — that is
  the **third** `RowIdRefusal` throw in the same file, the one raised with
  `"rowid"` as the operation, and it is the one that does **not** move. The
  staged path never materialises a rowid.

### D4 — MERGE, later

The rewriter carries `MergeQueryNode`, and T-SQL has `MERGE`; the mapping is
a D1 row when asked for. Until then MERGE keeps its bind-time PK requirement
(DuckDB's MERGE binder needs rowid for match classification — upstream).

### D5 — the DML switch

`mssql_dml_pushdown` (BOOLEAN, default **true**, `SetScope::GLOBAL`), read in
`SupportsPushdown(const QueryNode &)` for the three DML nodes and in
`SupportsPushdown(const SQLStatement &)` for CTAS. `mssql_remote_pushdown`
cannot serve as the DML lever: 079 D6 reads it **once at ATTACH** because it
also answers `IS_REMOTE` and `DatabaseManager`'s remote-catalog counter has to
stay consistent, so after attaching it is not a switch at all — and throwing
it would give up the read path too, which § 4's first risk should not cost.
The DML overloads, by contrast, are consulted per rewrite and read the same
database-level store (`DBConfig::TryGetCurrentSetting`), so this one can be
flipped at any time, costs one lookup per rewrite when on, and leaves every
pushed SELECT alone when off (the statement then takes D3's path). There is
still **no per-session** form — D6's reason is unchanged, and the docs say so.

## 2. Work

### W1 — the DML writer and the count vehicle

`UpdateQueryNode` / `DeleteQueryNode` / `InsertQueryNode` rendering in
`SQLWriter`, resolving columns against the entries 079 D1 recorded
**thread-locally for this rewrite** — never a fresh lookup by name, which
without a `ClientContext` would read the shared cache and miss the
transaction's own layer (#380); the `IDENTITY_INSERT` bracket reused from
077 W2; the omitted-DEFAULT veto, with `has_default` added to the column
metadata query; the count form over `MSSQLStatementConnection` (static shape,
the `Count` column, one global init and no parallel local state, lazy, and
the three read-only refusals through `CheckWriteAccess`);
`mssql_dml_pushdown` (D5); row-count and statistics invalidation after a
pushed DML. The DML token loop exists once (065 D3): the three shipped
executors and the new path call one `ExecuteDmlBatch`.

The **CTAS** shape is its own PR (§ 2.7), not part of this one:
`EXECUTE_STATEMENT` claimed with `SupportsPushdown(SQLStatement)` vetoing
every other statement shape, the `sp_describe_first_result_set` type source
and its execution-time veto, the CREATE and the load in one server
transaction, catalog invalidation.

### W2 — the 066 remainder

`CollectSinkCatalogs` += `LOGICAL_UPDATE` / `LOGICAL_DELETE`;
`defer_execution_` and its buffers removed from the update/delete
executors; the transaction suite proves a scan feeding an UPDATE inside
`BEGIN … COMMIT` materialises and the connection is Idle for the sink.

A **pool of one connection** (`mssql_connection_limit = 1`) must work in
autocommit too. Since #380 `MaterializeSharedConnectionScans` also
materialises in autocommit when the catalog's pool limit is 1
(`HasSingleConnectionPool`): the scans and the sink take turns at the one
connection. CTAS and INSERT … SELECT already work that way. UPDATE and
DELETE do not yet: their scan streams while the executor's batches ask the
pool for the only connection, and they wait out `mssql_acquire_timeout`.
Counting `LOGICAL_UPDATE` / `LOGICAL_DELETE` as sinks closes that as well.
Two consequences for the rest of this spec:

- **The staged path (D3) must take its connection after the source scan
  has given it back.** On a pool of one it must not take it at init. A
  `BulkLoadSession::Adopt` in the operator's global sink state holds the
  connection before the source scan's InitGlobal runs, which is exactly why
  #380 turned CTAS's bulk load off on a pool of one (`ResolveConnectionMode`
  in `mssql_ctas_executor.cpp`). **D3 settles it: acquire at the first
  `Sink`**, which is the shape `BulkLoadSession::DeferAdoption` /
  `AdoptDeferred` already gives COPY and CTAS. Falling back to the
  `VALUES`-join statements on a pool of one is **not** the alternative: rung
  3 has no `VALUES`-join form, and W5 and acceptance 4 both require a staged
  rung-3 statement to run there.
- **A pushed statement (D1) needs no scan at all,** so it runs on a pool of
  one as it is. The fallback is what has to be tested there.

### W3 — the ladder

Rung 3 in `ChooseRowIdKey`'s caller (the plan-time resolution); the
non-comparable-column exclusion (`is_cast_required` / `is_geometry` /
`text`|`ntext`|`image`) and its named refusal; the two NULL-safe join forms
with the capability probe riding the ATTACH collation query (the
`SERVERPROPERTY` pair, the `EngineEdition IN (5, 8)` branch, the INTERSECT
default, the answer cached on the catalog); the volatile guard and the
unpushed-predicate refusal; the staged delivery — `#stage_<uuid>`, `Adopt` at
the first `Sink`, the batched JOIN DELETE and the **single-statement** rung-3
UPDATE; the DISTINCT stage, and the cost-only LOB exclusion with its
SET/WHERE restriction; `mssql_dml_stage_threshold` and the crossover
measurement that fixes its default; the bind-time refusals replaced by the
plan-time ones, with RETURNING and the `rowid` pseudo-column still refused at
bind.

### W4 — riding cleanups from 065 D5

`mssql_dml_use_prepared` (registered, read into `DMLConfig::use_prepared`,
referenced nowhere else) is **deprecated, not removed**: unregistering an
extension option makes `SET mssql_dml_use_prepared = …` throw and kill the
rest of a `.duckdbrc`, so it stays registered as a documented no-op for one
minor release (the spec 047 precedent for `mssql_open` / `mssql_close`) with
a CHANGELOG line, and goes the release after. `EnsurePKLoaded` must not
degrade a discovery error to "no key" — spec 077 (#350) made it record
`discovery_error` and name it in the refusal; with D3 that is what keeps a
hiccup from silently changing which path a statement takes. (Spec 084 D5
superseded this: the key now comes in the same batch as the table's metadata,
so a failure fails the metadata load and no "no key" answer can come from it.)

### W5 — tests

- The bite test (065 § 4.1): a statement with an unmapped function in WHERE
  takes the fallback — asserted by the `remote_pushdown` counter and a
  `dml_staged` / `dml_values_join` counter under `MSSQL_COUNTERS`.
- Pushed correctness: filtered UPDATE / DELETE against expected row sets
  (multi-column SET, CASE in SET, functions in WHERE, empty match, full
  table, `UPDATE … FROM` a remote join); count fidelity on both paths;
  transactions (pushed statement inside BEGIN / COMMIT / ROLLBACK on the
  pinned connection, mixed with reads and a sink).
- The ladder: PK → rung 1, PK-less with a unique index → rung 2, keyless →
  rung 3, asserted through counters; keyless UPDATE / DELETE with
  duplicates (both move); NULL-bearing keys in both join forms; the
  volatile guard's refusal on a keyless table and its absence on a keyed
  one; #140's reproduction end to end.
- `INSERT … SELECT` both remote: the rows never reach the client (counter +
  `mssql_pool_stats` bytes), identity bracket when the list names the
  column; `INSERT INTO local SELECT remote` pushed by the rewriter alone.
- The agreement pattern from 079 W5, with its 2026-09-23 harness: the
  setting is read once at ATTACH (079 D6), so a `.test` file cannot `SET` it
  between two runs — the suite attaches the **same DSN twice**, once with the
  ATTACH option `remote_pushdown false` (`mssql_storage.cpp`), and runs
  every pushed DML shape through both aliases, comparing the table state and
  the `Count` after. Exempt by design, with the expectation stated instead of
  compared: a pushed INSERT whose column list omits a server DEFAULT (vetoed,
  so there is nothing to compare) and a rung-3 statement over duplicate rows
  (both rows move — documented semantics, not agreement).
- The `Count` column: a pushed UPDATE's result has one BIGINT column named
  `Count` with the server's affected rows, identical to the shipped path's;
  and one client-visible count path asserted (the statement type is now
  `SELECT`, so the test pins what actually changed rather than asserting it
  did not).
- Exactly once: a pushed UPDATE under `SET threads = 4` affects its rows
  **once**; a `PREPARE`d pushed DML executed twice affects them twice.
- Rung 3's hazards: a keyless UPDATE whose SET produces values that collide
  with another stage batch's key (the `(1,1)`/`(2,2)`, `SET a = a + 1` shape
  of D3, forced past `mssql_dml_stage_threshold` and past the ~100k batch
  size) affects each row once; a keyless table carrying an `xml` column and
  one carrying a `geometry` column — refused by name when the remaining
  columns do not distinguish the rows, staged normally when they do; a
  keyless UPDATE whose SET reads a LOB column that the cost exclusion would
  have dropped (the column stays in the key, the result is deterministic).
- Both join forms: the NULL-bearing key cases run against a server that takes
  `IS NOT DISTINCT FROM` and against one that gets the INTERSECT form (forced
  by overriding the cached probe answer in a debug build).
- After the `EXECUTE_STATEMENT` claim, `DROP TABLE` / `ALTER TABLE` /
  `CREATE SCHEMA` / `CREATE VIEW` against the catalog still take the shipped
  path and behave exactly as before.
- `mssql_dml_pushdown = false`: the pushed shapes take D3's path with
  `mssql_remote_pushdown` still on, and the pushed SELECT counter still
  moves.
- RETURNING and the pseudo-column: `UPDATE … RETURNING` / `DELETE …
  RETURNING` on a keyless table refused by name at bind; `SELECT rowid FROM
  keyless` still refused; both unchanged on a keyed table.
- 065's acceptance 1 as a bench: `UPDATE t SET x = 1 WHERE <pushable>` on
  1M matching rows before / after on the wide fixture.
- `ATTACH … (READ_ONLY)`: a pushed UPDATE / DELETE / INSERT … SELECT / CTAS
  is refused with the shipped path's class and message (the catalog guard and
  the vehicle's bind each exercised, the second by reaching the vehicle
  directly); the same under `SET access_mode = read_only`; `EXPLAIN` and
  `PREPARE` of a pushed DML and of a pushed CTAS change no rows and create no
  table.
- Pool of one (`mssql_connection_limit = 1`, `mssql_acquire_timeout`
  short so a regression fails fast): UPDATE and DELETE on both the pushed
  and the fallback path, in autocommit and inside `BEGIN … COMMIT`,
  including a staged (rung 3) statement. Extends
  `test/sql/transaction/transaction_single_connection_pool.test` (#380),
  whose header names the gap.
- Rung 3 under load: a keyless DELETE of 200k rows with a concurrent reader
  scanning the table from another session — completes, no 1205; a rung-3
  statement whose predicate the scan does not push is refused by name.

### W6 — docs

README / website DML page ("what runs on the server", the ladder, the
volatile guard, the count), DATAMODEL (the DML flow diagram: rewriter →
one statement | plan → ladder → stage), CLAUDE.md DML line and the two new
settings (`mssql_dml_pushdown`, `mssql_dml_stage_threshold`), CHANGELOG.

One subsection of its own, next to 079's "result types change for a pushed
statement": **what a pushed DML changes for a client**. The result is the
same `Count` row, but the statement is a `SELECT` to DuckDB — `StatementType`,
`StatementProperties::return_type` (`QUERY_RESULT`, not `CHANGED_ROWS`) and an
empty `modified_databases`, so a binding that reads the statement type rather
than the result (`cursor.rowcount`, `executeUpdate()`) sees a query. Named
with `mssql_dml_pushdown` as the way back, and with the rung-3 duplicate
semantics (duplicates move together) in the same place.

### 2.7 — four sequential PRs

**Not one PR.** Spec 079 was revised away from exactly that plan — its § 2 now
ships five sequential PRs "because the whole is 7–8 thousand lines and the
size is the risk § 4 names" — and this spec is no smaller. Each is merged
before the next is opened (never stacked, never two open at once), and the
order follows the dependencies rather than the W numbers: W2's pool-of-one
conclusions constrain W3's connection handling, and W3's path is what a
vetoed pushed statement falls back to, so both land before the rewriter work
that can veto into them.

| PR | contents | stands on its own because |
|---|---|---|
| **1** | **W2** — the 066 remainder: `CollectSinkCatalogs` += `LOGICAL_UPDATE` / `LOGICAL_DELETE`, `defer_execution_` and its buffers gone, the transaction and pool-of-one suites | behind no setting and valuable alone: it fixes UPDATE and DELETE on a pool of one in autocommit and in a transaction — the last statements #380 left unable to run there — and retires the defer machinery |
| **2** | **W3 + W4** — the ladder (rungs 1–3, the non-comparable refusal, the probe, the staged delivery, `mssql_dml_stage_threshold`), the bind-time refusals moved, the `mssql_dml_use_prepared` deprecation and `EnsurePKLoaded`'s discovery error | **closes #140 by itself**, with no rewriter involved: a keyless table becomes writable on the shipped path |
| **3** | **W1** — the DML writer, the count vehicle, `mssql_dml_pushdown`, the read-only guards, UPDATE / DELETE / INSERT … SELECT pushed behind the setting; the agreement harness | the mechanism end to end on the three DML shapes, with PR 2's path underneath every veto |
| **4** | the CTAS shape (`EXECUTE_STATEMENT`, the describe type source, one server transaction) and **W6**'s docs | the one shape needing a round trip before it decides; droppable without touching PRs 1–3 |

**Spec 079 is already shipped through its PR E2** (`mssql_remote_pushdown`
defaults to **true**, `mssql_settings.cpp`: "On by default since PR E2"), so
this spec cannot borrow 079's "the setting stays off until the last PR"
safety: the rewriter is live on every attached catalog today, and PR 3 turns
pushed DML on for all of them the moment it merges. `mssql_dml_pushdown`
nonetheless ships **true** (owner, 2026-10-08) — a staged rollout was
considered and declined. **W5's suite is the gate instead**, and that places a
condition on PR 3 rather than on the setting: the agreement suite must be
green through both ATTACH aliases, on every shape in D1's table, with the
rung-3 hazards and the exactly-once assertions, **before PR 3 merges** — not
after, and not behind a flag nobody turns on. A setting that defaults to false
gets the suite it deserves, which is the argument for not having one. The
switch stays as the escape hatch § 4's first risk needs (a user who hits the
native-string divergence on a pushed DELETE), not as a rollout stage. PRs 1
and 2 change the shipped path only and are on from the moment they merge,
which is the point of putting them first.

## 3. Not proposed

- `%%physloc%%` as a row identifier (undocumented, rows move) — 067 § 5.
- A strict-string DML mode: the measured form (`DATALENGTH` pair, § 8.5)
  stays in the record; native is the decision.
- RETURNING through `OUTPUT` — a later D1 row; the shipped path keeps its
  RETURNING behaviour, and a statement with RETURNING is not pushed.
- Pushing a relaxed predicate for a DML (research § 2).
- A per-session pushdown switch, for the DML half or the read half — 079 D6's
  reason stands (`Supports` takes no `ClientContext`); `mssql_dml_pushdown`
  (D5) is instance-wide like its neighbour.
- Batching the rung-3 UPDATE behind a generation guard — D3 names the shape
  and the hazard it would have to close; one statement is the decision until a
  measurement makes it untenable.
- Reading the target's column types for a pushed CTAS from anywhere but the
  describe: the rewriter has no bound query, and `SELECT INTO` would let the
  server pick them (D1's **CTAS**).

## 4. Risks

- **A pushed DELETE removes the server's set** — the padded and
  case-variant rows a local `=` would not match. This is today's behaviour
  on the rowid path too; documented in one place with the SELECT rule, and
  `mssql_dml_pushdown = false` (D5) is the lever that does not also cost the
  read path.
- **A pushed DML is a SELECT to DuckDB** — `StatementType`, `return_type`
  (`QUERY_RESULT`) and an empty `modified_databases`; the `Count` row is
  unchanged but a binding keyed on the statement type is not (D1, W6). The
  peer of 079's "result types change for a pushed statement".
- **`INSERT … SELECT` and identity seed** — SQL Server's identity seed is
  not transactional (077); a rolled-back pushed insert still advances it,
  as a rolled-back statement insert does.
- **Rung 3 on a wide table** — the stage carries every column; LOB columns
  make the JOIN expensive (067 § 2's note): measured in W3, LOB-sized columns
  excluded from the key for cost when the rest is unique **and** the SET and
  WHERE do not read them (D3).
- **Rung 3 on a table with a non-comparable column** — `xml`, `geometry`,
  `sql_variant`, `hierarchyid`, `text`/`ntext`/`image` cannot be in the key
  at all, so a keyless table whose other columns do not distinguish its rows
  has **no** rung 3 and is refused by name (D3). That is a real table shape
  this spec does not make writable; the fix for the user is a unique index.
- **The join-form probe** — read at ATTACH from `SERVERPROPERTY`, so a
  server that hides those properties, or an edition this text does not know,
  silently takes the INTERSECT form. Correct everywhere, slower on 2022+;
  the cost is a non-SARGable JOIN either way, so the blast radius is
  performance, not results.
- **The remainder's blast radius** — `CollectSinkCatalogs` is on the
  planner's path for every DML; the transaction suite is the guard.
- **Rung 3 locks** — a JOIN that cannot seek scans the target per batch and
  escalates toward a table X lock: concurrent readers block for the batch;
  the stage-fully-then-join order (D3) keeps the statement's own scan out of
  the cycle, and RCSI on the database is the user's lever for readers.

## 5. Acceptance

1. `UPDATE t SET x = 1 WHERE <pushable>` on 1M matching rows: no scan round
   trip, the counter shows the rewriter, wall time collapses to the
   server's statement time (before / after on the wide fixture).
2. #140's reproduction passes on all three rungs; the unpushable keyless
   statement with a volatile function is refused by name.
3. **The count a pushed statement reports equals the count the shipped path
   reports on the same data** (the pushed count *is* the server's DONE-token
   count, so comparing it with itself proves nothing); the column is named
   `Count` and is BIGINT on both paths. The one documented difference is
   stated rather than compared: on rung 3 duplicate rows move together, so
   the count can exceed the number of rows DuckDB's local plan selected. The
   full DML suite is green through both ATTACH aliases of W5's harness.
4. Inside a transaction no DML defers: the connection is Idle after each
   statement, the transaction suite proves it.
   On a pool of one connection, UPDATE and DELETE run in autocommit and in a
   transaction, on both paths (#380 left them the last statements that could
   not).
5. The token loop exists once; `mssql_dml_use_prepared` is a **registered
   no-op** with its CHANGELOG deprecation line and removal scheduled for the
   following release (W4 — unregistering it now would make
   `SET mssql_dml_use_prepared = …` throw and kill the rest of a
   `.duckdbrc`).
6. A pushed DML executes exactly once per execution (`threads = 4`) and twice
   for two `EXECUTE`s of one `PREPARE`; `EXPLAIN` and `PREPARE` of a pushed
   DML or CTAS change nothing and create nothing.
7. A read-only catalog and a read-only database each refuse every pushed
   write with the shipped path's message, by both guards.
8. `mssql_dml_pushdown = false` leaves pushed SELECTs pushed and sends every
   DML down D3's path.
