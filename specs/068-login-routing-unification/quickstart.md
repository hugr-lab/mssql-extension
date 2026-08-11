# Quickstart — spec 068

How to build, run, and observe the routing work. Everything below runs without
an Azure subscription or an Active Directory; the two live checks are listed
last and are optional for development.

## 0. Branch state

The spec text arrives from PR #257 (`spec/068-login-routing`); implementation
happens on this worktree's branch. The base must include PR #254/#255 — the
DONE-token size fix — because D4's "ROUTING behind a DONEINPROC run" pin only
means anything on top of it.

```bash
git log --oneline -1            # expect 571e465 or later
```

## 1. Parser pins — no server, no DuckDB, ~10 s

The fastest loop while working on the D1 ordering. Extends the existing file.

```bash
make test-login-error-state
```

Or the manual compile (what CI does, mirrored in the file's header comment):

```bash
c++ -std=c++17 -pthread -I src/include -I duckdb/src/include \
    -I /opt/homebrew/opt/simdutf/include \
    test/cpp/test_login_error_state.cpp \
    src/tds/tds_packet.cpp src/tds/tds_protocol.cpp src/tds/tds_types.cpp \
    src/tds/encoding/utf16.cpp \
    -L /opt/homebrew/opt/simdutf/lib -lsimdutf \
    -o build/test/test_login_error_state && ./build/test/test_login_error_state
```

New cases to expect green: route-with-LOGINACK, route-without-LOGINACK, routed
target field extraction, ROUTING behind a DONEINPROC run.

## 2. Hop-driver pins — loopback fake TDS server, no DuckDB, ~15 s

```bash
make test-login-routing-hops
```

The target compiles the TDS layer only (see research.md R3): connection +
socket + TLS TUs + protocol + simdutf + OpenSSL. Two listeners bind
`127.0.0.1:0`; the test reads the assigned ports back, so nothing collides with
a developer's SQL Server or another CI job.

Debugging a failure:

```bash
MSSQL_DEBUG=2 ./build/test/test_login_routing_hops
```

Level 1 prints the `ROUTING: parsed server=…` line from the parser and the
driver's per-hop line; level 2 adds the response hex dump. Note the fake server
speaks `use_encrypt=false`, so there is no TLS to unwrap in the dump.

## 3. Full unit suite + build

```bash
make            # release build (vcpkg TLS)
make test       # unit tests, no SQL Server
```

On macOS run `make vcpkg-setup` first if this is a fresh tree, or the simdutf /
C++17 conflict bites at `utf16.cpp`.

## 4. Regression against a real (non-routing) server

The docker SQL Server does not route, which is the point: it proves the
zero-hop path is unchanged.

```bash
make docker-up
make integration-test          # .test files actually run only via this target
make docker-down
```

Watch for: same login latency, `mssql_pool_stats` unchanged, no new lines at
`MSSQL_DEBUG=1` during connect.

## 5. Integrated-auth refactor — self-contained KDC, no AD needed

```bash
cd test/kerberos
docker compose up -d --build
docker compose exec test-client /run-tests.sh
docker compose down -v
```

This exercises the D3 signature change end to end (fresh authenticator per
connection, SPN `MSSQLSvc/sql.example.com:1433`). It does **not** exercise a
hop — no routing front-end exists in the stack. That gap is the spec's
acknowledged risk; the mitigation is that the hop failure path names the SPN it
tried.

## 6. Live smoke (optional, maintainer-only)

- **Azure**: dispatch the Azure Test workflow on the branch. Whether the gateway
  routes depends on the subscription's connection policy; either way it
  regression-checks the fedauth path that the refactor moved.
- **Manual routing check** on any Azure SQL DB, from inside Azure (Redirect
  policy) or with the policy set to Redirect:

  ```sql
  ATTACH 'Server=<name>.database.windows.net;Database=<db>;User Id=<u>;Password=<p>;Encrypt=yes'
      AS azdb (TYPE mssql);
  ```

  With `MSSQL_DEBUG=1` a routed login prints `ROUTING: parsed server=…` followed
  by the driver's hop line. Before this spec, SQL auth against a routing
  endpoint printed an authentication failure instead.

## 7. Docs to update in the same PR

`docs/tds-protocol.md`, `docs/architecture.md`, `Kerberos.md`, `DATAMODEL.md`
(the connection layer's login flow becomes a hop loop — CLAUDE.md makes this
mandatory when an end-to-end flow changes), plus a changelog line for the
SQL-auth behaviour change on route-with-LOGINACK.
