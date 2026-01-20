# Implementation Plan: Skip Tests in Community Extensions CI

**Branch**: `015-ci-test-skip` | **Date**: 2026-01-20 | **Spec**: [spec.md](./spec.md)
**Input**: Feature specification from `/specs/015-ci-test-skip/spec.md`

## Summary

Configure the MSSQL extension to skip tests in the DuckDB community-extensions CI environment, where SQL Server is not available. The implementation adds `test_config` to `description.yml` with `SKIP_TESTS=1` to completely skip tests during community-extensions CI builds.

## Technical Context

**Language/Version**: YAML configuration + Shell/Makefile
**Primary Dependencies**: DuckDB extension-ci-tools (Makefile-based test runner)
**Storage**: N/A (configuration files only)
**Testing**: Verification via community-extensions CI workflow
**Target Platform**: All platforms supported by community-extensions CI (Linux, macOS, Windows)
**Project Type**: Configuration change (no code changes)
**Performance Goals**: N/A
**Constraints**: Must use supported community-extensions CI mechanisms (`test_config` field in `description.yml`)
**Scale/Scope**: Single configuration file change

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Status | Notes |
|-----------|--------|-------|
| I. Native and Open | N/A | No changes to TDS implementation |
| II. Streaming First | N/A | No changes to data streaming |
| III. Correctness over Convenience | PASS | Tests are skipped explicitly, not silently failing |
| IV. Explicit State Machines | N/A | No state machine changes |
| V. DuckDB-Native UX | N/A | No catalog changes |
| VI. Incremental Delivery | PASS | Read-only operations unaffected; test skipping is explicit |

**Gate Result**: PASS - Configuration change only, no principle violations.

## Project Structure

### Documentation (this feature)

```text
specs/015-ci-test-skip/
├── plan.md              # This file
├── research.md          # Phase 0 output
├── quickstart.md        # Phase 1 output
└── tasks.md             # Phase 2 output (from /speckit.tasks)
```

### Source Code (repository root)

```text
# Files to modify
description.yml          # Add test_config field

# Files for reference (no changes)
Makefile                 # Uses SKIP_TESTS from extension-ci-tools
extension-ci-tools/      # Provides SKIP_TESTS mechanism
test/sql/                # Existing test files (no changes needed)
```

**Structure Decision**: Configuration-only change. Single file modification to `description.yml` in repository root.

## Complexity Tracking

No complexity violations - this is a minimal configuration change.
