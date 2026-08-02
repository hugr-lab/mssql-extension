# Spec 062 — INSERT via BCP by default

**Status:** Draft
**Date:** 2026-08-01
**Depends on:** spec 024 (BulkLoadBCP), spec 027 (CTAS over BCP), spec 060 (target types + UTF-8 write wire)

---

## 1. The gap

Three paths write rows to SQL Server. Two of them use the BCP protocol and one
does not:

| path | wire |
| --- | --- |
| `COPY … (FORMAT 'bcp')` | `INSERT BULK` |
| `CREATE TABLE … AS SELECT` | `INSERT BULK` (`mssql_ctas_use_bcp`, default true) |
| `INSERT INTO mssql.dbo.t SELECT …` | batched `INSERT INTO … VALUES (…), (…)` text |

There is no BCP anywhere in `src/dml/insert/` — no `BCPRowEncoder`, no
`INSERT BULK`. `mssql_insert_statement.cpp` and `mssql_batch_builder.cpp` build
SQL text in batches of `mssql_insert_batch_size` (default 1000) under
`mssql_insert_max_sql_bytes` (default 8 MB).

Spec 027 measured BCP at **2-10x** batched INSERT for CTAS. The same data takes
the same two paths here, so the same ratio should apply — and a large
`INSERT INTO … SELECT` is not a rare shape. It is what a user writes when the
target already exists, which is the normal case after the first load.

The text path also pays costs BCP does not: every value is rendered as a T-SQL
literal and escaped, the statement is parsed and compiled server-side per batch,
and the 8 MB cap turns one logical insert into many round trips.

## 2. What forces the text path, and what only looks like it does

**`RETURNING` genuinely does.** `INSERT BULK` returns no rows. The extension
already implements `RETURNING` by rewriting to `OUTPUT INSERTED`
(`mssql_insert_use_returning_output`, default true), which is a text statement
by construction. This is the one case that must keep the existing path.

**Everything else is available.** The target exists, so its column metadata —
types, lengths, collations, and the UTF-8 retarget spec 060 added — comes from
`TargetResolver::GetExistingTableColumnMetadata`, which COPY calls on every
statement. Column mapping by name, IDENTITY/DEFAULT columns being omitted from
the column list, the length guard, NULL framing: all of it already exists and is
exercised.

**Transactions are the question to answer first.** COPY takes an exclusive
connection for the duration of the load and holds it in Executing state.
`INSERT` participates in a DuckDB transaction that may already have a pinned
connection and may be interleaved with other statements. Whether an `INSERT
BULK` can be issued on a pinned connection mid-transaction, and what happens on
rollback, has to be established before anything is built — this is the risk that
decides the shape of the feature, not a detail.

## 3. Deliverables

- **D1 A decision function.** `INSERT` uses BCP when: no `RETURNING`, the source
  is a scan rather than a `VALUES` list of a few rows, and the row count
  estimate is above a threshold. A three-row `INSERT … VALUES` should stay on
  the text path — `INSERT BULK` has fixed setup and would lose.
- **D2 The sink.** Reuse `BCPRowEncoder` and the COPY writer wholesale. The
  physical operator differs; the encoding does not.
- **D3 Transaction behaviour**, per § 2: established by experiment first, then
  implemented. Includes what happens when the same transaction has already
  written through the text path.
- **D4 `mssql_insert_use_bcp`**, defaulting to true once D3 says it is safe,
  mirroring `mssql_ctas_use_bcp` — and the same escape hatch for the same
  reason.
- **D5 Measurement.** The bench already has write families and a wide-row step;
  add an INSERT group beside the COPY one so the two are comparable in the same
  run, interleaved. Claiming 2-10x without measuring this path would be
  repeating spec 027's number rather than confirming it.

## 4. Acceptance criteria

1. `INSERT … RETURNING` is byte-identical to today.
2. A small `INSERT … VALUES` is byte-identical to today.
3. A large `INSERT INTO … SELECT` produces the same rows as the text path,
   compared row by row, including non-ASCII, NULLs, and every type family.
4. Rollback discards the rows, and a failed insert leaves no partial batch
   visible after rollback.
5. Measured against the text path in one interleaved run, with client CPU and
   wall clock, on the same fixture as the COPY families.

## 5. Risks

- **Transaction semantics** (§ 2) — the whole feature depends on the answer.
- **Error attribution.** The text path reports which statement failed and can
  point at a row; BCP fails at finalize, after the batch is sent. Spec 060 made
  the length guard fire client-side before sending, which helps, but a
  constraint violation still surfaces late and with less context. That is a
  real regression in diagnosability and needs a deliberate answer.
- **IDENTITY and computed columns** already work in COPY (issue #125) and the
  same handling has to be reached here rather than reimplemented.
