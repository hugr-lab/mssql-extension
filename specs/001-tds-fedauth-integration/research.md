# Research: TDS FEDAUTH Integration (Phase 2)

**Created**: 2026-02-05
**Status**: Complete
**Purpose**: Technical research for TDS FEDAUTH protocol integration

---

## TDS Protocol - FEDAUTH Extension

### Protocol Overview

TDS v7.4+ supports Federated Authentication (FEDAUTH) for Azure AD token-based login. This enables passing pre-acquired OAuth2 tokens instead of username/password credentials.

**Key Protocol Components**:
1. **PRELOGIN** - Advertise FEDAUTH requirement via option 0x06
2. **LOGIN7** - Include FEDAUTH feature extension (0x02) with UTF-16LE token
3. **FEDAUTHINFO** - Server response with STS URL (optional, for redirect scenarios)

### PRELOGIN Packet Changes

Add FEDAUTHREQUIRED option (0x06) when connecting to Azure endpoints:

```text
PRELOGIN Options (existing):
├── VERSION (0x00)      - TDS version negotiation
├── ENCRYPTION (0x01)   - Encryption negotiation
├── INSTOPT (0x02)      - Instance name
├── THREADID (0x03)     - Client thread ID
├── MARS (0x04)         - Multiple Active Result Sets
└── TERMINATOR (0xFF)

PRELOGIN Options (with FEDAUTH):
├── VERSION (0x00)
├── ENCRYPTION (0x01)
├── INSTOPT (0x02)
├── THREADID (0x03)
├── MARS (0x04)
├── FEDAUTHREQUIRED (0x06) ← NEW: 1 byte, value 0x01
└── TERMINATOR (0xFF)
```

**FEDAUTHREQUIRED Option Format**:
- Option ID: 0x06
- Data Length: 1 byte
- Value: 0x01 (client requires federated authentication)

### LOGIN7 FEDAUTH Extension

When using Azure auth, add FEDAUTH feature extension to LOGIN7 packet:

```text
LOGIN7 Packet Structure:
├── Fixed Header (94 bytes)
├── Variable Data (hostname, username, app name, server name, etc.)
├── FeatureExt Offset → Points to Feature Extensions
└── Feature Extensions:
    ├── FEATUREEXT_FEDAUTH (0x02)
    │   ├── FeatureId: 0x02
    │   ├── FeatureDataLen: 4 + token_length_bytes
    │   ├── Options: 4 bytes
    │   │   ├── Byte 0: FedAuthLibrary (0x02 = ADAL/MSAL)
    │   │   ├── Byte 1-2: Workflow (0x01 = client provides token)
    │   │   └── Byte 3: Reserved
    │   └── Token: UTF-16LE encoded JWT access token
    └── FEATUREEXT_TERMINATOR (0xFF)
```

**FedAuth Library Values**:

| Value | Library | Description | Our Use |
|-------|---------|-------------|---------|
| 0x01 | SSPI | Windows integrated auth | No |
| **0x02** | ADAL/MSAL | Azure AD library | **Yes** |
| 0x03 | Security Token | Pre-acquired token | No |

### Token Encoding

The access token MUST be encoded as UTF-16LE for the TDS protocol:

```cpp
// Token is acquired as UTF-8 from OAuth2 endpoint
std::string token_utf8 = "eyJ0eXAiOiJKV1QiLCJhbGciOiJSUzI1NiIs...";

// Convert to UTF-16LE for TDS FEDAUTH packet
std::vector<uint8_t> token_utf16le;
encoding::Utf8ToUtf16LE(token_utf8, token_utf16le);

// In LOGIN7 FEDAUTH extension:
// 4 bytes: Options (library=0x02, workflow=0x01)
// token_utf16le.size() bytes: UTF-16LE encoded token
```

### FEDAUTHINFO Response Token

Server may return FEDAUTHINFO (0xEE) with Azure AD guidance:

```text
FEDAUTHINFO Token (0xEE):
├── TokenType: 0xEE
├── Length: 4 bytes (total data length)
├── CountOfInfoIDs: DWORD
└── InfoData[]:
    ├── InfoID: DWORD (1=SPN, 2=STSURL)
    ├── InfoDataLen: DWORD
    └── InfoData: variable-length data
```

