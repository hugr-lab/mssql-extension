# Spec 047 — Quickstart (local validation)

How to verify spec 047's fixes locally during implementation. Read this *before* starting Phase 1 — these are the commands the SC gates will run, so wire them up early.

## Prerequisites

- macOS / Linux dev host (Windows works too, but commands below use POSIX paths)
- Docker running (for SQL Server test container)
- DuckDB submodule initialized
- `make docker-up` succeeds, `make docker-status` shows healthy
- `set -a && source .env && set +a` exports `MSSQL_TEST_*` credentials

## Pre-flight build

```bash
GEN=ninja make debug
```

## Reproduce the bugs (baseline — pre-spec-047)

Confirm these fail on `main` before starting implementation so you have a "before" baseline.

### Issue #96 — Python loop ("Context already exists")

```bash
python3 -c "
import duckdb
sql = '''
INSTALL mssql FROM local_build_debug;
LOAD mssql;
CREATE OR REPLACE SECRET s (TYPE mssql, HOST 'localhost', PORT 1433, DATABASE 'TestDB', USER 'sa', PASSWORD 'TPassw0rd!', USE_ENCRYPT FALSE);
ATTACH '' AS TO_MSSQL (TYPE mssql, SECRET s);
'''
for i in range(1, 4):
    con = duckdb.connect(':memory:')
    try:
        con.execute(sql); print(f'iter {i}: ok')
    except Exception as e:
        print(f'iter {i}: ERROR — {e}')
    finally:
        con.close()
"
# Today: iter 1 ok, iter 2+ "Context 'TO_MSSQL' already exists".
# After 047: all 3 iterations ok.
```

### ATTACH credential validation (SC-010 baseline)

```bash
./build/debug/duckdb -unsigned -c "
    INSTALL mssql FROM local_build_debug;
    LOAD mssql;
    ATTACH 'Server=localhost,1433;Database=TestDB;User Id=sa;Password=WRONG' AS bad (TYPE mssql);
    SELECT 'attach passed silently' AS status;
    SELECT * FROM bad.dbo.TestSimplePK LIMIT 1;  -- THIS is where today's error appears
"
# Today: ATTACH passes, SELECT fails with "Login failed for user 'sa'".
# After 047: ATTACH fails immediately with the login error.
```

### Silent-shutdown reliability (SC-003 baseline)

```bash
for i in $(seq 1 100); do
    ./build/debug/duckdb -unsigned -c "
        INSTALL mssql FROM local_build_debug;
        LOAD mssql;
        ATTACH 'Server=localhost,1433;Database=TestDB;User Id=sa;Password=TPassw0rd!' AS db (TYPE mssql);
        SELECT 1 FROM db.dbo.TestSimplePK LIMIT 1;
    " > /dev/null
done
sleep 1
docker exec mssql-dev /opt/mssql-tools/bin/sqlcmd -S localhost -U sa -P TPassw0rd! -Q "
    SELECT COUNT(*) AS leaked_sockets
    FROM sys.dm_exec_connections
    WHERE program_name LIKE '%DuckDB%'
" -h -1 -W
# Today: leaked sockets > 0.
# After 047: leaked sockets = 0.
```

## Validate the fix (per-phase)

### Phase 1 (per-catalog pool ownership + ATTACH validation)

```bash
GEN=ninja make debug 2>&1 | tail -5
GEN=ninja make test
GEN=ninja make integration-test

# SC-010 spot check
./build/debug/duckdb -unsigned -c "
    INSTALL mssql FROM local_build_debug; LOAD mssql;
    ATTACH 'Server=localhost,1433;Database=TestDB;User Id=sa;Password=WRONG' AS bad (TYPE mssql);
"
# Expected: ATTACH throws "Login failed for user 'sa'..." (or similar verbatim TDS).
```

### Phase 2 (diagnostic enumeration via catalog list)

```bash
./build/debug/duckdb -unsigned -c "
    INSTALL mssql FROM local_build_debug; LOAD mssql;
    ATTACH 'Server=localhost,1433;Database=TestDB;User Id=sa;Password=TPassw0rd!' AS a (TYPE mssql);
    ATTACH 'Server=localhost,1433;Database=TestDB;User Id=sa;Password=TPassw0rd!' AS b (TYPE mssql);
    SELECT context_name, total_connections, idle_connections, pinned_count FROM mssql_pool_stats();
"
# Expected: 2 rows (a, b). Today (singleton): 1 row (dedup'd by name).
```

### Phase 3 (g_context_managers removed)

```bash
grep -rn "g_context_managers\|MSSQLContextManager" src/ src/include/
# Expected: 0 matches.
```

