# Spec 096: Azure AD tokens from a managed identity and from a workload identity

Status: **draft**, 2026-11-28, second revision. Nothing here is decided by the owner yet; § Questions for
the owner lists what the first PRs wait on. Tracked in #515 (§ Azure AD authentication, next release); the
report behind it is #656. Line references were taken at main `adee0c29`. This revision adds § D0 (token
sources) and answers the nine findings of the design review of the first one (roborev job 2723).

Markers: **[doc]** Microsoft Learn product documentation, linked under § Sources; **[measure]** to be
measured on a real Azure resource or against duckdb-azure before the PR that depends on it (§ Measurements);
**[code]** read in this repository; **(derived)** a rule this spec derives, open to review.

## Problem

The extension acquires Azure AD tokens itself and takes only the parameters of the azure extension's
secret. Today, by secret:

| Azure secret | Today |
|---|---|
| `PROVIDER service_principal` | client credentials, works |
| `PROVIDER access_token` | the given token, works |
| `PROVIDER credential_chain`, `CHAIN 'env'` | `AZURE_TENANT_ID` / `AZURE_CLIENT_ID` / `AZURE_CLIENT_SECRET`, a client secret only |
| `PROVIDER credential_chain`, `CHAIN 'cli'` | `az account get-access-token`, works |
| `PROVIDER credential_chain`, `CHAIN 'interactive'` | device code, works |
| `PROVIDER credential_chain`, no `CHAIN` | env only: the default list holds `env`, which is tried first and ends the acquisition |
| `PROVIDER managed_identity` | fails: "Managed identity not yet implemented" (`azure_token.cpp:492`) |
| `managed_identity` inside a `CHAIN` | never reached: the dispatch has no arm for it |
| a federated token (AKS workload identity) | read by nothing |

Five facts in the code make this more than one missing branch [code]:

- **The HTTP client is TLS only.** `azure_http.cpp` has two `HttpPost` functions, both over
  `httplib::SSLClient`, and no GET. The VM metadata endpoint is plain HTTP on a link-local address, and the
  App Service and Arc endpoints are plain HTTP on the machine itself.
- **The secret reader takes five keys:** `tenant_id`, `client_id`, `client_secret`, `chain`,
  `access_token` (`azure_secret_reader.cpp:62-86`). An identity named by object id or resource id is not
  read.
- **`credential_chain` is not a chain.** `AcquireToken` picks one step by a fixed priority -- env, then
  cli, then interactive -- whatever order the user wrote, and a failed step ends the acquisition
  (`azure_token.cpp:470-490`). There is no fall-through. Items it does not know are ignored when a known
  one is present.
- **An empty `CHAIN` is `cli;env;managed_identity`** to `ParseChain` (`azure_token.cpp:168-176`), of
  which the dispatch runs `env` alone.
- **An acquisition has no deadline and no interrupt.** `AcquireToken` takes neither; the `ClientContext`
  overload keeps only `*context.db` (`azure_token.cpp:415`); `HttpPost` waits a fixed 30 s per phase
  (`azure_http.hpp:30-35`); the `cli` step reads `az` through `popen` with no timeout
  (`azure_token.cpp:358`); and a pool refill hands its `connect_timeout` to `Connect` only after the token
  call returns (`mssql_catalog.cpp:211-222`).

The consequence for a user: a VM, a Container App or an AKS pod that has an identity and no secret cannot
ATTACH without a detour (`az login --identity` and `CHAIN 'cli'`, or a token fetched by hand and passed as
`ACCESS_TOKEN`, which cannot be refreshed).

## Scope

In scope:
- **R0:** the token sources behind one interface (§ D0), with no change of behaviour.
- **R1:** an HTTP GET with request headers, over plain HTTP and HTTPS, and a deadline and an interrupt
  that reach every step of an acquisition.
- **R2:** managed identity on a VM / VMSS (the instance metadata service), system- and user-assigned.
- **R3:** managed identity on App Service, Functions and Container Apps.
- **R4:** AKS workload identity: a federated token exchanged as a client assertion.
- **R5:** `credential_chain` as a chain: the steps in order, with fall-through, `managed_identity` and
  `workload_identity` among them.
