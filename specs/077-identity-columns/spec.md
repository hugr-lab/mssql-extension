# Spec 077 — IDENTITY columns, and a rowid that does not need a primary key

**Status:** in progress on `spec/077-identity-columns` from `main` `b84e259`
(spec 062 merged as #348 on 2026-09-16). Reconnaissance done on 2026-09-16
against the local docker server, SQL Server 2025 RTM-CU8. Spec and
implementation in one PR.
**Builds on:** spec 062 — `MSSQLColumnInfo::is_identity` already reaches
`MSSQLInsertColumn` from `sys.columns` through the metadata cache (W4), and
`MSSQLStatementConnection` already gives every DML statement one connection of
its own (W1c), which is what makes session state such as `IDENTITY_INSERT`
scopable.
**Scope, set by the owner on 2026-09-16 and deliberately narrow:** the INSERT
path gets no special machinery. It does one thing — when a statement supplies a
value for an identity column, permit it the way T-SQL permits it, with
`IDENTITY_INSERT`. Nothing is hidden from DuckDB, no column list is rewritten,
and the bulk path is not taught about identity. **Loading many rows fast is
COPY's job**, and COPY already does it (§ 0.5).
**Closes:** [#327](https://github.com/hugr-lab/mssql-extension/issues/327), as
**won't-fix with the reason recorded** (W5), not as a feature. The count check
that rejects an `INSERT` without a column list belongs to DuckDB's binder and
runs before any extension code; there is no seam. W5b is the register of every
place this spec refuses something, and what each refusal has to say.
**Out of scope, by decision:** a patch to DuckDB's insert binder. We do not
carry or propose an upstream change for this.
**Out of scope, by decision:** SQL_VARIANT and the native `json` type
(researched alongside this, recorded in § 8, implementation is its own spec);
UPDATE/DELETE through a `#temp` table (spec 065/066); sequences and
`DEFAULT NEXT VALUE FOR`, which raise the same implicit-column-list question
and can reuse W2's machinery once it exists.

## 0. Measured ground

Everything in this section was run against the server, not read from
documentation. The probe scripts live in the session scratchpad; the outcomes
are quoted verbatim because three of them decide the design.

### 0.1 What IDENTITY actually guarantees

| Claim | Outcome |
|---|---|
| Two identity columns in one table | **Refused**, error 2744 "Multiple identity columns specified for table 'IdTwo'. Only one identity column per table is allowed." |
| `INT IDENTITY(1,1) NULL` | **Refused**, error 8147 "Could not create IDENTITY attribute on nullable column 'a'" |
| `sys.columns.is_nullable` for an identity column | always 0 |
| Values unique without a unique index | **No.** `SET IDENTITY_INSERT` re-inserted an existing value; `DBCC CHECKIDENT(..., RESEED, 0)` then a plain INSERT produced a third copy. Three rows with `a = 1`. |
| Base type | `DECIMAL(38,0)`, `NUMERIC(20,0)` and `TINYINT` all create as IDENTITY. Not always BIGINT-shaped. |
| Step | `IDENTITY(0,-1)` yields 0, −1, −2. Descending and negative are legal. |
| An index appears automatically | No. The probe table stayed a HEAP. |
| A VIEW's column | `sys.columns` reports `is_identity = 1` for the inherited column. |

**So identity is a default-value generator, not a constraint.** It says
"NOT NULL" and "the server fills this in", and nothing about uniqueness. Any
design that treats it as a row identity is wrong on a table where nobody
declared a key.

### 0.2 What the server allows when a value IS supplied

| Form | Outcome |
|---|---|
| No column list, values for the non-identity columns only | OK, server assigns |
| No column list, a value for every column, `IDENTITY_INSERT OFF` | error 8101 |
| No column list, a value for every column, `IDENTITY_INSERT ON` | **error 8101 again** |
| Column list naming the identity, `IDENTITY_INSERT ON` | OK |
| Column list naming the identity, `IDENTITY_INSERT OFF` | error 544 |
| `SET IDENTITY_INSERT` for a second table while one is ON | error 8107 |

Error 8101 states the rule outright: "An explicit value for the identity
column in table 'dbo.IdIns' can only be specified **when a column list is
used** and IDENTITY_INSERT is ON."

This is the fact that makes the whole feature tractable: **the extension never
emits a bare `INSERT INTO t VALUES (…)`.** `MSSQLCatalog::PlanInsert` always
builds an explicit column list from `insert_col_indices`. So the form the
server refuses is one we cannot produce, and the only question left is which
columns go into the list we do produce, and whether `IDENTITY_INSERT` brackets
it.

`IDENTITY_INSERT` is session state and one table at a time. Since spec 062
W1c a DML statement owns its connection, so the bracket has a natural scope.

### 0.3 `IDENTITY_INSERT` is session state, but it is guarded by a table permission

Two things about the mechanics, both measured, because W2 rests on them and
neither is obvious from the name of the statement.

**It persists across batches on the same connection, which is exactly the
shape W2 needs.** `SET IDENTITY_INSERT dbo.IdPerm ON` in one batch, the INSERT
in the next, `OFF` in a third: the middle INSERT lands and an INSERT after the
`OFF` fails with 544. It also reaches into a nested `sp_executesql`. What does
**not** work is the reverse nesting — a `SET IDENTITY_INSERT` issued *inside*
`EXEC(…)` is restored when that nested batch ends, like any other SET option,
so it must be sent as a batch of its own and never wrapped.

**It checks ALTER on the table, not INSERT.** As a user holding only SELECT
and INSERT:

```
Msg 1088: Cannot find the object "dbo.IdPerm" because it does not exist or you
do not have permissions.
```

After `GRANT ALTER ON dbo.IdPerm` the same statement succeeds. So a setting
that changes nothing but the session is nevertheless gated on a schema-level
privilege — turning it on lets the caller write a column the schema says the
server owns. The wording of 1088 is the problem: a user who can read and
insert into the table is told the table may not exist. W2 owes them a better
message.

### 0.4 What the extension does today

| Statement against `dbo.IdNow (rid BIGINT IDENTITY PRIMARY KEY, a INT, b VARCHAR(20))` | Today |
|---|---|
| `INSERT INTO t.dbo.IdNow (a, b) VALUES (1, 'x')` | works, server assigns `rid` |
| `INSERT INTO t.dbo.IdNow (rid, a, b) VALUES (10, 1, 'x')` | `IO Error … [544] Cannot insert explicit value for identity column … when IDENTITY_INSERT is set to OFF` |
| `INSERT INTO t.dbo.IdNow VALUES (11, 2, 'y')` (positional, every column) | the same 544 |
| `INSERT INTO t.dbo.IdNow VALUES (3, 'z')` (positional, one short) | `Binder Error: table "IdNow" has 3 columns but 2 values were supplied` — issue #327 |
| `SELECT rowid FROM` a table keyed by `BIGINT IDENTITY UNIQUE`, no PK | `Referenced column "rowid" not found` |
| `UPDATE` the same table | `MSSQL: UPDATE/DELETE requires a table with a primary key.` |

The first row already works because `PlanInsert` drops the columns the INSERT
did not name — it reads the `op.bound_defaults[i] == nullptr` tell that DuckDB
2.0 leaves behind — so the server applies its own identity and defaults.

### 0.5 What COPY does today, and what INSERT BULK does with an identity

`COPY … TO 'mssql://t/dbo/CopyIdent' (FORMAT 'bcp', CREATE_TABLE false)`
against `CopyIdent (rid BIGINT IDENTITY PRIMARY KEY, a INT, b VARCHAR(20))`:

| Source | Result |
|---|---|
| `SELECT 1 AS a, 'x' AS b` | rows land, server assigns `rid` 1 and 2 |
| `SELECT 500 AS rid, 3 AS a, 'z' AS b` | **`rid` 500 lands**, no hint sent, no error |

Two things follow. COPY already resolves target columns **by name**
(`TargetResolver::BuildColumnMapping`, case-insensitive), so it already
implements "is the identity column among the source columns" without knowing
what an identity column is. And `INSERT BULK` **keeps a listed identity value
by default** — no `KEEP_IDENTITY` hint was sent and the value was honoured. A
bulk load that omits the column gets server-assigned values; one that lists it
keeps what it sends.

That second fact settles a spec 062 question rather than reopening it. An
INSERT naming the identity column was routed to the statement path on the
grounds that "INSERT BULK keeps the value unconditionally". It does, and that
turns out to be the *right* behaviour for a bulk load, which is why COPY needs
no work here at all. The INSERT routing stays as it is, on purpose (W3).

## 1. What follows

1. **rowid must stop meaning "primary key".** It should mean "a key this
   server guarantees to address one row": a PK, or failing that a unique index
   that is neither filtered nor nullable. Identity qualifies on those terms and
   on no others, which is exactly how it should be treated.
2. **The INSERT column list is already the right shape.** The only thing
   missing is the `IDENTITY_INSERT` bracket for the case where the identity
   column is in it. That is W2, and it is the whole of the INSERT work.
3. **Being one value short cannot be fixed where the bug is.**
   `Binder::BindInsert` compares the value count against
   `table.GetColumns().PhysicalColumnCount()`, and `ColumnList` keeps out of
   `physical_columns` only columns whose `TableColumnType` is `GENERATED` —
   the enum has exactly two values. Marking the identity column GENERATED is
   not a way round it: a reference to a generated column is resolved through
   its expression instead of the scan, so reading the column would break. And
   the extension gets no say, because the binder rejects the statement before
   any extension code runs. With an upstream change ruled out, the only lever
   left would be hiding the column from DuckDB, and that is not worth its
   price — see W5.
4. **COPY needs no new rule**, only a name for the rule it already follows,
   tests, and documentation.

## 2. Work items

### W1 — rowid from a primary key **or** a usable unique index

`PK_DISCOVERY_SQL_TEMPLATE` (`src/catalog/mssql_primary_key.cpp`) filters
`kc.type = 'PK'` and nothing else. Widen it to consider every index on the
object and pick one deterministically.

**Usable** means all of:

| Condition | Read from | Why |
|---|---|---|
| `is_unique = 1` | `sys.indexes` | otherwise values repeat |
| `has_filter = 0` | `sys.indexes` | a filtered unique index covers a subset of rows; the rest are unaddressable |
| every key column `is_nullable = 0` | `sys.index_columns` joined to `sys.columns`, `is_included_column = 0` | measured: a unique index on a nullable column accepts exactly one NULL row and refuses the second, so NULL is neither unique nor addressable |
| `is_disabled = 0`, `is_hypothetical = 0` | `sys.indexes` | a disabled index enforces nothing |

**Choice, in order** (deterministic, so two sessions never disagree):

1. the primary key, if there is one — unchanged behaviour for every table that
   has one;
2. else the usable unique index with the fewest key columns;
3. else the one whose key is narrowest in declared bytes;
4. else, among equals, the one whose single key column `is_identity` — an
   identity value is never updated by the application, so a rowid built on it
   cannot move under an UPDATE;
5. else the lowest `index_id`.

`PrimaryKeyInfo` grows a `source` (`PRIMARY_KEY` / `UNIQUE_INDEX`) and the
index name, because every error message that currently says "has no primary
key" has to say what it looked for and why each candidate was rejected. The
rename of the struct itself (`RowIdKeyInfo`) is part of this item; it is a
mechanical rename in one commit with no test changed, so it stays readable in
review.

Two notes that are not new but become visible here. The key must be **stable
for the duration of the statement**: an UPDATE that assigns to the very column
the rowid is built from is already a hazard on the PK path, and picking a
user-maintained unique key widens it. W6 adds the test that pins today's
behaviour so the hazard is documented rather than discovered. And the
discovery query rides in the same batch as the table's metadata since spec
076, so widening it costs no extra round trip.

### W2 — `IDENTITY_INSERT` when the identity column is in the list

The rule, stated in terms of what DuckDB hands us:

| The columns DuckDB supplies | What we emit |
|---|---|
| the identity column is not among them | the list without it — today's behaviour, server assigns |
| the identity column is among them | the list with it, bracketed by `SET IDENTITY_INSERT <target> ON` / `OFF` |

Both the explicit form `INSERT INTO t (rid, a, b) VALUES (…)` and the
positional form with a value for every column land in the second row, which is
what "если их количество одинаковое" means once DuckDB has resolved it: the
positional form fills `column_index_map` for every column, so by the time
`PlanInsert` runs the two are indistinguishable, and should be.

Mechanics:

- `ON` is sent on the statement's own connection before the first batch, `OFF`
  after the last, **and on every failure path before the connection is
  released**. It is session state, and `mssql_reset_connection = false` means
  the pool will not clear it for us — a leaked `ON` would make the next
  statement on that connection silently accept identity values, and a second
  target would fail with 8107.
- Inside an explicit transaction the connection is pinned and shared, so the
  same discipline applies to the pinned connection, and the `OFF` must not wait
  for COMMIT.
- The target of `SET IDENTITY_INSERT` is a **table**. A VIEW target is left
  alone: the server refuses an explicit identity value through a view on its
  own terms, and that error is the honest one to surface.
- The `ON` and `OFF` go as **batches of their own**, never wrapped in `EXEC`:
  § 0.3 measured that a SET inside a nested batch is restored when that batch
  ends, so a wrapped one would silently do nothing and the INSERT would fail
  with 544.
- `SET IDENTITY_INSERT` checks **ALTER on the table** (§ 0.3), and a caller
  without it gets error 1088, which says the object "does not exist or you do
  not have permissions". That message is actively misleading for someone who
  can read and insert into the table, and it is a statement the user did not
  write. So this failure is caught and re-reported: what was attempted, on
  which table, and that `SET IDENTITY_INSERT` needs ALTER while INSERT does
  not. The server's 1088 goes in the message as the cause.

### W3 — the bulk path stays as spec 062 left it

Deliberately nothing. Spec 062 routes an INSERT naming the identity column to
the statement path (`statement_path_reason = "explicit identity column
'<name>'"`), and that stays. § 0.5 shows the bulk wire would keep the value
correctly, so lifting the gate is *possible*; it is not *wanted*. An INSERT
that names identity values is a hand-written statement, its rows are counted in
tens, and the statement path with W2's bracket is the exact T-SQL a person
would have written themselves.

**Loading many rows with their identity values is COPY**, which needs no new
code at all (W4) and is already the fast path: spec 062 § 6.8 measured 1M rows
at 74.71 s through statements against 0.51 s through the bulk wire. The
documentation says this in those words, so a user who hits the statement path's
speed has somewhere to go.

The cost of not doing it is one line in the docs instead of a routing change, a
hint decision, a NULL-on-the-bulk-wire measurement and a parallel-writer
question. That is the trade, taken on purpose.

### W4 — COPY into an existing table

No behaviour change; formalise what § 0.5 measured.

- A source column matching the identity column by name supplies the value and
  it is kept. A source that omits it gets server-assigned values.
- No warning. COPY keeps bcp semantics deliberately — the same reason
  `CHECK_CONSTRAINTS` is off by default there — and silently honouring a
  supplied identity is bcp's behaviour.
- `CREATE_TABLE true` and CTAS create a table with no identity column, so
  there is nothing to decide on that path.
- Documented in `website/docs/writing/` next to the COPY options, and tested
  both ways.

### W5 — the implicit column list: #327 is closed as won't-fix, with the reason

`INSERT INTO t VALUES (1, 'x')` against a table with an identity column keeps
failing with `Binder Error: table "t" has 3 columns but 2 values were
supplied`. Nothing in this spec changes that, and the issue stays open.

The reason is in § 1.3: the binder rejects the statement before any extension
code runs, and the count it checks comes from what the catalog reports as
columns. The only lever the extension has is to stop reporting the identity
column at all, re-exposing it as a named virtual column the way `rowid` works.
That was specified here and **rejected**, because of what it costs:

| | if identity columns were hidden |
|---|---|
| `SELECT *`, `DESCRIBE`, `duckdb_columns()` | the identity column is not there |
| `CREATE TABLE … AS SELECT * FROM t` | the copy has no such column |
| `INSERT INTO t (rid, …)` | fails at bind, the name does not resolve |
| W2's `IDENTITY_INSERT` path | unreachable for that table |

The last two rows are the decisive ones: hiding does not even give #327 what it
asked for. The issue wants the column out of the *implicit* list while an
explicit insert into it still reaches the server. A hidden column cannot do
that, there is no way to name it. So the choice would have been between the two
behaviours, per session, for every table — and the first two rows say the price
is paid by everyone with an `OrderID INT IDENTITY PRIMARY KEY`, which is the
most common shape in a SQL Server schema.

**The issue does not stay open.** Leaving it open implies someone will get to
it, and nobody will, because there is nowhere to get to it from. It is closed
when this ships, with a comment that says three things and no more: the count
check belongs to DuckDB's binder and runs before any extension code, so the
extension cannot widen it; the workaround is to name the columns, which is what
SQL Server's own error 8101 demands anyway, so a generated-SQL layer that names
them is portable rather than merely working around us; and for the DuckLake
shape specifically the answer is COPY, which takes its column list from the
source by name, never asks DuckDB to bind an INSERT, and is the fast path as
well. The comment also records what was considered and rejected, so reopening
starts from the cost table above rather than from scratch.

Rejected alternatives, recorded so they are not re-proposed: marking the
column GENERATED (breaks reads, see § 1.3); hiding the column, per the table
above; emulating the count comparison in the extension (unreachable).

### W5b — the refusals this spec owns

Where the extension cannot do what was asked, it has to say so at the layer
that knows why. Four places, and the message shape for each. None of them is a
generic wrapper; each names the thing the user can change.

| Situation | Where it is caught | What the message must carry |
|---|---|---|
| No usable key for `rowid`, so UPDATE or DELETE cannot bind | `GetRowIdColumns` / `GetRowIdType`, the same place that throws the "no primary key" error today | that it looked for a primary key **and** for a unique index; for each unique index it rejected, the index name and the reason (filtered, or which key column is nullable); and that adding a `PRIMARY KEY` or a non-filtered `UNIQUE` on NOT NULL columns is the fix |
| `SET IDENTITY_INSERT` refused for want of ALTER, server error 1088 | around the `ON` batch in the statement path | that the extension issued `SET IDENTITY_INSERT` because the statement supplied a value for the identity column; that this needs ALTER on the table while INSERT does not; the table name; and the server's 1088 as the cause. Never the bare 1088, which claims the table may not exist |
| An INSERT with no column list against a table with an identity column | nowhere — DuckDB's binder, before us | nothing we can add (§ 1.3). This is why the documentation carries the worked example, and why #327 closes with the reason rather than staying open |
| A second `SET IDENTITY_INSERT` target while one is on, server error 8107 | same place as the 1088 case | it cannot happen for one statement, so if it is ever seen it means a previous statement leaked its `ON`. The message says that outright, because it is a bug in us and the user needs to know it is not theirs |

The first two are the ones a user will actually meet. The rule for both: the
message names the statement the extension generated, not just the one the user
wrote, because the user did not write ours and cannot debug what they cannot
see.

### W6 — tests

New file `test/sql/insert/insert_identity.test`:

- explicit list without the identity column — server assigns, values ascend;
- explicit list with it — lands verbatim, and a second statement on the same
  connection afterwards is not silently in `IDENTITY_INSERT` mode (the leak
  test: run it with `SET mssql_reset_connection = false`);
- positional with every column — same as the explicit list;
- a failing statement in the middle — `IDENTITY_INSERT` is off afterwards;
- inside an explicit transaction, then ROLLBACK;
- a view whose base table has an identity column;
- more rows than `mssql_insert_bcp_threshold` while naming the identity column
  — it stays on the statement path (W3) and now succeeds instead of failing
  with 544, which is the assertion that pins the routing decision;
- `DECIMAL(38,0) IDENTITY` and `TINYINT IDENTITY` targets, because the type is
  not always BIGINT;
- `IDENTITY(0,-1)`, because the values descend.

New file `test/sql/rowid/rowid_unique_index.test`:

- `BIGINT IDENTITY UNIQUE`, no PK — `rowid` resolves, UPDATE and DELETE work;
- a unique index on a **nullable** column — refused, and the message says why;
- a **filtered** unique index — refused, and the message says why;
- both a PK and a unique index — the PK wins;
- two usable unique indexes — the documented tie-break, asserted so the choice
  cannot drift;
- a table with neither — the message names both things it looked for, and for
  each unique index it rejected, says which one and why (W5b);
- an UPDATE that assigns to the rowid key column — pins today's behaviour.

`test/sql/copy/copy_identity.test` for W4, both directions.

C++ unit tests for the candidate-choice function, which is pure given the
index metadata and is where the tie-break belongs.

### W7 — documentation

`CLAUDE.md` (the rowid line and the DML line), `DATAMODEL.md` (the catalog
layer's key discovery, whose diagram says "primary key"),
`website/docs/writing/dml.md` and `CHANGELOG.md`. The README's rowid sentence
needs the same widening.

Two things the documentation must say in so many words, because they are the
whole user-facing shape of this spec. An `INSERT` that supplies identity values
works and is bracketed for you, **and stays on the statement path**, so it is
for tens of rows, not millions. Loading many rows with their identity values is
`COPY`, which takes the column list from the source by name: include a column
named like the identity column and its values are kept, omit it and the server
assigns. The numbers from spec 062 § 6.8 belong next to that sentence, so the
choice is obvious without measuring anything.

## 3. Surface

**No new settings, no new functions, no new ATTACH options, no wire-format
change.** Everything here is either a widened metadata query (W1) or two extra
batches on a path that fails outright today (W2). That is the point of the
narrow scope: nothing new to configure, and nothing that changes for a user who
does not supply identity values.

## 4. Risks

- **rowid changing under existing users.** A table that has a PK is
  unaffected; a table that has only a unique index gains a rowid where it had
  none, which turns a clean binder error into working UPDATE/DELETE. Nothing
  that worked stops working.
- **A leaked `IDENTITY_INSERT`.** The failure mode is silent acceptance of
  identity values on the next statement over the same pooled connection. This
  is why W6 has a leak test with `mssql_reset_connection = false`, where the
  pool will not paper over a mistake.
- **The tie-break drifting.** Two usable unique indexes with equal key counts
  and widths is not exotic. The rule is asserted in a test, not left to
  `sys.indexes` ordering.
- **A user reading "identity now works" as "identity inserts are fast".** They
  are not, they are the statement path by design (W3). The documentation has to
  carry the COPY sentence, or this ships a performance surprise.

## 5. Decided, not left open

1. **The rowid key is cached with the table entry, exactly as the primary key
   is today.** It is read at bind time, so a round trip per statement would be
   a real cost on every UPDATE and DELETE; a stale answer here is a clean
   binder error rather than the hang a stale *shape* causes on the write path
   (which is why spec 062 W2 reads the shape live instead); and
   `mssql_invalidate_cache()` already exists for exactly this. Consequence to
   document, because it will be asked: a unique index added through
   `mssql_exec` does not make `rowid` appear until the cache is invalidated,
   and `mssql_exec_invalidate_cache` is `false` by default. Nothing about
   freshness changes from today.
2. **The permission case gets no automated test.** It needs a login, a grant
   and a session running as someone else, which the suite has no precedent for
   and which does not mix with a pooled connection. Verified by hand while
   writing this spec (§ 0.3); the spec is the record.

## 6. Not proposed

- **Hiding identity columns from DuckDB**, in any form, under any setting.
  W5 has the reasoning and the cost table.
- **Teaching the bulk path about identity.** W3. COPY is the fast path and it
  already works.
- Making identity columns read-only in UPDATE. SQL Server refuses that itself,
  with a clear message.
- Reading `IDENT_CURRENT` / seed / increment into the catalog. Nothing in the
  extension needs them, and they cost a round trip.
- `SET IDENTITY_INSERT` for COPY. The bulk wire keeps the value without it
  (§ 0.5), so the statement would be noise.

## 7. Bench

None. W1 adds predicates to a metadata query that already rides in an existing
batch. W2 adds two short batches to an INSERT that fails outright today, so
there is no "before" to compare against. Nothing on any hot path moves.

## 8. Researched alongside, not in scope: SQL_VARIANT and `json`

Recorded here so the follow-up spec starts from measurements rather than
repeating them.

- `sys.columns` describes a `sql_variant` column as `max_length` 8016 with no
  collation. The type lives **per value**; `SQL_VARIANT_PROPERTY` reads back
  base type, precision, scale, max length and collation. Verified round-trip
  for int, bigint, bit, decimal, float, money, varchar, nvarchar, varbinary,
  uniqueidentifier, date, time, datetime, datetime2 and datetimeoffset.
- It cannot hold `varchar(max)` / `nvarchar(max)`, `varbinary(max)`, `xml` or
  the native `json` type — all error 206 "Operand type clash" — and per the
  documentation also text/ntext/image, rowversion, the spatial types, CLR UDTs
  and sql_variant itself. **The payload cap is 8000 bytes**: `varbinary(8000)`
  and `nvarchar(4000)` both fit.
- On the wire it is TDS type `0x62` (`sp_describe_first_result_set` reports
  `tds_type_id` 98, `tds_length` 8009). The MS-TDS value envelope is a 4-byte
  length, a 1-byte base type, a 1-byte property-byte count, the properties
  (5-byte collation plus 2-byte max length for strings, precision and scale
  for decimal, scale for the datetime family), then the value. The arithmetic
  agrees: 8000 payload + 7 nvarchar property bytes + 2 header bytes = 8009.
- **Today's read path loses data silently.** The column is `is_cast_required`,
  so `table_scan.cpp` rewrites it as `CAST(v AS NVARCHAR(MAX))`, which is
  style 0: a `datetime2(7)` holding 10:11:12.1234567 arrives as
  `Sep 16 2026 10:11AM`, a `time(3)` as `10:11AM`, and `varbinary 0x0102` as
  `ȁ`. `CONVERT(…, 126)` fixes the datetime family and then fails on
  varbinary ("The style 126 is not supported for conversions from varbinary to
  nvarchar"), so no single style covers a mixed column. A `CASE` over
  `SQL_VARIANT_PROPERTY(v,'BaseType')` is the cheap interim; decoding `0x62`
  natively is the real fix. This deserves its own issue whatever the spec
  does.
- **The target type already exists in our pin.** DuckDB 2.0 ships a native
  `VARIANT` (`LogicalTypeId::VARIANT = 109`, with a builder at
  `common/types/variant/variant_builder.hpp`), and its `VariantLogicalType`
  tags cover every SQL_VARIANT base type one for one, including DECIMAL, BLOB,
  UUID, the TIME/TIMESTAMP family and the TZ forms.
- **The reverse direction does not fit, and the decision is already taken.** A
  DuckDB VARIANT is unbounded and nests; SQL_VARIANT is 8000 bytes, flat, and
  refuses every MAX type. A large JSON or a nested VARIANT therefore has no
  `sql_variant` to go into. **The owner's call on 2026-09-16: error, never
  truncate, and document exactly what fits.** So the write path of the
  follow-up spec owes three things — a refusal that names the base type and the
  8000-byte cap and the value that overflowed, a documented table of what a
  DuckDB VARIANT may contain for a `sql_variant` target, and a named
  alternative in the same message: `nvarchar(max)`, `varbinary(max)` (which
  does exist, up to 2 GB, and which our binary codec already handles), or the
  native `json` type. The check is per value, not per column, because the
  column's declaration says nothing about what a row holds.
- **The native `json` type of SQL Server 2025 needs almost nothing.**
  `sys.types` names it `json` with `max_length` −1, and on the wire it arrives
  as **`varchar(max)` with `Latin1_General_100_BIN2_UTF8`** (`system_type_id`
  and `tds_type_id` both 167). The binary kernel already reads that. What is
  missing is only the catalog's type table: `json` is not in
  `IsKnownSQLServerType`, so a column of it is marked `is_cast_required` and
  gets a pointless `CAST` to NVARCHAR(MAX). Adding the name is a one-line
  change with a test, and it is version-gated — the local server is 2025, CI
  runs 2022, where the type does not exist.
