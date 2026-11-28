# Spec 096: Azure AD tokens from a managed identity and from a workload identity

Status: **draft**, 2026-11-28. Nothing here is decided by the owner yet; § Questions for the owner lists what
the first PR waits on. Tracked in #515 (§ Azure AD authentication, next release); the report behind it is
#656. Line references were taken at main `adee0c29`.

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
| `PROVIDER managed_identity` | fails: "Managed identity not yet implemented" (`azure_token.cpp:492`) |
| `managed_identity` inside a `CHAIN` | never reached: the dispatch has no arm for it |
| a federated token (AKS workload identity) | read by nothing |

Four facts in the code make this more than one missing branch [code]:

- **The HTTP client is TLS only.** `azure_http.cpp` has two `HttpPost` functions, both over
  `httplib::SSLClient`, and no GET. The VM metadata endpoint is plain HTTP on a link-local address, and the
  App Service and Arc endpoints are plain HTTP on localhost.
- **The secret reader takes five keys:** `tenant_id`, `client_id`, `client_secret`, `chain`,
  `access_token` (`azure_secret_reader.cpp:62-86`). An identity named by object id or resource id is not
  read.
- **`credential_chain` is not a chain.** `AcquireToken` picks one step by a fixed priority -- env, then
  cli, then interactive -- whatever order the user wrote, and a failed step ends the acquisition
  (`azure_token.cpp:470-490`). There is no fall-through.
- **An empty `CHAIN` already names it.** `ParseChain` answers `cli;env;managed_identity` for an empty
  chain (`azure_token.cpp:168-176`), which the dispatch then cannot run.

The consequence for a user: a VM, a Container App or an AKS pod that has an identity and no secret cannot
ATTACH without a detour (`az login --identity` and `CHAIN 'cli'`, or a token fetched by hand and passed as
`ACCESS_TOKEN`, which cannot be refreshed).

## Scope

In scope:
- **R1:** an HTTP GET with request headers, over plain HTTP and HTTPS, with a timeout and bounded retries.
- **R2:** managed identity on a VM / VMSS (the instance metadata service), system- and user-assigned.
- **R3:** managed identity on App Service, Functions and Container Apps.
- **R4:** AKS workload identity: a federated token exchanged as a client assertion.
- **R5:** `credential_chain` as a chain: the steps in order, with fall-through, `managed_identity` and
  `workload_identity` among them.
- **R6:** managed identity on an Azure Arc-enabled server (the challenge flow).
- **R7:** tests against a local mock of every endpoint on every PR, and a live lane on Azure resources.

Out of scope:
- Service Fabric, Cloud Shell and Azure ML managed identity endpoints, and the legacy `MSI_ENDPOINT` /
  `MSI_SECRET` form of App Service. Each is a named refusal that says what is supported.
- A certificate credential for a service principal.
- Sovereign clouds beyond honouring `AZURE_AUTHORITY_HOST` in R4 (§ D5). The scope stays
  `https://database.windows.net//.default` (`azure_token.hpp:39`).
- Using the azure extension's own token acquisition. duckdb-azure answered on #656 that the handling is not
  shared; this spec keeps the extension's own.
- The token cache's key (§ D7 records what it is; changing it is not this spec's).

## Measurements

To make before the PR that depends on each. A result goes in `measured/<id>.md`.

| Id | Question | Needed by |
|---|---|---|
| M1 | What keys does the azure extension's secret store for `PROVIDER managed_identity` -- `client_id`, `object_id`, `resource_id`? Does it have a provider or a chain step for workload identity, and under what name? | PR 2, PR 4 |
| M2 | On a VM with one system-assigned and two user-assigned identities: the token request with no id, with each of the three ids, and with an id not assigned. Status codes and bodies. | PR 2 |
| M3 | The metadata service's answer under load and at boot: which statuses ask for a retry (404, 410, 429, 5xx are the candidates), and whether a `Retry-After` header comes with them. | PR 1, PR 2 |
| M4 | The response's fields on each endpoint: `expires_on` as a string or a number, and whether the token is a JWT whose `exp` equals it. | PR 2, PR 3 |
| M5 | On App Service and on a Container App: the environment (`IDENTITY_ENDPOINT`, `IDENTITY_HEADER`), the request with each id parameter, the status for an identity not assigned. | PR 3 |
| M6 | In an AKS pod with workload identity: the environment, the token file's audience and lifetime, the exchange's request and answer, and the failure for a missing federated credential. | PR 4 |
| M7 | How long a connect to `169.254.169.254` takes to fail off Azure (a laptop, a GitHub runner, a container with no route). It bounds the probe timeout of § D6. | PR 5 |
| M8 | On an Arc-enabled server: the API version the agent takes, the 401's `WWW-Authenticate` value, the key file's directory, size and permissions. | PR 6 |

## Design

### D1: the HTTP client (R1)

