# Spec 074 — `Encrypt`, `TrustServerCertificate` and `HostNameInCertificate`, as the Microsoft drivers define them

The extension's TLS surface is one switch. `TrustServerCertificate` is
documented as an alias of `Encrypt` (`website/docs/connection/index.md`,
"TrustServerCertificate Parameter"), and giving both with different values is
an ATTACH error (`test/sql/attach/attach_trust_cert.test` asserts it). The
Microsoft drivers — ODBC 18, `Microsoft.Data.SqlClient` 4.0, JDBC 10.2,
`go-mssqldb` — treat them as two independent settings with a third,
`HostNameInCertificate`, and since 2022 all of them default to *encrypt, and
verify*. This spec adopts that contract, spelling for spelling and default for
default: `Encrypt` decides whether the session is encrypted;
`TrustServerCertificate` decides whether the server's certificate is checked
against the client's trust store and the connected host name, and is `false`
by default; `HostNameInCertificate` names the subject the certificate must
carry when it differs from the address dialled.

Feature parity with a known contract, not a design of our own. The one thing
here that is ours is the default flip, and § 4 says what it costs.

## 0. Ground

- **The contract**, from the drivers' own documentation:
  - ODBC Driver 18 release notes: `Encrypt` defaults to `yes`,
    `TrustServerCertificate` to `no`, `HostnameInCertificate` added; the
    driver "validates the server certificate by default".
  - `Microsoft.Data.SqlClient` 4.0 breaking changes: `Encrypt=true` default;
    `TrustServerCertificate=false` keeps its meaning — validate — and now
    matters. 5.1 added `HostNameInCertificate` and `ServerCertificate`.
  - `go-mssqldb` README: `encrypt`, `TrustServerCertificate`,
    `hostNameInCertificate`, `certificate` (a CA file), `tlsmin`. On a routing
    hop it verifies the **routed** host unless `hostNameInCertificate` was
    given, in which case that name applies to every hop.
  - `sqlcmd` 18 follows ODBC 18, which is why `docker/docker-compose.yml`'s
    healthcheck already passes `-C` (trust the server certificate) to reach the
    container's self-generated certificate.
- **What the TDS layer already has.** `TdsSocket::EnableTls` hands the SNI
  name to `TlsTdsContext::WrapSocket`; that name is `host_`, and after a
  routing hop `host_` is the routed host (spec 068), so the name the
  certificate must match is already at the point where the TLS object is
  built. OpenSSL is 3.5.4, static, from vcpkg; `SSL_set1_host` and friends are
  available.
- **Where the trust store comes from.** The Azure OAuth client
  (`src/azure/azure_http.cpp`, cpp-httplib 0.53.1 from DuckDB's `third_party`)
  loads the platform store itself: Windows `ROOT` + `CA` via crypt32, macOS
  keychain (system / admin / user domains) via Security.framework, other Unix
  through OpenSSL's default paths (`/etc/ssl/certs` as built, overridable with
  `SSL_CERT_FILE` / `SSL_CERT_DIR`). `CMakeLists.txt` already links crypt32
  and the two Apple frameworks for it. The TDS TLS context gets the same
  routine.
