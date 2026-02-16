# Tasks: Fix datetime2(0) Truncation and URI Password Parsing

**Branch**: `038-fix-datetime2-uri-parsing`
**Plan**: [plan.md](plan.md)

## Phase 1: Core Fixes

- [X] **T1**: Fix `ConvertDatetime2()` scale-aware time conversion in `src/tds/encoding/datetime_encoding.cpp`
- [X] **T2**: Fix `ConvertTime()` scale-aware time conversion in `src/tds/encoding/datetime_encoding.cpp`
- [X] **T3**: Fix `ParseUri()` to use `rfind('@')` in `src/mssql_storage.cpp`

## Phase 2: Integration Tests

- [X] **T4**: Add datetime2 scale integration test in `test/sql/integration/datetime2_scale.test`
- [X] **T5**: Add URI password parsing test in `test/sql/attach/attach_uri_password.test`

## Phase 3: Validation

- [X] **T6**: Build and run all tests (95 passed, 2602 assertions, 20 skipped)
