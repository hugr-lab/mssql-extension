# Implementation Plan: Fix datetime2(0) Truncation and URI Password Parsing

**Branch**: `038-fix-datetime2-uri-parsing` | **Date**: 2026-02-16 | **Spec**: [spec.md](spec.md)
**Input**: Feature specification from `/specs/038-fix-datetime2-uri-parsing/spec.md`

## Summary

Fix two bugs: (1) datetime2 and TIME type decoding ignores the scale parameter, producing corrupt time values for scales 0–6; (2) URI parser uses `find('@')` instead of `rfind('@')`, breaking passwords containing `@`. Both are small, targeted fixes with unit tests.

## Technical Context

**Language/Version**: C++17 (C++11-compatible for ODR on Linux)
**Primary Dependencies**: DuckDB (main branch), OpenSSL (vcpkg)
**Storage**: N/A (remote SQL Server via TDS protocol)
**Testing**: DuckDB unittest framework (no SQL Server required for unit tests), SQLLogicTest (integration)
**Target Platform**: Linux (GCC), macOS (Clang), Windows (MSVC, MinGW)
**Project Type**: Single (DuckDB extension)
**Performance Goals**: N/A (bug fix, no performance impact)
**Constraints**: Must not regress existing datetime2(7) or connection string parsing behavior
**Scale/Scope**: 2 files changed, 2 new test files

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Status | Notes |
|-----------|--------|-------|
| I. Native and Open | PASS | Fix is in our native TDS implementation, no external libraries |
| II. Streaming First | PASS | No buffering changes, fix is in per-row decoding |
| III. Correctness over Convenience | PASS | Directly fixes silent data corruption (issue #73) |
| IV. Explicit State Machines | PASS | No state machine changes |
| V. DuckDB-Native UX | PASS | Fix ensures correct type mapping (datetime2 semantics preserved) |
| VI. Incremental Delivery | PASS | Two independent bug fixes, each testable independently |

No violations. Gate passes.

## Project Structure

### Documentation (this feature)

```text
specs/038-fix-datetime2-uri-parsing/
├── plan.md              # This file
├── research.md          # Phase 0 output
├── spec.md              # Feature specification
└── checklists/
    └── requirements.md  # Spec quality checklist
```

### Source Code (files to modify)

```text
src/
├── tds/encoding/
│   └── datetime_encoding.cpp     # FIX: ConvertDatetime2() and ConvertTime() scale handling
└── mssql_storage.cpp             # FIX: ParseUri() find('@') → rfind('@')

test/
├── cpp/
│   ├── test_datetime_encoding.cpp  # NEW: Unit tests for all datetime2 scales
│   └── test_uri_parsing.cpp        # NEW: Unit tests for URI parsing with special chars
└── sql/
    └── integration/
        └── datetime2_scale.test    # NEW: Integration test for datetime2(0) through datetime2(7)
```

**Structure Decision**: Existing single-project structure. Two source files modified, two or three test files added.

## Complexity Tracking

No constitution violations to justify.
