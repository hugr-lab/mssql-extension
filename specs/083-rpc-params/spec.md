# Spec 083: parameters over RPC, and staging that allocates on demand

Status: in progress on `spec/083-rpc-params`, branched from #407
(`spec/081-shape-vehicle`); its PR targets that branch and merges into it
BEFORE #407 merges into main (the owner's call, 2026-10-06).

Origin: the mssql-ducklake side measured one DuckLake catalog load step by
step against postgres_scanner on the same DuckDB build: ~0.4-0.5 ms more per
catalog scan, whatever the row count (16 scans a load, ~154 loads to read a
table with 100 schema versions). This spec removes the two causes that are the
extension's own.

## Recon (2026-10-06)

Local docker SQL Server 2025, macOS arm64; the machine was shared with other
workloads, so wall times vary by about +-0.1 ms between runs. Server time comes
from `sys.dm_exec_sessions` deltas over 2000 scans in one transaction, so it is
not affected by that noise.

**Where a scan's time goes.** `sample` of a loop of empty catalog scans with a
pushed filter inside a transaction, main thread:
- 53-62% waiting on the socket (`recvfrom`);
- ~10% DuckDB's parse, 3-4% plan and optimize, ~3% the remote-pushdown rewriter;
- the extension's own code a few percent.

The extension sends the same number of requests per scan as postgres_scanner:
one. `BEGIN` goes lazily, once per transaction; there is no describe, SET, ping
or statistics query on the scan path. So the difference is what the one request
costs.

**R1: the batch form of `sp_executesql` is most of the server's time.** A pushed
filter is sent today as an ad-hoc batch:

    DECLARE @p0 bigint = 42;
    EXEC sp_executesql N'SELECT ... WHERE [snap] <= @p0', N'@p0 bigint', @p0 = @p0

