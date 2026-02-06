# Quick Start: TDS FEDAUTH Integration (Phase 2)

**Created**: 2026-02-05
**Status**: Complete
**Purpose**: Developer setup and verification steps for TDS FEDAUTH implementation

---

## Prerequisites

### Required

1. **Phase 1 Complete**: Azure token infrastructure must be working
   - `mssql_azure_auth_test()` function available and tested
   - Azure secret reading from DuckDB Azure extension working
   - libcurl linked for OAuth2 token acquisition

2. **Development Environment**:
   - C++17 compiler (GCC 9+, Clang 10+, MSVC 2019+)
   - DuckDB main branch (submodule in `duckdb/`)
   - OpenSSL 1.1.0+ (via vcpkg)
   - libcurl (via vcpkg)

3. **Azure Resources** (for integration testing):
   - Azure AD tenant with service principal
   - Azure SQL Database (or Fabric Warehouse) with Azure AD auth enabled
   - Service principal granted database access

### Optional

- Azure CLI (`az`) for CLI credential testing
- Microsoft Fabric Warehouse access for Fabric-specific tests

---

## Development Setup

### 1. Build the Extension

```bash
# Clean build
make clean && make

# Or debug build for development
make debug
```

### 2. Verify Phase 1 Works

```sql
-- Load extensions
LOAD azure;
LOAD mssql;

-- Create Azure secret
CREATE SECRET test_azure (
    TYPE azure,
    PROVIDER service_principal,
    TENANT_ID 'your-tenant-id',
    CLIENT_ID 'your-client-id',
    CLIENT_SECRET 'your-client-secret'
);

-- Test token acquisition (Phase 1)
SELECT mssql_azure_auth_test('test_azure');
-- Expected: Token string (eyJ0eXAi...) or error message
```

### 3. Set Up Test Environment Variables

For integration tests, set these environment variables:

```bash
# Required for Azure SQL tests
export AZURE_TEST_TENANT_ID="your-tenant-id"
export AZURE_TEST_CLIENT_ID="your-client-id"
export AZURE_TEST_CLIENT_SECRET="your-client-secret"
export AZURE_SQL_TEST_HOST="yourserver.database.windows.net"
export AZURE_SQL_TEST_DATABASE="testdb"

# Optional for Fabric tests
export FABRIC_TEST_HOST="yourworkspace.datawarehouse.fabric.microsoft.com"
export FABRIC_TEST_DATABASE="warehouse"
```

---

## Implementation Verification

### Step 1: PRELOGIN with FEDAUTHREQUIRED

Verify PRELOGIN packet includes FEDAUTHREQUIRED option for Azure endpoints.

**Unit Test**:

```cpp
// test/cpp/test_fedauth_encoding.cpp
TEST_CASE("PRELOGIN includes FEDAUTHREQUIRED for Azure", "[fedauth]") {
    PreloginOptions options;
    options.fedauth_required = true;

    auto packet = options.Build();

    // Verify FEDAUTHREQUIRED option (0x06) is present with value 0x01
    // ... assertions
}
```

**Manual Verification** (with MSSQL_DEBUG=2):

```bash
MSSQL_DEBUG=2 ./build/release/duckdb -c "
LOAD azure; LOAD mssql;
CREATE SECRET az (TYPE azure, PROVIDER service_principal, ...);
CREATE SECRET sql (TYPE mssql, HOST 'test.database.windows.net', DATABASE 'db', AZURE_SECRET 'az');
ATTACH '' AS db (TYPE mssql, SECRET sql);
"
```

Expected debug output should show PRELOGIN with option 0x06.

### Step 2: LOGIN7 with FEDAUTH Extension

Verify LOGIN7 packet includes FEDAUTH feature extension with UTF-16LE token.

**Unit Test**:

```cpp
// test/cpp/test_fedauth_encoding.cpp
TEST_CASE("LOGIN7 includes FEDAUTH extension", "[fedauth]") {
    FedAuthData fedauth;
    fedauth.library = FedAuthLibrary::MSAL;
    fedauth.token_utf16le = encoding::Utf8ToUtf16LE("test_token");

    auto extension = FeatureExtension::CreateFedAuth(fedauth);

    REQUIRE(extension.feature_id == FeatureExtId::FEDAUTH);
    REQUIRE(extension.data.size() == 4 + fedauth.token_utf16le.size());
    // ... more assertions
}
```

### Step 3: UTF-16LE Token Encoding

Verify token encoding is correct.

**Unit Test**:

```cpp
// test/cpp/test_fedauth_encoding.cpp
TEST_CASE("Token encoded as UTF-16LE", "[fedauth]") {
    std::string token = "eyJ0eXAiOiJKV1Q";

    auto encoded = encoding::Utf8ToUtf16LE(token);

    // Verify each ASCII character is followed by 0x00
    REQUIRE(encoded.size() == token.size() * 2);
    for (size_t i = 0; i < token.size(); i++) {
        REQUIRE(encoded[i * 2] == static_cast<uint8_t>(token[i]));
        REQUIRE(encoded[i * 2 + 1] == 0x00);
    }
}
```

### Step 4: TLS Hostname Verification

Verify hostname verification for Azure endpoints.

**Unit Test**:

```cpp
// test/cpp/test_hostname_verification.cpp
TEST_CASE("Hostname verification patterns", "[tls]") {
    // Exact match
    REQUIRE(MatchHostname("server.database.windows.net", "server.database.windows.net"));

    // Wildcard match
    REQUIRE(MatchHostname("server.database.windows.net", "*.database.windows.net"));

    // Wildcard doesn't match multiple labels
    REQUIRE_FALSE(MatchHostname("a.b.database.windows.net", "*.database.windows.net"));

    // Wrong domain
    REQUIRE_FALSE(MatchHostname("server.database.windows.net", "*.fabric.microsoft.com"));
}
```

