# Feature Specification: Extend MSSQL Secret

**Feature Branch**: `azure-002-extend-mssql-secret`
**Created**: 2026-02-03
**Status**: Future / Planning
**Dependencies**: 001-azure-secret-reuse

## Problem Statement

The MSSQL secret currently requires `user` and `password` for SQL Server authentication. To support Azure AD authentication, the secret needs an optional `azure_secret` field that references a DuckDB Azure secret for token-based authentication.

---

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Create MSSQL Secret with Azure Auth (Priority: P1)

A user wants to create an MSSQL secret that uses Azure AD authentication instead of SQL authentication.

**Acceptance Scenarios**:

1. **Given** an existing Azure secret `my_azure`, **When** user creates MSSQL secret with `azure_secret 'my_azure'`, **Then** secret is created successfully without requiring user/password
2. **Given** MSSQL secret with `azure_secret`, **When** user also provides `user` and `password`, **Then** secret is created (fields ignored for Azure auth)
3. **Given** non-existent Azure secret name, **When** user creates MSSQL secret, **Then** clear error: "Azure secret 'name' not found"

---

### User Story 2 - Backward Compatibility (Priority: P1)

Existing MSSQL secrets with SQL authentication must continue to work unchanged.

**Acceptance Scenarios**:

1. **Given** existing MSSQL secret with `user` and `password`, **When** user attaches database, **Then** SQL authentication works as before
2. **Given** MSSQL secret without `azure_secret`, **When** `user` or `password` is missing, **Then** validation error (existing behavior)
3. **Given** MSSQL secret created before this feature, **When** loaded from persistent storage, **Then** works unchanged

---

### User Story 3 - Secret Validation (Priority: P1)

The system must validate MSSQL secret configuration at creation time.

**Acceptance Scenarios**:

1. **Given** MSSQL secret with neither SQL auth nor Azure auth, **When** created, **Then** error: "Either user/password or azure_secret required"
2. **Given** MSSQL secret with `azure_secret` referencing invalid type, **When** created, **Then** error: "Secret 'name' is not an Azure secret"
3. **Given** valid Azure secret reference, **When** MSSQL secret created, **Then** validation passes

---

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: MSSQL secret MUST accept optional `azure_secret` parameter
- **FR-002**: When `azure_secret` is present, `user` and `password` MUST be optional
- **FR-003**: When `azure_secret` is absent, `user` and `password` MUST be required (existing behavior)
- **FR-004**: System MUST validate Azure secret exists at MSSQL secret creation time
- **FR-005**: System MUST validate Azure secret is TYPE azure
- **FR-006**: Full backward compatibility with existing MSSQL secrets

### Extended MSSQL Secret Fields

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `host` | VARCHAR | Yes | SQL Server hostname |
| `port` | INTEGER | Yes | TCP port (default 1433) |
| `database` | VARCHAR | Yes | Database name |
| `user` | VARCHAR | Conditional | Required if no `azure_secret` |
| `password` | VARCHAR | Conditional | Required if no `azure_secret` |
| `use_encrypt` | BOOLEAN | No | TLS encryption (default: true) |
| `catalog` | BOOLEAN | No | Enable catalog (default: true) |
| **`azure_secret`** | VARCHAR | No | **NEW**: Name of Azure secret for Entra ID auth |

### Validation Rules

```
IF azure_secret IS PROVIDED:
    - Validate Azure secret exists
    - Validate Azure secret is TYPE azure
    - user/password are optional (ignored if provided)
ELSE:
    - user is required
    - password is required
```

---

## Success Criteria *(mandatory)*

- **SC-001**: MSSQL secret with `azure_secret` creates successfully
- **SC-002**: MSSQL secret without `azure_secret` requires user/password (existing behavior)
- **SC-003**: Invalid Azure secret reference fails with clear error
- **SC-004**: All existing MSSQL secret tests pass (no regression)

---

## Technical Context (For Planning Reference)

### Modified Files

```
src/
├── mssql_secret.cpp           # Add azure_secret handling
├── include/mssql_secret.hpp   # Add MSSQL_SECRET_AZURE_SECRET constant
```

### Validation Logic

```cpp
string ValidateMSSQLSecretFields(ClientContext &context, const CreateSecretInput &input) {
    auto azure_secret_it = input.options.find(MSSQL_SECRET_AZURE_SECRET);
    bool has_azure_secret = azure_secret_it != input.options.end() &&
                            !azure_secret_it->second.ToString().empty();

    if (has_azure_secret) {
        // Validate Azure secret exists and is correct type
        auto azure_secret_name = azure_secret_it->second.ToString();
        auto &secret_manager = SecretManager::Get(context);
        auto match = secret_manager.LookupSecret(transaction, azure_secret_name, "azure");
        if (!match.HasMatch()) {
            return StringUtil::Format("Azure secret '%s' not found", azure_secret_name);
        }
        // user/password not required
    } else {
        // Existing validation: require user and password
        // ... existing code ...
    }
    return "";  // Valid
}
```

### Registration

```cpp
// Add to named_parameters in RegisterMSSQLSecretType()
create_func.named_parameters[MSSQL_SECRET_AZURE_SECRET] = LogicalType::VARCHAR;  // Optional
```

---

## Out of Scope

- Token acquisition (handled in phase 1)
- TDS authentication flow (handled in phase 4)
- Test function (handled in phase 3)
