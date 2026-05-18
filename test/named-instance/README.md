# Spec 046 — Named-Instance Resolution test stack

Self-contained docker-compose stack for exercising the SQL Server Browser
resolver (`InstanceResolver`) against a faithful mock Browser. No real
Windows, no real SQL Server Browser service, no fixed local port binding.
Works on macOS Apple Silicon, Linux x86_64, and Linux ARM64.

Modelled on [test/kerberos/](../kerberos/) (spec 042).

## What's here

```
test/named-instance/
├── docker-compose.yml
├── mock-browser/
│   ├── Dockerfile        # python:3.12-slim, ~5MB
│   └── browser.py        # MC-SQLR responder + fault-injection modes
├── sql/
│   ├── Dockerfile        # mssql/server:2022-latest, baked to listen on 11433
│   ├── entrypoint.sh     # background sqlservr + apply init.sql once
│   └── init.sql          # CREATE DATABASE NamedInstTest + dbo.Probe
├── test-client/
│   ├── Dockerfile        # ubuntu:24.04 multi-stage; builds resolver binary
│   └── run-tests.sh      # smoke test driver (mounted into the container)
└── README.md
```

## Usage

```bash
cd test/named-instance
docker compose up -d --build --wait    # ~5 min cold start (pulls SQL image)
docker compose exec test-client /run-tests.sh
docker compose down -v
```

> **Apple Silicon note**: `mcr.microsoft.com/mssql/server:2022-latest`
> ships an `amd64`-only manifest. On macOS arm64 hosts Docker Desktop
> runs it under Rosetta emulation. The stack works correctly but SQL
> Server boot is ~3× slower (~30 s vs ~10 s on native amd64). The
> `mock-browser` and `test-client` images are multi-arch and run
> natively. If you see a Docker warning about platform mismatch on the
> `sql` service, that's expected.

Expected output ends with:

```
[run-tests] ALL SMOKE TESTS PASSED
```

## Fault-injection (negative-path smoke)

The mock browser honours `MOCK_BROWSER_MODE`:

| Mode       | Behaviour                                                  | Resolver outcome                            |
|------------|------------------------------------------------------------|---------------------------------------------|
| `respond`  | (default) reply with a faithful SVR_RESP                   | `OK port=11433`                             |
| `silent`   | drop the request                                           | `FAIL Unreachable: ... after Ns (1 send + 1 retry)` |
| `garbage`  | reply with random bytes                                    | `FAIL Malformed: ...`                       |
| `truncate` | reply with header + half body                              | `FAIL Malformed: ...`                       |
| `slow`     | sleep 5s before replying (exceeds default 3s timeout)      | `FAIL Unreachable: ... timeout`             |

Set per-invocation via `docker compose`:

```bash
MOCK_BROWSER_MODE=silent docker compose up -d --build --wait
docker compose exec test-client /run-tests.sh   # expects Unreachable
```

The unit tests under `test/cpp/test_instance_resolver.cpp` already
cover every fault-injection mode in-process; the docker stack proves
the same behaviour holds with real Linux UDP networking between
containers.

## Why two hostnames

`browser.example.com` and `sql.example.com` are two separate Docker
DNS entries on the same internal bridge network. The mock advertises
`sql.example.com:11433` in its SVR_RESP payload; the resolver then
opens a TCP socket to that hostname (not back to `browser.example.com`).

This matches real-world Browser semantics — in failover-cluster setups
Browser can run on a separate host from the engine and advertise the
engine's hostname. Using a single alias would not exercise that path
and would mask cluster-related bugs.

## Why port 11433 (not 1433)

If SQL Server listened on the default 1433, a test could pass without
the resolver ever doing anything (the client might just connect to
1433 by default). Pinning the engine to 11433 means any successful
attach proves the resolver translated `TESTINST → 11433`. The mock's
`MOCK_BROWSER_INSTANCES` env var advertises 11433 by default.

## Phase 2 scope vs Phase 3

This stack and its `run-tests.sh` exercise the C++ resolver binary
directly — the **DuckDB extension's ATTACH path is NOT yet wired to
call the resolver** (that's Phase 3 / PR #2). So the stack proves the
mock is wire-compatible with the resolver, and that the resolver works
end-to-end through real Linux UDP networking, but it cannot yet exercise
`ATTACH 'Server=sql.example.com\TESTINST;...'`.

When Phase 3 lands, `test-client/Dockerfile` should grow a stage that
builds the full extension + DuckDB CLI (mirroring
`test/kerberos/test-client/Dockerfile`), and `run-tests.sh` should add
SQL-level ATTACH tests reading from `dbo.Probe`.

## Troubleshooting

- **`compose up` hangs on mock-browser**: check `docker compose logs mock-browser`. If it shows nothing, the bind on 1434/udp may have failed (port conflict — though that would be unusual since 1434 is exposed only inside the compose network).
- **SQL Server healthcheck fails after `start_period`**: `docker compose logs sql` — first-boot Developer edition takes ~30s to initialise on slow hosts. Increase `start_period` if needed.
- **resolver says `DNS lookup failed for 'browser.example.com'`**: networking issue — usually fixed by `docker compose down -v && docker compose up -d --build --wait` to rebuild the bridge network.
