# mssql-extension Development Guidelines

DuckDB extension for SQL Server via a custom TDS protocol implementation (no FreeTDS/ODBC).

## Technology

- **Language**: C++17 (DuckDB extension standard)
- **DuckDB**: main branch (2.0 API track, spec 069). The 1.5.x world lives on branch `duckdb-v1.5.5` for 0.2.x maintenance releases; main compiles against the pinned duckdb submodule SHA only (no dual-API shim since spec 069 retired `mssql_compat.hpp`)
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
make docker-up          # Start SQL Server container
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
- **Catalog integration**: DuckDB Catalog/Schema/Table APIs with incremental metadata cache (lazy loading, TTL-based expiration, point invalidation), primary key discovery for rowid support.
- **DML**: INSERT uses batched VALUES; UPDATE/DELETE use rowid-based VALUES JOIN pattern with deferred execution in transactions.
- **DDL**: `CREATE TABLE IF NOT EXISTS` silently succeeds when table exists; `CREATE OR REPLACE TABLE` drops and recreates. Auto-TABLOCK enabled for new table creation (CTAS/COPY TO).
- **Filter pushdown**: DuckDB filter expressions translated to T-SQL WHERE clauses. Function mapping for common string/date/arithmetic operations.
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
| `mssql_connection_limit` | 64 | Max connections per context |
| `mssql_connection_timeout` | 30 | TCP timeout (seconds) |
| `mssql_idle_timeout` | 300 | Idle connection timeout (seconds) |
| `mssql_min_connections` | 0 | Min connections to maintain |
| `mssql_acquire_timeout` | 30 | Connection acquire timeout (seconds) |
| `mssql_connection_cache` | true | Enable connection pooling |
| `mssql_reset_connection` | true | Reset a pooled connection's **session** before the next statement uses it (the TDS `RESET_CONNECTION` bit — what `sp_reset_connection` does). This is why a `##global` temp table does not survive between statements: it lives as long as its creating session, and the reset ends that session on the same physical connection (`@@SPID` unchanged). There is no selective form — one bit, two variants, and `RESET_CONNECTION_SKIP_TRAN` drops `##g` and `#loc` alike (issue #189). `false` is not "keep my temp tables" but "**I own this session's state**": `SET` options, isolation level, session variables, `CONTEXT_INFO`, cursors — and an open transaction that then keeps its locks until that connection is used again. Honoured by every release path (autocommit release — which is what `mssql_exec` uses —, result-stream close, COMMIT, ROLLBACK); for the transaction paths it is captured at BEGIN, because `TransactionManager::RollbackTransaction` gets no `ClientContext`. Also honoured by `ReleaseBcpConnectionOnError`, the mid-bulk-load failure path: it has no `ClientContext` by design (issue #178, runs from destructors on worker threads), so the answer is carried to it — and its `reset_on_release` parameter has **no default**, so a new caller cannot silently skip the question. Prerequisite for it: the write path's target policy already refuses a second bulk-load writer against a session-scoped `#temp` target (spec 063 D1) — without that, turning the reset off lets a parallel writer find a **stale same-named** temp table on another pooled session and land rows there silently. |
| `mssql_metadata_timeout` | 300 | Metadata query timeout in seconds (0 = no timeout) |
| `mssql_attach_validation_timeout` | 0 | ATTACH-time eager credential validation timeout in seconds (0 = inherit `mssql_connection_timeout`). Spec 047 FR-011. |
| `mssql_catalog_cache_ttl` | 0 | Metadata cache TTL (0 = manual via `mssql_refresh_cache()`) |
| `mssql_exec_invalidate_cache` | false | Auto-invalidate the catalog cache after DDL run via `mssql_exec()` (CREATE/DROP/ALTER/TRUNCATE/RENAME/EXEC). Default `false` (like Postgres `postgres_execute`): invalidate manually with `mssql_invalidate_cache()`. Set `true` to auto-invalidate. Issue #151. |
| `mssql_insert_batch_size` | 1000 | Rows per INSERT statement |
| `mssql_insert_max_rows_per_statement` | 1000 | Hard cap per INSERT |
| `mssql_insert_max_sql_bytes` | 8MB (8388608) | INSERT SQL size limit |
| `mssql_insert_use_returning_output` | true | Use OUTPUT INSERTED for RETURNING |
| `mssql_dml_batch_size` | 500 | Rows per UPDATE/DELETE batch |
| `mssql_dml_max_parameters` | 2000 | Max parameters per UPDATE/DELETE statement |
| `mssql_dml_use_prepared` | true | Use prepared statements for DML |
| `mssql_enable_statistics` | true | Report each MSSQL scan's row count to the DuckDB optimizer (`TableFunction::cardinality`). Until the roadmap's "step 0" callback landed this setting was read by **nothing**, and every MSSQL scan planned as `~1 row` — a 200000-row table and a 50-row table looked identical, so join order and build-side choice around them were arbitrary. The estimate is free at plan time: statistics cache, then the count the catalog already loaded; it never opens a connection to plan a query, and reports **nothing** rather than 0 when the count is unknown (a VIEW has no `sys.partitions` rows and reports 0 while returning millions). Set `false` to restore the estimate-less behaviour. |
| `mssql_statistics_cache_ttl_seconds` | 300 | How long a cached row count stays usable — read at the point of use, from the SESSION that asks, never written into the shared per-catalog provider (one session's `SET` must not govern another's plans). It governs the PLANNER's cardinality lookup. It does **not** age out a count the CATALOG loaded (`PreloadRowCount`) on the table-LISTING path: `SHOW ALL TABLES` / `duckdb_tables()` exempt those deliberately, because ageing them would cost a connection plus a `sys.dm_db_partition_stats` round trip **per table** on a catalog that had just loaded every count in one query. Such entries are refreshed by invalidation instead — `mssql_invalidate_cache()`, DDL, COPY/CTAS — not by time. A DMV-sourced count ages normally on every path. |
| `mssql_copy_flush_rows` | 102400 | Rows per bulk-load batch — the boundary the **server** sees between DONE tokens, not a client buffer size. 102400 is SQL Server's own threshold for writing a batch straight into a **compressed columnstore rowgroup**; below it every row goes to the delta store and stays there (a delta rowgroup only closes on its own at 1048576 rows), so a smaller load never compresses at all. Measured on 1M rows — one OPEN rowgroup and 53 MB at the old 100000 default, nine COMPRESSED rowgroups and 7 MB at 102400 (spec 057). Raised from 100000, which silently defeated `table_kind = 'columnstore'`. One value for every target: batch size measured nearly flat on a heap, so the extra 2400 rows cost nothing there. |
| `mssql_copy_parallel_writers` | 0 | Concurrent bulk-load connections a single COPY **or CTAS** may open (spec 057 step 7). `0` derives it from DuckDB's thread count, capped at 8; `1` disables parallel loading. Each writer is an independent `INSERT BULK` on its own pooled connection — the bound on this path is SQL Server's **ingest** rate (`send()` blocks because the receive window stays full while the server lays rows down), and the server parallelises across sessions, so more sessions is the only lever that moves it. Measured on 44 columns x 1M rows: 10.55 s at 1 writer, **3.24 s at 4**, 3.51 s at 8 — it plateaus past 4. **Ignored inside an explicit transaction**, where the connection is pinned: a second connection would sit outside the transaction and its rows would not roll back with the rest. A **ceiling, not a floor**, in two more ways: one DuckDB thread driving the sink yields one writer whatever this says (which is why the parallel tests pin `SET threads`), and against a **clustered columnstore** target the extra writers are held back until the load has sunk one full 102400-row rowgroup on the shared writer (spec 070 W2) — so a small columnstore load legitimately finishes on one writer rather than splitting into batches too small to compress. Heap targets fan out immediately. The warm-up threshold is SQL Server's own rowgroup constant, not `mssql_copy_flush_rows`. |
| `mssql_copy_tablock` | `auto` | `auto` \| `true` \| `false`. `auto` decides from the target's shape: **heap on**, **anything clustered off** (rowstore *and* columnstore) — concurrent bulk loaders on a heap take mutually compatible BU locks so the hint lets them run together, while against a clustered rowstore index the same hint serialises them. Replaces the "enable for newly created tables" rule of issue #45, which never actually fired: the flag it tested was set from `TryGetCurrentSetting` succeeding, which it always does (spec 057). The columnstore half of this policy was wrong until spec 057 step 7 made the loads concurrent and exposed it: TABLOCK **serialises** them. Measured on 2M rows, clustered columnstore — hint ON 8.92 s with SQL Server pinned at ~99% CPU (exactly one core), hint OFF 5.23 s peaking at 305% (three cores), and **identical compression either way** (17 COMPRESSED rowgroups, 1740800 rows, both). What decides whether rows land compressed is `mssql_copy_flush_rows` crossing 102400, not the lock. |
| `mssql_utf8_collation` | `Latin1_General_100_BIN2_UTF8` | Collation given to VARCHAR columns created by CTAS when `mssql_ctas_text_type` is `VARCHAR` **and** the server granted `UTF8SUPPORT` at login (issue #225). Without a UTF-8 collation such a column takes the database's code page and SQL Server replaces everything outside it with `?` **on insert**, silently. BIN2 matches what Fabric Warehouse uses as its own default: binary comparison, no linguistic rules — and therefore case- and accent-SENSITIVE, so `WHERE name = 'abc'` stops matching `'ABC'`. Per-column override is the annotation `x::MSSQL_VARCHAR(n, 'collation')`; this setting moves it globally. The collation is stored in the schema and governs every later comparison against the column. Empty adds no `COLLATE` clause, so the column inherits the database default: correct when that default is already UTF-8 (Fabric), and the way back to the pre-#225 behaviour otherwise. |
| `mssql_catalog_native_types` | true | Report `MSSQL_VARCHAR(n)` / `MSSQL_NVARCHAR(n)` for the string columns of attached tables instead of a bare `VARCHAR` (spec 060). This is what lets a target created from an MSSQL source inherit its declared lengths and collations with no cast written by anyone. Costs a changed `DESCRIBE` / `duckdb_columns()` type name, which is what turning it off restores. Read when a table entry is built, so `mssql_invalidate_cache()` applies a change to already-loaded tables. |
| `mssql_default_string_length` | 0 | Length given to an **unannotated** VARCHAR column created by CTAS or COPY; 0 = MAX, which is what a plain VARCHAR has always meant. Set it and the extension's own overflow guard enforces it before the batch is sent. Above SQL Server's inline limit (4000 nvarchar / 8000 varchar) the column stays MAX. Per-column control is a cast to `MSSQL_NVARCHAR(n)` / `MSSQL_VARCHAR(n)`. |
| `mssql_default_table_kind` | `HEAP` | Shape of a table created by CTAS/COPY: `HEAP` or `COLUMNSTORE`. `COLUMNSTORE` runs `CREATE CLUSTERED COLUMNSTORE INDEX` right after the CREATE and **before** the load, so the load writes compressed rowgroups directly instead of needing a rebuild. Per-statement: the COPY `table_kind` option, or `CREATE TABLE ... WITH (table_kind = '...')`. |
| `mssql_ctas_use_bcp` | true | Use BCP protocol for CTAS data transfer (2-10x faster than INSERT) |
| `mssql_convert_varchar_max` | true | Convert VARCHAR(MAX) to NVARCHAR(MAX) in catalog queries for UTF-8 compatibility |
| `mssql_named_instance_resolution` | true | Resolve `Server=host\instance` to the instance's dynamic TCP port via the SQL Server Browser (UDP 1434) at ATTACH time (spec 045). Set `false` in environments that strip outbound UDP 1434 — a named instance then errors instead of silently using port 1433, and you connect with an explicit `Server=host,port`. |
| `mssql_browser_timeout_seconds` | 3 | SQL Server Browser UDP query timeout in seconds for named-instance resolution. On the ATTACH critical path, so kept short; the resolver retries once. |
| `mssql_tds_packet_size` | 16384 | TDS frame size requested in LOGIN7, clamped to [512, 32767]. The server answers with `min(requested, its own maximum)` and never raises it, so this is the ceiling for every packet in both directions — it bounds recv() count on reads and send() count on BCP writes. Raised from the long-standing 4096 in spec 055: measured −28% client CPU / −43% wall on read and −27% CPU on write (`test/bench/bench_results_live_server.md`). Costs server memory per session (16 KB vs 4 KB per pooled connection); set to 4096 to restore the old behaviour. |
| `mssql_utf8_support` | true | Advertise the TDS `UTF8SUPPORT` feature extension (0x0A) in LOGIN7 (issue #225). A server that acknowledges it sends columns whose collation is a UTF-8 one as UTF-8 (`0xA7`) instead of transcoding them to UTF-16 (`0xE7`): measured 85.8 MB -> 43.9 MB on the wire and 0.44 s -> 0.25 s wall for 1M ~41-char values, because the client copies the bytes into the vector instead of running the UTF-16 batch decode. Requesting it is safe on any server — one that lacks the feature omits the acknowledgement and nothing changes — so this setting exists to turn the request **off**. |
| `mssql_login7_max_packet` | 0 | **Test-only** (issue #138). Max LOGIN7 TDS packet size (bytes) for integrated auth; lowers the fragmentation boundary so the multi-packet send path can be exercised without an AD-sized Kerberos PAC. 0 = production default (4096); effective values clamped to [256, 32767]. |

## ATTACH Options & Secret Parameters (Catalog Filters)

| Parameter | Type | Description |
|-----------|------|-------------|
| `schema_filter` | VARCHAR | Regex pattern to filter visible schemas (case-insensitive, partial match via `regex_search`) |
| `table_filter` | VARCHAR | Regex pattern to filter visible tables/views (case-insensitive, partial match via `regex_search`) |
| `lazy_validation` (or `LazyValidation`) | BOOLEAN | Skip the eager ATTACH-time TCP+LOGIN7 credential check. Default `false` (eager — wrong creds / unreachable host surface as ATTACH errors). Set `true` for container/orchestrator startup where the SQL Server may not yet be reachable; first query then pays the connection-establishment cost as in the pre-spec-047 behaviour. Bounded by `mssql_attach_validation_timeout`. Spec 047 FR-011. |
| `azure_tenant_id` (or `azure_tenant`) | VARCHAR | Tenant to acquire the Azure AD token against, overriding the tenant carried by the `azure_secret`. Registered as an MSSQL-secret parameter since spec 032 and **read by nothing** until 2026-08-14 (PR #264 review): with no override, `credential_chain` + `interactive` falls back to `AZURE_DEFAULT_TENANT` = `common` inside `azure_device_code.cpp`, so every interactive ATTACH in a single-tenant org authenticated against `/common/` with no way to say otherwise. `TENANT_ID` on the *azure* secret is not the way to do this — duckdb-azure rejects it on `provider='credential_chain'`. The value is part of the `TokenCache` key, so two tenants do not share a token. |
| `Application Name` / `ApplicationName` / `App Name` / `application_name` | VARCHAR | LOGIN7 `program_name` propagated to SQL Server (visible via `APP_NAME()` / `sys.dm_exec_sessions.program_name`). URI form uses spaceless `applicationname` query parameter; secret form uses `application_name` (canonical) or `applicationname`. Empty falls back to `"DuckDB MSSQL Extension"`; values longer than 128 chars are clamped client-side. Closes [issue #82](https://github.com/hugr-lab/mssql-extension/issues/82) (spec 047 FR-014). |

Available in: ATTACH options, ADO.NET connection strings (`SchemaFilter`/`TableFilter`), URI query parameters, and MSSQL secrets. ATTACH options override secret/connection string values.

## Extension Functions

| Function | Type | Description |
|----------|------|-------------|
| `mssql_scan(context, query)` | Table | Execute raw T-SQL, stream results |
| `mssql_exec(context, sql)` | Scalar | Execute T-SQL, return affected row count |
| `mssql_pool_stats([context])` | Table | View connection pool statistics |
| `mssql_refresh_cache(context)` | Scalar | Refresh metadata cache (eager full reload) |
| `mssql_invalidate_cache(context [, schema [, table]])` | Scalar | Lazy point invalidation — whole catalog / one schema / one table (keeps other tables' cached columns) |
| `mssql_preload_catalog(context [, schema])` | Scalar | Bulk-load all metadata in one round trip |
| `mssql_open(conn_string)` | Scalar | [DEPRECATED] Open standalone diagnostic connection (spec 047 FR-010 — prefer ATTACH + catalog-bound functions) |
| `mssql_close(handle)` | Scalar | [DEPRECATED] Close diagnostic connection (spec 047 FR-010) |
| `mssql_ping(handle)` | Scalar | [DEPRECATED] Test connection liveness (spec 047 FR-010) |
| `mssql_close_all()` | Scalar | [DEPRECATED] Close every open `mssql_open` handle in one shot; returns the count of handles closed. Recommended shutdown hook for hosts using the diagnostic API. (spec 047 FR-013) |
| `mssql_azure_auth_test(secret, tenant?)` | Scalar | Test Azure AD token acquisition |
| `mssql_kerberos_auth_test(host [, port])` | Scalar | Test POSIX Kerberos auth path (spec 042); returns OK + SPN / principal / token size, or verbatim GSSAPI error |
| `mssql_kerberos_auth_test_secret(secret_name)` | Scalar | Same but reads keytab / SPN-override / etc. from an MSSQL secret |
| `mssql_winsspi_auth_test(host [, port])` | Scalar | Windows SSPI peer of `mssql_kerberos_auth_test` (spec 042 Phase 4); returns OK + SPN / UPN / token size, or verbatim SSPI error |
| `mssql_winsspi_auth_test_spn(spn)` | Scalar | Same but takes an explicit SPN (overrides default `MSSQLSvc/<host>:<port>` derivation) |

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
