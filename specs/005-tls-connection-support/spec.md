# Feature Specification: TLS Connection Support

**Feature Branch**: `005-tls-connection-support`
**Created**: 2026-01-16
**Status**: Implemented
**Completed**: 2026-01-16
**Input**: Build TLS support for our DuckDB MS SQL (TDS) extension. Add an optional TLS layer controlled by connection option `use_encrypt` (default false). If `use_encrypt=true`, negotiate encryption via TDS PRELOGIN, then wrap the socket with TLS. Provide "trust server certificate" behavior. TLS must work on Linux/macOS/Windows using an embeddable library (mbedTLS/BearSSL).

## Clarifications

### Session 2026-01-16

- Q: What observability signals should TLS operations emit? → A: Debug-level log messages for handshake start/success/failure
- Q: What latency overhead is acceptable for TLS handshake? → A: ≤500ms additional latency for TLS handshake

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Encrypted Connection to SQL Server (Priority: P1)

As a DuckDB user, I want to connect to SQL Server over an encrypted TLS connection so that my credentials and data are protected in transit when connecting over untrusted networks.

**Why this priority**: TLS encryption is the core value of this feature. Without it, users cannot securely connect to SQL Server instances that require encryption or are accessed over public networks.

**Independent Test**: Can be fully tested by creating a secret with `use_encrypt=true`, attaching to a TLS-enabled SQL Server, and verifying the connection succeeds and data can be queried. Network traffic inspection would show encrypted content.

**Acceptance Scenarios**:

1. **Given** a SQL Server configured to accept encrypted connections and a DuckDB secret with `use_encrypt=true`, **When** I attach to the database, **Then** the connection is established using TLS encryption and queries execute successfully.
2. **Given** a valid TLS connection, **When** I execute queries and examine network traffic, **Then** the traffic between DuckDB and SQL Server is encrypted (not plaintext TDS packets).
3. **Given** a secret with `use_encrypt=true`, **When** TLS negotiation succeeds, **Then** subsequent TDS protocol operations (LOGIN7, SQL_BATCH, etc.) occur over the encrypted channel.

---

### User Story 2 - Plaintext Connection Remains Default (Priority: P1)

As a DuckDB user with existing unencrypted connections, I want TLS to be opt-in so that my existing workflows continue to work without modification.

**Why this priority**: Backward compatibility is critical. Existing users must not experience breaking changes when upgrading to a version with TLS support.

**Independent Test**: Can be fully tested by creating a secret without `use_encrypt` or with `use_encrypt=false`, connecting to SQL Server, and verifying the connection works exactly as before (plaintext TDS).

**Acceptance Scenarios**:

1. **Given** a secret without the `use_encrypt` option specified, **When** I attach to the database, **Then** the connection uses plaintext TDS (no TLS) as it does today.
2. **Given** a secret with `use_encrypt=false`, **When** I attach to the database, **Then** the connection uses plaintext TDS.
3. **Given** an existing workflow using secrets created before TLS support was added, **When** I use the updated extension, **Then** connections behave identically to before (no TLS negotiation attempted).

---

### User Story 3 - Clear TLS Error Messages (Priority: P1)

As a DuckDB user, I want clear error messages when TLS connection fails so that I can diagnose and fix connection problems.

**Why this priority**: Without clear error messages, users cannot troubleshoot TLS failures, leading to frustration and support burden.