- **One more function.** `HttpGet(url, headers, timeout)` beside the two `HttpPost`s, returning the same
  `HttpResponse`, plus the response headers the Arc flow needs (`WWW-Authenticate`) and a retry reads
  (`Retry-After`). `azure_http.cpp` stays the single httplib compilation unit.
- **Plain HTTP only where no network is crossed (derived).** A plain-HTTP request is sent only to a
  loopback or link-local address. Any other host over plain HTTP is refused by name before a socket opens.
  The token leaves the machine only over TLS.
- **No proxy.** A request to a metadata endpoint never goes through a proxy; Microsoft's troubleshooting
  guidance names proxies as the cause of 502 / 504 from the metadata address [doc]. httplib uses none
  unless told to, and this spec tells it nothing.
- **Retries.** Bounded exponential backoff on the statuses M3 confirms, a `Retry-After` honoured when it
  is shorter than what is left. The whole acquisition, retries included, stays inside
  `mssql_connection_timeout`: a token request must not make a pool refill outlast the login it serves.
- **Interrupts.** A wait between retries asks `ClientContext::IsInterrupted` where a context exists (ATTACH,
  `mssql_azure_auth_test`). A pool refill has none and is bounded by the timeout alone.

### D2: which managed identity endpoint (R2, R3, R6)

