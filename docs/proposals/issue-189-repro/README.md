# Issue #189 reproduction

Demonstrates the four failure modes described in
[issue #189](https://github.com/hugr-lab/mssql-extension/issues/189) and analyzed in
[../issue-189-connection-lease-proposal.md](../issue-189-connection-lease-proposal.md):
there is no connection lifetime a shared temp table (`#`/`##`) can anchor to.

This is a **sidecar only** — it reuses the repo's standard SQL Server dev container
(`docker/docker-compose.yml`) rather than provisioning its own, so there's nothing new
to keep in sync.

## Run it

From the repo root:

```bash
make docker-up                                            # starts + initializes mssql-dev
docker compose -f docs/proposals/issue-189-repro/docker-compose.yml run --rm repro
make docker-down                                           # when done
```

Exit code `0` means all four failure modes were reproduced (i.e. the limitation
described in #189 is present). A non-zero exit, or fewer than 4/4 in the summary line,
means something reproduced differently than expected — worth a closer look.

By default the sidecar installs the released **community** `mssql` extension (what
users actually run). To point it at a local Linux build instead, uncomment the volume
mount and `MSSQL_EXTENSION_PATH` line in `docker-compose.yml`:

```yaml
    volumes:
      - ./repro.py:/repro.py:ro
      - ../../../build/release/extension/mssql:/ext:ro
    environment:
      - MSSQL_EXTENSION_PATH=/ext/mssql.duckdb_extension
```

(The build must target `linux_amd64` — the sidecar image is `python:3.12-slim`
regardless of host architecture. `docker/docker-compose.linux-ci.yml` produces one.)

## What it checks

| ID | Scenario | Expected today |
|----|---|---|
| R1 | `mssql_exec` creates `##shared` (autocommit), then poll `tempdb.sys.tables` | Table dropped as soon as the creating pooled connection is next reused |
| R2 | `mssql_exec` creates `#local` (autocommit), then a second statement reads it | `Invalid object name '#local'` — no pinning outside a transaction |
| R3 | `BEGIN` → create `##tx` → read it (same pinned connection) → `COMMIT` → poll | Readable inside the transaction; dropped within 1 pool statement after COMMIT |
| R4 | Create `##locked` in an **open** transaction; a second connection reads it while a timer commits after 3s | Second connection blocks on the Sch-M lock until the COMMIT lands |

## Notes

* The compose file joins the network `make docker-up` creates (project name `docker`
  → network `docker_default`). If you run the dev container under a different compose
  project name, override it: `REPRO_NETWORK=myproject_default docker compose ... run --rm repro`.
* The sidecar creates its own `Repro189` database (drop + recreate on each run) so it
  never touches the dev container's `TestDB` fixtures.
* The container installs `libgssapi-krb5-2` at startup — the released 0.2.1-era Linux
  binary (pre-[spec 053](../../../specs/053-lazy-gssapi-linking/plan.md)) still
  hard-links `libgssapi_krb5.so.2`, which the slim Python image doesn't ship.
* This stack doubles as the fix's acceptance harness: once `mssql_lease`/`mssql_release`
  land, extend `repro.py` with the happy-path equivalent (lease → create `##` tables →
  concurrent pooled readers succeed → release → tables dropped) and it should print
  0/4 reproduced for the current scenarios plus a passing new one.
