# Tasks: DDL Schema Support

**Feature Branch**: `035-ddl-schema-support`
**Plan**: [plan.md](plan.md)

## Phase 1: Core Implementation

- [X] T001 Add IF NOT EXISTS handling to CreateSchema in src/catalog/mssql_catalog.cpp
- [X] T002 Add IF EXISTS handling to DropSchema in src/catalog/mssql_catalog.cpp

## Phase 2: Build & Test

- [X] T003 Build the extension and verify compilation
- [X] T004 Run unit tests (86 test cases, 2435 assertions)
- [X] T005 Run integration tests against SQL Server (36 test cases, 916 assertions)