### Phase 4 (result stream registry → catalog)

```bash
grep -rn "MSSQLResultStreamRegistry" src/ src/include/
# Expected: 0 matches.

# Functional smoke — mssql_scan still works
./build/debug/duckdb -unsigned -c "
    INSTALL mssql FROM local_build_debug; LOAD mssql;
    ATTACH 'Server=localhost,1433;Database=TestDB;User Id=sa;Password=TPassw0rd!' AS db (TYPE mssql);
    SELECT * FROM mssql_scan('db', 'SELECT TOP 5 * FROM dbo.TestSimplePK') LIMIT 5;
"
```

### Phase 5 (multi-instance + issue #96 tests)

```bash
./build/debug/test/unittest "test_multi_instance_pool_isolation*"   # 3 scenarios green
./build/debug/test/unittest "test_issue_96_attach_loop*"            # closes #96
./build/debug/test/unittest "test/sql/attach/attach_validates_credentials.test"  # SC-010
```

### Phase 6 (polish + bench)

```bash
# SC-004 grep gate
grep -rn 'MssqlPoolManager\|MSSQLContextManager\|MSSQLResultStreamRegistry' src/ src/include/
# Expected: 0 matches.
# NOTE: MSSQLConnectionHandleManager NOT in this grep — it stays (legitimate, deprecated functions).

# Bench parity (vs main-at-kickoff baseline saved in bench_results.md)
MSSQL_BENCH_ROW_COUNT=1000000 \
MSSQL_BENCH_DUCKDB_BIN=$(pwd)/build/release/duckdb \
MSSQL_BENCH_OUTPUT=/tmp/bench_codec_e2e_spec047_run1.txt \
bash test/bench/bench_codec_e2e.sh

# Deprecation markers in place
./build/debug/duckdb -unsigned -c "
    INSTALL mssql FROM local_build_debug; LOAD mssql;
    SELECT function_name, description FROM duckdb_functions()
    WHERE function_name IN ('mssql_open','mssql_close','mssql_ping');
"
# Expected: each description starts with "[DEPRECATED]".

# State inventory deliverable
cat specs/047-process-state-cleanup/state_inventory.md
# Expected: zero "migrate" entries; legitimate set = TokenCache + handle manager + Winsock + OpenSSL + thread-locals.
```

## SC checklist (before opening PR)

| SC | How to verify |
|---|---|
| SC-001 | `./build/debug/test/unittest "test_multi_instance_pool_isolation/scenario_1*"` |
| SC-002 | `./build/debug/test/unittest "test_multi_instance_pool_isolation/scenario_2*"` |
| SC-003 | `./build/debug/test/unittest "test_multi_instance_pool_isolation/scenario_3*"` + manual leaked-sockets check above |
| SC-004 | `grep -rn 'MssqlPoolManager\|MSSQLContextManager\|MSSQLResultStreamRegistry' src/ src/include/` → 0 |
| SC-005 | Phase 2 manual check above (2 attaches → 2 rows in `mssql_pool_stats()`) |
| SC-006 | (Dropped — handle-isolation requirement is moot now that the handle manager stays singleton) |
| SC-007 | `GEN=ninja make test && GEN=ninja make integration-test` — full green |
| SC-008 | `cat specs/047-process-state-cleanup/state_inventory.md` — zero "migrate" entries |
| SC-009 | `./build/debug/test/unittest "test_issue_96_attach_loop*"` |
| SC-010 | `./build/debug/test/unittest "test/sql/attach/attach_validates_credentials.test"` |

## Common pitfalls

- **ATTACH validation timeout**: don't block ATTACH indefinitely on `pool.Acquire()`. Honor `mssql_attach_validation_timeout` (default = `mssql_connection_timeout`) so an unreachable host fails ATTACH within seconds, not forever.
- **Validation acquire MUST return the connection to pool**: don't leak. Use a scope guard / `Pool::ConnectionHandle` RAII (whatever idiom matches the pool's existing API) so even an exception in the next step releases the connection.
- **`lazy_validation` option must be respected by all 3 auth paths**: SQL auth, FEDAUTH, Integrated. Don't hard-code eager validation.
- **Result stream registry move**: `mssql_scan` Bind already does a `manager.HasContext(bind_data->context_name)` check at line 121 today; after Phase 4 that becomes a catalog lookup. Make sure the catalog reference acquired in Bind survives into the `RegisterStream(...)` call.
- **TokenCache + HandleManager NOT in SC-004 grep** — they're legitimate. Don't accidentally include them in the "delete all singletons" sweep.