- **Test servers.** The docker SQL Server runs on its self-generated
  certificate (errorlog: "A self-generated certificate was successfully loaded
  for encryption"). Azure SQL and Fabric present publicly-chained certificates;
  Fabric reaches its target through a routing hop.

## 1. The contract

### D1 — `Encrypt` (default `true`): unchanged

`Encrypt=yes|true|1` requests TLS in PRELOGIN and fails if the server declines;
`Encrypt=no|false|0` sends `ENCRYPT_NOT_SUP`, and the session is plaintext.
Note that this is a stronger statement than the Microsoft drivers make —
they still encrypt the login packet under `Encrypt=false`; this extension
does not, and that stays out of scope (§ 3). With `Encrypt=false` the two
options below are ignored: there is no certificate to check. Spellings stay:
ADO.NET `Encrypt` / `Use Encryption for Data`, URI `encrypt` / `ssl` /
`use_ssl`, secret `use_encrypt`.

### D2 — `TrustServerCertificate` (default `false`): verify, or accept

`false`: the server's certificate chain must validate against the platform
trust store (D4) and its subject must match the expected name (D3, D5). The
handshake fails otherwise, with OpenSSL's own reason and the two ways out named
in the message:

```text
TLS handshake failed: certificate verification failed for sql.example.com: self-signed
certificate. Set TrustServerCertificate=yes to accept this server's certificate without
verification, or HostNameInCertificate=<name> if the certificate is valid but issued
for a different name
```

(ATTACH wraps it in its own `MSSQL connection validation failed: TLS negotiation
failed to sql.example.com:1433 ...` line, which is where the port is.)

The reason is `X509_verify_cert_error_string(SSL_get_verify_result())` verbatim
— `self-signed certificate`, `unable to get local issuer certificate`,
`hostname mismatch`, `certificate has expired` — so a user can search for it.

`true`: the handshake accepts any certificate. The channel is still encrypted;
what is given up is knowing who is at the other end.

The two settings are independent. `Encrypt=true;TrustServerCertificate=false`
— the canonical secure ADO.NET string — is accepted; the "Conflicting values"
error and the alias go. Spellings: ADO.NET `TrustServerCertificate`, URI
`trustservercertificate`, secret `trust_server_certificate` (BOOLEAN). Values
as for `Encrypt`.

### D3 — `HostNameInCertificate` (default empty): the expected name

The name the certificate must carry, for when it differs from the address
dialled: an IP literal, an SSH tunnel to `localhost`, a CNAME the certificate
was not issued for. Empty means the host dialled. On a routing hop the
expected name follows the hop — the routed host, exactly as SNI already does —
unless `HostNameInCertificate` was given, in which case it applies to every
hop, as in `go-mssqldb`. Ignored under `TrustServerCertificate=true`.
Spellings: ADO.NET `HostNameInCertificate` (ODBC's `HostnameInCertificate`
matches too — keys are compared case-insensitively), URI
`hostnameincertificate`, secret `host_name_in_certificate` (VARCHAR).

### D4 — The trust store is the platform's, loaded the way the Azure client loads it

Windows: `ROOT` and `CA` system stores. macOS: keychain trust settings, all
three domains, **and** OpenSSL's default paths — httplib skips the paths once
the keychain yields anything, which silently disables `SSL_CERT_FILE` on
macOS; here both load, always. Other Unix: OpenSSL's default paths. No
`ca_certificate=` / `certificate=` parameter: a private CA belongs in the
platform store or in `SSL_CERT_FILE`, and a third place to put the same thing
is a third place to get it wrong (§ 3).

### D5 — "Match" is OpenSSL's definition, not ours

`SSL_set1_host(expected)` with `X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS`: SAN
`dNSName` entries, CN fallback under OpenSSL's rules, one-label wildcards. An
expected name that parses as an IP literal goes through
`X509_VERIFY_PARAM_set1_ip_asc` instead, so `Server=10.0.0.5` verifies against
an `iPAddress` SAN. No hand-written matcher.

## 2. The work

### W1 — The TLS layer takes a policy

`TlsOptions { bool verify_certificate = true; std::string expected_host; }`
in `tds_tls_context.hpp`. `TdsConnection::SetTlsOptions(const TlsOptions &)`
is a setter called before `Authenticate*`, like `SetRequestedPacketSize`, so
the three `Authenticate*` signatures do not grow. `TdsConnection` resolves the
expected name at `EnableTls` time — `expected_host.empty() ? host_ :
expected_host` — which is what makes D3's hop rule fall out of spec 068's
retargeting rather than needing code of its own. `TdsSocket::EnableTls`
forwards the options; `TlsImpl::Initialize` sets `SSL_VERIFY_PEER` and loads
the store (D4) when verifying, `SSL_VERIFY_NONE` when trusting; `WrapSocket`
sets SNI as now and, when verifying, the expected name (D5). `Handshake`'s
failure message is D2's when `SSL_get_verify_result` is not `X509_V_OK`, and
the existing OpenSSL error text otherwise. The `MSSQL_DEBUG` success line says
which of the two policies the session was opened under.

### W2 — The options reach every connection the extension opens

`MSSQLConnectionInfo` gains `trust_server_certificate` (default `false`) and
`host_name_in_certificate`. The three parsers (ADO.NET, URI, secret) fill them;
the alias branch in `ParseConnectionString` goes. Every site that builds a
`TdsConnection` calls `SetTlsOptions` from the info: the four pool factories in
`mssql_catalog.cpp` and the four ATTACH-validation paths in `mssql_storage.cpp`.
The deprecated `mssql_open` family, the one other path that opened a
connection from a connection string, is removed (W6), so no connection is
opened outside these eight sites.

### W3 — The test estate follows the default

The docker server's certificate is self-generated, so every DSN that reaches
it says so: `MSSQL_TEST_DSN` / `MSSQL_TESTDB_DSN` gain
`;TrustServerCertificate=yes` and `MSSQL_TEST_DSN_TLS` gains
`&trustservercertificate=true` in the Makefile (CI builds its DSNs there and
nowhere else); the test files that spell a `localhost` DSN inline get the same
(21 files); the Kerberos stack's examples already carry it. The Azure lane
runs with the default: the chain check against Azure SQL, and against Fabric
through its routing hop, is the live acceptance.

### W4 — Tests

- **Server-free**, `test/cpp/test_tls_verification.cpp` in
  `STANDALONE_TEST_SOURCES`: an OpenSSL server in-process on a socket pair,
  on a certificate generated at runtime. `TlsImpl` does direct socket I/O
  when no TDS callbacks are set, so no TDS framing is needed. The outcomes:
  verify against the self-signed certificate fails and the message carries
  `self-signed certificate` and both hints; trust succeeds and a cipher is
  negotiated; with the certificate made trusted (`SSL_CERT_FILE` pointed at
  it) and the expected name equal to its SAN, verification succeeds; with the
  same trust and a different expected name, it fails with `hostname mismatch`.
  Those are the four cells of D2 × D3; three more pin D5 and the trust
  branch: an IP literal matches the `iPAddress` SAN, a different IP fails
  with `IP address mismatch`, and under `TrustServerCertificate=true` the
  expected name is ignored.
- **Docker**, `test/sql/tls/trust_server_certificate.test`: the default
  string fails with the reason and the hint; `TrustServerCertificate=yes`
  connects; `Encrypt=true;TrustServerCertificate=false` is accepted by the
  parser (it fails at the handshake, not at ATTACH parsing);
  `attach_trust_cert.test` is replaced, its "Conflicting values" cases
  inverted.
- **Azure lane**: a wrong `HostNameInCertificate` against Azure SQL fails with
  `hostname mismatch`; the existing Azure and Fabric tests, now under the
  default, are the positive cases.

### W5 — Documentation

`website/docs/connection/index.md` TLS section rewritten around the three
options and the message; README feature line; `CLAUDE.md` options table;
`DATAMODEL.md` TLS invariant (the context verifies unless told not to; the
expected name follows the routed host); `CHANGELOG.md` under **Breaking**.

### W6 — The deprecated diagnostic-handle API goes

`mssql_open` / `mssql_close` / `mssql_ping` / `mssql_close_all` and the
`MSSQLConnectionHandleManager` singleton behind them. Spec 047 marked them
`[DEPRECATED]` (v0.2.1) for removal at the next major boundary, and the first
release on the duckdb 2.0 line is that boundary. Removing them here, rather
than teaching them the new options, is the smaller change and leaves exactly
the eight W2 sites opening connections. Their four test files, the
`mssql_pool_stats` neighbour in `mssql_diagnostic.cpp` stays, docs and
CHANGELOG (**Removed**) follow.

## 3. What this spec does not propose

- **`Encrypt=strict`** (TDS 8.0: TLS before PRELOGIN, `tds/8.0` ALPN). A
  different handshake; its own spec if ever.
- **Login-only encryption under `Encrypt=false`.** The Microsoft drivers'
  compromise for legacy servers; this extension has never done it and
  documents `Encrypt=false` as plaintext.
- **A CA-file parameter** (D4).
- **`ServerCertificate=` pinning** (SqlClient 5.1 / ODBC 18.1): exact-match
  against a file. Rare; not asked for.
- **Revocation checking** (CRL / OCSP). OpenSSL does none without explicit
  CRL loading, and the drivers that do it lean on SChannel.
- **Client certificates.** SQL Server does not authenticate clients by
  certificate.

## 4. Risks

- **The default flip is a breaking change** for every connection to a server
  on a self-signed certificate — the docker image, and any on-prem instance
  that never had one installed. They fail at the first connection with D2's
  message, which names the one-word fix. The CHANGELOG entry is under
  **Breaking**, and the v2.0-line release is the boundary to do it at; the
  same flip cost the ODBC 18 and SqlClient 4.0 upgrades exactly this and was
  kept.
- **Minimal Linux images without `ca-certificates`** fail every verified
  handshake with `unable to get local issuer certificate`. The message is
  OpenSSL's; the docs say what package to install and that `SSL_CERT_FILE`
  is honoured.
- **Fabric's routing hop.** The routed worker's certificate must match the
  routed host name for D3's default to hold. `go-mssqldb` relies on the same
  thing. **Checked live before merge**: the full Azure lane run from the
  branch with an `azure` extension built against the 2.0 pin — 26 cases, 276
  assertions, none skipped — passes `fabric_types.test` through the Fabric
  Warehouse's login-time redirect under the default verification. Had it not
  held, `HostNameInCertificate` was the documented way through.
- **Windows store enumeration** (`CertEnumCertificatesInStore`) runs once per
  TLS context, i.e. once per pooled connection creation, not per query. The
  Azure client already pays it once per token request.
- **MinGW.** The store routine needs `<wincrypt.h>` and `d2i_X509`; crypt32
  is already linked for httplib, which compiles the same calls under MinGW
  today. The Windows CI job is dispatched on the PR.
- **This branch and spec 073 (#333) both edit the pool factories** in
  `mssql_catalog.cpp`. Whichever merges second resolves a mechanical conflict
  (one adds a timeout argument, the other a setter call); no semantic overlap.

## 5. Acceptance

- Docker: the default string fails with the reason and both hints;
  `TrustServerCertificate=yes` connects; `Encrypt=true;TrustServerCertificate=false`
  is not a parse error.
- Azure SQL and Fabric (through its hop) connect under the default; a wrong
  `HostNameInCertificate` fails with `hostname mismatch`.
- The server-free test covers the four D2 × D3 cells, the IP-literal pair and
  the ignored name under trust, and is in
  `STANDALONE_TEST_SOURCES`.
- Full suite green with the W3 DSNs; docs and CHANGELOG updated.