### Step 5: Endpoint Detection

Verify endpoint type detection.

**Unit Test**:

```cpp
// test/cpp/test_fedauth_encoding.cpp
TEST_CASE("Endpoint detection", "[fedauth]") {
    // Azure SQL
    REQUIRE(IsAzureEndpoint("server.database.windows.net"));
    REQUIRE(GetEndpointType("server.database.windows.net") == EndpointType::AzureSQL);

    // Fabric
    REQUIRE(IsFabricEndpoint("ws.datawarehouse.fabric.microsoft.com"));
    REQUIRE(GetEndpointType("ws.datawarehouse.fabric.microsoft.com") == EndpointType::Fabric);

    // On-premises
    REQUIRE_FALSE(IsAzureEndpoint("sqlserver.company.com"));
    REQUIRE(GetEndpointType("sqlserver.company.com") == EndpointType::OnPremises);
}
```

### Step 6: Azure SQL Connection

Verify end-to-end connection to Azure SQL Database.

**Integration Test** (requires Azure credentials):

```sql
-- test/sql/azure/azure_service_principal.test
# name: test/sql/azure/azure_service_principal.test
# group: [azure]

require mssql
require-env AZURE_TEST_TENANT_ID
require-env AZURE_TEST_CLIENT_ID
require-env AZURE_TEST_CLIENT_SECRET
require-env AZURE_SQL_TEST_HOST

statement ok
LOAD azure;

statement ok
LOAD mssql;

statement ok
CREATE SECRET test_azure (
    TYPE azure,
    PROVIDER service_principal,
    TENANT_ID '${AZURE_TEST_TENANT_ID}',
    CLIENT_ID '${AZURE_TEST_CLIENT_ID}',
    CLIENT_SECRET '${AZURE_TEST_CLIENT_SECRET}'
);

statement ok
ATTACH 'Server=${AZURE_SQL_TEST_HOST};Database=${AZURE_SQL_TEST_DATABASE:-testdb}' AS azuredb (
    TYPE mssql,
    AZURE_SECRET 'test_azure'
);

query I
SELECT 1;
----
1

statement ok
DETACH azuredb;
```

---

## Test Commands

### Run Unit Tests (No Azure Required)

```bash
# All unit tests
make test

# Only FEDAUTH-related tests
./build/release/unittest "*fedauth*"
./build/release/unittest "*hostname*"
```

### Run Azure Integration Tests

```bash
# Set environment variables first (see above)

# All Azure tests
make azure-integration-test

# Specific test
./build/release/duckdb -c ".read test/sql/azure/azure_service_principal.test"
```

### Run All Tests

```bash
# Unit tests + existing SQL Server integration tests
make test-all

# With Azure tests (if credentials available)
AZURE_TEST_TENANT_ID=... make test-all
```

---

## Debug Tips

### Enable TDS Protocol Debugging

```bash
# Level 1: Basic connection info
MSSQL_DEBUG=1 ./build/release/duckdb

# Level 2: Packet details
MSSQL_DEBUG=2 ./build/release/duckdb

# Level 3: Full trace (very verbose)
MSSQL_DEBUG=3 ./build/release/duckdb
```

### Check Token Acquisition

```sql
-- Verify token works
SELECT mssql_azure_auth_test('my_azure_secret');

-- Check for errors
-- Error: "Azure AD error AADSTS7000215: Invalid client secret"
-- Fix: Check CLIENT_SECRET value
```

### TLS Debugging

```bash
# Verbose OpenSSL output
OPENSSL_DEBUG=1 ./build/release/duckdb
```

---

## Common Issues

### Token Acquisition Fails

**Symptom**: `mssql_azure_auth_test()` returns error

**Check**:

1. Verify Azure secret values (tenant_id, client_id, client_secret)
2. Ensure service principal exists in Azure AD
3. Check network connectivity to `login.microsoftonline.com`

### Connection Fails with Valid Token

**Symptom**: Token acquired but LOGIN7 fails

**Check**:

1. Service principal has database access (Azure SQL IAM)
2. Database firewall allows your IP
3. FEDAUTH extension is being sent (use MSSQL_DEBUG=2)

### TLS Hostname Verification Fails

**Symptom**: TLS error with hostname mismatch

**Check**:

1. Hostname matches Azure SQL endpoint exactly
2. No proxy or DNS override changing the endpoint
3. Certificate is valid Azure certificate

### Existing SQL Auth Tests Fail

**Symptom**: Regression in non-Azure tests

**Check**:

1. SQL auth path unchanged (no azure_secret in secret)
2. PRELOGIN for on-prem doesn't include FEDAUTHREQUIRED
3. TLS hostname verification disabled for on-prem

---

## Success Criteria Checklist

- [ ] Unit tests pass: FEDAUTH encoding, hostname matching, endpoint detection
- [ ] Phase 1 token acquisition still works
- [ ] Azure SQL connection with service principal works
- [ ] Azure SQL connection with CLI credentials works
- [ ] Catalog operations work on Azure SQL
- [ ] DML operations work on Azure SQL
- [ ] Existing SQL auth tests pass (zero regressions)
- [ ] Clear error messages for auth failures
- [ ] Fabric connection works (basic operations)
- [ ] COPY/BCP works on Azure SQL