- **R6:** managed identity on an Azure Arc-enabled server (the challenge flow).
- **R7:** tests against a local mock of every endpoint on every PR, and a live lane on Azure resources.

Out of scope:
- Service Fabric, Cloud Shell and Azure ML managed identity endpoints, and the legacy `MSI_ENDPOINT` /
  `MSI_SECRET` form of App Service on its own. Each is a named refusal that says what is supported.
- A certificate credential for a service principal.
- Sovereign clouds beyond honouring `AZURE_AUTHORITY_HOST` in R4 (§ D5). The scope stays
  `https://database.windows.net//.default` (`azure_token.hpp:39`).
- Using the azure extension's own token acquisition. duckdb-azure answered on #656 that the handling is not
  shared; this spec keeps the extension's own.
- A timeout that kills a running `az` (§ D1 says what the `cli` step gets).
- Anything in `src/tds`: every source here ends as the same FEDAUTH token, and `AuthStrategyFactory`
  already chooses between SQL auth, FEDAUTH, Kerberos and SSPI.

## Measurements

To make before the PR that depends on each. A result goes in `measured/<id>.md`.

| Id | Question | Needed by |
|---|---|---|
| M1 | The azure extension's secret: the keys it accepts and stores for `PROVIDER managed_identity` (`client_id`, `object_id`, `resource_id`?) and for `PROVIDER credential_chain` (does it refuse them there, as it refuses `TENANT_ID`: `AZURE.md:152-156`?); what it stores for an omitted `CHAIN`; which chain values it accepts (`default`?); whether it has a provider or a chain step for workload identity, and its name. | PR 2, PR 4, PR 5 |
| M2 | The VM endpoint's answers, status and body, on: a VM with one system-assigned and two user-assigned identities (no id, each of the three ids, an id not assigned); a VM with no identity; a VM with user-assigned identities only and no id asked. | PR 2 |
| M3 | The metadata service's answer under load and at boot: which statuses ask for a retry (404, 410, 429, 5xx are the candidates), and whether a `Retry-After` header comes with them. | PR 1, PR 2 |
| M4 | The response's fields on each endpoint: `expires_on` as a string or a number, and whether the token is a JWT whose `exp` equals it. | PR 2, PR 3 |
| M5 | On App Service (Windows and Linux) and on a Container App: every identity variable in the environment (`IDENTITY_ENDPOINT`, `IDENTITY_HEADER`, `MSI_ENDPOINT`, `MSI_SECRET`), the **host form** of `IDENTITY_ENDPOINT` (a name, a loopback literal, a private address), the request with each id parameter, the status for an identity not assigned. | PR 3 |
| M6 | In an AKS pod with workload identity: the environment, the token file's audience and lifetime, the exchange's request and answer, and the failure for a missing federated credential. | PR 4 |
| M7 | How long a connect to `169.254.169.254` takes to fail off Azure (a laptop, a GitHub runner, a container with no route). It bounds the probe timeout of § D6. | PR 5 |
| M8 | On an Arc-enabled server: every identity variable in the environment and the host form of `IDENTITY_ENDPOINT`, the API version the agent takes, the 401's `WWW-Authenticate` value, the key file's directory, size and permissions. | PR 6 |
| M9 | `az account get-access-token`: exit code and text when `az` is not installed, installed and not logged in, logged in with an expired refresh token, and refused for the resource. | PR 5 |
| M10 | The identity variables a Service Fabric application sees, from the Azure SDK's source if no cluster is at hand. It fixes the first row of § D2. | PR 2 |

## Design

### D0: token sources (R0)

Today the sources are branches of one function. Six more would roughly double it, the chain of § D6 needs
an outcome the current `TokenResult` cannot express (`success` or not, `azure_token.hpp:63-76`), and the
deadline of § D1 and the tests of § Tests each need something handed to every source. So the sources go
behind one interface first.

