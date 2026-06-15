# Data Model: Lazy GSSAPI/Kerberos Linking

This feature has no persisted or wire data. The "entities" are the in-process
runtime-loader structures introduced by the shim.

## GssApiFns (function-pointer table)

Caches resolved addresses of the 8 GSSAPI functions used by the authenticator.

| Field | Source symbol | Used for |
|-------|---------------|----------|
| `display_status` | `gss_display_status` | Format major/minor error text |
| `release_buffer` | `gss_release_buffer` | Free output tokens |
| `acquire_cred_from` | `gss_acquire_cred_from` | Keytab / ccache-override credential acquisition |
| `import_name` | `gss_import_name` | Import SPN / principal names |
| `init_sec_context` | `gss_init_sec_context` | SPNEGO negotiation round |
| `delete_sec_context` | `gss_delete_sec_context` | Cleanup context |
| `release_cred` | `gss_release_cred` | Cleanup credential |
| `release_name` | `gss_release_name` | Cleanup name |

- **Population (Linux)**: `dlsym` from the `libgssapi_krb5` handle.
- **Population (macOS)**: direct `&gss_*` addresses (framework linked).
- **Lifecycle**: filled once via `std::call_once`; process-lifetime; never freed.

## Krb5Fns (function-pointer table — Linux only)

Caches the MIT `krb5_*` symbols used by raw mode and the test functions. Only
populated when a krb5 mode (raw/keytab) or `mssql_kerberos_auth_test*` runs; never
requested on macOS.

Functions: `init_context`, `parse_name`, `get_init_creds_password`,
`free_principal`, `get_error_message`, `free_error_message`, `free_context`,
`cc_resolve`, `cc_initialize`, `cc_store_cred`, `cc_close`, `cc_destroy`,
`free_cred_contents` (raw mode) plus test-only `cc_default`, `cc_get_principal`,
`unparse_name`, `free_unparsed_name`.

- **Population**: `dlsym` from a separately-dlopened `libkrb5` handle.

## Krb5RuntimeUnavailable (error type)

`std::runtime_error` subclass thrown when a required library/symbol cannot be
resolved.

| Attribute | Meaning |
|-----------|---------|
| message | Names the missing `.so` + `dlerror()` + install-package recommendation (FR-005) |

- **State transitions**: raised at first `GetGssApi()`/`GetKrb5()` when load fails.
  In a connection path it propagates as the auth failure; in a test function it is
  caught and returned as the result string (FR-006).

## Loader state (module-private)

| State | Type | Notes |
|-------|------|-------|
| gss once-flag + cached table | `std::once_flag` + `GssApiFns` | Thread-safe single load (FR-009) |
| krb5 once-flag + cached table | `std::once_flag` + `Krb5Fns` | Linux only |
| handles | `void *` | Held for process lifetime; not `dlclose`d |