| form of the same scan | server CPU per call |
|---|---:|
| the batch above (today's default) | 0.26-0.29 ms |
| `EXEC sp_executesql ..., @p0 = 42` (no DECLARE) | 0.155 ms |
| a literal `SELECT ... WHERE [snap] <= 42` | 0.064-0.068 ms |
| the same call as an RPC request (prototype) | **0.061-0.069 ms** |

- The outer batch text changes with every value, so the server parses and
  compiles it on every call. Then it runs `sp_executesql`.
- An RPC request (TDS packet type 3, ProcID 10) has no outer batch. The server
  looks up the inner statement's cached plan and runs it, which costs what a
  literal costs while keeping the one-plan-per-shape property of spec 076.
- Client-side, wall per scan: batch 1.15-1.31 ms, RPC 0.83-1.09 ms (three
  alternating rounds).
- This also explains mssql-ducklake's literal-vs-parameter paradox. Literals win
  on a repeated value (no outer batch), and lose on a real load where values
  change (every literal statement compiles in full).

**R2: every scan allocates and zero-fills its staging buffers, even an empty
one.** `RowStager::Configure` sizes each string column's payload buffer for a
full chunk up front, with `vector::resize`, which zero-fills: up to 2 MB for a
bounded `nvarchar(n)` (`nvarchar(255)` is ~1 MB) and 64 KB for a MAX column.
- On a 10-string-column table, `Configure` was 4.9% of an empty scan (`bzero`
  4.3%).
- Prototype: no payload allocation at `Configure`; the buffer grows from 4 KB on
  the first value. Client CPU of an empty scan went 0.792 -> 0.716 ms; a 3-row
  scan was unchanged.

**Not causes:** TLS, which shows the same gap without encryption; the wire bytes
of a request and response; the server's per-session settings. SQL Server under
Rosetta (amd64 image on Apple Silicon) is slower per request than a native
postgres; that is the environment, not the extension.

**The prototype** (commit "recon: sp_executesql over RPC", behind
`MSSQL_RECON_RPC`):
- It encodes integer literals as `bigint` and `N'...'` as `nvarchar`, and falls
  back to the batch for anything else.
- The full suite passes with it on except `filter_pushdown_hugeint.test`, where
  a `decimal(38,0)` constant overflowed the prototype's `strtoll`. The real
  encoder must work from the declared type, never from the literal text.

## Decisions

- **D1: one RPC entry point in the TDS layer.**
  As built: a request is a `tds::Request` (batch text, or an RPC body from
  `tds::RpcRequestBuilder` with the batch text beside it for logs and errors),
  and `TdsConnection::ExecuteRequest` sends either through the one private
  `SendRequest` that `ExecuteBatch` uses too. It handles the same Idle -> Executing transition, the same transaction
  descriptor in ALL_HEADERS, RESET_CONNECTION on the first packet, and the same
  multi-packet send. The response is the same token stream, so the result
  stream and the simple-query reader consume it unchanged. RETURNSTATUS and
  DONEPROC are already handled, because a batch's `EXEC` produces them too.
  The prototype's sentinel-in-the-text is removed.
- **D2: parameters are typed values, not literal text.** `SqlParamSet` keeps,
  per parameter, the DuckDB `Value` and the declared SQL Server type.
  - The RPC encoder writes TYPE_INFO and the value from the declared type,
    reusing the codec families' BCP value encoders (spec 045 / 057). Those
    already write the same TDS wire forms for INSERT BULK.
  - Types: integers, bit, real/float, decimal/numeric (including the HUGEINT
    rank as `decimal(38,0)`), money, date/time/datetime2/datetimeoffset/
    datetime/smalldatetime, uniqueidentifier, char/varchar with the column's
    collation, nchar/nvarchar, binary/varbinary, and NULL of any of them.
  - As built, a declaration with no RPC encoding (a CLR type such as
    `geometry` / `geography` / `hierarchyid`, an alias type, `sql_variant`)
    or a value its declared type cannot take keeps the batch form for the
    whole call, decided before anything is sent. Refusing them, as first
    planned, would have broken calls that work today (`'@g geometry'`), and
    the two forms are the same call to the server: the declarations and the
    statement text are identical, only the carrier differs. Strings go as
    `nvarchar` and binary as `varbinary`, converted by the server to the
    declared type exactly as it converted the batch's `N'...'` / `0x...`
    literal; `datetime` / `smalldatetime` values go as `datetime2(7)`, `money`
    as `decimal(19,4)`.
- **D3: every `sp_executesql` call site moves.**
  - The catalog scan's pushed filters (`table_scan.cpp`).
  - `mssql_scan_params`, `mssql_scan_params_unsafe` and `mssql_exec_params`
    (`mssql_functions.cpp`).
  - The metadata queries (`mssql_metadata_cache.cpp`, `mssql_primary_key.cpp`,
    `mssql_statistics.cpp`, `target_resolver.cpp`).
  - `prepared := true`: each execution is an `sp_execute` RPC call (ProcID
    12), with the handle and the values as positional typed parameters.
    Measured: server CPU 304 -> 240 ms over 500 executions. `sp_prepare`
    stays a batch (`... SELECT @h`): it runs once per bind, and its RPC form
    returns the handle as a RETURNVALUE token, which the reader does not parse
    yet. That comes with OUTPUT parameters (D5).
  - `mssql_scan_parameterize_filters = false` keeps meaning "literals in the
    text".
- **D4: staging allocates on demand.** `ColumnStaging::Configure` reserves
  nothing for a Var column's payload. The first value grows it, starting at
  4 KB and doubling, so an empty result allocates no payload at all. Whether
  the first growth should jump straight to the declared bound (one allocation
  for a full chunk of a bounded column) is measured in the PR, not assumed.
  `payload_bounded` then means "reached its bound", so the arena's shrink rule
  keeps holding.
  - As built: `ColumnStaging::payload_bound` is the chunk's provable worst case
    (`max_value_bytes * 2048`, within the 2 MB budget, else 0), and
    `GrowPayload` grows from `STAGING_INITIAL_PAYLOAD_BYTES` (4 KB) by doubling,
    stopping at the bound. The first growth does not jump straight to the
    bound: a bounded column fills its worst case in at most ~9 doublings
    (4 KB -> 1 MB for `nvarchar(255)`), each a copy of what is staged so far,
    so the whole growth copies less than the bound once, once per stream; a
    result of a few rows -- the catalog scans this spec is about -- stays at
    4 KB instead of the bound.
- **D5: out of scope, enabled by D1/D2.** Table-valued parameters, OUTPUT
  parameters and a procedure's return value to the caller
  (`mssql_exec_params` returning rows) are later work. mssql-ducklake asked for
  them. The encoder and the token handling here are what they build on.

## Later (not in this spec)

- **String parameters as UTF-8.** On a connection the server granted
  `UTF8SUPPORT` (SQL Server 2019+), a string parameter could go as `varchar`
  (0xA7) with a UTF-8 collation and its bytes as they are, the way the BCP
  write path already sends UTF-8 columns (spec 060), instead of being transcoded
  to UTF-16.
  - The semantics would not change: the comparison runs on the DECLARED
    parameter type, and the server converts the argument to it through Unicode,
    exactly as it converts the nvarchar argument today.
  - The gain is small. `@stmt` and `@params` must stay `nvarchar` (sp_executesql
    takes nothing else), and they are most of the call; the values DuckLake
    sends are short.
  - It also adds a second inline/PLP boundary: 8000 bytes of UTF-8, up to 4
    bytes a character.
  - Worth it only if a measurement shows string parameter transfer to matter.

## Tests

- **Every parameter type of D2, over RPC.** A round trip through
  `mssql_scan_params` and a filter pushed on a column of that type, compared
  with today's results; NULL of each; the HUGEINT rank (the prototype's miss).
- **Inside a transaction:** RPC requests carry the descriptor (error 3989
  otherwise); a pool of one.
- **`prepared := true`** over RPC, including the handle's lifetime.
- **Server cost.** A test asserting through `sys.dm_exec_sessions` that 200
  pushed-filter scans cost the server less than a stated budget, so the batch
  form cannot come back unnoticed. The budget is loose enough for CI.
- **Staging.** An empty scan of a wide table allocates no payload, via the
  existing staging counters; the current shrink and grow tests.
- **Parameter collation (review of step 1).** A string parameter goes as
  `nvarchar` with Latin1_General_CI_AS bytes in its TYPE_INFO. Its conversion
  to a declared `varchar` should follow the database's collation (#361's code
  page), whatever those bytes say. To be confirmed against a database whose
  default is not code page 1252 before the PR leaves draft.

## Measured in the PR

- The table in R1 again, per call site: catalog scan, `mssql_scan_params`,
  metadata.
- mssql-ducklake's catalog-load step table, before and after, on their 300-table
  lake.
