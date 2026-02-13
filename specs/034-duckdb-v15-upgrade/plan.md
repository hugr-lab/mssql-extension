# Implementation Plan: DuckDB v1.5 Variegata Upgrade

**Branch**: `034-duckdb-v15-upgrade` | **Date**: 2026-02-13 | **Spec**: [spec.md](spec.md)
**Input**: Feature specification from `/specs/034-duckdb-v15-upgrade/spec.md`

## Summary

Upgrade the DuckDB submodule from v1.4.4 (commit `6ddac802ff`) to the v1.5-variegata branch (commit `f480e78169`). The extension's existing compatibility layer (`mssql_compat.hpp` + CMake auto-detection) already handles the critical `GetData` → `GetDataInternal` API change. All other v1.5 changes are backward-compatible or have default implementations. The upgrade requires switching the submodule, building, running tests, and fixing any compilation or test failures.

## Technical Context

**Language/Version**: C++17 (DuckDB extension standard, C++11-compatible for ODR on Linux)
**Primary Dependencies**: DuckDB v1.5-variegata (6,275 commits ahead of v1.4.4), OpenSSL (vcpkg), libcurl (vcpkg)
**Storage**: N/A (remote SQL Server via TDS protocol)
**Testing**: DuckDB SQLLogicTest framework + C++ unit tests
**Target Platform**: macOS (Clang), Linux (GCC), Windows (MSVC, MinGW)
**Project Type**: DuckDB C++ extension (single project)

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Status | Notes |
|-----------|--------|-------|
| I. Native and Open | PASS | No new external dependencies |
| II. Streaming First | PASS | No changes to streaming architecture |
| III. Correctness over Convenience | PASS | PK-based rowid semantics unchanged |
| IV. Explicit State Machines | PASS | TDS state machine unchanged |
| V. DuckDB-Native UX | PASS | Catalog integration maintained |
| VI. Incremental Delivery | PASS | Upgrade is independently testable |

## Research Findings

### DuckDB v1.5-variegata API Changes

| Change | Risk | Extension Impact |
|--------|------|-----------------|
| `PhysicalOperator::GetData` → `GetDataInternal` | MITIGATED | Already handled by `MSSQL_GETDATA_METHOD` macro |
| `OperatorCachingMode` enum introduced | NONE | Extension doesn't override caching methods |
| `GetColumnSegmentInfo` signature change (added `QueryContext`) | NONE | Not overridden by extension |
| `SchemaCatalogEntry::CreateCoordinateSystem` added | NONE | Default throws `NotImplementedException` |
| `Catalog::SupportsCreateTable` added | NONE | Optional validation hook |
| `TableFunction` new callbacks (async, statistics) | LOW | Backward-compatible additions |
| Variant type storage changes | NONE | Extension uses TDS encoding, not DuckDB variants |

### CMake Auto-Detection (Already Working)

The build system auto-detects the DuckDB API by scanning `physical_operator.hpp` for `GetDataInternal`. When found, it defines `MSSQL_DUCKDB_NIGHTLY=1`, which activates the compat macro. This mechanism works for v1.5-variegata without changes.

## Project Structure

### Source Code (no new files expected)

```text
src/include/mssql_compat.hpp    # May need updates if new API breaks found
CMakeLists.txt                  # API detection logic (already handles v1.5)
duckdb/                         # Submodule → switch to v1.5-variegata
```

## Implementation Steps

### Step 1: Switch DuckDB Submodule

```bash
cd duckdb
git fetch origin
git checkout v1.5-variegata
cd ..
```

### Step 2: Clean Build

```bash
make clean
make
```

### Step 3: Fix Compilation Errors (if any)

Based on research, the most likely issues:
- New pure virtual methods requiring override
- Changed method signatures
- New header includes required

Fix pattern: use `#ifdef MSSQL_DUCKDB_NIGHTLY` guards in `mssql_compat.hpp` for conditional compilation.

### Step 4: Run Unit Tests

```bash
make test
```

### Step 5: Run Integration Tests

```bash
make docker-up
make integration-test
```

### Step 6: Fix Test Failures (if any)

Analyze failures, determine if caused by:
- Extension code vs DuckDB behavior changes
- Test expectation changes (e.g., error message wording)

## Complexity Tracking

No constitution violations expected. This is a straightforward dependency upgrade.
