# Feature Specification: Catalog & Data Integration

**Feature Branch**: `azure-006-catalog-data-integration`
**Created**: 2026-02-03
**Status**: Future / Planning
**Dependencies**: 004-auth-flow-integration

## Problem Statement

Azure-authenticated connections to Azure SQL Database and Microsoft Fabric may behave differently than on-premises SQL Server for catalog operations, statistics, DDL, and bulk operations. This phase verifies all extension features work correctly and adds any necessary Azure/Fabric-specific handling.

---

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Catalog Operations with Azure Auth (Priority: P1)

A user needs to browse schemas, tables, and columns on Azure SQL Database using Azure AD authentication.

**Acceptance Scenarios**:

1. **Given** Azure-authenticated connection, **When** user queries `duckdb_schemas()`, **Then** all accessible schemas listed
2. **Given** Azure-authenticated connection, **When** user queries `duckdb_tables()`, **Then** all accessible tables listed with correct metadata
3. **Given** Azure-authenticated connection, **When** user queries `duckdb_columns()`, **Then** column types mapped correctly
4. **Given** Azure SQL with limited permissions, **When** catalog queried, **Then** only accessible objects shown (no permission errors)

---

### User Story 2 - Data Operations with Azure Auth (Priority: P1)

A user needs to perform all standard data operations on Azure SQL.

**Acceptance Scenarios**:

1. **Given** Azure-authenticated connection, **When** user executes SELECT, **Then** results returned correctly
2. **Given** Azure-authenticated connection, **When** user executes INSERT with RETURNING, **Then** inserted values returned
3. **Given** Azure-authenticated connection, **When** user executes UPDATE/DELETE, **Then** rows modified correctly
4. **Given** Azure-authenticated connection, **When** user uses transactions (BEGIN/COMMIT/ROLLBACK), **Then** transaction semantics preserved

---

### User Story 3 - COPY/BCP with Azure Auth (Priority: P1)

A user needs to bulk load data into Azure SQL using COPY command.

**Acceptance Scenarios**:

1. **Given** Azure-authenticated connection, **When** user executes COPY TO, **Then** data transferred via BCP protocol
2. **Given** Azure-authenticated connection, **When** user creates temp table via COPY, **Then** temp table accessible within transaction
3. **Given** Azure SQL with INSERT permissions only, **When** user attempts COPY, **Then** operation succeeds (or clear permission error)

---

### User Story 4 - Microsoft Fabric Warehouse (Priority: P1)

A user needs to work with Microsoft Fabric SQL endpoints, which may have different capabilities.

**Acceptance Scenarios**:

1. **Given** Fabric Warehouse connection, **When** user queries catalog, **Then** schemas/tables listed (may differ from standard SQL Server)
2. **Given** Fabric Warehouse connection, **When** user executes SELECT, **Then** results returned correctly
3. **Given** Fabric Warehouse connection, **When** user attempts unsupported DDL, **Then** clear error message (not cryptic TDS error)
4. **Given** Fabric Warehouse connection, **When** user attempts COPY/BCP, **Then** operation succeeds or clear "not supported" error

---

### User Story 5 - Statistics with Azure Auth (Priority: P2)

A user needs query optimization via statistics on Azure SQL.

**Acceptance Scenarios**:

1. **Given** Azure SQL connection, **When** `mssql_enable_statistics = true`, **Then** row counts retrieved
2. **Given** Azure SQL without DBCC permissions, **When** statistics queried, **Then** graceful degradation (basic stats only)
3. **Given** Fabric Warehouse, **When** statistics queried, **Then** available statistics returned (may be limited)

---

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: Catalog operations MUST work with Azure-authenticated connections
- **FR-002**: All DML operations (SELECT, INSERT, UPDATE, DELETE) MUST work
- **FR-003**: COPY/BCP operations MUST work with Azure auth
- **FR-004**: Transactions MUST work correctly with Azure auth
- **FR-005**: Statistics retrieval MUST work (with graceful degradation for limited permissions)
- **FR-006**: Microsoft Fabric Warehouse MUST be supported with documented limitations
- **FR-007**: If Azure/Fabric-specific issues arise, dedicated code paths MUST be added

### Azure SQL vs Fabric Comparison

| Feature | Azure SQL | Fabric Warehouse |
|---------|-----------|------------------|
| Catalog queries | Full support | May differ |
| SELECT | Full support | Full support |
| INSERT | Full support | Limited |
| UPDATE/DELETE | Full support | Limited |
| DDL | Full support | Limited |
| COPY/BCP | Full support | TBD |
| DBCC statistics | With permissions | Not available |
| Transactions | Full support | Limited |

### Fabric-Specific Handling (If Needed)

```cpp
bool IsFabricEndpoint(const std::string &host) {
    return host.find(".datawarehouse.fabric.microsoft.com") != std::string::npos ||
           host.find(".pbidedicated.windows.net") != std::string::npos;
}

// In catalog queries
if (IsFabricEndpoint(connection_params.host)) {
    // Use Fabric-specific catalog queries
    return ExecuteFabricCatalogQuery(context, query_type);
} else {
    // Use standard SQL Server catalog queries
    return ExecuteStandardCatalogQuery(context, query_type);
}
```

---

## Success Criteria *(mandatory)*

- **SC-001**: Schema discovery works on Azure SQL and Fabric
- **SC-002**: Table/column metadata retrieval works
- **SC-003**: All DML operations work on Azure SQL
- **SC-004**: COPY/BCP operations work on Azure SQL
- **SC-005**: Fabric Warehouse basic operations documented and working
- **SC-006**: Fabric limitations clearly documented
- **SC-007**: No regressions for on-premises SQL Server

---

## Technical Context (For Planning Reference)

### Potential Fabric Catalog Differences

Fabric may use different system views or have restricted access to standard SQL Server metadata:

```sql
-- Standard SQL Server
SELECT * FROM sys.schemas;
SELECT * FROM sys.tables;
SELECT * FROM INFORMATION_SCHEMA.COLUMNS;

-- Fabric may require
SELECT * FROM INFORMATION_SCHEMA.SCHEMATA;
SELECT * FROM INFORMATION_SCHEMA.TABLES;
-- etc.
```

### Modified Files (If Fabric-Specific Handling Needed)

```
src/
├── catalog/
│   ├── mssql_catalog.cpp      # Add Fabric detection/handling
│   └── mssql_metadata_cache.cpp
├── table_scan/
│   └── table_scan_bind.cpp    # Fabric-specific query generation
└── include/
    └── mssql_platform.hpp     # IsFabricEndpoint() utility
```

### Testing Approach

1. **Azure SQL Database**: Full feature validation
2. **Microsoft Fabric**: Document capabilities, add handling for limitations
3. **On-premises SQL Server**: Regression testing (no changes expected)

### Fabric Test Environment Variables

```bash
FABRIC_TEST_HOST=workspace.datawarehouse.fabric.microsoft.com
FABRIC_TEST_DATABASE=mywarehouse
# Uses same Azure credentials as Azure SQL tests
```

---

## Documentation Updates

- README: Azure SQL section with feature compatibility
- README: Microsoft Fabric section with known limitations
- CLAUDE.md: Fabric endpoint detection and handling
- Troubleshooting: Common Azure/Fabric errors and solutions

---

## Out of Scope

- Performance optimization for Azure/Fabric
- Azure Synapse Analytics dedicated pools (different from Fabric)
- Azure SQL Managed Instance (should work but not explicitly tested)
