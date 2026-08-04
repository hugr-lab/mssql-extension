# RFC: Trusted custom extension repositories (per-origin signing keys)

**Status:** Draft proposal for `duckdb/duckdb` (Discussion → RFC → PR)
**Author:** hugr-lab
**Target:** DuckDB extension install/load subsystem
**Source baseline:** DuckDB v1.5.x (`src/main/extension/*`)

---

## TL;DR

Today DuckDB offers exactly two trust levels for extensions:

1. **Only DuckDB's own keys** (core + community), which are hard-compiled into the engine; or
2. **`allow_unsigned_extensions = true`** — a global kill-switch that disables signature verification for *everything*.

There is no middle ground. Anyone who needs to distribute a **private / proprietary / air-gapped** extension is forced to turn verification off entirely, which DuckDB itself calls *"problematic in itself"* in the [Community Extensions announcement](https://duckdb.org/2024/07/05/community-extensions).

This RFC proposes the missing middle: **per-origin, opt-in, pinned trust for self-hosted extension repositories**, reusing DuckDB's existing signing crypto, the existing Secret Manager, and the existing httpfs credential resolution. A repository advertises its public signing key via a `.well-known` discovery document; DuckDB pins it on first use (TOFU) and verifies binaries from that origin against it — while the engine defaults and DuckDB's own keys are untouched.

```sql
-- one-time, explicit trust decision (auto-discovers + pins the publisher key)
CALL duckdb_register_extension_repo('mycorp', 'https://ext.mycorp.com');

INSTALL mssql FROM mycorp;   -- verified against mycorp's pinned key
LOAD   mssql;
```

This is exactly the model every mature package manager already has (APT `Signed-By=`, Cargo alternate registries, cosign `--key`, OIDC `.well-known` discovery). DuckDB already ships a *second* hard-coded key set for community extensions — this generalizes that idea to a runtime-registered, origin-scoped third tier.

---

## 1. Motivation

### 1.1 The current trust scale has a gap

```
[ DuckDB core/community keys ]  ──────(nothing here)──────  [ allow_unsigned_extensions = trust everything ]
```

Extension binaries are **native code with full process access**, so signature verification is the only thing standing between a user and arbitrary code execution. DuckDB's threat model is correct and conservative. But the only escape hatch for third-party distribution is the *global* `allow_unsigned_extensions` flag, which:

- disables verification for **all** repositories at once, including a compromised mirror of `core`;
- gives no way to express "I trust exactly this one publisher for exactly this one repository";
- provides no provenance, no pinning, no rotation story.

### 1.2 DuckDB has already acknowledged this

From the [Community Extensions blog post](https://duckdb.org/2024/07/05/community-extensions):

> Third-party extensions were previously unable to sign the extensions using official keys, forcing users to use the `allow_unsigned_extensions` option that disables signature checks which is problematic in itself.

DuckDB's answer was a **centralized, curated** community repository. That is excellent for public OSS extensions, but it structurally cannot serve:

- proprietary / closed-source extensions;
- enterprise / air-gapped / on-prem distribution;
- pre-release or per-customer vendor builds;
- compliance regimes that mandate self-hosting of artifacts.

For all of these, the only option today is "turn off signing." This RFC is the **decentralized complement** to community extensions: let a trusted party run its own signed repository without forking DuckDB.

### 1.3 Related prior context in DuckDB

- [#9709 — "Unsecured extension file downloads can be easily compromised through man-in-the-middle attacks"](https://github.com/duckdb/duckdb/issues/9709): the conclusion there is that **signing**, not TLS, is the integrity guarantee. This RFC keeps that principle — trust is anchored in a pinned signing key, not in the transport.

---

## 2. Prior art

| System | Mechanism | Relevance |
|---|---|---|
| **APT** | `Signed-By=/usr/share/keyrings/vendor.gpg` per-source key pinning | The canonical model: you never disable GPG globally to add a third-party PPA — you pin that PPA's key to that source. |
| **Cargo** | alternate registries with their own auth | Per-registry trust + credentials |
| **pip / PyPI** | TUF (PEP 458), `--trusted-host` | Scoped trust + discovery |
| **Sigstore / cosign** | `cosign verify --key vendor.pub` | Per-key verification of artifacts |
| **OIDC / OAuth** | `/.well-known/openid-configuration` | Automatic key/metadata discovery over TLS |
| **DuckDB itself** | `community_public_keys[]`, gated by `allow_community_extensions` | A *second* hard-coded key tier already exists — this RFC makes a *third* tier runtime-registerable and origin-scoped. |

The takeaway: multi-key, gated, scoped trust is not a novel concept. It is the default in every package ecosystem, and DuckDB is already 90% of the way there internally.

---

## 3. Goals and non-goals

### Goals

- Allow a **self-hosted repository** to serve extensions that DuckDB will verify against the **publisher's** key.
- Keep the **trust decision explicit and pinned**, not a blanket "trust everything."
- **Reuse** existing signing crypto, the Secret Manager, and httpfs credential resolution — no new dependencies.
- Make the common path **frictionless** (no manual key pasting): the repository advertises its key; DuckDB discovers and pins it.
- **Do not weaken any existing default.** Stock behavior is unchanged unless the user explicitly opts in.

### Non-goals

- Not changing the signature cryptography (RSA over the two-level SHA-256 hash stays as-is).
- Not adding a new transport (HTTP/HTTPS/S3/local repositories already exist).
- Not asking DuckDB to host or curate anything.
- Not replacing or competing with the community repository.

---

## 4. Design

### 4.1 Two orthogonal concerns: access vs trust

The design deliberately separates two things DuckDB currently conflates:

| Concern | Question | Mechanism | New code? |
|---|---|---|---|
| **Access** (confidentiality / auth) | "Can I download the file?" | existing httpfs / S3 / Azure secrets, resolved by **scope** | **none** |
| **Trust** (integrity / authenticity) | "Is this binary authentic?" | publisher signing key, discovered + pinned | this RFC |

A public repository needs only trust. A private repository needs both — and the access half is already solved by the Secret Manager's scope-based resolution (see §4.6). Keeping them as **separate secrets** means the trust material (a *public* key, safe to store in plaintext) never has to live in the same object as a sensitive access token.

### 4.2 Discovery document: `/.well-known/duckdb-extensions.json`

A repository advertises its signing key(s) at a well-known path, fetched over **HTTPS only** (cert-validated via httpfs):

```jsonc
{
  "schema_version": 1,
  "repository": "mycorp",
  "url_template": "/${revision}/${platform}/${name}.duckdb_extension.gz",
  "signing_keys": [
    {
      "kid": "mycorp-2026",
      "algorithm": "RSA-SHA256",
      "public_key": "-----BEGIN PUBLIC KEY-----\nMIIBIjAN...\n-----END PUBLIC KEY-----",
      "valid_to": "2027-01-01"
    }
  ]
}
```

- `signing_keys` is an array to support **overlapping key rotation** (old + new key valid simultaneously during a window).
- Parsed with the bundled yyjson — no new dependency.

### 4.3 Storage: a new `extension_repository` secret type

The pinned trust anchor lives in the **Secret Manager** as a new secret type, implemented as a `KeyValueSecret` (the same base httpfs uses for `s3`/`http`):

```sql
CREATE PERSISTENT SECRET mycorp (
    TYPE          extension_repository,
    URL           'https://ext.mycorp.com',
    SIGNING_KEY   '-----BEGIN PUBLIC KEY-----
MIIBIjAN...
-----END PUBLIC KEY-----',
    KEY_ID        'mycorp-2026'
);
```

Using the Secret Manager for storage gives us, for free:

- **persistence** across restarts (`PERSISTENT`);
- **scope** matching (the URL prefix), consistent with how access secrets resolve;
- a **drop / overwrite** lifecycle (rotation = re-register);
- **visibility** via `duckdb_secrets()`.

This means we invent **no bespoke pin-file format or location** — the secret *is* the pin store.

### 4.4 Convenience function: `duckdb_register_extension_repo()`

So users never have to paste a PEM key by hand, a thin wrapper performs discovery and creates the secret:

```sql
CALL duckdb_register_extension_repo('mycorp', 'https://ext.mycorp.com');
-- 1. GET https://ext.mycorp.com/.well-known/duckdb-extensions.json  (via httpfs, honoring access secrets)
-- 2. print the key fingerprint  (the explicit TOFU moment)
-- 3. CREATE PERSISTENT SECRET mycorp TYPE extension_repository (...)
```

Output on first registration:

```
Trusting new extension repository 'https://ext.mycorp.com'
  signing key:  SHA256:ab12cd…  (kid: mycorp-2026)
  stored as:    PERSISTENT SECRET 'mycorp'  (TYPE extension_repository)
```

Power users can still `CREATE SECRET … TYPE extension_repository` manually with an out-of-band key for maximum assurance.

### 4.5 Listing: `duckdb_extension_repositories()`

A table function (essentially a typed view over `duckdb_secrets()` filtered to `extension_repository`) for observability:

| name | url | key_id | key_fingerprint | valid_to |
|---|---|---|---|---|
| `mycorp` | `https://ext.mycorp.com` | `mycorp-2026` | `SHA256:ab12…` | `2027-01-01` |

Combined with the existing `duckdb_extensions()` columns `install_mode` and `installed_from`, an operator can fully audit *what is installed from where and verified by which key*.

### 4.6 Access to private repositories (no new credential chain)

For a **closed** repository, the *download* needs credentials. DuckDB already resolves these by scope at HTTP time, so an ordinary httpfs/S3 secret just works:

```sql
CREATE SECRET mycorp_access (
    TYPE  http,
    EXTRA_HTTP_HEADERS MAP {'Authorization': 'Bearer <token>'},
    SCOPE 'https://ext.mycorp.com'
);
```

When `INSTALL`/discovery issues `HTTPUtil::Request(url)`, the scope-matching access secret is picked up automatically — for both the binary download **and** the `.well-known` fetch. **We write zero new credential-chain code for access.** The signing key itself is *public*, so it needs no provider chain at all — it is a trust anchor, not a credential.

### 4.7 Verification flow

Verification stays exactly where it is today — **at load time only** — reusing the existing `MbedTlsWrapper::IsValidSha256Signature` path. INSTALL is left byte-for-byte unchanged: it still performs only the metadata check in `CheckExtensionMetadataOnInstall` and never verifies the signature, just as for core and community extensions today. The **only** change is *which key set* `LOAD` consults, selected by the extension's recorded origin:

**On LOAD** (`TryInitialLoad`):

```text
read .info  ->  install_mode, repository_url
origin = repository_url
if origin is core/community/empty:
    keys = GetPublicKeys(allow_community)                 # unchanged behavior
else if allow_custom_extension_repositories and secret(origin) exists:
    keys = GetPublicKeys(false) ∪ secret(origin).signing_keys
else:
    fall back to today's behavior (core keys only) -> fails as it does now
verify file via CheckExtensionSignature(handle, meta, keys)
```

DuckDB's own keys remain the root of trust and always validate. A vendor key **only** adds trust for **its own origin** — registering `mycorp`'s key never lets an attacker-served binary from a *different* origin pass verification.

**Why load-only (no install-time check).** Keeping verification at load preserves symmetry with how core and community extensions already work, and it means the install path (`extension_install.cpp`) needs **no changes at all** for the trust mechanism. The entire trust surface reduces to two points: the registration step (§4.4), which performs discovery and pins the key, and this single load-time key-selection change. The only thing we forgo is "fail fast at install": an extension downloaded from an origin that has not been registered installs fine and fails only at `LOAD` — identical to how an unsigned extension behaves today.

### 4.8 Pinning and rotation (TOFU done safely)

The well-known key is served by the same origin as the binary, so naively trusting it on every load would be no better than "trust TLS." Trust is therefore **pinned**:

- **Registration** is the single TOFU moment: discover, print fingerprint, pin into the secret.
- **Loads** verify against the *pinned* key in the secret — they never silently adopt a new server-advertised key.
- **Rotation** is safe-by-construction: a newly advertised key is accepted automatically only while the **currently pinned key is still present** in the discovery document (overlap window). If the pinned key disappears, DuckDB **refuses** and requires an explicit `CALL duckdb_register_extension_repo(..., refresh => true)`.
- **Strict (non-TOFU) pinning** needs no new syntax: instead of letting `duckdb_register_extension_repo()` discover the key, the user supplies it out-of-band via plain `CREATE SECRET … TYPE extension_repository (SIGNING_KEY '-----BEGIN PUBLIC KEY----- …')`. The key is then never fetched from the network, so there is no first-use trust window at all.

### 4.9 Opt-in gate

A new setting, **off by default**, mirroring `allow_community_extensions`:

```sql
SET allow_custom_extension_repositories = true;   -- default: false
```

With the flag off, none of the above is reachable and stock behavior is byte-for-byte identical. Turning it on is an explicit, auditable decision — exactly like `add-apt-repository`.

---

## 5. End-to-end examples

**Public, signed repository:**

```sql
SET allow_custom_extension_repositories = true;
CALL duckdb_register_extension_repo('mycorp', 'https://ext.mycorp.com');
INSTALL mssql FROM mycorp;
LOAD   mssql;
```

**Private, signed repository (access via httpfs secret):**

```sql
SET allow_custom_extension_repositories = true;

CREATE SECRET mycorp_access (
    TYPE  http,
    EXTRA_HTTP_HEADERS MAP {'Authorization': 'Bearer <token>'},
    SCOPE 'https://ext.mycorp.com'
);

CALL duckdb_register_extension_repo('mycorp', 'https://ext.mycorp.com');  -- discovery uses mycorp_access
INSTALL mssql FROM mycorp;
LOAD   mssql;
```

**Audit:**

```sql
SELECT extension_name, install_mode, installed_from FROM duckdb_extensions() WHERE installed_from = 'mycorp';
SELECT * FROM duckdb_extension_repositories();
```

---

## 6. Security analysis

| Property | Status quo (`allow_unsigned`) | This RFC |
|---|---|---|
| Scope of trust | global — every repo, including `core` mirrors | a single named origin |
| Trust anchor | none (verification off) | pinned publisher key |
| Default | must flip a global flag to do anything 3rd-party | feature off by default; per-repo opt-in |
| Provenance | none | `installed_from` + pinned fingerprint, auditable |
| MITM resistance | none | binary must be signed by the pinned key |
| Key rotation | n/a | explicit, overlap-window |

**Why this is strictly safer than the only option people have today:** it replaces "trust everything" with "trust exactly this publisher's key for exactly this repository," leaving DuckDB's own keys and the default-deny posture intact. That is least-privilege.

**Honest limitation:** when the key is auto-discovered, first-use trust is TOFU — equivalent to SSH `known_hosts` or `apt-key` workflows. This is a deliberate, well-understood trade-off. Callers who need a stronger anchor skip discovery entirely and create the secret with an out-of-band key (`CREATE SECRET … TYPE extension_repository (SIGNING_KEY '…')`), so no key is ever fetched from the network. Discovery is HTTPS-only with certificate validation; plain-HTTP repositories are refused for custom trust.

---

## 7. Implementation sketch

All paths relative to `duckdb/duckdb`, baseline v1.5.x.

| # | Component | Files | Est. |
|---|---|---|---|
| A | `allow_custom_extension_repositories` setting (default false) | `src/include/duckdb/main/settings.hpp`, `src/main/settings/custom_settings.cpp`, `src/main/config.cpp` | ~20 LOC |
| B | `.well-known` discovery: fetch + parse (yyjson) | new `src/main/extension/extension_repository_discovery.{cpp,hpp}` | ~120–180 LOC |
| B2 | `extension_repository` secret type (`RegisterSecretType` + `CreateSecretFunction`, `KeyValueSecret`) | new file + registration | ~60–100 LOC |
| C | `duckdb_register_extension_repo()` (discovery + CREATE SECRET) | discovery file | ~40 LOC |
| C2 | `duckdb_extension_repositories()` table function | new `src/function/table/system/…` | ~50 LOC |
| D | Thread origin keys into verify: `GetPublicKeys`, `CheckKnownSignatures`, `CheckExtensionSignature` | `src/main/extension/extension_helper.{hpp,cpp}`, `src/main/extension/extension_load.cpp` | ~50 LOC |
| F | LOAD reads `.info` → origin → looks up secret → repo keys; plumb Secret Manager into verify | `src/main/extension/extension_load.cpp` (`TryInitialLoad`) | ~50–80 LOC |
| H | sqllogictests + docs page | `test/sql/…`, `docs/` | bulk of wall-clock |

**Reused unchanged:** `MbedTlsWrapper::IsValidSha256Signature` (crypto); `ExtensionUrlTemplate` / `InstallFromHttpUrl` / `HTTPUtil` (download); **the entire install path** (`extension_install.cpp`) — INSTALL is not modified; httpfs/S3 secret scope resolution (access); `.info` sidecar + its versioned serialization (`src/storage/serialization/serialize_extension_install_info.cpp`); the `installed_from` / `install_mode` columns in `duckdb_extensions()`. Strict pinning, rotation, and the out-of-band key path are all expressed through ordinary `CREATE SECRET` / `DROP SECRET` on the new type — no new `INSTALL` grammar or option parser.

**No new third-party dependency** — JSON via bundled yyjson, SHA-256/RSA via bundled mbedtls, HTTP+auth via httpfs/`HTTPUtil`, persistence via the Secret Manager.

**Minimum viable PR:** A + B + B2 + C + D + F (gate + discovery + secret type + register helper + load-time verification). C2 / H land as a follow-up layer to keep the first PR small and reviewable.

---

## 8. Risks and open questions

1. **LOAD currently verifies without a query context.** `TryInitialLoad(DatabaseInstance &db, …)` operates on a filename and tries all hard-coded keys. Looking up a secret needs a transaction/context (`SecretManager::GetSecretByName(ClientContext&, …)`). Some load paths carry a context, some only `db`. Threading the Secret Manager into the verification point is the one genuinely structural change and the main thing to get right in review. Direct-loaded / `CUSTOM_PATH` extensions with no repository keep today's behavior (core keys only).
2. **Discovery transport.** Restrict discovery and download to HTTPS (or signed S3) with certificate validation; refuse custom trust over plain HTTP.
3. **Discovery caching.** Cache the discovery document per install session to avoid re-fetching per extension.
4. **Persistent secret storage is plaintext by default.** Fine for a *public* signing key; this is an additional reason to keep access tokens in a separate secret rather than bundling them into `extension_repository`.
5. **Naming / location of the well-known path** and the exact discovery schema are open for bikeshedding with maintainers.
6. **Interaction with `allow_unsigned_extensions`.** When unsigned is on, custom-repo verification is moot (everything loads); the gate is only meaningful while signing is enforced.

---

## 9. Backwards compatibility

- Feature is gated off by default; with the flag unset, behavior is identical to today.
- The `.info` sidecar format is unchanged — the existing `repository_url` field is all that LOAD needs to find the matching repository secret. No new on-disk format.
- DuckDB's core and community keys are unchanged and remain the root of trust.

---

## 10. Alternatives considered

- **Custom DuckDB build with the publisher key baked into `public_keys[]`.** Works, but forces every vendor to fork and ship their own engine build (the "MotherDuck pattern"). High friction; does not help the broader ecosystem. This RFC removes that need.
- **Manual key registration only (no discovery).** Safe but clumsy — users must obtain and paste PEM keys. We keep this as the power-user path but make discovery the default.
- **Global `allow_unsigned_extensions`** — the status quo. Strictly worse: no scoping, no pinning, no provenance.

---

## 11. Future work

- `KEY_FINGERPRINT` pin sets and automatic rotation telemetry.
- Optional support for detached signatures / multiple algorithms (e.g., Ed25519) in the discovery schema.
- A `duckdb_extension_repository_verify()` diagnostic to dry-run discovery + pinning without installing.
- Sigstore/transparency-log integration as an optional stronger anchor than TOFU.