**InfoID Values**:
- 1 (SPN): Service Principal Name
- 2 (STSURL): Security Token Service URL (for token acquisition)

**Note**: In most cases with pre-acquired tokens, FEDAUTHINFO is informational only. We should log it but don't need to act on it since we already have the token.

---

## TLS Hostname Verification

### Requirement

Azure endpoints require TLS with proper hostname verification:
- `*.database.windows.net` - Azure SQL Database
- `*.datawarehouse.fabric.microsoft.com` - Microsoft Fabric
- `*.pbidedicated.windows.net` - Power BI Dedicated (Fabric)
- `*.sql.azuresynapse.net` - Azure Synapse Analytics

On-premises SQL Server should continue accepting self-signed certificates (existing behavior).

### OpenSSL Implementation

Modern OpenSSL (1.1.0+) provides built-in hostname checking:

```cpp
// For Azure endpoints - enable hostname verification
SSL_set_hostflags(ssl, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
SSL_set1_host(ssl, host.c_str());
SSL_set_verify(ssl, SSL_VERIFY_PEER, nullptr);

// For on-premises - accept any certificate (existing behavior)
SSL_set_verify(ssl, SSL_VERIFY_NONE, nullptr);
```

### Wildcard Certificate Matching

Azure uses wildcard certificates (e.g., `*.database.windows.net`). The matching rules are:

1. Wildcard `*` matches only a single label (no dots)
2. `*.database.windows.net` matches `myserver.database.windows.net`
3. `*.database.windows.net` does NOT match `a.b.database.windows.net`

```cpp
bool MatchHostname(const std::string &host, const std::string &pattern) {
    // Exact match
    if (pattern == host) return true;

    // Wildcard match (*.example.com)
    if (pattern.size() > 2 && pattern[0] == '*' && pattern[1] == '.') {
        auto suffix = pattern.substr(1);  // .example.com
        if (host.size() > suffix.size() &&
            host.compare(host.size() - suffix.size(), suffix.size(), suffix) == 0) {
            auto prefix = host.substr(0, host.size() - suffix.size());
            return prefix.find('.') == std::string::npos;  // No dots in prefix
        }
    }
    return false;
}
```

---

## Azure AD Token Acquisition

### OAuth2 Client Credentials Flow

Phase 1 implemented token acquisition via libcurl. The flow for service principal:

```text
POST https://login.microsoftonline.com/{tenant_id}/oauth2/v2.0/token
Content-Type: application/x-www-form-urlencoded

grant_type=client_credentials
&client_id={client_id}
&client_secret={client_secret}
&scope=https://database.windows.net/.default
```

Response:
```json
{
  "token_type": "Bearer",
  "expires_in": 3599,
  "access_token": "eyJ0eXAiOiJKV1QiLCJhbGciOiJSUzI1NiIs..."
}
```

### Required Scope

The scope `https://database.windows.net/.default` works for:
- Azure SQL Database
- Azure SQL Managed Instance
- Microsoft Fabric SQL Endpoint
- Azure Synapse Analytics

### Token Lifetime

- Default expiration: ~1 hour (3600 seconds)
- Refresh strategy: 5 minutes before expiration
- No refresh token in client credentials flow - acquire new token

---

## Endpoint Detection

### Azure SQL Database

Pattern: `*.database.windows.net`

Examples:
- `myserver.database.windows.net`
- `myserver-eastus.database.windows.net`

### Microsoft Fabric

Patterns:
- `*.datawarehouse.fabric.microsoft.com`
- `*.pbidedicated.windows.net`

Examples:
- `xyz123.datawarehouse.fabric.microsoft.com`
- `workspace-abc.pbidedicated.windows.net`

### Azure Synapse Analytics

Pattern: `*-ondemand.sql.azuresynapse.net`

Examples:
- `myworkspace-ondemand.sql.azuresynapse.net`

### Detection Logic

```cpp
bool IsAzureEndpoint(const std::string &host) {
    return host.find(".database.windows.net") != std::string::npos ||
           host.find(".datawarehouse.fabric.microsoft.com") != std::string::npos ||
           host.find(".pbidedicated.windows.net") != std::string::npos ||
           host.find(".sql.azuresynapse.net") != std::string::npos;
}

bool IsFabricEndpoint(const std::string &host) {
    return host.find(".datawarehouse.fabric.microsoft.com") != std::string::npos ||
           host.find(".pbidedicated.windows.net") != std::string::npos;
}
```

