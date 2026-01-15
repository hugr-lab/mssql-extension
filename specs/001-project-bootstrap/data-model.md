# Data Model: Project Bootstrap and Tooling

**Feature**: 001-project-bootstrap
**Date**: 2026-01-15

## Overview

This feature establishes test data in SQL Server for validating the development
environment. The tables are designed to support future Row Identity Model testing
per the project constitution.

## Database: mssql_test

The test database contains sample schemas and tables for development and integration
testing.

**Collation**: UTF-8 (per constitution Version Baseline)

## Schema: dbo

Default schema for test tables.

## Entities

### TestSimplePK

Validates scalar rowid mapping (single-column primary key).

| Column      | Type         | Constraints      | Description                    |
|-------------|--------------|------------------|--------------------------------|
| id          | INT          | PRIMARY KEY      | Auto-incrementing identifier   |
| name        | NVARCHAR(100)| NOT NULL         | Sample text field              |
| value       | DECIMAL(10,2)| NULL             | Sample numeric field           |
| created_at  | DATETIME2    | NOT NULL DEFAULT | Record creation timestamp      |

**Primary Key**: `id` (scalar)

**Sample Data**: At least 3 rows with varying data types to validate type mapping.

**Row Identity Mapping**: `rowid` = `id` (integer scalar)

---

### TestCompositePK

Validates STRUCT rowid mapping (multi-column primary key).

| Column      | Type         | Constraints      | Description                    |
|-------------|--------------|------------------|--------------------------------|
| region_code | CHAR(2)      | PRIMARY KEY      | Two-letter region code         |
| item_id     | INT          | PRIMARY KEY      | Item identifier within region  |
| description | NVARCHAR(200)| NOT NULL         | Item description               |
| quantity    | INT          | NOT NULL DEFAULT | Stock quantity                 |
| updated_at  | DATETIME2    | NOT NULL DEFAULT | Last update timestamp          |

**Primary Key**: `(region_code, item_id)` (composite)

**Sample Data**: At least 3 rows spanning multiple regions to validate composite key
handling.

**Row Identity Mapping**: `rowid` = `STRUCT(region_code, item_id)`

---

## SQL Server Initialization Script

The init script creates the database, schema, and tables with sample data.

Location: `docker/init/init.sql`

**Execution**: Automatically run by SQL Server container on first startup via
docker-compose volume mount.

**Idempotency**: Script checks for existing objects before creation to support
container restarts.

## Type Mapping Notes

These tables use SQL Server types that map to common DuckDB types:

| SQL Server Type | DuckDB Type   | Notes                          |
|-----------------|---------------|--------------------------------|
| INT             | INTEGER       | 32-bit signed integer          |
| CHAR(n)         | VARCHAR       | Fixed-length string            |
| NVARCHAR(n)     | VARCHAR       | Unicode string (UTF-16 in SQL) |
| DECIMAL(p,s)    | DECIMAL(p,s)  | Exact numeric                  |
| DATETIME2       | TIMESTAMP     | Date and time                  |

## Constraints

Per constitution principles:

- All tables have explicit primary keys (no heap tables)
- No use of `%%physloc%%` or other physical row locators
- UTF-8 collation for text columns where supported
