# Data Model: TLS Connection Support

**Feature Branch**: `005-tls-connection-support`
**Date**: 2026-01-16

## Entities

### TlsTdsContext

TLS context wrapper that manages mbedTLS state for a single encrypted connection.

| Field | Type | Description |
|-------|------|-------------|
| `ssl_` | `mbedtls_ssl_context` | SSL/TLS connection context |
| `conf_` | `mbedtls_ssl_config` | SSL configuration (shared pattern possible) |
| `ctr_drbg_` | `mbedtls_ctr_drbg_context` | Random number generator |
| `entropy_` | `mbedtls_entropy_context` | Entropy source |
| `net_ctx_` | `mbedtls_net_context` | Network context wrapping socket fd |
| `initialized_` | `bool` | Whether TLS handshake completed successfully |
| `last_error_` | `std::string` | Last TLS error message |

**Lifecycle**:
- Created when `use_encrypt=true` and PRELOGIN succeeds with `ENCRYPT_ON/ENCRYPT_REQ`
- Handshake performed before LOGIN7
- Lives with TdsConnection until connection close
- Destroyed on connection close (proper cleanup order)

**State Transitions**:
```
[Uninitialized] --Initialize()--> [Configured]
[Configured] --Handshake()--> [Connected] or [Error]
[Connected] --Close()--> [Uninitialized]
```

### MssqlSecret (Extended)

Existing secret entity extended with `use_encrypt` option.

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `host` | `string` | (required) | SQL Server hostname |
| `port` | `uint16_t` | 1433 | SQL Server port |
| `database` | `string` | (required) | Database name |
| `user` | `string` | (required) | SQL Server login |
| `password` | `string` | (required) | SQL Server password |
| `use_encrypt` | `bool` | `false` | Enable TLS encryption |

**Validation Rules**:
- `use_encrypt` must be boolean (`true` or `false`)
- When not specified, defaults to `false` (backward compatible)

### ConnectionPoolKey (Extended)

Pool key extended to include encryption setting for pool isolation.

| Field | Type | Description |
|-------|------|-------------|
| `host` | `string` | SQL Server hostname |
| `port` | `uint16_t` | SQL Server port |
| `database` | `string` | Database name |
| `user` | `string` | SQL Server login |
| `use_encrypt` | `bool` | TLS encryption enabled |

**Identity Rule**: Connections with different `use_encrypt` values MUST NOT share a pool.

### EncryptionOption (Existing Enum)

Used in PRELOGIN packet to negotiate encryption.

| Value | Name | Meaning |
|-------|------|---------|
| 0x00 | `ENCRYPT_OFF` | Encryption disabled |
| 0x01 | `ENCRYPT_ON` | Encryption requested/accepted |
| 0x02 | `ENCRYPT_NOT_SUP` | Encryption not supported |
| 0x03 | `ENCRYPT_REQ` | Encryption required by server |

**Usage**:
- Client sends `ENCRYPT_ON` when `use_encrypt=true`
- Client sends `ENCRYPT_NOT_SUP` when `use_encrypt=false` (existing behavior)

## Relationships

```
MssqlSecret 1──────* TdsConnection
    │                    │
    └─ use_encrypt       └─ TlsTdsContext (optional, if use_encrypt=true)
           │                     │
           └─────────────────────┴─> ConnectionPool (keyed by use_encrypt)
```

## Error Types

| Error Code | Description | User Action |
|------------|-------------|-------------|
| `TLS_NOT_SUPPORTED` | Server responded with ENCRYPT_NOT_SUP/OFF | Verify server TLS config |
| `TLS_HANDSHAKE_FAILED` | mbedTLS handshake error | Check server certificate, network |
| `TLS_HANDSHAKE_TIMEOUT` | Handshake exceeded timeout | Increase timeout, check network |
| `TLS_INIT_FAILED` | mbedTLS initialization error | Internal error, check logs |
| `TLS_SEND_FAILED` | TLS write error | Connection lost, retry |
| `TLS_RECV_FAILED` | TLS read error | Connection lost, retry |