---

## Azure vs Fabric Comparison

| Feature | Azure SQL Database | Microsoft Fabric Warehouse |
|---------|-------------------|---------------------------|
| Port | 1433 | 1433 |
| TLS | Required | Required |
| Auth | Azure AD or SQL | Azure AD only |
| SELECT | Full support | Full support |
| INSERT | Full support | Limited |
| UPDATE/DELETE | Full support | Limited |
| DDL | Full support | Limited |
| COPY/BCP | Full support | TBD (needs testing) |
| DBCC statistics | With permissions | Not available |
| Transactions | Full support | Limited |
| Catalog views | Full sys.* views | Subset of sys.* views |

### Fabric Limitations

Microsoft Fabric Warehouse has significant limitations compared to Azure SQL:
1. **Read-mostly workload** - Optimized for analytics, not OLTP
2. **Limited DML** - INSERT may be restricted, UPDATE/DELETE often unavailable
3. **No DBCC** - Statistics retrieval via DBCC not supported
4. **Schema differences** - Some sys.* views may differ or be unavailable

**Mitigation**: Detect Fabric endpoints and provide graceful degradation with clear error messages.

---

## Error Handling

### Azure AD Error Codes

| Code | Description | User Action |
|------|-------------|-------------|
| AADSTS7000215 | Invalid client secret | Check client_secret value |
| AADSTS700016 | Application not found in tenant | Verify client_id and tenant_id |
| AADSTS50034 | User account doesn't exist | Verify user principal |
| AADSTS65001 | User hasn't consented | Grant admin consent |
| AADSTS70011 | Invalid scope | Check OAuth2 scope |
| AADSTS90002 | Tenant not found | Verify tenant_id |

### Error Message Format

```text
Azure AD authentication failed: AADSTS7000215 - Invalid client secret provided.
Verify the client_secret in your Azure secret 'my_azure_secret'.
```

### TDS Authentication Errors

| Error | Cause | Resolution |
|-------|-------|------------|
| Login failed | Token rejected by SQL Server | Check Azure AD app permissions |
| TLS handshake failed | Certificate verification failed | Verify endpoint hostname |
| Connection refused | Network/firewall issue | Check Azure SQL firewall rules |

---

## Implementation Decisions

### Decision 1: Token Acquisition Before LOGIN7

**Choice**: Acquire token BEFORE building LOGIN7 packet, not during.

**Rationale**: Token acquisition may involve network calls (OAuth2) or CLI invocation (`az account get-access-token`). Doing this during packet construction would complicate error handling and add latency in the critical path.

### Decision 2: PRELOGIN Always Sent

**Choice**: Always send PRELOGIN packet for both SQL and Azure auth paths.

**Rationale**: TDS spec requires PRELOGIN. Only difference is Azure auth adds FEDAUTHREQUIRED option.

### Decision 3: OpenSSL-Only Hostname Verification

**Choice**: Use OpenSSL APIs only for hostname verification, no additional libraries.

**Rationale**: OpenSSL is already a dependency. Adding another library for hostname verification adds complexity without benefit.

### Decision 4: Graceful Fabric Degradation

**Choice**: Detect Fabric endpoints and provide clear error messages for unsupported operations.

**Rationale**: Fabric has different capabilities. Silent failures are confusing; clear errors enable users to understand limitations.

---

## References

- [MS-TDS Protocol Specification](https://docs.microsoft.com/en-us/openspecs/windows_protocols/ms-tds/)
- [MS-TDS 2.2.7.1 FEDAUTH Feature Extension](https://docs.microsoft.com/en-us/openspecs/windows_protocols/ms-tds/773a62b6-ee89-4c02-9e5e-344f39b4bf76)
- [Azure AD Authentication for Azure SQL](https://docs.microsoft.com/en-us/azure/azure-sql/database/authentication-aad-overview)
- [OAuth2 Client Credentials Flow](https://docs.microsoft.com/en-us/azure/active-directory/develop/v2-oauth2-client-creds-grant-flow)
- [Microsoft Fabric Documentation](https://docs.microsoft.com/en-us/fabric/)
- [OpenSSL SSL_set1_host](https://www.openssl.org/docs/man1.1.1/man3/SSL_set1_host.html)
