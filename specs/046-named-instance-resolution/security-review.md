# Security Review — Spec 046 PR #116 (Phases 0–2)

**Branch**: `task/spec046-phase0-2-resolver`
**PR**: [#116](https://github.com/hugr-lab/mssql-extension/pull/116)
**Reviewer**: self-review (Claude Code, security-review skill)
**Date**: 2026-05-16
**Scope**: 1,896 insertions across 15 files (Phases 0–2: parser + UDP resolver + docker test stack)

Two passes were performed:

1. **General security review** — full taxonomy: input validation, auth/authz, crypto, injection, data exposure.
2. **Credential-leak audit** — explicit scan for paths where passwords, tokens, or other secrets could reach logs (stdout, `docker compose logs`, CI artifacts, error messages).

**Result: zero qualifying findings in either pass.** Details below.

---

## Pass 1 — General security review

### Parser memory-safety (highest-value target)

`src/connection/instance_resolver.cpp:208-263` (`ParseBrowserResponse`) processes
arbitrary bytes from a network peer that an on-path attacker can spoof (UDP, no
auth, no integrity). Audited line-by-line:

- **Header length guard** at line 210 (`len < 3`) — correct.
- **Advertised size**: u16 assembled from `data[1] | data[2] << 8`; `available =
  len - 3` is non-underflowing because `len >= 3` is enforced. The `advertised >
  available` check at line 230 prevents OOB reads.
- **Trailing-NUL strip** at line 242: `data[3 + body_len - 1]` is in-bounds
  because `body_len <= advertised <= available`, so the index is at most
  `len - 1`.
- **`TokenScanner`** (`cpp:95-141`) is correctly half-open over `[data+3,
  data+3+body_len)`; every `cur_` dereference is preceded by `cur_ < end_`;
  `assign(start, cur_-start)` is length-bounded.
- **`std::stoi`** on the attacker-controlled `tcp` value at line 184 is wrapped
  in `catch (...)` (catches both `std::invalid_argument` and `std::out_of_range`);
  the port is then range-clamped to `(0, 65535]`.
- **Outer parse loop** (line 252) cannot livelock: `ParseRecord` either advances
  `cur_` or returns with `instance_name.empty()`, which breaks the outer loop.

No memory-safety, integer-overflow, or unchecked-cast issues found.

### UDP transport path

`cpp:373-454` (`SendAndRecvOnce`):

- Fixed 1472-byte recv buffer; no integer arithmetic on `n` before the bounded
  `resize(n)` (n is checked `< 0` first).
- `getaddrinfo` is called on the user-supplied hostname (semi-trusted: comes
  from the connection string). Not vulnerable to anything beyond ordinary DNS
  resolution against attacker-named hosts, which is inherent to any client.
- No format-string vulnerabilities (all `ostringstream` operator<<, no
  `printf`-family calls with user data as format string).

### Settings

`src/connection/mssql_settings.cpp:51-63` — two new options registered through
DuckDB's standard `AddExtensionOption` with `ValidatePositive` on the timeout.
No new attack surface.

### Test infrastructure

- `Makefile` `test-instance-resolver` target only adds a test binary, no
  production change.
- `test/named-instance/test-client/run-tests.sh` consumes env vars (trusted
  per security-review precedents) and feeds them to a local binary; no
  untrusted-input → shell path.
- `.github/workflows/named-instance.yml` uses `pull_request` (not
  `pull_request_target`), runs in standard GHA-isolated runners, uses no
  `secrets.*`, has no privileged actions.

### Status of Phase 3 plumbing

The resolver is compiled into the extension (`CMakeLists.txt` adds the TU)
but no call site in `src/` invokes it yet. Phase 3 (wiring into
`MSSQLConnectionInfo::FromConnectionString`) is explicitly deferred to PR #2.
The parser is currently dead code in production. The audit above still
applies because Phase 3 will activate it without further parser changes.

---

## Pass 2 — Credential / password leak audit

### Production code paths

**No credential paths exist.**

- `src/connection/instance_resolver.cpp` receives only `host`, `instance`,
  `timeout_seconds`. Never sees a password, token, or any cred-bearing field.
  Phase 3 plumbs it *behind* the existing connection-string parser; cred fields
  are stripped before the resolver is called.
- Error messages include `host` and `instance` only — both are non-secret
  connection-string fragments.
- `HexDump` (`cpp:66-84`) prints up to 32 bytes of the *Browser response*
  (server-supplied). The MC-SQLR wire protocol carries hostname + port +
  version — no credentials.
- `mssql_settings.cpp` adds two settings (timeout int, boolean toggle). No
  secret handling.
- No `MSSQL_DEBUG_LOG`, no `std::cerr` for prod paths, no env-var dumping.

### Mock-browser / test stack

**One hardcoded test-only password (`TestPassword1`), excluded per the
security-review precedents (HARD EXCLUSION #2: "Secrets or credentials stored
on disk if they are otherwise secured").**

Locations:

- `test/named-instance/docker-compose.yml:69` — `MSSQL_SA_PASSWORD:
  "TestPassword1"`
- `test/named-instance/docker-compose.yml:80` — `sqlcmd … -P TestPassword1` in
  the healthcheck
- `test/named-instance/sql/entrypoint.sh:21` — `SA_PASSWORD="${MSSQL_SA_PASSWORD:-TestPassword1}"`
  fallback
- `test/named-instance/sql/entrypoint.sh:33,43` — passed via `-P
  "${SA_PASSWORD}"` to sqlcmd

This is:

- A throwaway literal — same value `test/kerberos/` already uses (spec 042
  established this pattern).
- Confined to the docker-compose internal network; the SQL Server port is
  `expose:`, not `ports:`, so no host-facing exposure.
- Not a *new* credential — this PR followed an existing test-stack convention.

The sqlcmd healthcheck redirects to `/dev/null 2>&1`, so the password isn't
echoed on failure. `-P TestPassword1` is visible in `docker top` / `ps` inside
the SQL container, but that's the standard sqlcmd ergonomics issue and
applies to every dockerised SQL Server test stack.

### CI workflow

`.github/workflows/named-instance.yml`:

- No `secrets.*` references — workflow consumes no GHA secrets.
- On failure runs `docker compose logs sql/mock-browser/test-client`. None
  of these services authenticate with real credentials; SQL Server's stock
  log output does not echo `MSSQL_SA_PASSWORD`. The only password in the
  system is the throwaway `TestPassword1` above.
- `pull_request` trigger (not `pull_request_target`), so untrusted PR code
  can't access repo secrets even if any were added later.

### `run-tests.sh` echoes

Echoes `BROWSER_HOST`, `SQL_HOST`, `EXPECTED_PORT`, `INSTANCE`, and the
resolver's stdout. All are hostnames / ports / instance names. The
test-client never authenticates to SQL Server — it just TCP-probes the
resolved port. No credentials in the test-client's environment at all.

### `browser.py` logs

Logs `(client_addr, requested_instance, matched_instances, response_size)`.
The MC-SQLR request carries only an instance name byte string. No credential
field exists in this protocol.

---

## Findings summary

| # | Issue | Severity | Status |
|---|-------|----------|--------|
| 1 | Parser OOB / overflow on attacker-controlled UDP | n/a | None found — bounds checks correct |
| 2 | UDP transport integer-handling | n/a | None found — recv buffer is fixed; n is sign-checked before use |
| 3 | Format string / printf with user data | n/a | None — all use `ostringstream` |
| 4 | Hardcoded `TestPassword1` in test stack | None | **Excluded** — test-only throwaway, identical to existing spec-042 stack, not introduced by this PR |
| 5 | Production-code credential exposure | None | No production code in this PR handles credentials |
| 6 | CI log capture exposing secrets | None | Workflow uses no secrets; `docker compose logs` captures only test-stack logs with no real credentials |
| 7 | Shell injection via env vars in `run-tests.sh` | None | Env vars trusted per precedent; only hostnames/ports/instance names flow through |
| 8 | Python `MOCK_BROWSER_INSTANCES` parsing | None | Env vars trusted per precedent |

**Net result: 0 HIGH, 0 MEDIUM, 0 LOW security findings.**

The parser was written defensively (size-field validation, half-open scanner,
exception-wrapped `stoi`, lenient-but-bounded record termination) and the UDP
transport correctly bounds the recv buffer. No credential or PII paths exist
in production code paths added by this PR.

## Follow-up obligations

When **Phase 3** wires the resolver into `MSSQLConnectionInfo::FromConnectionString`
(PR #2), the security review should be re-performed with focus on:

1. Whether any credential-bearing connection-string field accidentally flows
   into resolver error messages.
2. Whether the integrated-auth SPN derivation against the *discovered* port
   under Kerberos creates any new exposure (it should not — SPN format is
   public).
3. Whether the new SQL test files under `test/sql/named_instance/` accidentally
   commit non-test credentials.