- **The interface,** in `duckdb::mssql::azure` (`src/azure/azure_token_source.{hpp,cpp}`):

  ```cpp
  enum class TokenOutcomeKind { Token, Unavailable, Failed };
  struct TokenOutcome {
      TokenOutcomeKind kind;
      std::string access_token;                        // Token
      std::chrono::system_clock::time_point expires_at; // Token
      std::string reason;                              // Unavailable, Failed
  };
  struct AcquireRequest {
      std::chrono::steady_clock::time_point deadline;
      std::function<bool()> interrupted; // may be empty
      bool allow_interactive;
  };
  class TokenSource {
  public:
      virtual ~TokenSource() = default;
      virtual const char *Name() const = 0;            // "env", "managed_identity (VM)", ...
      virtual TokenOutcome Acquire(const AcquireRequest &request) = 0;
  };
  ```

- **`Unavailable` and `Failed`** are the two refusals § D6 tells apart: the source had nothing to try, or
  its identity provider refused it. A source asked for outright (`PROVIDER service_principal`) reports
  both as an error; only a chain reads the difference.
- **One implementation per source:** service principal, access token, env, CLI, interactive, and, as the
  later PRs add them, workload identity and one per managed-identity endpoint (VM, App Service, Arc).
- **What a source is given at construction:** the secret's values it needs, and an `Environment`: a lookup
  from a variable's name to its value or nothing, plus the platform paths a source reads (Arc's key
  directory). The production factory passes `getenv` and the fixed paths. A source never calls `getenv`
  itself. This is the tests' seam (§ Tests), and it is not an override a user can set.
- **One factory,** `MakeTokenSources(secret, environment)`, returns the ordered list for a secret: one
  source for a plain provider, the chain's steps for `credential_chain`. It is one switch in one function.
  No registry, no plugin mechanism, no per-source configuration classes.
- **`AcquireToken` keeps what is common:** the cache lookup, the list's loop, the token's own `exp` as the
  cache's expiry (`azure_token.cpp:500-513`), and the cache write. Its signature gains the
  `AcquireRequest` (§ D1).
- **One acquisition per cache key at a time (derived).** Callers that find no valid token wait for the one
  already running and take its result. Without it every pool refill past the refresh margin asks the
  metadata endpoint at once, which is a way to earn the 429 that § D1 then retries.
- **C++11-compatible,** as the rest of the extension must be for the Linux ODR rule: no `std::optional`,
  no structured bindings.

PR 0 moves the five existing sources behind the interface with today's behaviour kept exactly, the fixed
priority of the chain included: its factory returns the one source today's dispatch would pick. Every
existing Azure test passes unchanged.

### D1: the HTTP client, the deadline and the interrupt (R1)

- **One more function.** `HttpGet(url, headers, deadline)` beside the two `HttpPost`s, returning the same
  `HttpResponse`, plus the response headers the Arc flow needs (`WWW-Authenticate`) and a retry reads
  (`Retry-After`). `azure_http.cpp` stays the single httplib compilation unit. `HttpPost` takes the
  deadline too.
- **The deadline reaches every step.** `AcquireRequest::deadline` is set by the caller and handed to each
  source, which gives every HTTP call what is left of it. A source that finds none left answers `Failed`,
  naming itself and the timeout.
- **One budget for the token and the login (derived; question Q6).** A pool refill computes one deadline
  from `mssql_connection_timeout`, acquires the token under it and gives `Connect` the remainder, so a
  refill takes no longer than the setting says. ATTACH's validation does the same from
  `mssql_attach_validation_timeout` where set. `mssql_azure_auth_test` uses `mssql_connection_timeout`.
- **The interrupt.** `AcquireRequest::interrupted` is `ClientContext::IsInterrupted` where a context
  exists (ATTACH, `mssql_azure_auth_test`), asked between retries and between a chain's steps. A pool
  refill has none and is bounded by the deadline alone.
- **The `cli` step is the exception.** `az` is a child process read through `popen`, which has no portable
  timeout. Its time counts against the deadline: if `az` returns after it, the acquisition fails naming
  `cli` and the timeout, and the steps after it are not run. Killing a running `az` is out of scope.
