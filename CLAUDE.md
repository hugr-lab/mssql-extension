# mssql-extension Development Guidelines

DuckDB extension for SQL Server via a custom TDS protocol implementation (no FreeTDS/ODBC).

## Technology

- **Language**: C++17 (DuckDB extension standard)
- **DuckDB**: `v2.0-cyanoptera`, DuckDB's v2.0 pre-release stabilisation branch (2.0 API track, spec 069). Tracked instead of `main` since the branch was cut on 2026-09-03: it is where the 2.0 line is being stabilised, and `main` has already diverged from it. The 1.5.x world lives on branch `duckdb-v1.5.5` for 0.2.x maintenance releases; main compiles against the pinned duckdb submodule SHA only (no dual-API shim since spec 069 retired `mssql_compat.hpp`)
- **TLS**: OpenSSL via vcpkg (statically linked, symbol visibility controlled)
- **Platforms**: Linux (GCC), macOS (Clang), Windows (MSVC, MinGW/Rtools 4.2)

## Project Structure

```text
src/
  azure/                # Azure AD authentication infrastructure
  catalog/              # DuckDB catalog integration + transaction manager
  codec/                # Per-type-family codec layer (spec 045): boolean/integer/
                        # float/decimal/money/string/binary/datetime/uuid. Each
                        # <family>_codec.cpp owns EncodeToBcp / DecodeFromTds /
                        # FormatSqlLiteral / FormatDdlTypeName for its types.
                        # literal_format.cpp + type_family.cpp are the dispatchers.
  connection/           # Connection pooling, provider, settings
  dml/                  # DML operations
    insert/             # INSERT (batched VALUES, OUTPUT INSERTED for RETURNING)
    update/             # UPDATE (rowid-based, VALUES JOIN pattern)
    delete/             # DELETE (rowid-based, VALUES JOIN pattern)
  include/              # Headers (mirrors src/ layout)
  query/                # Query execution and result streaming
  table_scan/           # Table scan with filter/projection pushdown
  tds/                  # TDS protocol implementation
    auth/               # Authentication strategies (SQL auth, FEDAUTH)
    encoding/           # Type encoding (datetime, decimal, GUID, UTF-16)
    tls/                # TLS via OpenSSL with custom BIO callbacks
test/
  sql/                  # SQLLogicTest files (require SQL Server for most)
    attach/             # ATTACH/DETACH tests
    azure/              # Azure AD authentication tests (no SQL Server required)
    catalog/            # Catalog, DDL, filter pushdown, statistics
    copy/               # COPY TO MSSQL (BulkLoadBCP) tests
    ctas/               # CREATE TABLE AS SELECT tests
    dml/                # UPDATE and DELETE tests
    insert/             # INSERT tests
    integration/        # Core integration (pool, TLS, large data)
    query/              # Query-level tests
    rowid/              # Rowid pseudo-column tests
    tds_connection/     # TDS protocol tests
    transaction/        # Transaction management tests
  cpp/                  # C++ unit tests (no SQL Server required)
docs/                   # Architecture documentation (see docs/architecture.md)
docker/                 # SQL Server container and Linux CI build
```

## Versioning

- The extension version is defined in `CMakeLists.txt` as `MSSQL_EXTENSION_VERSION` (e.g., `set(MSSQL_EXTENSION_VERSION "0.1.10")`)
- This is passed to C++ code via the `MSSQL_VERSION` compile definition and returned by `mssql_version()`
- **When releasing a new version**: update `MSSQL_EXTENSION_VERSION` in `CMakeLists.txt` and `version` in `vcpkg.json`

## Commands

```bash
# Build
make                    # Release build (with TLS via vcpkg)
make debug              # Debug build
make clean              # Remove build artifacts

# Test
make test               # Unit tests (no SQL Server required)
make docker-up          # Start SQL Server container (no .env needed; `cp .env.example .env` to override port/password)
make integration-test   # Integration tests (requires SQL Server)
make test-all           # All tests
make test-debug         # Tests with debug build

# Docker
make docker-up          # Start SQL Server test container
make docker-down        # Stop container
make docker-status      # Check container health

# Load extension in DuckDB CLI
./build/release/duckdb
# Or dynamically:
duckdb --unsigned -c "INSTALL mssql FROM local_build_debug; LOAD mssql;"
```

## Code Style

- C++17, follow DuckDB extension conventions
- Use clang-format (version 14): `find src -name '*.cpp' -o -name '*.hpp' | xargs clang-format -i`

## Naming Conventions

### Files
- Source files: `mssql_<component>.cpp` for extension code, `tds_<component>.cpp` for TDS protocol
- Headers mirror source layout in `src/include/`
- Test files: `test_<component>.cpp` (C++), `<feature>_<scenario>.test` (SQL)

### Namespace Prefix Rule

**Critical**: The `MSSQL` or `Tds` prefix depends on namespace placement:

| Namespace | Prefix | Rationale | Examples |
|-----------|--------|-----------|----------|
| `duckdb` (common) | **Required** (`MSSQL`, `Tds`) | Avoid name collisions in shared namespace | `MSSQLCatalog`, `MSSQLTransaction`, `TdsConnection`, `TdsSocket` |
| `duckdb::mssql` | **No prefix** | Already scoped by namespace | `ConnectionProvider`, `PoolStatistics`, `InsertConfig` |
| `duckdb::tds` | **No prefix** | Already scoped by namespace | `Connection`, `Socket`, `PacketType` |
| `duckdb::tds::tls` | **No prefix** | Already scoped by namespace | `TlsContext`, `TlsBio` |
| `duckdb::encoding` | **No prefix** | Already scoped by namespace | `TypeConverter`, `DatetimeEncoding` |

### Classes and Structs
- **PascalCase** always. Prefix per namespace rule above.
- Info/metadata structs: `PrimaryKeyInfo`, `ColumnInfo`, `TableMetadata`, `PoolStatistics`
- Config structs: `InsertConfig`, `DMLConfig`, `PoolConfig`

### Methods
- **PascalCase** (DuckDB convention): `GetConnection()`, `SetPinnedConnection()`, `ExecuteBatch()`
- Getters: `Get<Property>()` — Setters: `Set<Property>()`
- Boolean queries: `Is<State>()`, `Has<Property>()` (e.g., `IsAlive()`, `HasPinnedConnection()`)

### Variables
- **Member variables**: `snake_case` with trailing underscore: `connection_pool_`, `schema_mutex_`, `transaction_descriptor_[8]`
- **Local variables**: `snake_case` without underscore: `row_count`, `schema_name`
- **Constants**: `constexpr` with UPPER_SNAKE_CASE: `TDS_VERSION_7_4`, `TDS_HEADER_SIZE`, `DEFAULT_IDLE_TIMEOUT`

### Enums
- `enum class` with PascalCase values: `ConnectionState::Idle`, `PacketType::SQL_BATCH`, `MSSQLCacheState::LOADED`
- All-caps for protocol constants: `DONE_FINAL`, `ENCRYPT_ON`

### Namespaces
- `duckdb` — main extension entry points and DuckDB API overrides (prefixed)
- `duckdb::tds` — TDS protocol layer (no prefix)
- `duckdb::tds::tls` — TLS implementation (no prefix)
- `duckdb::mssql` — MSSQL-specific utilities (no prefix)
- `duckdb::encoding` — Type encoding/decoding (no prefix)

### Test Naming
- SQL test files: `# group: [sql]`, `[mssql]`, `[integration]`, `[transaction]`, `[dml]`
- SQL test context names: unique per file, prefixed by operation (e.g., `txtest`, `mssql_upd_scalar`)
- Test tables: PascalCase (`TestSimplePK`, `TxTestOrders`) or snake_case for simple ones (`tx_test`)

## Documentation

- **`DATAMODEL.md`** is the layered-architecture reference (TDS → connection pool → catalog → cache → codec) with Mermaid diagrams. **On every PR, consider whether `DATAMODEL.md` needs updating** — if the change adds/removes a layer component, alters an invariant (lifetime, cache invalidation, threading, ownership), or changes an end-to-end flow, update the relevant layer section (and its diagram) in the same PR.
- Keep architecture/internals diagrams in `DATAMODEL.md`; keep `README.md` user-facing (link into `DATAMODEL.md` rather than duplicating internals).
- Use GitHub-rendered ```mermaid``` fenced blocks for diagrams.

## Key Architecture Concepts

