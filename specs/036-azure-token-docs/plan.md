# Implementation Plan: Azure Token-Only Secret & Documentation Updates

**Branch**: `036-azure-token-docs` | **Date**: 2026-02-13 | **Spec**: [spec.md](spec.md)
**Input**: Feature specification from `/specs/036-azure-token-docs/spec.md`

## Summary

Add a new `access_token` provider for `TYPE azure` secrets, allowing users to store a pre-provided JWT token in an Azure secret and reuse it across multiple MSSQL connections via `AZURE_SECRET`. Update AZURE.md with new provider docs and README.md with PK update limitation (issues #57, #53).

## Technical Context

**Language/Version**: C++17 (C++11-compatible for ODR on Linux) + DuckDB v1.5-variegata
**Primary Dependencies**: DuckDB extension API, OpenSSL (vcpkg), libcurl (vcpkg)
**Storage**: N/A (remote SQL Server via TDS protocol)
**Testing**: DuckDB SQLLogicTest (unittest framework)
**Target Platform**: Linux (GCC), macOS (Clang), Windows (MSVC)
**Project Type**: DuckDB extension (single project)

## Constitution Check

| Principle | Status | Notes |
|-----------|--------|-------|
| I. Native and Open | PASS | No new external dependencies |
| II. Streaming First | PASS | No change to streaming behavior |
| III. Correctness over Convenience | PASS | Token validation unchanged; clear errors for missing fields |
| IV. Explicit State Machines | PASS | No state machine changes |
| V. DuckDB-Native UX | PASS | New secret provider follows DuckDB patterns |
| VI. Incremental Delivery | PASS | Backward-compatible enhancement |

## Project Structure

### Files Modified (this feature)

```text
src/include/azure/azure_secret_reader.hpp     # Add access_token field to AzureSecretInfo
src/azure/azure_secret_reader.cpp             # Read access_token from Azure secret
src/azure/azure_token.cpp                     # Handle access_token provider in AcquireToken()
src/mssql_extension.cpp                       # Register access_token provider for TYPE azure
AZURE.md                                      # Document new access_token provider
README.md                                     # Add PK update limitation notice
test/sql/azure/azure_secret_token_only.test   # New: unit test for access_token provider
```

## Implementation Steps

### Step 1: Add `access_token` field to AzureSecretInfo

**File**: `src/include/azure/azure_secret_reader.hpp`

- Add `std::string access_token` field to `AzureSecretInfo` struct
- Update provider comment to include `access_token`

### Step 2: Read `access_token` from Azure secret

**File**: `src/azure/azure_secret_reader.cpp`

- In `ReadAzureSecret()`, read `access_token` value from the KeyValueSecret
- For `access_token` provider: validate that access_token is non-empty
- Skip service_principal validation (tenant_id/client_id/client_secret not needed)

### Step 3: Handle `access_token` provider in AcquireToken

**File**: `src/azure/azure_token.cpp`

- In `AcquireToken()`, add `access_token` provider branch before other providers
- When provider is `access_token`: return the token directly from `info.access_token`
- Use token cache with a reasonable expiry (1 hour default, or parse JWT exp claim)

### Step 4: Register `access_token` provider for TYPE azure

**File**: `src/mssql_extension.cpp`

- Register a `CreateSecretFunction` for type `azure`, provider `access_token`
- Named parameters: `ACCESS_TOKEN` (VARCHAR, required)
- Creation function stores the token in a KeyValueSecret, marks it as redacted

### Step 5: Update AZURE.md

**File**: `AZURE.md`

- Add "6. Pre-provided Access Token" section under Authentication Methods
- Show example: `CREATE SECRET ... (TYPE azure, PROVIDER access_token, ACCESS_TOKEN '...')`
- Show multi-connection reuse via AZURE_SECRET

### Step 6: Update README.md

**File**: `README.md`

- Add "Updating primary key columns is not supported" to Limitations section (issue #53)

### Step 7: Add unit test

**File**: `test/sql/azure/azure_secret_token_only.test`

- Test creating Azure secret with `access_token` provider succeeds
- Test that it appears in `duckdb_secrets()` with redacted token
- Test backward compatibility: existing Azure secret providers still work