- **Plain HTTP only where no network is crossed (derived).** A plain-HTTP request is sent only to a host
  that is, as written, a loopback or link-local IP literal or the name `localhost`; `localhost` is dialled
  as `127.0.0.1` / `::1`, so no DNS answer decides trust. Any other host over plain HTTP is refused by name
  before a socket opens. PR 1 ships this rule as written. If M5 or M8 finds a platform whose
  `IDENTITY_ENDPOINT` is a private address that is neither, the PR that adds that platform admits the
  endpoint its environment names and restates the rule as "never plain HTTP to a host a secret or this
  extension chose"; that change is the owner's to accept then.
- **No proxy.** A request to a metadata endpoint never goes through a proxy; Microsoft's troubleshooting
  guidance names proxies as the cause of 502 / 504 from the metadata address [doc]. httplib uses none
  unless told to, and this spec tells it nothing.
- **Retries.** Bounded exponential backoff on the statuses M3 confirms, a `Retry-After` honoured when it
  is shorter than what is left of the deadline.

### D2: which managed identity endpoint (R2, R3, R6)

One function decides from the `Environment`, by the first rule that matches. The variable sets are from
recollection of the Azure SDKs and are confirmed by M5, M8 and M10 before the PR that relies on each.

| # | Environment | Endpoint | Request |
|---|---|---|---|
| 1 | `IDENTITY_SERVER_THUMBPRINT` set | Service Fabric | refused by name (out of scope) |
| 2 | `IDENTITY_ENDPOINT` and `IDENTITY_HEADER` | App Service, Functions, Container Apps | GET `$IDENTITY_ENDPOINT?resource=…&api-version=2019-08-01`, header `X-IDENTITY-HEADER` [doc] |
| 3 | `IDENTITY_ENDPOINT` and `IMDS_ENDPOINT` | Azure Arc | GET, then the challenge (§ D4) |
| 4 | `IDENTITY_ENDPOINT` alone | unknown | refused by name, listing the variables found |
| 5 | `MSI_ENDPOINT`, without rule 2's pair | the legacy App Service form, Cloud Shell, Azure ML | refused by name (out of scope) |
| 6 | none of these variables | VM / VMSS | GET `http://169.254.169.254/metadata/identity/oauth2/token?api-version=2018-02-01&resource=…`, header `Metadata: true` [doc] |

- **Rule 1 is first** because a Service Fabric application is believed to see rule 2's pair as well; and
  rule 5 is after rule 2 because App Service is believed to set `MSI_ENDPOINT` beside the new pair.