One function decides from the environment, in this order (the Azure SDKs' order, derived):

| Environment | Endpoint | Request |
|---|---|---|
| `IDENTITY_ENDPOINT` and `IDENTITY_HEADER` | App Service, Functions, Container Apps | GET `$IDENTITY_ENDPOINT?resource=…&api-version=2019-08-01`, header `X-IDENTITY-HEADER` [doc] |
| `IDENTITY_ENDPOINT` and `IMDS_ENDPOINT` | Azure Arc | GET, then the challenge (§ D4) |
| `IDENTITY_ENDPOINT` with `IDENTITY_SERVER_THUMBPRINT`, or `MSI_ENDPOINT` | out of scope | refused by name |
| none of them | VM / VMSS | GET `http://169.254.169.254/metadata/identity/oauth2/token?api-version=2018-02-01&resource=…`, header `Metadata: true` [doc] |

- **The resource** is `https://database.windows.net/`: the endpoints take a resource, not a `/.default`
  scope.
- **The decision is logged** at `INFO` with the endpoint's kind, never its header value.
- **A test seam that is also a feature:** the VM endpoint's host is taken from
  `AZURE_POD_IDENTITY_AUTHORITY_HOST` when set, as the Azure SDKs take it (derived; to confirm against the
  SDK source). Under § D1's rule it must still be loopback or link-local.

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

### D4: the answers

- **Success.** `access_token` is taken. The cache's expiry is the token's own `exp`, as for every other
  path since spec 073 (`azure_token.cpp:500-513`); `expires_on` is used only for a token that does not
  parse as a JWT (M4).
- **Failure.** The error names the endpoint's kind, the HTTP status and the identity asked for by the form
  it was given in, and the endpoint's own `error_description` when the body carries one. A 400 from the VM
  endpoint adds that the identity is probably not assigned to this machine [doc].
- **Never in a message or a log:** a token, `IDENTITY_HEADER`'s value, the Arc key, a federated token. The
  credential-in-log gate gets a case for each.
- **The Arc challenge** [doc]: the first GET answers 401 with `WWW-Authenticate: Basic realm=<path>`. The
  file at that path is read and the request sent again with `Authorization: Basic <contents>`. The path is
  checked before it is opened (derived, the SDKs' rule to confirm in M8): inside the agent's token
  directory (`/var/opt/azcmagent/tokens`, `%ProgramData%\AzureConnectedMachineAgent\Tokens`), a `.key`
  file, no larger than 4096 bytes. Reading it needs membership of the agent's local group (`himds` on
  Linux) [doc]; a permission failure says so.

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
- **The secret's values win over the environment** for tenant and client id where the secret has them, as
  `azure_tenant_id` already overrides the tenant.

### D6: `credential_chain` as a chain (R5)

- **Steps:** `env`, `workload_identity`, `managed_identity`, `cli`, `interactive`. An unknown step is an
  error at ATTACH that lists these.
- **Order: as written** (question Q1). An empty `CHAIN` keeps `ParseChain`'s `cli;env;managed_identity`.
- **Fall-through on "unavailable" only** (derived, the Azure SDKs' `DefaultAzureCredential` rule):
  - *unavailable*: the step has nothing to try -- its environment variables are not set, `az` is not
    installed, no metadata endpoint answers. The next step runs.
  - *failed*: the step reached its identity provider and was refused -- a wrong secret, an identity not
    assigned, an expired federated credential. The chain ends with that reason. Falling through would
    authenticate as someone else than the configuration says.
- **The error of a chain that found nothing** lists every step with its own reason, one line each.
- **`interactive`** is never run from a pool refill, as today (`allow_interactive`).
- **The metadata probe is short.** Off Azure the link-local address answers nothing, so inside a chain
  the VM endpoint's first connect gets a timeout of its own (1 s proposed; M7). `PROVIDER
  managed_identity` asked for outright uses the full budget of § D1: there the user said the endpoint
  exists.

### D7: what does not change

- **Refresh.** A pooled connection asks `AcquireToken(db, secret, tenant, allow_interactive = false)` at
  every login (`mssql_catalog.cpp:211`), and the cache hands out a token until five minutes before its
  expiry. A managed identity's token is refreshed by the same path with no new code.
- **The cache key** is the secret's name, with the tenant override appended, per `DatabaseInstance`
  (`azure_token.cpp:422-426`). It does not see the secret's contents: a `CREATE OR REPLACE SECRET` naming
  another identity is served the cached token until the catalog invalidates it. That is today's behaviour
  for every provider; it is recorded here because a user switching between two user-assigned identities
  under one secret name will meet it (question Q4).
- **`mssql_azure_auth_test`** goes through `AcquireToken` and so tests every new path as it is.
- **FEDAUTH.** The token reaches LOGIN7 as before; nothing in `src/tds` changes.

## Tests (R7)

- **A mock of each endpoint, on every PR.** A loopback HTTP server in the C++ tests, in the PR-gated job
  where spec 068's loopback TDS server already runs. It answers as each endpoint does: the VM endpoint
  (every id parameter, 400 for an unknown identity, the retry statuses, then success), App Service (the
  header checked), Arc (401 with a realm, then the key checked). The endpoints are found through their own
  environment variables, so the tests need no seam beyond § D2's.
- **Workload identity.** The request is built by a function tested on its own (file read at each call,
  the form's fields); the HTTPS exchange itself is the live lane's.
- **The chain.** Each step replaced by a stub: order as written, fall-through on unavailable, stop on
  failed, the error listing every step.
- **The rules of § D1 and § D4:** plain HTTP to a routable host refused; an Arc key path outside the
  directory, with another extension or too large refused; no secret value in any message.
- **SQL tests** in `test/sql/azure/`, skipped without their environment like the existing ones: one per
  endpoint, `mssql_azure_auth_test` and an ATTACH.
- **The live lane** (question Q3): a VM with a system-assigned and a user-assigned identity, a Container
  App, an AKS cluster with workload identity, each granted a login on one Azure SQL database. Manual
  dispatch, like today's Azure job.

## PRs

| PR | Contents | Closes |
|---|---|---|
| 1 | § D1: `HttpGet`, plain HTTP under its rule, retries; the mock server | |
| 2 | § D2 / D3 / D4 for the VM endpoint; the secret's id keys (M1); `PROVIDER managed_identity` | #656 |
| 3 | App Service, Functions, Container Apps | |
| 4 | § D5: workload identity, and `CHAIN 'env'` taking a federated token | |
| 5 | § D6: the chain, with `managed_identity` and `workload_identity` as steps | |
| 6 | Azure Arc | |

Each PR updates `AZURE.md` and the CHANGELOG for what it ships. PR 2 alone answers #656; PR 5 changes
behaviour for existing chains and waits on Q1.

## Questions for the owner

- **Q1: the chain's order.** Today `CHAIN 'cli;env'` runs env first, because the priority is fixed. § D6
  runs the steps as written. That changes which credential an existing two-step chain uses. As written
  (proposed, with a CHANGELOG entry under Changed), or keep the fixed priority and add the new steps to
  it?
- **Q2: fall-through.** On "unavailable" only (proposed), or on any failure?
- **Q3: the live lane.** Which subscription, and who creates the VM, the Container App and the AKS
  cluster? Without them PR 2-4 ship on the mock and the measurements alone.
- **Q4: the cache key.** Leave it (proposed, documented in `AZURE.md`), or add the identity to it in this
  spec?
- **Q5: Arc.** In this release as PR 6, or deferred until someone asks?

## Sources

- [Use managed identities on a virtual machine to acquire an access token](https://learn.microsoft.com/en-us/entra/identity/managed-identities-azure-resources/how-to-use-vm-token)
- [How managed identities work with Azure virtual machines](https://learn.microsoft.com/en-us/entra/identity/managed-identities-azure-resources/how-managed-identities-work-vm)
- [Managed identities, Azure App Service](https://learn.microsoft.com/en-us/azure/app-service/overview-managed-identity)
- [Managed identities in Azure Container Apps](https://learn.microsoft.com/en-us/azure/container-apps/managed-identity)
- [Access Azure resources with managed identity on Azure Arc-enabled servers](https://learn.microsoft.com/en-us/azure/azure-arc/servers/managed-identity-authentication)
- [Microsoft Entra Workload ID on AKS](https://learn.microsoft.com/en-us/azure/aks/workload-identity-overview)
- [Troubleshoot Azure-hosted application authentication](https://learn.microsoft.com/en-us/azure/developer/java/sdk/troubleshooting-authentication-azure-hosted)