- **Custom TDS implementation**: No external TDS/ODBC library. TDS v7.4 protocol with PRELOGIN, LOGIN7, SQL_BATCH, ATTENTION packet types.
- **Connection pool**: Thread-safe pool, **owned per `MSSQLCatalog` (spec 047; since the issue #178 review a `shared_ptr` whose SOLE strong reference is the catalog — result streams hold `weak_ptr` handles for safe destructor-time release)** — pool lifetime is bounded by catalog lifetime; no singleton, no cross-instance sharing. Two ATTACHes against the same DSN under different aliases get independent pools; DETACH tears down the pool deterministically via RAII. Idle timeout, background cleanup, configurable limits as before.
- **Transaction support**: Connection pinning maps DuckDB transactions to SQL Server transactions via 8-byte ENVCHANGE descriptors. See `docs/transactions.md`.
- **Catalog integration**: DuckDB Catalog/Schema/Table APIs with incremental metadata cache (lazy loading, TTL-based expiration, point invalidation; **the shared cache holds committed state only** — inside an explicit transaction a miss loads on the pinned connection into the transaction's own `MSSQLTransactionMetadata`, the names the transaction changed are forgotten in the shared cache at COMMIT/ROLLBACK, and `mssql_refresh_cache` / `mssql_preload_catalog` are refused once the transaction has used any MSSQL catalog; issue #380; at COMMIT/ROLLBACK what the transaction loaded and did not change is published into the shared cache from memory, with no round trip (`PublishTransactionMetadata`; withheld after `mssql_exec` DDL, under READ UNCOMMITTED, for filter-hidden names, and entirely when the shared cache was invalidated while the transaction ran -- `GetInvalidationEpoch`, so any DDL through the catalog withholds it); inside a transaction the shared metadata cache is read into the transaction's own layer, and a miss there is never "does not exist"; issue #383, review of #386), rowid key discovery for rowid support — the primary key if it is usable, else a usable unique index (spec 077 W1: not filtered, not disabled, key columns NOT NULL and matchable by a literal; a `DATETIME` or `SQL_VARIANT` key is not, #358/#354, and such a table falls through to another unique index or refuses by name).
- **Read path (spec 075)**: `mssql_scan` describes at Bind and executes at InitGlobal; in a transaction every scan of a catalog materialises at init under the catalog's `MaterializeMutex`, and the planner also materialises the catalog scans of any catalog the plan **sinks into** (COPY's BulkLoadBCP, INSERT) — the sink and the scan feeding it share the ONE pinned connection. The per-table metadata queries (columns, PK, row count, table list) send the names as `sp_executesql` parameters, so one server plan serves every table (#334); since spec 076 the primary key rides in the same batch as the table's metadata, so a fresh table costs one round trip before its first row.
- **DML**: INSERT loads through `INSERT BULK` above `mssql_insert_bcp_threshold` rows (spec 062; staged in a `MSSQLStagedRows` buffer until the threshold decides, then the operator's `BulkLoadSession` on the pinned or a pool connection, parallel writers where locks allow) and as batched VALUES statements otherwise; UPDATE/DELETE use rowid-based VALUES JOIN pattern with deferred execution in transactions. **Every DML statement is atomic in autocommit** (spec 062 W1c/W2, #344): one connection per statement (`MSSQLStatementConnection`), its batches bracketed in a server transaction by `mssql::LoadTransaction`, which captures the ENVCHANGE transaction descriptor a batch-sent `BEGIN TRANSACTION` answers with (every later request must carry it — error 3989 otherwise) and clears it at COMMIT/ROLLBACK. **An INSERT that names the identity column** (spec 077 W2) is bracketed on that same connection with `SET IDENTITY_INSERT … ON` before its first batch and `OFF` on every way out — COMMIT, Fail, the destructor — and a connection on which OFF was not confirmed is discarded rather than pooled; it stays on the statement path (tens of rows — loading many rows with their identity values is COPY's job, which keeps a source column named like the identity column). A NULL in a named identity column is refused before anything is sent: DuckDB hands `DEFAULT` and `NULL` to the extension identically.
- **DDL**: `CREATE TABLE IF NOT EXISTS` silently succeeds when table exists; `CREATE OR REPLACE TABLE` drops and recreates. Auto-TABLOCK enabled for new table creation (CTAS/COPY TO). Catalog DDL and CTAS run on a pool connection and autocommit, outside an explicit transaction (spec 057) — except on a pool of ONE connection (issue #380), where a CTAS in a transaction runs whole on the pinned connection (checks, CREATE, rows as INSERT statements) and rolls back with it; on a pool of one in autocommit the optimizer materialises the scans of a catalog the plan sinks into (CTAS counts as a sink, like COPY and INSERT) as it does in a transaction, a raw `mssql_scan` / `mssql_scan_params` of it materialises at init the same way, and a COPY's / CTAS's bulk load keeps BCP but takes its connection on its FIRST chunk (`BulkLoadSession::DeferAdoption` / `AdoptDeferred`), after the source has given the connection back (review of #382: CTAS used to fall back to INSERT statements, and COPY and raw scans waited out the acquire timeout).
- **Filter pushdown**: DuckDB filter expressions translated to T-SQL WHERE clauses. Function mapping for common string/date/arithmetic operations. Since spec 076 the constants are `@pN` parameters of an `sp_executesql` batch, declared from the column's SQL Server type (`FilterEncoder::DeclarationForColumn`), and the statement text is one fixed string per filter shape; `mssql_scan_parameterize_filters = false` restores literals.
- **DuckDB API (2.0 track, spec 069)**: main targets duckdb main only — `mssql_compat.hpp` (spec 051) is retired; 1.5.x compatibility lives on branch `duckdb-v1.5.5`. The 2.0 contracts that bit during migration and MUST be honored by new code: producers filling vectors call `DataChunk::SetChildCardinality` (vectors carry their own size; the deprecated `SetCardinality` sets only the chunk count and every consumer keyed on `Vector::size()` sees an empty vector); scalar functions that throw at runtime declare `SetFallible()`; writes go through `FlatVector::GetDataMutable`/`ValidityMutable` (checked in release — hoist out of row loops; the per-value `DecodeFromTds` bodies use the `*Unsafe` forms, justified in `specs/069-duckdb-v2-migration/bench-results.md`); every TableFilter handed to a `filter_pushdown` scan must be applied by the scan (DuckDB does not re-check — refused filters run client-side via the net in `table_scan.cpp`); `Identifier` stays at the DuckDB API boundary, internal metadata/TDS/SQL text remain `std::string`.

## Windows Build Support

- **ssize_t**: `src/include/tds/tds_platform.hpp` provides Windows-compatible typedef
- **MSVC**: `x64-windows-static-release` vcpkg triplet
- **MinGW**: `x64-mingw-static` triplet (Rtools 4.2, not 4.3 due to linker bugs)
- **CI**: Trigger Windows builds via Actions -> CI -> Run workflow -> Check "Run Windows build jobs"

## Debug Environment Variables

| Variable | Description |
|----------|-------------|
| `MSSQL_DEBUG=1..3` | TDS protocol debug level (1=basic, 3=trace) |
| `MSSQL_DML_DEBUG=1` | DML operation debugging (generated SQL, batch sizes, rowid values) |
| `MSSQL_COUNTERS=1` | Per-stream and per-COPY performance counters **without** the logging that distorts them. Use this, not `MSSQL_DEBUG`, whenever a number is going to be quoted. `MSSQL_DEBUG>=1` still enables them for existing scripts, and says so next to the output — but its own logging runs inside the phases being timed: on read, level 2 `fprintf`s every token from inside the parse timer (`parse` measured 22 → 1133 ns/row and the socket wait collapsed, i.e. the numbers inverted); on write, the sink used to log per chunk and the logger flushes, inflating the client CPU it reported about 4×. `make counters-test` runs the SQL suite with them on, which is the only way the counter code path is exercised at all (issue #233 was a crash that lived there). |

## Extension Settings (SET in DuckDB)

| Setting | Default | Description |
|---------|---------|-------------|
| `mssql_connection_limit` | 64 | Max connections per context. **At 1** the scans and the sink of one statement have to take turns on the single connection, so the extension materialises rather than streams: the optimizer flags catalog scans of a catalog the plan sinks into (in autocommit as well as in a transaction), a raw `mssql_scan` materialises at InitGlobal whenever the limit is 1, and a CTAS loads with INSERT statements instead of a bulk load. The cost is that a raw scan at limit 1 buffers its whole result and loses early exit — `SELECT * FROM mssql_scan(db, 'SELECT * FROM huge') LIMIT 5` reads every row — where above 1 it streams (issue #380, reviews of #382). |
| `mssql_connection_timeout` | 30 | Connection timeout (seconds): the TCP dial **and** every login-phase read that follows (PRELOGIN, TLS, LOGIN7 / FEDAUTH responses), for ATTACH validation and for every pool refill. Until issue #302 it governed ATTACH validation only — the pool factories passed no timeout to `Connect` and the login reads had `DEFAULT_CONNECTION_TIMEOUT` spelled out, so a server that accepts the dial and hangs up (Azure SQL on an expired token) cost the compiled-in 30 s per attempt whatever this said. `0` (or less) means the 30 s default, **not** "no timeout" as for the metadata/query timeouts next to it: a dial or a login read that never completes must not hang a pool refill forever, and now that the setting reaches those reads a literal 0 would be `poll(0)`, an instant failure on every pooled connection (spec 073 review). |
| `mssql_idle_timeout` | 300 | Idle connection timeout (seconds) |
| `mssql_min_connections` | 0 | Connections opened at ATTACH and kept while idle (issue #324). Until #324 it only kept idle connections from being closed and never opened one. `ConnectionPool::Prewarm` opens them in `MSSQLCatalog::Initialize`, before the collation query, with one thread per login. ATTACH's eager validation IS the pool's first login (`MSSQLCatalog::ValidateThroughPool`), so the connection that proved the credentials stays in the pool and a plain ATTACH costs one login, not two. Measured locally: a plain ATTACH takes 0.20 s (0.40 s before) and `min_connections 4` takes 0.39 s. The slots are reserved first, so a concurrent `Acquire` cannot overshoot the limit. A failure does not fail the ATTACH, which the eager validation has already vouched for; a shortfall is logged as a WARNING. Skipped under `lazy_validation`, and the ATTACH option is refused beside it. Also an ATTACH option (`min_connections`), because DuckLake's `METADATA_PARAMETERS` forwards ATTACH options, not settings. |
| `mssql_acquire_timeout` | 30 | How long `Acquire` waits for a pooled connection to be **released** when the pool is at its limit. It is **not** how long a failed creation takes to report: since issue #302 a factory failure (expired Azure AD token, wrong password, refused dial) with nothing active fails at once and names the reason — `could not create a connection: Login failed for user 'sa'.` — and with others active the pool keeps waiting for a release but retries creation on a backoff (250 ms doubling to 4 s), not on every wakeup; with nothing active there is no backoff, every statement dials once and fails at once with its own reason. The reason is per call (`Acquire(timeout, &why)`), and a successful creation clears the pool's recorded `last_create_error`, so an exhaustion timeout on a healthy pool is reported as a timeout, never as a creation failure another thread hit hours earlier (spec 073 review). Before, a failed creation was treated as a full pool and reported as `(timeout)` after this many seconds, with the reason discarded. |
| `mssql_connection_cache` | true | Enable connection pooling |
| `mssql_reset_connection` | true | Reset a pooled connection's **session** before the next statement uses it (the TDS `RESET_CONNECTION` bit — what `sp_reset_connection` does). This is why a `##global` temp table does not survive between statements: it lives as long as its creating session, and the reset ends that session on the same physical connection (`@@SPID` unchanged). There is no selective form — one bit, two variants, and `RESET_CONNECTION_SKIP_TRAN` drops `##g` and `#loc` alike (issue #189). `false` is not "keep my temp tables" but "**I own this session's state**": `SET` options, session variables, `CONTEXT_INFO`, cursors — and an open transaction that then keeps its locks until that connection is used again. Honoured by every release path (autocommit release — which is what `mssql_exec` uses —, result-stream close, COMMIT, ROLLBACK); for the transaction paths it is captured at BEGIN, because `TransactionManager::RollbackTransaction` gets no `ClientContext`. Also honoured by `ReleaseBcpConnectionOnError`, the mid-bulk-load failure path: it has no `ClientContext` by design (issue #178, runs from destructors on worker threads), so the answer is carried to it — and its `reset_on_release` parameter has **no default**, so a new caller cannot silently skip the question. Prerequisite for it: the write path's target policy already refuses a second bulk-load writer against a session-scoped `#temp` target (spec 063 D1) — without that, turning the reset off lets a parallel writer find a **stale same-named** temp table on another pooled session and land rows there silently. |
| `mssql_metadata_timeout` | 300 | Metadata query timeout in seconds (0 = no timeout) |
| `mssql_attach_validation_timeout` | 0 | ATTACH-time eager credential validation timeout in seconds (0 = inherit `mssql_connection_timeout`). Spec 047 FR-011. |
| `mssql_catalog_cache_ttl` | 0 | Metadata cache TTL (0 = manual via `mssql_refresh_cache()`) |
| `mssql_exec_invalidate_cache` | false | Auto-invalidate the catalog cache after DDL run via `mssql_exec()` (CREATE/DROP/ALTER/TRUNCATE/RENAME/EXEC). Default `false` (like Postgres `postgres_execute`): invalidate manually with `mssql_invalidate_cache()`. Set `true` to auto-invalidate. Issue #151. The DDL test is `mssql::SqlMayChangeSchema` (`query/mssql_ddl_detect.hpp`): CREATE / DROP / ALTER / TRUNCATE / EXEC / EXECUTE / sp_rename as whole words outside literals, delimited identifiers and comments (a substring match until the review of #382). Inside an explicit transaction, `false` still stops the **transaction's own** lookups from trusting the shared cache after such DDL (`MarkChangedLocally`, review of #382). Otherwise a rowid key learned on the pinned connection from an index the transaction later rolled back would be written into a shared entry. |
| `mssql_insert_batch_size` | 1000 | Rows per INSERT statement. Under it a cap of 1000 constants per statement always applies (spec 062 W1b: `min(batch_size, 1000 / inserted columns)` rows), because SQL Server auto-parameterises a multi-row VALUES INSERT only up to 1000 constants — below the line N distinct inserts of one shape share one Prepared plan at 14 µs/row, above it every statement compiles its own Adhoc plan at 70 µs/row and leaves it in the cache (the 74 s of a 1M-row INSERT). A clustered-index target parameterises only up to ~250 rows a statement (the plan must stay trivial); past that it compiles per statement at the cheap end (5.3 vs 4.7 ms for 333 rows), so the cap is about the cliff, not the cache. An `sp_executesql` form with a DECLARE block was measured 16× slower and is not the answer. |
| `mssql_insert_max_rows_per_statement` | 1000 | Hard cap per INSERT |
| `mssql_insert_max_sql_bytes` | 8MB (8388608) | INSERT SQL size limit |
| `mssql_insert_use_returning_output` | true | Use OUTPUT INSERTED for RETURNING |
| `mssql_insert_use_bcp` | true | Spec 062. An INSERT with more rows than `mssql_insert_bcp_threshold`, no `RETURNING` and no explicitly named identity column loads through `INSERT BULK` — the wire COPY uses, with `CHECK_CONSTRAINTS, FIRE_TRIGGERS, KEEP_NULLS` so it still behaves like an INSERT statement (a bulk load ignores all three by default; measured, a CHECK violation in row 950 of 1000 loaded every row). Batch size, TABLOCK and writer count come from the `mssql_copy_*` settings. Parallel writers only on a target with NO nonclustered index — a bare heap under TABLOCK, or a bare clustered columnstore without it. Measured: a heap under TABLOCK takes a compatible BU lock while the same heap carrying a `PRIMARY KEY NONCLUSTERED` takes Sch-M (compatible with nothing) and each extra writer stalls 30 s; a clustered columnstore carrying a nonclustered index is worse still — the extra writer times out mid-stream and the whole INSERT fails and rolls back (30.8 s / 0 rows against 0.99 s / 400k on one writer): each writer holds its own server transaction until Finalize commits them together, and conflicting locks deadlock CLIENT-SIDE (measured: a hang past ten minutes on a heap on row locks). `false` is the escape hatch. |
| `mssql_insert_bcp_threshold` | 1000 | Spec 062. Rows up to which an INSERT is sent as statements — counted as they arrive in a staging buffer, never estimated. Measured (spec § 6.2): the paths cross near 300 rows on a local server (bulk's fixed cost is two round trips, `INSERT BULK` + `DONE`; statements cost ~12 µs/row plus a round trip per 333 rows) and near 1000 at a 20 ms RTT, so 1000 is where an INSERT is at most three statements and the bulk path never loses by more than a round trip or two. |
| `mssql_dml_batch_size` | 500 | Rows per UPDATE/DELETE batch |
| `mssql_dml_max_parameters` | 2000 | Max parameters per UPDATE/DELETE statement |
| `mssql_dml_use_prepared` | true | Use prepared statements for DML |
| `mssql_scan_parameterize_filters` | true | Spec 076. The constants of a pushed filter travel as `sp_executesql` parameters instead of literals in the query text, so the server keeps **one** plan per filter shape rather than one ad-hoc plan per distinct value set (measured: 40 scans with 40 distinct two-predicate filters = 40 cached plans / 2.3 MB before, 1 / 57 KB after; wall time unchanged on a local server). Each parameter is declared from the **column** it is compared with — varchar stays varchar so an index on it is still seekable, the width is never narrower than the constant so nothing is truncated into a false match, a non-ASCII constant goes as `varchar` when every character of it is representable in both the column's and the database's code page (#361 — the seek survives on `SQL_` collations, where an nvarchar parameter puts a CONVERT_IMPLICIT on the column; the pages are the server's, `COLLATIONPROPERTY(…, 'CodePage')`, read with the column metadata and the database collation; representability answered from tables for 874 and 1250–1258, `mssql::CodePageCanEncode`) and as `nvarchar` otherwise (a varchar variable takes the database's code page, not the column's — #321); `IN` lists and `IS NULL` stay literal. `false` is the escape hatch for parameter sniffing (a plan compiled for one value reused for a worse one) and what a reviewer flips to compare plans. |
| `mssql_enable_statistics` | true | Report each MSSQL scan's row count to the DuckDB optimizer (`TableFunction::cardinality`). Until the roadmap's "step 0" callback landed this setting was read by **nothing**, and every MSSQL scan planned as `~1 row` — a 200000-row table and a 50-row table looked identical, so join order and build-side choice around them were arbitrary. The estimate is free at plan time: statistics cache, then the count the catalog already loaded; it never opens a connection to plan a query, and reports **nothing** rather than 0 when the count is unknown (a VIEW has no `sys.partitions` rows and reports 0 while returning millions). Set `false` to restore the estimate-less behaviour. |
| `mssql_statistics_cache_ttl_seconds` | 300 | How long a cached row count stays usable — read at the point of use, from the SESSION that asks, never written into the shared per-catalog provider (one session's `SET` must not govern another's plans). It governs the PLANNER's cardinality lookup. It does **not** age out a count the CATALOG loaded (`PreloadRowCount`) on the table-LISTING path: `SHOW ALL TABLES` / `duckdb_tables()` exempt those deliberately, because ageing them would cost a connection plus a `sys.dm_db_partition_stats` round trip **per table** on a catalog that had just loaded every count in one query. Such entries are refreshed by invalidation instead — `mssql_invalidate_cache()`, DDL, COPY/CTAS — not by time. A DMV-sourced count ages normally on every path. |
| `mssql_copy_flush_rows` | 102400 | Rows per bulk-load batch — the boundary the **server** sees between DONE tokens, not a client buffer size. 102400 is SQL Server's own threshold for writing a batch straight into a **compressed columnstore rowgroup**; below it every row goes to the delta store and stays there (a delta rowgroup only closes on its own at 1048576 rows), so a smaller load never compresses at all. Measured on 1M rows — one OPEN rowgroup and 53 MB at the old 100000 default, nine COMPRESSED rowgroups and 7 MB at 102400 (spec 057). Raised from 100000, which silently defeated `table_kind = 'columnstore'`. One value for every target: batch size measured nearly flat on a heap, so the extra 2400 rows cost nothing there. |
| `mssql_copy_parallel_writers` | 0 | Concurrent bulk-load connections a single COPY **or CTAS** may open (spec 057 step 7). `0` derives it from DuckDB's thread count, capped at 8; `1` disables parallel loading. Each writer is an independent `INSERT BULK` on its own pooled connection — the bound on this path is SQL Server's **ingest** rate (`send()` blocks because the receive window stays full while the server lays rows down), and the server parallelises across sessions, so more sessions is the only lever that moves it. Measured on 44 columns x 1M rows: 10.55 s at 1 writer, **3.24 s at 4**, 3.51 s at 8 — it plateaus past 4. **Ignored inside an explicit transaction**, where the connection is pinned: a second connection would sit outside the transaction and its rows would not roll back with the rest. A **ceiling, not a floor**, in two more ways: one DuckDB thread driving the sink yields one writer whatever this says (which is why the parallel tests pin `SET threads`), and against a **clustered columnstore** target the extra writers are held back until the load has sunk one full 102400-row rowgroup on the shared writer (spec 070 W2) — so a small columnstore load legitimately finishes on one writer rather than splitting into batches too small to compress. Heap targets fan out immediately. The warm-up threshold is SQL Server's own rowgroup constant, not `mssql_copy_flush_rows`. And never above **`mssql_connection_limit`** (issue #380, COPY, CTAS and INSERT via BCP alike). An extra writer takes its connection with `ConnectionPool::TryAcquire`, which never waits and does not count toward `acquire_timeout_count`: it gets an idle connection, or a new one while the pool is below its limit, or none. In `mssql_pool_stats` a probe that GETS a connection counts in `acquire_count` and `acquire_wait_total_ms` like any other checkout; one that comes back empty counts in neither, so a writer asking once per chunk does not inflate the figures or skew the average wait (review of #382). With none, the claim answers `Busy`: the thread shares the global writer and asks again on a later chunk. `Unavailable`, which ends the asking, is left for a reached slot cap or a refused bulk load. It used to wait `mssql_acquire_timeout` for a connection the statement itself was holding: a 300k-row CTAS in a transaction on a pool of two took 30 s, and now takes 0.84 s. The first cut of #380 capped the writers at the limit minus one instead, and the review of #382 caught that this wastes a connection whenever the statement holds none. |
| `mssql_copy_tablock` | `auto` | `auto` \| `true` \| `false`. `auto` decides from the target's shape: **heap on**, **anything clustered off** (rowstore *and* columnstore) — concurrent bulk loaders on a heap take mutually compatible BU locks so the hint lets them run together, while against a clustered rowstore index the same hint serialises them. Replaces the "enable for newly created tables" rule of issue #45, which never actually fired: the flag it tested was set from `TryGetCurrentSetting` succeeding, which it always does (spec 057). The columnstore half of this policy was wrong until spec 057 step 7 made the loads concurrent and exposed it: TABLOCK **serialises** them. Measured on 2M rows, clustered columnstore — hint ON 8.92 s with SQL Server pinned at ~99% CPU (exactly one core), hint OFF 5.23 s peaking at 305% (three cores), and **identical compression either way** (17 COMPRESSED rowgroups, 1740800 rows, both). What decides whether rows land compressed is `mssql_copy_flush_rows` crossing 102400, not the lock. |
| `mssql_utf8_collation` | `Latin1_General_100_BIN2_UTF8` | Collation given to VARCHAR columns created by CTAS when `mssql_ctas_text_type` is `VARCHAR` **and** the server granted `UTF8SUPPORT` at login (issue #225). Without a UTF-8 collation such a column takes the database's code page and SQL Server replaces everything outside it with `?` **on insert**, silently. BIN2 matches what Fabric Warehouse uses as its own default: binary comparison, no linguistic rules — and therefore case- and accent-SENSITIVE, so `WHERE name = 'abc'` stops matching `'ABC'`. Per-column override is the annotation `x::MSSQL_VARCHAR(n, 'collation')`, where n may be `'MAX'` (or `0`/`-1`) for the unbounded form — issue #321; a bare `MAX` keyword cannot work, DuckDB's parser rejects a non-constant type modifier before the extension sees it. This setting moves it globally. The collation is stored in the schema and governs every later comparison against the column. Empty adds no `COLLATE` clause, so the column inherits the database default: correct when that default is already UTF-8 (Fabric), and the way back to the pre-#225 behaviour otherwise. |
| `mssql_catalog_native_types` | true | Report `MSSQL_VARCHAR(n)` / `MSSQL_NVARCHAR(n)` for the string columns of attached tables instead of a bare `VARCHAR` (spec 060). This is what lets a target created from an MSSQL source inherit its declared lengths and collations with no cast written by anyone. Costs a changed `DESCRIBE` / `duckdb_columns()` type name, which is what turning it off restores. Read when a table entry is built, so `mssql_invalidate_cache()` applies a change to already-loaded tables. |
| `mssql_default_string_length` | 0 | Length given to an **unannotated** VARCHAR column created by CTAS or COPY; 0 = MAX, which is what a plain VARCHAR has always meant. Set it and the extension's own overflow guard enforces it before the batch is sent. Above SQL Server's inline limit (4000 nvarchar / 8000 varchar) the column stays MAX. Per-column control is a cast to `MSSQL_NVARCHAR(n)` / `MSSQL_VARCHAR(n)`. |
| `mssql_default_table_kind` | `HEAP` | Shape of a table created by CTAS/COPY: `HEAP` or `COLUMNSTORE`. `COLUMNSTORE` runs `CREATE CLUSTERED COLUMNSTORE INDEX` right after the CREATE and **before** the load, so the load writes compressed rowgroups directly instead of needing a rebuild. Per-statement: the COPY `table_kind` option, or `CREATE TABLE ... WITH (table_kind = '...')`. |
| `mssql_ctas_use_bcp` | true | Use BCP protocol for CTAS data transfer (2-10x faster than INSERT) |
| `mssql_convert_varchar_max` | true | Convert VARCHAR(MAX) to NVARCHAR(MAX) in catalog queries for UTF-8 compatibility |
| `mssql_named_instance_resolution` | true | Resolve `Server=host\instance` to the instance's dynamic TCP port via the SQL Server Browser (UDP 1434) at ATTACH time (spec 045). Set `false` in environments that strip outbound UDP 1434 — a named instance then errors instead of silently using port 1433, and you connect with an explicit `Server=host,port`. |
| `mssql_browser_timeout_seconds` | 3 | SQL Server Browser UDP query timeout in seconds for named-instance resolution. On the ATTACH critical path, so kept short; the resolver retries once. |
| `mssql_tds_packet_size` | 16384 | TDS frame size requested in LOGIN7, clamped to [512, 32767]. The server answers with `min(requested, its own maximum)` and never raises it, so this is the ceiling for every packet in both directions — it bounds recv() count on reads and send() count on BCP writes. Raised from the long-standing 4096 in spec 055: measured −28% client CPU / −43% wall on read and −27% CPU on write (`test/bench/bench_results_live_server.md`). Costs server memory per session (16 KB vs 4 KB per pooled connection); set to 4096 to restore the old behaviour. |
| `mssql_warn_non_utf8_collation` | true | Log a `WARNING` into `duckdb_logs` when a CHAR/VARCHAR/TEXT column arrives with a non-UTF-8 collation — its bytes are handed to a DuckDB VARCHAR verbatim, and DuckDB VARCHAR is UTF-8 by contract (issue #224). Checked over COLMETADATA once per stream, never per row: #224 is deliberately **documented rather than enforced**, because validating would cost the scan's hot path to make a hand-written `mssql_scan()` fail loudly, and a non-UTF8 varchar never reaches the binary kernel on the catalog path anyway (`NeedsNVarcharConversion` casts it server-side), so the guard would have billed the UTF-8 configuration #225 optimised for. The reason an off switch exists: `SQL_Latin1_General_CP1_CI_AS` is the SQL Server **installation default**, so a raw `mssql_scan()` over a legacy-collation server logs one line per varchar column per query — including for data that is pure ASCII and therefore already valid. Server INFO tokens (`PRINT`, `RAISERROR` ≤ 10) are unaffected by this setting and log at `INFO`, not `WARNING`, so they cannot dilute it. |
| `mssql_utf8_support` | true | Advertise the TDS `UTF8SUPPORT` feature extension (0x0A) in LOGIN7 (issue #225). A server that acknowledges it sends columns whose collation is a UTF-8 one as UTF-8 (`0xA7`) instead of transcoding them to UTF-16 (`0xE7`): measured 85.8 MB -> 43.9 MB on the wire and 0.44 s -> 0.25 s wall for 1M ~41-char values, because the client copies the bytes into the vector instead of running the UTF-16 batch decode. Requesting it is safe on any server — one that lacks the feature omits the acknowledgement and nothing changes — so this setting exists to turn the request **off**. |
| `mssql_test_fail_metadata_after_rows` | 0 | **Test-only** (issue #317). Make a metadata query throw once it has parsed this many rows; 0 = off. Exists because every cause of a mid-query metadata failure in the wild — timeout, reset connection, killed session — arrives from outside and cannot be induced from SQL, so the invariant "a metadata load that fails leaves the cache exactly as it was" was untestable and got broken twice (#178 in `Refresh()`, #317 in `LoadAllSchemasMetadata`). Off is free: `ExecuteMetadataQuery` passes the row callback through unwrapped, so the row loop carries no test. |
| `mssql_test_fail_parse_after_tokens` | 0 | **Test-only** (issues #323, #344). Put the TDS token parser of a DML response (INSERT batch, INSERT … RETURNING, UPDATE, DELETE) into Error after this many tokens, as a desync would; 0 = off. A desync cannot be induced from SQL since spec 072, so without it the four response loops' handling of one — drain without buffering, report it naming what the server executed, never hang — was untestable, and stayed broken in those four copies after #323 fixed the fifth. Off costs one comparison per token in those loops only; the result stream is untouched. |
| `mssql_login7_max_packet` | 0 | **Test-only** (issue #138). Max LOGIN7 TDS packet size (bytes) for integrated auth; lowers the fragmentation boundary so the multi-packet send path can be exercised without an AD-sized Kerberos PAC. 0 = production default (4096); effective values clamped to [256, 32767]. |

## ATTACH Options & Secret Parameters (Catalog Filters)

| Parameter | Type | Description |
|-----------|------|-------------|
| `schema_filter` | VARCHAR | Regex pattern to filter visible schemas (case-insensitive, partial match via `regex_search`) |
| `table_filter` | VARCHAR | Regex pattern to filter visible tables/views (case-insensitive, partial match via `regex_search`) |
| `transaction_isolation` (ADO.NET `TransactionIsolation`) | VARCHAR | Issue #331. Level of the server transaction a DuckDB transaction opens: `default` (unset: send nothing, as before), `read_uncommitted` / `read_committed` / `repeatable_read` / `serializable` / `snapshot` (sent as its own `SET TRANSACTION ISOLATION LEVEL` before BEGIN on the pinned connection), or `auto` (SNAPSHOT when `sys.databases.snapshot_isolation_state` = 1, probed at ATTACH in the collation query -- only for `snapshot`/`auto` off Fabric/Synapse, so everyone else's ATTACH query is unchanged; else nothing). Explicit `snapshot` on a database with it OFF is refused at ATTACH; Fabric Warehouse gets nothing (enforces snapshot); Synapse gets nothing and refuses explicit levels but `read_uncommitted`. **The reset does not clear the isolation level** (measured: SQL Server keeps it across `RESET_CONNECTION`), so a transaction that set one sends `SET ... READ COMMITTED` as its own statement after COMMIT / ROLLBACK -- appended to the ROLLBACK batch it is lost when the server already aborted the transaction (3960 update conflict, stale descriptor) -- and a connection whose level could not be put back is closed. SNAPSHOT makes a concurrently changed row an update conflict (3960) instead of a re-read; its readers are not blocked by a concurrent TABLOCK heap bulk load, READ COMMITTED readers are. |
| `default_schema` (ADO.NET `DefaultSchema`) | VARCHAR | Issue #322. The schema unqualified names resolve against (`db.table`, `USE db`, CTAS into `db.table`, COPY into `db.table` or `mssql://db/table`); empty = `dbo`, as before. The exact name as the server spells it (the catalog's schema lookup is case-sensitive). Not checked against the server at ATTACH -- a missing schema is named at its first unqualified use -- but an explicit default that `schema_filter` hides is refused at ATTACH (an unset `dbo` hidden by the filter is not: existing ATTACHes pairing such a filter with qualified names keep working). Trimmed; empty -- including an explicit `''` that clears a secret's value -- means `dbo`. Deliberately not `SCHEMA_NAME()`: SQL Server resolves unqualified names default-then-`dbo`, DuckDB has one default schema, so following the login's default would break unqualified references to `dbo` objects. |
| `preload` | BOOLEAN | Issue #324. `mssql_preload_catalog(name)` run by the ATTACH itself (`MSSQLCatalog::Preload`, shared with the function), which is where DuckLake's `METADATA_PARAMETERS` can reach it. A failure fails the ATTACH, because it was asked for. It is refused beside `lazy_validation true` and `catalog false`, with each contradiction named. |
| `min_connections` | BIGINT | Issue #324. The ATTACH form of `mssql_min_connections`, and it wins over the setting. A string (`'4'`) is accepted as well. |
| `lazy_validation` (or `LazyValidation`) | BOOLEAN | Skip the eager ATTACH-time TCP+LOGIN7 credential check. Default `false` (eager — wrong creds / unreachable host surface as ATTACH errors). Set `true` for container/orchestrator startup where the SQL Server may not yet be reachable; first query then pays the connection-establishment cost as in the pre-spec-047 behaviour. Bounded by `mssql_attach_validation_timeout`. Spec 047 FR-011. |
| `TrustServerCertificate` / URI `trustservercertificate` / secret `trust_server_certificate` | BOOLEAN | Spec 074. Default `false`: the server's certificate chain must validate against the platform trust store (Windows ROOT+CA, macOS keychain plus OpenSSL paths, Linux OpenSSL paths; `SSL_CERT_FILE`/`SSL_CERT_DIR` override) and its subject must match the host dialled, else the handshake fails with OpenSSL's reason and both ways out named. `true` accepts any certificate. Independent of `Encrypt` (the pre-074 alias and its "Conflicting values" error are gone); ignored under `Encrypt=false`. Every test DSN in the Makefile **and** in `scripts/ci/integration_test.sh` (the CI lane's own copy of those defaults) carries `TrustServerCertificate=yes`, and the CI smoke secret in `scripts/sql/smoke_test.sql` says `trust_server_certificate true`, because the docker server runs on its self-generated certificate. Connection string / URI / secret, not an ATTACH option. |
| `HostNameInCertificate` (ODBC `HostnameInCertificate`) / URI `hostnameincertificate` / secret `host_name_in_certificate` | VARCHAR | Spec 074. The name the certificate must carry when it differs from the address dialled (IP literal, SSH tunnel, alias). Empty = the host dialled; after a routing hop the routed host, unless set, in which case it applies to every hop (go-mssqldb). An IP literal is matched against iPAddress SANs. Ignored under `TrustServerCertificate=true`. Carried by `tds::TlsOptions` through `TdsConnection::SetTlsOptions` -- a setter, not a parameter on the three `Authenticate*` signatures. |
| `azure_tenant_id` (or `azure_tenant`) | VARCHAR | Tenant to acquire the Azure AD token against, overriding the tenant carried by the `azure_secret`. Registered as an MSSQL-secret parameter since spec 032 and **read by nothing** until 2026-08-14 (PR #264 review): with no override, `credential_chain` + `interactive` falls back to `AZURE_DEFAULT_TENANT` = `common` inside `azure_device_code.cpp`, so every interactive ATTACH in a single-tenant org authenticated against `/common/` with no way to say otherwise. `TENANT_ID` on the *azure* secret is not the way to do this — duckdb-azure rejects it on `provider='credential_chain'`. The value is part of the `TokenCache` key, so two tenants do not share a token. |
| `Application Name` / `ApplicationName` / `App Name` / `application_name` | VARCHAR | LOGIN7 `program_name` propagated to SQL Server (visible via `APP_NAME()` / `sys.dm_exec_sessions.program_name`). URI form uses spaceless `applicationname` query parameter; secret form uses `application_name` (canonical) or `applicationname`. Empty falls back to `"DuckDB MSSQL Extension"`; values longer than 128 chars are clamped client-side. Closes [issue #82](https://github.com/hugr-lab/mssql-extension/issues/82) (spec 047 FR-014). |

Available in: ATTACH options, ADO.NET connection strings (`SchemaFilter`/`TableFilter`), URI query parameters, and MSSQL secrets. ATTACH options override secret/connection string values. A boolean ATTACH option (`lazy_validation`, `catalog`, `order_pushdown`, `preload`) also takes a string — `'true'`/`'false'`, `'yes'`/`'no'`, `'1'`/`'0'`, DuckDB's own boolean cast — because DuckLake's `METADATA_PARAMETERS` is a `MAP(VARCHAR, VARCHAR)` and can send nothing else (issue #325).

## Extension Functions

| Function | Type | Description |
|----------|------|-------------|
| `mssql_scan(context, query [, prepared := false])` | Table | Execute raw T-SQL, stream results. Since spec 075 Bind asks `sp_describe_first_result_set` for the shape and the query runs at InitGlobal — a `DESCRIBE`/`EXPLAIN` no longer executes it, and inside a transaction the rows are materialised at init under `MaterializeMutex` so the pinned connection is Idle for the next scan or sink. A batch the server cannot describe (one that reads a `#temp` table it creates) is still run at Bind, as before. `prepared := true` compiles once via `sp_prepare` (shape from its answer, execution by handle on the session that holds it) and degrades to the describe when the server prepares without a shape. |
| `mssql_scan_params(context, statement, STRUCT [, declarations] [, prepared := false])` | Table | `mssql_scan` with parameters: `{'p': 1}` becomes `@p`, declared from the DuckDB type (spec 075 W5 table: VARCHAR → `nvarchar(4000)`/`nvarchar(max)`, `MSSQL_VARCHAR(n)` exact, TIMESTAMP → `datetime2(6)`, …) or by the caller's own list (`'@ts datetime, @c varchar(8)'`), and sent through `sp_executesql` — one server plan per statement text, shared by every session. A bare NULL, a nested type or a name that is not a T-SQL identifier is refused with the fix named. |
| `mssql_exec(context, sql)` | Scalar | Execute T-SQL, return affected row count |
| `mssql_exec_params(context, statement, STRUCT [, declarations])` | Scalar | `mssql_exec` with the same parameter contract as `mssql_scan_params`; one round trip per row, the plan reused across rows and sessions. |
| `mssql_pool_stats([context])` | Table | View connection pool statistics |
| `mssql_refresh_cache(context)` | Scalar | Refresh metadata cache (eager full reload). Refused inside a transaction that has used **any** MSSQL catalog (issue #380; narrowed from "any open transaction" in the review of #382, so a transaction wrapping unrelated DuckDB work is allowed — but not narrowed to one catalog, because two ATTACHes of the same DSN are independent catalogs and alias A's uncommitted DDL would hang a load on alias B) |
| `mssql_invalidate_cache(context [, schema [, table]])` | Scalar | Lazy point invalidation — whole catalog / one schema / one table (keeps other tables' cached columns) |
| `mssql_preload_catalog(context [, schema])` | Scalar | Bulk-load all metadata in one round trip. Refused inside a transaction that has used **any** MSSQL catalog (issue #380, review of #382 — same reasoning as `mssql_refresh_cache`) |
| `mssql_azure_auth_test(secret, tenant?)` | Scalar | Test Azure AD token acquisition |
| `mssql_kerberos_auth_test(host [, port])` | Scalar | Test POSIX Kerberos auth path (spec 042); returns OK + SPN / principal / token size, or verbatim GSSAPI error |
| `mssql_kerberos_auth_test_secret(secret_name)` | Scalar | Same but reads keytab / SPN-override / etc. from an MSSQL secret |
| `mssql_winsspi_auth_test(host [, port])` | Scalar | Windows SSPI peer of `mssql_kerberos_auth_test` (spec 042 Phase 4); returns OK + SPN / UPN / token size, or verbatim SSPI error |
| `mssql_winsspi_auth_test_spn(spn)` | Scalar | Same but takes an explicit SPN (overrides default `MSSQLSvc/<host>:<port>` derivation) |

Every scalar and table function is registered through `mssql::RegisterDocumentedFunction` (`src/mssql_function_docs.cpp`, issue #371), which gives it the description, examples and category `duckdb_functions()` reports, and names the scalar parameters in the signature. Those names are **API**: the binder matches named arguments against them (`mssql_exec(context := 'db', sql := '...')`), so renaming one breaks callers. `test/sql/mssql_function_docs.test` fails a function registered without documentation. A table function's positional parameters stay `col0`, `col1`, … in `duckdb_functions()`: DuckDB names them by position whatever the registration says.

## Active Technologies
- C++17 (DuckDB extension standard) + DuckDB (main branch), OpenSSL (vcpkg), Winsock2 (Windows system library) (019-fix-winsock-init)
- C++17 (DuckDB extension standard) + DuckDB (main branch), existing TDS layer (specs 001-019) (020-multi-statement-scan)
- In-memory (result streaming, connection pool state) (020-multi-statement-scan)
- C++17 (DuckDB extension standard) + DuckDB (main branch), OpenSSL (vcpkg), existing TDS protocol layer (022-mssql-ctas)
- SQL Server 2019+ (remote), in-memory (result streaming, connection pool state) (022-mssql-ctas)
- C++17 (DuckDB extension standard) + DuckDB (main branch), OpenSSL (via vcpkg for TLS) (023-pool-stats-validation)
- In-memory (connection pool state, metadata cache) (023-pool-stats-validation)
- C++17 (DuckDB extension standard) + DuckDB (main branch), TDS BulkLoadBCP protocol (0x07), OpenSSL (via vcpkg for TLS) (024-mssql-copy-bcp)
- SQL Server 2019+ (remote target), in-memory (batch buffering, connection pool state) (024-mssql-copy-bcp)
- SQL Server 2019+ (remote target), in-memory (connection pool state) (025-bcp-improvements)
- C++17 (DuckDB extension standard) + DuckDB (main branch), existing TDS protocol layer, OpenSSL (vcpkg) (026-varchar-nvarchar-conversion)
- SQL Server 2019+ (remote target), in-memory (batch buffering) (027-ctas-bcp-integration)
- C++17 (DuckDB extension standard) + DuckDB (main branch), OpenSSL (vcpkg for TLS), libcurl (vcpkg for OAuth2 HTTP), DuckDB Azure extension (runtime, for Azure secret management) (001-azure-token-infrastructure)
- In-memory (token cache, no persistence required) (001-azure-token-infrastructure)
- C++17 (DuckDB extension standard) + DuckDB (main branch), OpenSSL (vcpkg), libcurl (vcpkg for Azure OAuth2) (031-connection-fedauth-refactor)
- In-memory (connection pool state, token cache) (031-connection-fedauth-refactor)
- C++17 (DuckDB extension standard, but C++11 compatible for ODR) + libcurl (OAuth2 HTTP), OpenSSL (TLS), DuckDB Azure extension (secret management) (032-fedauth-token-provider)
- In-memory token cache (TokenCache singleton) (032-fedauth-token-provider)
- C++17 (C++11-compatible for ODR with DuckDB) + DuckDB (main branch), OpenSSL (vcpkg), TDS protocol layer (033-fix-catalog-scan)
- In-memory metadata cache (`MSSQLMetadataCache`) (033-fix-catalog-scan)
- C++17 (DuckDB extension standard, C++11-compatible for ODR on Linux) + DuckDB v1.5-variegata (6,275 commits ahead of v1.4.4), OpenSSL (vcpkg), libcurl (vcpkg) (034-duckdb-v15-upgrade)
- N/A (remote SQL Server via TDS protocol) (034-duckdb-v15-upgrade)
- C++17 (C++11-compatible for ODR on Linux) + DuckDB v1.5-variegata (extension API) (035-ddl-schema-support)
- C++17 (C++11-compatible for ODR on Linux) + DuckDB v1.5-variegata + DuckDB extension API, OpenSSL (vcpkg), libcurl (vcpkg) (036-azure-token-docs)
- C++17 (C++11-compatible for ODR on Linux) + DuckDB (main branch), OpenSSL (vcpkg), cpp-httplib (bundled in DuckDB third_party) (037-replace-libcurl-httplib)
- N/A (in-memory token cache, no change) (037-replace-libcurl-httplib)
- C++17 (C++11-compatible for ODR on Linux) + DuckDB (main branch), OpenSSL (vcpkg), existing TDS protocol layer (039-order-pushdown)
- C++17 (C++11-compatible for ODR on Linux) + DuckDB (main branch), OpenSSL (vcpkg), custom TDS protocol layer (040-fix-datetimeoffset-nbc)
- 042-integrated-authentication: Added integrated authentication (Kerberos on POSIX, SSPI on Windows)
  - POSIX: system GSSAPI (libgssapi_krb5 on Linux, GSS.framework on macOS via `-framework GSS`)
  - Windows: secur32.dll (Phase 4 — not yet implemented)
  - Connection-string keys (verbatim from `microsoft/go-mssqldb`): `authenticator`, `krb5-configfile`, `krb5-keytabfile`, `krb5-credcachefile`, `krb5-realm`, `service_principal_name`
  - Aliases: `Trusted_Connection=yes`, `Integrated Security=SSPI/true`

## Azure AD Authentication

The extension supports Azure AD authentication for Azure SQL Database and Microsoft Fabric. Authentication is implemented using DuckDB's bundled **cpp-httplib** (with OpenSSL) for OAuth2 token acquisition (no Azure SDK or libcurl dependency).

**Supported methods:**
- **Service Principal**: Client credentials flow with tenant_id, client_id, client_secret
- **Azure CLI**: Uses `az account get-access-token` for developers with `az login`
- **Device Code Flow**: Interactive authentication for MFA-enabled accounts

**Implementation files:**
- `src/azure/azure_http.cpp` - HTTP client wrapper (single httplib compilation unit)
- `src/azure/azure_token.cpp` - OAuth2 token acquisition
- `src/azure/azure_device_code.cpp` - RFC 8628 device code flow
- `src/azure/azure_secret_reader.cpp` - Reads Azure secrets from DuckDB Azure extension
- `src/azure/azure_test_function.cpp` - `mssql_azure_auth_test()` function

See `AZURE.md` for user documentation.

## Integrated Authentication (Kerberos / SSPI)

POSIX Kerberos and Windows SSPI integrated authentication, shipped via spec 042.

**Supported credential modes (POSIX):**
- **CredCache** (default): uses `kinit` ticket from `KRB5CCNAME` / `/tmp/krb5cc_<uid>`. Works on Linux and macOS.
- **Keytab**: `krb5-keytabfile=/path/to.keytab` + `User Id=svc@REALM`. Linux only (MIT Kerberos extensions required).
- **Raw**: secret-only — cleartext passwords are **never** accepted from a connection string. Linux only.

**Implementation files:**
- `src/include/tds/auth/iauthenticator.hpp` — three-method interface (`InitialBytes` / `NextBytes` / `Free`), modeled on `microsoft/go-mssqldb`'s `integratedauth.IntegratedAuthenticator`
- `src/tds/auth/krb5_authenticator.{hpp,cpp}` — GSSAPI implementation (POSIX, compiled when `MSSQL_ENABLE_KRB5` is defined). All `gss_*`/`krb5_*` calls go through the runtime shim (spec 053).
- `src/tds/auth/gssapi_runtime.{hpp,cpp}` — **spec 053 (#161)**: lazy `dlopen`/`dlsym` loader for GSSAPI/krb5. On Linux the extension carries NO link-time (`DT_NEEDED`) dependency on `libgssapi_krb5.so.2`/`libkrb5.so.3`; the library is loaded on first `authenticator=krb5` use (thread-safe via `std::call_once`). Throws `Krb5RuntimeUnavailable` (names the missing `.so` + install package) when absent. macOS fills the table with direct `&gss_*` addresses (system framework, always present). CMake keeps the GSSAPI/krb5 include dirs but drops `target_link_libraries` on Linux.
- `src/tds/auth/winsspi_authenticator.{hpp,cpp}` — Windows SSPI implementation via `secur32.dll` Negotiate package (compiled when `MSSQL_ENABLE_SSPI` is defined; CMake auto-enables on `_WIN32`)
- `src/include/tds/auth/integrated_auth_strategy.hpp` — adapter wrapping `IAuthenticator` in the existing `AuthenticationStrategy` interface
- `src/tds/auth/auth_strategy_factory.cpp` — `AuthStrategyFactory::Create` dispatches `KRB5` / `WINSSPI` based on `info.auth_method`
- `src/tds/tds_connection.cpp` `AuthenticateIntegrated()` — SPNEGO continuation loop on `0xED` SSPI tokens
- `src/tds/tds_protocol.cpp` `BuildLogin7WithSSPI` + `BuildSSPIMessage` — LOGIN7 with `fIntSecurity` bit (0x80) + SSPI Message packet type 0x11
- `src/connection/mssql_pool_manager.cpp` `GetOrCreatePoolWithIntegratedAuth` — pool factory builds a fresh authenticator per connection so kinit-refreshed tickets are picked up
- `src/tds/auth/krb5_test_function.cpp` — registers `mssql_kerberos_auth_test(host[, port])` and `mssql_kerberos_auth_test_secret(secret_name)` scalar functions. Mirrors `mssql_azure_auth_test` for Azure; exercises `Krb5Authenticator::InitialBytes()` without connecting to SQL Server. Compiled with a no-op fallback so the functions are always registered (returns "compiled without Kerberos support" when `MSSQL_ENABLE_KRB5` is undefined).

**Test infrastructure:** `test/kerberos/` — self-contained docker-compose stack (KDC + SQL Server + test-client). No real Active Directory required:

```bash
cd test/kerberos
docker compose up -d --build
docker compose exec test-client /run-tests.sh
docker compose down -v
```

The test KDC's realm is `EXAMPLE.COM`, principal is `testuser@EXAMPLE.COM` (password `testpass`), SPN is `MSSQLSvc/sql.example.com:1433`. The test-client uses a multi-stage Dockerfile that builds the extension inside Linux, so the stack works on macOS hosts too.

**Platform matrix:**

| Platform | CredCache | Keytab | Raw | Status |
|---|---|---|---|---|
| Linux x86_64 / ARM64 | yes | yes | yes (secret only) | Phase 3 shipped |
| macOS ARM64 | yes | rejected at construction | rejected at construction | Phase 3 shipped |
| Windows x64 | yes (logon session) | n/a | n/a | Phase 4 shipped |

**Connection-string surface (verbatim from `microsoft/go-mssqldb`):**

| Key | Purpose |
|---|---|
| `authenticator=krb5` / `authenticator=winsspi` | Explicit form |
| `Trusted_Connection=yes` | pyodbc alias — resolves to `krb5` on POSIX, `winsspi` on Windows |
| `Integrated Security=SSPI` / `Integrated Security=true` | ADO.NET alias — same resolution |
| `krb5-configfile=/path/to/krb5.conf` | Per-connection krb5.conf override (Linux only, via cred_store `config` element) |
| `krb5-keytabfile=/path/to/file.keytab` | Selects keytab mode |
| `krb5-credcachefile=FILE:/path` | ccache override (Linux only, via cred_store `ccache` element) |
| `krb5-realm=REALM.COM` | Required for keytab when User Id lacks `@REALM` |
| `service_principal_name=MSSQLSvc/host:port` | Override default SPN derivation |

All also accepted on `CREATE SECRET` (with underscore naming: `krb5_keytabfile`, etc.).

**Key design decisions (do NOT re-litigate — these were settled during ultrareview):**
- Raw mode is **SECRET-ONLY**. The validator unconditionally rejects `Password` in any connection string when integrated auth is selected. Defends against cleartext passwords in connection-string logs.
- `User Id` requires either a keytab or a Password (in a secret). CredCache mode rejects bare `User Id` — was silently authenticating as the ambient ccache holder before fix.
- Default SPN form is `MSSQLSvc/<fqdn>:<port>` (canonical Kerberos principal-name form, matches AD default registration). `Krb5Authenticator` picks the `gss_import_name` name type based on whether the SPN contains `/`.
- No `setenv()` for per-connection overrides — uses `gss_acquire_cred_from` with cred_store elements (`ccache`, `config`). Thread-safe vs concurrent `getenv` on worker threads.
- macOS uses `GSS.framework` (Heimdal-derived subset) which lacks MIT extensions for keytab/raw modes; `Krb5Authenticator` constructor rejects those modes on macOS with a clear error pointing at the Linux container path.
- GSSAPI OIDs (SPNEGO, Kerberos, hostbased-service, krb5-principal-name) are **constructed inline** as `gss_OID_desc` literals — macOS's `GSS.framework` declares `GSS_C_NT_HOSTBASED_SERVICE` etc. as `extern gss_OID` in the header but does NOT export the symbols. Inline DER bytes avoid the link dependency on every platform.

See `Kerberos.md` for end-user documentation.

## Build Troubleshooting

### ODR (One Definition Rule) Errors on Linux

**Symptom:** Linux builds fail with "multiple definition of `duckdb::LogicalType::BIGINT`" and similar errors for constexpr static members.

**Root Cause:** DuckDB defaults to C++11. If the extension uses `target_compile_features(... cxx_std_17)`, it gets compiled with C++17 while DuckDB remains C++11. The different handling of `constexpr static` members (external linkage in C++11 vs inline in C++17) causes ODR violations when linking.

**What NOT to do in CMakeLists.txt:**
```cmake
# DO NOT USE - causes ODR errors on Linux when DuckDB is C++11:
set(CMAKE_CXX_STANDARD 17 CACHE STRING "..." FORCE)
target_compile_features(${EXTENSION_NAME} PRIVATE cxx_std_17)
```

**Correct Solution:** Don't force C++17 for the extension. Use DuckDB's default C++ standard (C++11) and avoid C++17-only features like structured bindings. The extension code should be compatible with C++11.

**Note:** This issue only manifests on GCC/Linux, not on Clang/macOS, because Clang is more lenient with ODR for constexpr static members.

## Recent Changes
- 049-fix-partitioned-table-catalog: Partitioned tables were unreadable ([#85](https://github.com/hugr-lab/mssql-extension/issues/85)). The three catalog metadata queries joined `sys.partitions` directly, and that view holds **one row per partition** — so on an N-partition table every object and every column came back N times, aborting catalog load with `Column with name <x> already exists!`. A plain clustered index reads fine; only partitioning triggers it. The same join silently took `approx_rows` from whichever partition surfaced first — typically an empty one — so the planner saw a partitioned table as nearly empty while the statistics provider (which already summed) disagreed. Both fixed by joining a pre-aggregated subquery. The subquery now also reports the object's physical shape — `index_kind` (`MSSQLIndexKind`: heap / clustered / clustered columnstore, from `sys.indexes.type`) and `partition_count` on `MSSQLTableMetadata` — at no extra round trip, which the write path needs: TABLOCK helps a heap and serialises a clustered rowstore index, and a clustered index that is not a primary key is invisible to PK discovery. Typed rather than a bool because a clustered columnstore also reports `index_id = 1` and wants the opposite TABLOCK decision.
- 047-process-state-cleanup: Process-wide singleton cleanup + ATTACH credential validation + Azure TokenCache namespacing + custom Application Name (closes [#96](https://github.com/hugr-lab/mssql-extension/issues/96), [#82](https://github.com/hugr-lab/mssql-extension/issues/82); spawns [#119](https://github.com/hugr-lab/mssql-extension/issues/119) for future spec 049). **Three singletons removed**: `MssqlPoolManager` → per-`MSSQLCatalog` `unique_ptr<ConnectionPool>`; `MSSQLContextManager` (spec 045 band-aid) → direct `Catalog::GetCatalog()` lookup; `MSSQLResultStreamRegistry` → per-catalog `RegisterStream` / `RetrieveStream` methods on `MSSQLCatalog`. **One singleton kept + deprecated**: `MSSQLConnectionHandleManager` backs `mssql_open` / `mssql_close` / `mssql_ping` (no catalog discriminator on those APIs); marked `[DEPRECATED]` group with companion `mssql_close_all()` shutdown helper, scheduled for removal with the functions in a future major release. **Security hardening**: Azure `TokenCache` keyed by `(uintptr_t(DatabaseInstance*), cache_key)` so two instances sharing a secret name no longer alias (FR-012); ATTACH eagerly validates credentials by default with `lazy_validation true` opt-out (FR-011); ATTACH error path audited to never echo password (T028a); `mssql_pool_stats` redaction grep gate (SC-005); explicit `noexcept` on the teardown chain (`~MSSQLCatalog` / `~ConnectionPool` / `~TdsConnection` / `~TdsSocket` / `~TlsTdsContext` / `~TlsImpl`) + debug-only `D_ASSERT(active_connections_.empty())` invariant. **Custom Application Name**: `Application Name=...` / `applicationname=...` / secret `application_name` propagated to LOGIN7 program_name; visible as `APP_NAME()` / `sys.dm_exec_sessions.program_name`; 128-char client-side clamp matches SQL Server's own limit. See `specs/047-process-state-cleanup/state_inventory.md` for the post-spec process-wide-static classification.
- 042-integrated-authentication Phase 4: Added Windows SSPI authentication via `secur32.dll`'s Negotiate package. `WinSspiAuthenticator` peer of `Krb5Authenticator`. Same `IAuthenticator` interface; shared SPNEGO continuation loop in `TdsConnection::AuthenticateIntegrated`.
- 045-type-codec-consolidation: Per-type encoding/decoding/literal/DDL logic consolidated into 9 family modules under `src/codec/` (boolean/integer/float/decimal/money/string/binary/datetime/uuid). 5 LogicalType-side dispatch sites collapsed to family-dispatch (`FamilyFromLogicalType` switch or `codec::FormatSqlLiteral` one-liner). 762 LOC removed across dispatch sites (3243→2481, −23.5%). Bonus: TIMESTAMP_MS/NS/S/TZ now round-trip losslessly through SQL Server DATETIME2(3/7/0/7) with full type-transparency (catalog reports the variant, encode/decode preserves native precision). Bonus: stale-ATTACH ContextManager fix (sqllogictest `--force-reload` pointer-reuse). Closes issue #91 (BCP nvarchar character-vs-byte length); closes issue #89 (VIEW catalog-vs-runtime type divergence). No new vcpkg deps. Per-row bench (1M rows): 0.988–1.015× ratio vs spec-044 baseline (well within 5% gate).
- 044-codec-consolidation: Finishes the simdutf migration started in 043 — every legacy `Utf16LE*` call site moves to the simdutf-backed wrapper, the wrapper is renamed back to `Utf16LE*` (legacy file path resurrected with new implementation), and the legacy hand-rolled converter survives only as a private invalid-input fallback. Includes codec microbenchmark (`make bench-utf16`) and an end-to-end before/after benchmark (`test/bench/bench_codec_e2e.sh`, 100M rows) recorded into `bench_results.md`. No new vcpkg deps.
- 043-refactoring-foundation: Added C++ (C++11-compatible ABI) + DuckDB (main branch), simdutf (vcpkg, statically linked, MIT) for LOGIN7 non-ASCII fix; OpenSSL unchanged
- 042-integrated-authentication: Added Kerberos (POSIX) integrated authentication via system GSSAPI. SPNEGO + LOGIN7 `fIntSecurity` bit + 0xED SSPI continuation tokens. Self-contained test stack at `test/kerberos/`.
- 041-xml-type-support: Added C++17 (C++11-compatible for ODR on Linux) + DuckDB (main branch), OpenSSL (vcpkg), existing TDS protocol layer
- 040-fix-datetimeoffset-nbc: Added C++17 (C++11-compatible for ODR on Linux) + DuckDB (main branch), OpenSSL (vcpkg), custom TDS protocol layer
- 039-order-pushdown: Added C++17 (C++11-compatible for ODR on Linux) + DuckDB (main branch), OpenSSL (vcpkg), existing TDS protocol layer


<!-- SPECKIT START -->
Active spec: 068-login-routing-unification. See implementation plan at
`specs/068-login-routing-unification/plan.md` for technical context,
research findings, data model, contracts, and quickstart.
<!-- SPECKIT END -->