**Independent Test**: Can be fully tested by intentionally causing various TLS failures (wrong port, server doesn't support TLS, handshake timeout) and verifying each produces a distinct, actionable error message.

**Acceptance Scenarios**:

1. **Given** `use_encrypt=true` and a SQL Server that does not support encryption, **When** I attempt to connect, **Then** I receive an error message indicating the server does not support TLS encryption.
2. **Given** `use_encrypt=true` and a connection timeout during TLS handshake, **When** the handshake times out, **Then** I receive an error message indicating TLS handshake timed out.
3. **Given** `use_encrypt=true` and a TLS protocol error (e.g., unsupported cipher), **When** the handshake fails, **Then** I receive an error message with TLS library details to aid debugging.

---

### User Story 4 - Trust Server Certificate by Default (Priority: P2)

As a DuckDB user connecting to a development or internal SQL Server with a self-signed certificate, I want `use_encrypt=true` to automatically trust the server certificate so that I can use TLS without complex certificate management.

**Why this priority**: Many development and internal environments use self-signed certificates. Requiring CA verification would make TLS impractical for these common use cases. This default behavior may be extended with verification options in future versions.

**Independent Test**: Can be fully tested by connecting with `use_encrypt=true` to a SQL Server using a self-signed certificate and verifying the connection succeeds.

**Acceptance Scenarios**:

1. **Given** `use_encrypt=true` and a SQL Server with a self-signed certificate, **When** I connect, **Then** the TLS connection is established (certificate is trusted without verification).
2. **Given** `use_encrypt=true` and a SQL Server with an expired certificate, **When** I connect, **Then** the TLS connection is established (certificate validity is not checked).
3. **Given** `use_encrypt=true`, **When** connecting to any TLS-enabled server, **Then** the server certificate is trusted automatically (no hostname or CA verification required).

---

### User Story 5 - Cross-Platform TLS Support (Priority: P2)

As a DuckDB user on Linux, macOS, or Windows, I want TLS connections to work on my platform so that I can use encrypted connections regardless of my operating system.

**Why this priority**: DuckDB extensions must work cross-platform. TLS support limited to one platform would fragment the user experience.

**Independent Test**: Can be fully tested by running the same TLS connection test on each supported platform and verifying all succeed.

**Acceptance Scenarios**:

1. **Given** a Linux system and `use_encrypt=true`, **When** I connect to SQL Server, **Then** TLS encryption works correctly.
2. **Given** a macOS system and `use_encrypt=true`, **When** I connect to SQL Server, **Then** TLS encryption works correctly.
3. **Given** a Windows system and `use_encrypt=true`, **When** I connect to SQL Server, **Then** TLS encryption works correctly.

---

### Edge Cases

- What happens when `use_encrypt=true` but the server responds with `ENCRYPT_NOT_SUP` in PRELOGIN? → Connection fails with error: "TLS requested but server does not support encryption."
- What happens when the server requires encryption (`ENCRYPT_REQ`) but `use_encrypt=false`? → Connection attempt proceeds without TLS; server will reject LOGIN7 with an authentication error (existing behavior preserved).
- What happens if TLS handshake succeeds but the server terminates connection immediately after? → Connection fails with error indicating server closed connection after TLS handshake.
- What happens if the TLS library fails to initialize? → Connection fails with error: "Failed to initialize TLS: [library error details]."
- What happens during connection pooling with TLS connections? → TLS state is preserved with the connection; pooled connections remain encrypted when reused.
- What happens if a TLS connection in the pool becomes stale (server closes it)? → Standard pool validation (ping) detects the dead connection; connection is removed from pool and a new TLS connection is established.

## Requirements *(mandatory)*

### Functional Requirements

#### Secret Configuration

- **FR-001**: System MUST support a `use_encrypt` option in mssql secrets that accepts boolean values (`true`/`false`).
- **FR-002**: The `use_encrypt` option MUST default to `false` when not specified (backward compatible).
- **FR-003**: System MUST expose the `use_encrypt` setting in secret creation syntax: `CREATE SECRET ... (TYPE mssql, host '...', use_encrypt true, ...)`.

#### TLS Negotiation via PRELOGIN

- **FR-004**: When `use_encrypt=true`, system MUST send `ENCRYPT_ON` in the PRELOGIN encryption option.
- **FR-005**: When `use_encrypt=false` or unset, system MUST continue sending `ENCRYPT_NOT_SUP` in PRELOGIN (existing behavior).
- **FR-006**: After PRELOGIN response, if client requested encryption and server responds with `ENCRYPT_OFF` or `ENCRYPT_NOT_SUP`, system MUST fail with a clear error message.
- **FR-007**: After PRELOGIN response, if client requested encryption and server responds with `ENCRYPT_ON` or `ENCRYPT_REQ`, system MUST initiate TLS handshake before sending LOGIN7.

#### TLS Handshake and Socket Wrapping

- **FR-008**: System MUST perform TLS handshake over the existing TCP socket after successful PRELOGIN negotiation.
- **FR-009**: After TLS handshake completes, all subsequent TDS packets (LOGIN7, SQL_BATCH, responses, etc.) MUST be sent/received through the TLS layer.
- **FR-010**: The TLS layer MUST be transparent to the existing TDS framing and token parsing code (same packet structure, just encrypted transport).
- **FR-011**: System MUST support TLS 1.2 as the minimum version (SQL Server 2016+ requirement).

#### Certificate Handling

- **FR-012**: When `use_encrypt=true`, system MUST trust the server certificate without verification (no CA verification, no hostname verification). This behavior may be extended with additional verification options in future versions.
- **FR-013**: System MUST accept any server certificate regardless of issuer, expiration, or hostname mismatch when `use_encrypt=true`.
- **FR-014**: Certificate verification options (CA path, hostname verification) are out of scope for this phase but the implementation SHOULD be designed to accommodate future extensions.

#### Error Handling

- **FR-015**: System MUST provide distinct error messages for: TLS not supported by server, TLS handshake failure, TLS timeout, TLS library initialization failure.
- **FR-016**: TLS errors MUST include relevant details from the TLS library to aid debugging.
- **FR-017**: When TLS connection fails, system MUST NOT fall back to plaintext (fail securely).

#### Observability

- **FR-024**: System MUST emit debug-level log messages for TLS handshake start, success, and failure events to aid troubleshooting.

#### Connection Pool Integration

- **FR-018**: TLS connections MUST work with the existing connection pool (pooled connections retain their TLS state).
- **FR-019**: Pool health checks (ping) MUST work correctly over TLS connections.
- **FR-020**: TLS and non-TLS connections MUST be kept in separate pools (keyed by secret configuration including `use_encrypt`).

#### TLS Library Requirements

- **FR-021**: System MUST use an embeddable TLS library that can be statically linked (no system OpenSSL dependency).
- **FR-022**: System MUST compile and work on Linux, macOS, and Windows.
- **FR-023**: TLS library MUST be managed through vcpkg for consistent cross-platform builds.

### Key Entities

- **TlsSocket**: A TLS-wrapped socket that provides the same interface as `TdsSocket` but encrypts/decrypts data transparently. Wraps the raw TCP file descriptor and handles TLS handshake, read, write, and shutdown.
- **use_encrypt**: Boolean secret option that controls whether TLS encryption is requested for the connection. When `true`, server certificates are trusted automatically (this default may be extended with verification options in future versions).
- **EncryptionOption**: Existing enum extended in usage: `ENCRYPT_ON` is now sent when `use_encrypt=true` is configured.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: Users can establish TLS-encrypted connections to SQL Server with `use_encrypt=true` successfully.
- **SC-002**: Existing plaintext connections continue to work without any configuration changes.
- **SC-003**: TLS connection establishment completes within the configured connection timeout (same timeout applies to TLS handshake).
- **SC-004**: TLS handshake adds ≤500ms additional latency compared to plaintext connection establishment; per-query encryption overhead is negligible.
- **SC-005**: TLS connections work correctly with connection pooling (connections are reused without re-handshaking).
- **SC-006**: All TLS failure scenarios produce distinct, actionable error messages.
- **SC-007**: TLS functionality works correctly on all three supported platforms (Linux, macOS, Windows).

## Scope Boundaries

### In Scope

- `use_encrypt` secret option for enabling TLS
- TLS negotiation via TDS PRELOGIN protocol
- TLS handshake after PRELOGIN (before LOGIN7)
- Transparent TLS wrapping of existing TDS socket
- Trust server certificate mode (no verification)
- TLS 1.2+ support
- Integration with existing connection pool
- Cross-platform support via embeddable TLS library (mbedTLS or BearSSL)
- Clear error messages for TLS failures
- Integration test with TLS-enabled SQL Server

### Out of Scope (Non-Goals)

- CA certificate verification (trusted CA paths)
- Hostname verification against certificate CN/SAN
- Client certificate authentication (mutual TLS)
- TLS session resumption/caching
- Configurable TLS version or cipher suites
- TLS for Azure SQL Database specifics (uses standard TLS)
- Forcing encryption when server doesn't require it (only client-requested encryption)

## Assumptions

- SQL Server is configured to accept TLS connections (has a valid server certificate).
- The TLS library (mbedTLS or BearSSL) can be integrated via vcpkg with static linking.
- "Trust server certificate" mode (when `use_encrypt=true`) is acceptable for the initial implementation. This matches common JDBC/ODBC driver behavior (e.g., `trustServerCertificate=true`). Future versions may add options for CA verification and hostname checking.
- TLS 1.2 is sufficient (SQL Server 2016+ supports TLS 1.2; older servers may require TLS 1.0/1.1 but are out of scope).
- The existing `TdsSocket` interface can be extended or wrapped to support TLS without major refactoring.
- Connection pool keying already considers secret configuration; adding `use_encrypt` to the key maintains pool isolation.
