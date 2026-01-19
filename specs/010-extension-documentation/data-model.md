# Data Model: Extension Documentation

**Date**: 2026-01-19
**Feature**: 010-extension-documentation

## Documentation Entity Model

This document defines the structure and relationships between documentation sections in README.md.

## Section Hierarchy

```text
README.md
├── Header (Title + Badges)
├── Overview
│   └── Features list
├── Quick Start
│   ├── Installation
│   ├── Connect to SQL Server
│   └── First Query
├── Connection Configuration
│   ├── Using Secrets
│   │   ├── CREATE SECRET syntax
│   │   └── ATTACH with secret
│   ├── Using Connection Strings
│   │   ├── ADO.NET format
│   │   └── URI format
│   └── TLS/SSL Configuration
├── Catalog Integration
│   ├── ATTACH/DETACH
│   ├── Schema Browsing
│   ├── Three-Part Naming
│   └── Cross-Catalog Joins
├── Query Execution
│   ├── Streaming SELECT
│   ├── Filter Pushdown
│   └── Projection Pushdown
├── Data Modification (INSERT)
│   ├── Basic INSERT
│   ├── INSERT with RETURNING
│   ├── Batch Configuration
│   └── Type Serialization
├── Function Reference
│   ├── mssql_version()
│   ├── mssql_execute()
│   ├── mssql_scan()
│   ├── mssql_exec()
│   ├── mssql_open()
│   ├── mssql_close()
│   ├── mssql_ping()
│   └── mssql_pool_stats()
├── Type Mapping
│   ├── Numeric Types
│   ├── String Types
│   ├── Binary Types
│   ├── Date/Time Types
│   └── Special Types (UUID, etc.)
├── Configuration Reference
│   ├── Connection Pool Settings
│   ├── Statistics Settings
│   └── INSERT Settings
├── Building from Source
│   ├── Prerequisites
│   ├── Build Commands
│   ├── TLS Support
│   └── Running Tests
├── Troubleshooting
│   ├── Connection Errors
│   ├── TLS Errors
│   ├── Type Errors
│   └── Performance Issues
└── Limitations
    ├── Unsupported Features
    └── Known Issues
```

## Section Dependencies

```text
Quick Start ──depends on──> Connection Configuration (basic)
Catalog Integration ──depends on──> Connection Configuration
Query Execution ──depends on──> Catalog Integration
Data Modification ──depends on──> Catalog Integration
Function Reference ──independent──
Type Mapping ──referenced by──> Query Execution, Data Modification
Configuration Reference ──referenced by──> Connection Configuration, Data Modification
Building from Source ──independent──
Troubleshooting ──references all──
```

## Content Requirements Per Section

### Header

- Extension name: "DuckDB MSSQL Extension"
- One-line description
- Badges: Build status (optional), DuckDB version compatibility

### Overview

- Purpose: Native TDS connectivity without ODBC/JDBC
- Key features (bullet list, 5-7 items)
- Current status (spec 009 - INSERT support)

### Quick Start

- Prerequisites: DuckDB installed, SQL Server accessible
- 3-step flow: Install → Connect → Query
- Complete, copy-paste example
- Expected output shown

### Connection Configuration

**Using Secrets**:

- Full CREATE SECRET syntax with all fields
- Field descriptions table
- ATTACH '' AS name (TYPE mssql, SECRET secret_name)

**Using Connection Strings**:

- ADO.NET format with all key aliases
- URI format with query parameters
- ATTACH 'connection_string' AS name (TYPE mssql)

**TLS/SSL**:

- Secret: `use_encrypt true`
- Connection string: `Encrypt=yes` or `?encrypt=true`
- Build requirement: loadable extension only

### Catalog Integration

- ATTACH syntax (both methods)
- SHOW SCHEMAS FROM context
- SHOW TABLES FROM context.schema
- DESCRIBE context.schema.table
- Three-part naming: context.schema.table
- Join example with local table

### Query Execution

- Basic SELECT through catalog
- Explain pushdown behavior
- Streaming behavior explanation
- Query cancellation mention

### Data Modification (INSERT)

- INSERT INTO context.schema.table VALUES (...)
- INSERT INTO ... SELECT ...
- INSERT ... RETURNING *
- Batch size configuration
- Identity column handling

### Function Reference

Each function:

- Signature (parameters with types)
- Return type
- Description (1-2 sentences)
- Example with output

### Type Mapping

- Table format: SQL Server | DuckDB | Notes
- Group by category (numeric, string, etc.)
- Unsupported types list

### Configuration Reference

- Table format: Setting | Type | Default | Range | Description
- Group by category (pool, statistics, insert)
- Usage examples for common tuning

### Building from Source

- Prerequisites: CMake, Ninja, vcpkg, C++17 compiler
- Clone with submodules
- make / make debug
- make test (unit tests)
- TLS: loadable build only
- Docker test environment

### Troubleshooting

- Connection refused: Check host/port/firewall
- Login failed: Check credentials
- TLS errors: Check build type, server certificate
- Type conversion errors: Check type mapping
- Slow queries: Check pushdown, network

### Limitations

- Unsupported types (XML, UDT, etc.)
- UPDATE/DELETE via mssql_exec only
- Windows auth not supported
- Transactions not supported

## Cross-Reference Requirements

| From Section | References | Link Type |
| ------------ | ---------- | --------- |
| Quick Start | Connection Configuration | "See X for details" |
| Catalog Integration | Function Reference (mssql_scan) | inline |
| Data Modification | Configuration Reference (INSERT settings) | "See X to tune" |
| Data Modification | Type Mapping | "See X for type conversion" |
| Troubleshooting | All sections | Contextual links |

## Validation Criteria

- [ ] All 8 functions documented with examples
- [ ] All 15 settings documented with defaults
- [ ] All 20+ type mappings documented
- [ ] Both connection string formats shown
- [ ] All secret fields documented
- [ ] TLS configuration explained
- [ ] Build instructions complete
- [ ] Troubleshooting covers common errors
- [ ] No broken internal links
- [ ] Examples tested against SQL Server
