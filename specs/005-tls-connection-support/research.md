# Research: TLS Connection Support

**Feature Branch**: `005-tls-connection-support`
**Date**: 2026-01-16

## TLS Library Selection

### Decision

**mbedTLS 3.x** - selected as the TLS library for this implementation.

### Rationale

1. **vcpkg Availability**: mbedTLS is fully available via vcpkg (`vcpkg install mbedtls`), version 3.6.x. BearSSL has no official vcpkg port, requiring manual integration.

2. **Cross-Platform Support**: Full CMake support for unified builds across Linux, macOS, and Windows.

3. **TLS Version Support**: Supports TLS 1.2 (required) and TLS 1.3 (future-proofing for TDS 8.0).

4. **Trust Server Certificate Mode**: Supports `MBEDTLS_SSL_VERIFY_NONE` to skip certificate verification.

5. **Socket Wrapping**: Can wrap existing socket file descriptors via `mbedtls_net_context` or custom I/O callbacks.

6. **Active Maintenance**: Maintained by ARM/Trusted Firmware with regular releases.

7. **License**: Apache 2.0 - compatible with open-source distribution.

### Alternatives Considered

| Library | Verdict | Reason |
|---------|---------|--------|
| BearSSL | Rejected | No vcpkg port, no TLS 1.3, beta status (v0.6), limited maintenance |
| OpenSSL | Rejected | Large binary (~20MB vs ~2MB), system dependency conflicts, complex API |
| wolfSSL | Rejected | GPLv2 license for free use (commercial license required for proprietary) |

## mbedTLS Integration Pattern

### TLS 1.2 Requirement for VERIFY_NONE

**Important**: mbedTLS 3.6.0+ does not support `MBEDTLS_SSL_VERIFY_NONE` with TLS 1.3. Must force TLS 1.2 maximum version when using "trust server certificate" mode:

```cpp
mbedtls_ssl_conf_max_tls_version(&conf, MBEDTLS_SSL_VERSION_TLS1_2);
mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_NONE);
```

This aligns with SQL Server 2016+ TLS 1.2 support and FR-011.

### Required mbedTLS Components

```cpp
#include <mbedtls/net_sockets.h>  // Network context
#include <mbedtls/ssl.h>          // SSL/TLS
#include <mbedtls/entropy.h>      // Entropy source
#include <mbedtls/ctr_drbg.h>     // Random number generator
#include <mbedtls/error.h>        // Error handling
```

### Initialization Sequence

1. Initialize all structures (`mbedtls_*_init`)
2. Seed random number generator (`mbedtls_ctr_drbg_seed`)
3. Wrap existing socket via `server_fd.fd = existing_tcp_socket`
4. Configure SSL defaults (`mbedtls_ssl_config_defaults`)
5. Set TLS 1.2 max version and VERIFY_NONE
6. Setup SSL context (`mbedtls_ssl_setup`)
7. Set I/O callbacks (`mbedtls_ssl_set_bio`)

### Handshake Pattern

```cpp
int ret;
while ((ret = mbedtls_ssl_handshake(&ssl)) != 0) {
    if (ret != MBEDTLS_ERR_SSL_WANT_READ &&
        ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
        // Fatal error - log and return
        break;
    }
}
```

### Send/Receive Pattern

- `mbedtls_ssl_write()` - may return partial writes; loop until complete
- `mbedtls_ssl_read()` - returns bytes read, 0 for EOF, negative for error
- Handle `MBEDTLS_ERR_SSL_WANT_READ/WRITE` for non-blocking operation
- `MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY` indicates graceful peer closure

### Cleanup Sequence (Order Matters!)

1. `mbedtls_ssl_close_notify()` - send close_notify alert
2. `mbedtls_ssl_free()` - free SSL context (must be before config)
3. `mbedtls_ssl_config_free()` - free configuration
4. `mbedtls_net_free()` - free network context (**does NOT close socket**)
5. `mbedtls_ctr_drbg_free()` / `mbedtls_entropy_free()` - free RNG
6. `close(socket_fd)` - close underlying socket separately

### vcpkg Configuration

**vcpkg.json**:
```json
{
  "name": "mssql-extension",
  "dependencies": ["mbedtls"]
}
```

**CMakeLists.txt**:
```cmake
find_package(MbedTLS CONFIG REQUIRED)
target_link_libraries(${TARGET_NAME} PRIVATE
    MbedTLS::mbedtls
    MbedTLS::mbedx509
    MbedTLS::mbedcrypto)
```

## TDS Protocol Integration

### TLS Handshake Timing in TDS Flow

Per MS-TDS specification, TLS handshake occurs between PRELOGIN and LOGIN7:

```
Client                              Server
  |                                    |
  |--- TCP Connect ------------------->|
  |                                    |
  |--- PRELOGIN (ENCRYPT_ON) --------->|
  |<-- PRELOGIN Response (ENCRYPT_ON) -|
  |                                    |
  |<<< TLS Handshake >>>>>>>>>>>>>>>>>>>|
  |                                    |
  |--- LOGIN7 (over TLS) ------------->|
  |<-- LOGINACK (over TLS) ------------|
  |                                    |
  |--- SQL_BATCH (over TLS) ---------->|
  |<-- Results (over TLS) -------------|
```

### PRELOGIN Encryption Option

When `use_encrypt=true`:
- Client sends `ENCRYPT_ON` (0x01) in PRELOGIN
- Server may respond with:
  - `ENCRYPT_ON` (0x01) - proceed with TLS
  - `ENCRYPT_REQ` (0x03) - proceed with TLS (server requires it)
  - `ENCRYPT_OFF` (0x00) - fail connection with error
  - `ENCRYPT_NOT_SUP` (0x02) - fail connection with error

When `use_encrypt=false`:
- Client sends `ENCRYPT_NOT_SUP` (0x02) - current behavior unchanged

## Connection Pool Considerations

### Pool Key Extension

TLS and non-TLS connections must be pooled separately. The pool key should include `use_encrypt` value:

```
Current key: (host, port, database, user)
New key:     (host, port, database, user, use_encrypt)
```

### TLS State Preservation

- TLS context must be preserved with pooled connections
- No re-handshake on pool reuse - TLS session continues
- Pool validation (ping) must work over TLS layer

## Error Message Mapping

| mbedTLS Error | User-Facing Message |
|--------------|---------------------|
| `MBEDTLS_ERR_SSL_TIMEOUT` | "TLS handshake timed out" |
| `MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY` | "Server closed TLS connection" |
| `MBEDTLS_ERR_SSL_HANDSHAKE_FAILURE` | "TLS handshake failed: [details]" |
| `MBEDTLS_ERR_SSL_*` (others) | "TLS error: [mbedtls_strerror output]" |
| Server `ENCRYPT_NOT_SUP` response | "TLS requested but server does not support encryption" |

## Test Environment

### SQL Server with TLS (Docker)

SQL Server Docker images support TLS with self-signed certificates:

```bash
docker run -e 'ACCEPT_EULA=Y' -e 'SA_PASSWORD=TestPassword1!' \
    -e 'MSSQL_TLS_FORCE_ENCRYPTION=1' \
    -p 1433:1433 \
    mcr.microsoft.com/mssql/server:2022-latest
```

Integration tests can verify TLS connections using this container.