- **The whole function ships in PR 2,** with rules 2 and 3 as named refusals ("managed identity on App
  Service is not supported yet") that PR 3 and PR 6 replace. Until then `PROVIDER managed_identity` on
  those platforms fails at once by name, where a VM-only function would dial the link-local address and
  time out. If Q5 defers Arc, rule 3 stays that refusal.
- **The resource** is `https://database.windows.net/`: the endpoints take a resource, not a `/.default`
  scope.
- **The decision is logged** at `INFO` with the rule's endpoint, never a header's value.
- **The VM endpoint's host** is taken from `AZURE_POD_IDENTITY_AUTHORITY_HOST` when set, as the Azure SDKs
  are believed to take it (to confirm against the SDK source). Under § D1's rule it must still be loopback
  or link-local.

### D3: which identity (R2, R3)

- **Three ways to name a user-assigned identity,** read from the azure secret under the keys M1 finds:
  client id, object id, resource id. At most one; two set is an error at ATTACH that names both.
- **None set:** the system-assigned identity.
- **The parameter's name differs by endpoint** [doc]:

  | | client id | object id | resource id |
  |---|---|---|---|
  | VM / VMSS | `client_id` | `object_id` | `msi_res_id` |
  | App Service, Functions, Container Apps | `client_id` | `principal_id` | `mi_res_id` |
  | Azure Arc | refused | refused | refused |

- **Arc has a system-assigned identity only.** An id in the secret there is refused by name, not dropped:
  a dropped id would authenticate as another identity than the one asked for.
- **`AZURE_CLIENT_ID` is not read for a managed identity** (derived). The secret says which identity; an
  environment variable changing it silently is the kind of surprise #656's reporter could not debug. § D5
  reads it, where it is the platform's own contract.
- **Inside a chain** (question Q7). If M1 finds that a `credential_chain` secret cannot carry the id keys,
  as it cannot carry `TENANT_ID`, the chain's `managed_identity` step asks for the system-assigned
  identity only, says so in `AZURE.md`, and a user-assigned identity is asked for with `PROVIDER
  managed_identity` (proposed). The alternative is an MSSQL-side parameter beside `azure_tenant_id`.

### D4: the answers

- **Success.** `access_token` is taken. The cache's expiry is the token's own `exp`, as for every other
  path since spec 073; `expires_on` is used only for a token that does not parse as a JWT (M4).
- **Failure.** The error names the source, the HTTP status and the identity asked for by the form it was
  given in, and the endpoint's own `error_description` when the body carries one. A 400 from the VM
  endpoint for a named identity adds that it is probably not assigned to this machine [doc].
- **Never in a message or a log:** a token, `IDENTITY_HEADER`'s value, the Arc key, a federated token. The
  credential-in-log gate gets a case for each.
- **The Arc challenge** [doc]: the first GET answers 401 with `WWW-Authenticate: Basic realm=<path>`. The
  file at that path is read and the request sent again with `Authorization: Basic <contents>`. The path is
  checked before it is opened (derived, the SDKs' rule to confirm in M8): inside the key directory the
  source was constructed with (`/var/opt/azcmagent/tokens`,
  `%ProgramData%\AzureConnectedMachineAgent\Tokens`), a `.key` file, no larger than 4096 bytes. Reading it
  needs membership of the agent's local group (`himds` on Linux) [doc]; a permission failure says so.

### D5: workload identity (R4)

- **The platform's contract** [doc]: the webhook sets `AZURE_FEDERATED_TOKEN_FILE`, `AZURE_CLIENT_ID`,
  `AZURE_TENANT_ID` and `AZURE_AUTHORITY_HOST` in the pod.
- **The exchange.** A POST to `<authority>/<tenant>/oauth2/v2.0/token` with `grant_type=client_credentials`,
  `client_id`, `scope=https://database.windows.net//.default`,
  `client_assertion_type=urn:ietf:params:oauth:client-assertion-type:jwt-bearer` and
  `client_assertion=<the file's contents>` (the identity platform's client-credentials grant with an
  assertion; the exact form is M6's to record).
- **The file is read at every acquisition,** never kept: Kubernetes replaces the projected token in place
  before it expires [doc].
- **The authority** is `AZURE_AUTHORITY_HOST` when set, else `login.microsoftonline.com`
  (`azure_token.hpp:34`). It must be HTTPS.
- **Where it is reached from:**
  - a chain step `workload_identity` (§ D6);
  - `CHAIN 'env'`: a client secret when `AZURE_CLIENT_SECRET` is set, else the federated token when
    `AZURE_FEDERATED_TOKEN_FILE` is set, else today's error, which then lists both forms;
  - a provider of its own only if M1 finds one in the azure extension's secret.
- **Tenant and client id:** the MSSQL-side `azure_tenant_id` wins for the tenant, as it does today. A
  value in the azure secret wins over the environment where M1 finds the secret can carry it; on a
  `credential_chain` secret that may be neither, and then the environment's are the only ones.

### D6: `credential_chain` as a chain (R5)

- **Steps:** `env`, `workload_identity`, `managed_identity`, `cli`, `interactive`.
- **Order: as written** (question Q1).
- **No `CHAIN`, or `CHAIN 'default'`** if M1 finds duckdb-azure accepts or stores that value:
  `env;workload_identity;managed_identity;cli` (proposed, the Azure SDKs' order for the steps this
  extension has; never `interactive`). `ParseChain`'s `cli;env;managed_identity` is not inherited: it was
  never run as a list.
- **Any other unknown step is an error at ATTACH** that lists the known ones. Today an unknown item beside
  a known one is ignored, so this refuses secrets that work now (question Q1).
- **Fall-through on `Unavailable` only** (derived; modelled on the Azure SDKs' chained credential, and
  differing from it where the table below says so). A `Failed` step ends the chain with its reason:
  falling through would authenticate as someone else than the configuration says.

  | Step | `Unavailable`: the next step runs | `Failed`: the chain ends |
  |---|---|---|
  | `env` | none of its variables is set | some are set and no form is complete (today's error, `azure_token.cpp:264-289`); a complete form refused |
  | `workload_identity` | `AZURE_FEDERATED_TOKEN_FILE` is not set | the file cannot be read; the exchange is refused |
  | `managed_identity` | no rule of § D2 but the VM's matches and its address does not answer the probe; the VM endpoint says this machine has no identity to give when none was named (M2 fixes how that answer is told from others) | a refusal of § D2; a named identity refused; any other answer after the retries |
  | `cli` | `az` is not installed; `az` is installed and not logged in (M9 fixes how that is recognised) | `az` is logged in and the token is refused; the deadline passed while it ran |
  | `interactive` | never | reached from a pool refill: today's message (`azure_token.cpp:477-486`); the device code refused or expired |

- **The step that gave the token is pinned (derived).** The cache keeps its name beside the token, per
  cache key. A refresh runs that step alone; if it is then `Unavailable` or `Failed`, the refresh fails
  naming it and does not fall through. So every connection of a pool logs in as the principal ATTACH found,
  for the catalog's life. With `CHAIN 'cli;managed_identity'` and an `az` login that lapses, new pool
  connections fail by name; they do not become the managed identity beside older ones that are the CLI
  user. The pin goes with the cache entry, which the catalog drops at DETACH.
- **The error of a chain that found nothing** lists every step with its own reason, one line each.
- **The metadata probe is short.** Off Azure the link-local address answers nothing, so inside a chain
  the VM endpoint's first connect gets a timeout of its own (1 s proposed; M7). `PROVIDER
  managed_identity` asked for outright uses what is left of the deadline: there the user said the endpoint
  exists.

### D7: what does and does not change

- **Refresh.** A pooled connection asks for a token at every login (`mssql_catalog.cpp:211`), and the
  cache hands one out until five minutes before its expiry. That stays. What changes is the call: it
  passes an `AcquireRequest` (§ D1's shared deadline, no interrupt, `allow_interactive = false`), runs the
  pinned step only (§ D6), and waits for an acquisition already running (§ D0).
- **The cache key** is the secret's name, with the tenant override appended, per `DatabaseInstance`
  (`azure_token.cpp:422-426`). It does not see the secret's contents: a `CREATE OR REPLACE SECRET` naming
  another identity is served the cached token until the catalog invalidates it. That is today's behaviour
  for every provider; it is recorded here because a user switching between two user-assigned identities
  under one secret name will meet it (question Q4).
- **`mssql_azure_auth_test`** goes through `AcquireToken` and so tests every new path as it is.
- **FEDAUTH.** The token reaches LOGIN7 as before; nothing in `src/tds` changes.

## Tests (R7)

- **No test changes the process environment.** Every source and § D2's function take their `Environment`
  (§ D0), so a test passes a map and the mock's port; `getenv` on a worker thread beside a `setenv` is the
  hazard the Kerberos work avoided with cred-store elements, and it is avoided here the same way.
- **A mock of each endpoint, on every PR.** A loopback HTTP server in the C++ tests, in the PR-gated job
  where spec 068's loopback TDS server already runs. It answers as each endpoint does: the VM endpoint
  (every id parameter, the answers M2 records, the retry statuses, then success), App Service (the header
  checked), Arc (401 with a realm under a temporary directory the source was constructed with, then the
  key checked).
- **§ D2's rules,** one case each, and the two that the order exists for: all three Service Fabric
  variables give its refusal; rule 2's pair with `MSI_ENDPOINT` gives App Service.
- **Workload identity.** The request is built by a function tested on its own (file read at each call,
  the form's fields); the HTTPS exchange itself is the live lane's.
- **The chain,** with stub sources: order as written; fall-through on `Unavailable`; stop on `Failed`;
  every row of § D6's table; the error listing every step; and the pin -- step 1 gives a token, then turns
  `Unavailable`, and the second acquisition fails naming step 1 and does not return step 2's token.
- **The deadline:** a source that sleeps past it fails by name; a refill's token and login together stay
  inside one budget; concurrent acquisitions of one key reach the mock once.
- **The rules of § D1 and § D4:** plain HTTP to a routable host or to a name other than `localhost`
  refused; an Arc key path outside the directory, with another extension or too large refused; no secret
  value in any message.
- **PR 0:** the existing Azure tests unchanged, plus one case per existing source through the interface.
- **SQL tests** in `test/sql/azure/`, skipped without their environment like the existing ones: one per
  endpoint, `mssql_azure_auth_test` and an ATTACH.
- **The live lane** (question Q3): a VM with a system-assigned and a user-assigned identity, a Container
  App, an AKS cluster with workload identity, each granted a login on one Azure SQL database. Manual
  dispatch, like today's Azure job.

## PRs

| PR | Contents | Closes |
|---|---|---|
| 0 | § D0: the five existing sources behind `TokenSource`; no change of behaviour | |
| 1 | § D1: `HttpGet`, plain HTTP under its rule, retries, the deadline and interrupt through `AcquireToken`, one acquisition per key; the mock server | |
| 2 | § D2 whole (App Service and Arc as "not yet" refusals), § D3 / D4 for the VM endpoint; the secret's id keys (M1); `PROVIDER managed_identity` | #656 |
| 3 | App Service, Functions, Container Apps | |
| 4 | § D5: workload identity, and `CHAIN 'env'` taking a federated token | |
| 5 | § D6: the chain, the pin, `managed_identity` and `workload_identity` as steps | |
| 6 | Azure Arc | |

Each PR updates `AZURE.md` and the CHANGELOG for what it ships. PR 0 and PR 1 change no behaviour a user
can see except that a token request now respects `mssql_connection_timeout`. PR 2 answers #656. PR 5
changes behaviour for existing chains and waits on Q1.

## Questions for the owner

- **Q1: what PR 5 changes for existing `credential_chain` secrets.** Three things, to accept together or
  apart:
  - a written chain runs in the order written (today `cli;env` runs env first);
  - no `CHAIN` runs `env;workload_identity;managed_identity;cli` and falls through (today: env only, and
    its failure is final);
  - an unknown step is refused at ATTACH (today it is ignored beside a known one; `default` is mapped if
    M1 finds it in use).

  As proposed, with a CHANGELOG entry under Changed, or keep the fixed priority and add the new steps to
  it?
- **Q2: fall-through.** On `Unavailable` only, with § D6's table (proposed), or on any failure?
- **Q3: the live lane.** Which subscription, and who creates the VM, the Container App and the AKS
  cluster? Without them PR 2-4 ship on the mock and the measurements alone.
- **Q4: the cache key.** Leave it (proposed, documented in `AZURE.md`), or add the identity to it in this
  spec?
- **Q5: Arc.** In this release as PR 6, or deferred until someone asks?
- **Q6: the budget.** One `mssql_connection_timeout` for the token and the login together (proposed), or
  a budget of its own for the token?
- **Q7: a user-assigned identity inside a chain,** if the azure secret cannot carry it there: system-
  assigned only with `PROVIDER managed_identity` as the way to name one (proposed), or an MSSQL-side
  parameter beside `azure_tenant_id`?

## Sources

- [Use managed identities on a virtual machine to acquire an access token](https://learn.microsoft.com/en-us/entra/identity/managed-identities-azure-resources/how-to-use-vm-token)
- [How managed identities work with Azure virtual machines](https://learn.microsoft.com/en-us/entra/identity/managed-identities-azure-resources/how-managed-identities-work-vm)
- [Managed identities, Azure App Service](https://learn.microsoft.com/en-us/azure/app-service/overview-managed-identity)
- [Managed identities in Azure Container Apps](https://learn.microsoft.com/en-us/azure/container-apps/managed-identity)
- [Access Azure resources with managed identity on Azure Arc-enabled servers](https://learn.microsoft.com/en-us/azure/azure-arc/servers/managed-identity-authentication)
- [Microsoft Entra Workload ID on AKS](https://learn.microsoft.com/en-us/azure/aks/workload-identity-overview)
- [Troubleshoot Azure-hosted application authentication](https://learn.microsoft.com/en-us/azure/developer/java/sdk/troubleshooting-authentication-azure-hosted)
