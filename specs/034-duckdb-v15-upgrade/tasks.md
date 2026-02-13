# Tasks: DuckDB v1.5 Variegata Upgrade

**Feature Branch**: `034-duckdb-v15-upgrade`
**Plan**: [plan.md](plan.md)

## Phase 1: Setup

- [X] T001 Switch DuckDB submodule to v1.5-variegata branch in duckdb/

## Phase 2: Build

- [X] T002 Clean build the extension against DuckDB v1.5-variegata

## Phase 3: Fix Compilation

- [X] T003 Fix compilation errors: StorageExtension::Register API + mssql_compat.hpp in delete header

## Phase 4: Unit Tests

- [X] T004 Run unit tests and verify all pass (86 test cases, 2435 assertions)

## Phase 5: Integration Tests

- [X] T005 Run integration tests against SQL Server and verify all pass (93 test cases, 2581 assertions)

## Phase 6: Polish

- [X] T006 No test failures — all tests pass without additional fixes
