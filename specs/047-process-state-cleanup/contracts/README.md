# Spec 047 — Contracts

Spec 047 is a refactor of internal C++ ownership. There are **no new public interfaces** — all extension functions (`mssql_scan`, `mssql_exec`, `mssql_pool_stats`, `mssql_refresh_cache`, `mssql_open`, `mssql_close`, `mssql_ping`, `mssql_preload_catalog`, etc.) keep their signatures and observable semantics (per Constraint #1 + SC-007).

## Internal API additions (header-only sketches)

These are sketches of the internal C++ changes; final headers live under `src/include/` and will follow the project's house style (snake_case_ trailing underscore for member vars; PascalCase methods; namespace-prefix rule per CLAUDE.md).

### `MSSQLCatalog` — new members + methods

Added to `src/include/catalog/mssql_catalog.hpp`:

```cpp
// Type change (was `shared_ptr<tds::ConnectionPool>` with no-op deleter)
private:
    std::unique_ptr<tds::ConnectionPool> connection_pool_;

    // FR-009: result stream registry moved from MSSQLResultStreamRegistry
    std::mutex streams_mutex_;
    std::unordered_map<std::string, std::unique_ptr<MSSQLResultStream>> active_streams_;

public:
    // Existing accessor — unchanged signature; now returns reference to the
    // unique_ptr-owned pool instead of the shared_ptr-aliased pool.
    tds::ConnectionPool &GetConnectionPool();

    // FR-009: new bridge methods for mssql_scan Bind→InitGlobal handoff.
    // RegisterStream generates a UUID, inserts, returns the UUID.
    std::string RegisterStream(std::unique_ptr<MSSQLResultStream> stream);
    // RetrieveStream is atomic find+erase+return; returns nullptr if not found.
    std::unique_ptr<MSSQLResultStream> RetrieveStream(const std::string &uuid);
```

### `tds::ConnectionPool` — pin counter migration

Added to `src/include/tds/tds_connection_pool.hpp` (FR-004):

```cpp
private:
    std::atomic<int64_t> pinned_count_{0};

public:
    void IncrementPinned();      // pinned_count_.fetch_add(1, std::memory_order_relaxed)
    void DecrementPinned();      // pinned_count_.fetch_sub(1, std::memory_order_relaxed)
    int64_t GetPinnedCount() const noexcept;  // pinned_count_.load(...)
```

`PoolStatistics` struct gains `int64_t pinned_count` field (sourced from `GetPinnedCount()`).

### ATTACH option additions (FR-011)

`mssql_storage.cpp` `MSSQLAttach` parses two new options:

- `lazy_validation` (BOOL, default `false`) — when `true`, skips eager credential validation; ATTACH succeeds even with bad credentials; bad credentials surface on first query (today's behavior).
- ADO.NET alias: `LazyValidation` (same semantics).

Plus a new pool setting:
- `mssql_attach_validation_timeout` (INTEGER seconds, default = value of `mssql_connection_timeout`) — timeout for the `pool.Acquire()` call during eager validation.

### Deleted symbols (SC-004 grep gate)

These three production-source symbols must have zero matches in `src/` + `src/include/` after spec 047:

- `MssqlPoolManager`
- `MSSQLContextManager`
- `MSSQLResultStreamRegistry`

(Plus the file `src/connection/mssql_pool_manager.{cpp,hpp}` and the corresponding entries in `CMakeLists.txt`.)

`MSSQLConnectionHandleManager` is NOT in the deleted set — it stays (legitimate, deprecated functions surround it).
