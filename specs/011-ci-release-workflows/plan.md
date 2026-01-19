# Implementation Plan: CI/CD Release Workflows

**Branch**: `011-ci-release-workflows` | **Date**: 2026-01-19 | **Spec**: [spec.md](./spec.md)
**Input**: Feature specification from `/specs/011-ci-release-workflows/spec.md`

## Summary

Implement GitHub Actions workflows for automated building, testing, and releasing of the mssql-extension across 4 platforms (linux_amd64, linux_arm64, osx_arm64, windows_amd64) and 3 DuckDB versions (1.4.1, 1.4.2, 1.4.3). The release workflow triggers on `v*` tags, builds 12 artifacts with standardized naming, runs smoke tests (full integration on Linux, load-only on macOS/Windows), generates SHA256 checksums, and uploads to GitHub Releases. A separate CI workflow validates PRs by building against DuckDB nightly on linux_amd64 and osx_arm64.

## Technical Context

**Language/Version**: YAML (GitHub Actions workflow syntax) + Bash scripts
**Primary Dependencies**: GitHub Actions runners, CMake, vcpkg, Ninja, Docker (Linux only)
**Storage**: N/A (workflow artifacts stored in GitHub)
**Testing**: Smoke tests via DuckDB CLI + SQL Server container on Linux
**Target Platform**: GitHub Actions (ubuntu-latest, ubuntu-22.04-arm, macos-14, windows-latest)
**Project Type**: Infrastructure/CI configuration
**Performance Goals**: Release builds complete within reasonable CI time limits
**Constraints**: GitHub Actions runner availability, SQL Server container on Linux only
**Scale/Scope**: 12 artifacts per release (4 platforms × 3 DuckDB versions)

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Relevance | Compliance |
|-----------|-----------|------------|
| I. Native and Open | N/A | CI/CD infrastructure, not extension code |
| II. Streaming First | N/A | CI/CD infrastructure, not extension code |
| III. Correctness over Convenience | **Relevant** | Smoke tests verify extension loads and connects correctly; failures are explicit |
| IV. Explicit State Machines | N/A | CI/CD infrastructure, not extension code |
| V. DuckDB-Native UX | N/A | CI/CD infrastructure, not extension code |
| VI. Incremental Delivery | **Relevant** | CI workflow validates incrementally (lint → build → smoke test); release publishes all artifacts |

**Gate Status**: PASS - This feature is CI/CD infrastructure that supports the extension but does not modify extension code. Relevant principles are satisfied: correctness via explicit test failures, incremental delivery via staged workflow jobs.

## Project Structure

### Documentation (this feature)

```text
specs/011-ci-release-workflows/
├── plan.md              # This file
├── research.md          # Phase 0 output
├── data-model.md        # Phase 1 output (workflow matrix definition)
├── quickstart.md        # Phase 1 output (local testing guide)
├── contracts/           # Phase 1 output (N/A for CI - no API contracts)
└── tasks.md             # Phase 2 output (/speckit.tasks command)
```

### Source Code (repository root)

```text
.github/
└── workflows/
    ├── ci.yml               # PR validation workflow (update existing)
    └── release.yml          # New release workflow

scripts/
├── ci/
│   ├── fetch_duckdb.sh      # Fetch DuckDB sources by version/tag
│   ├── build_extension.sh   # Cross-platform build helper
│   ├── smoke_test.sh        # Load-only smoke test
│   └── integration_test.sh  # Full SQL Server integration test
└── sql/
    └── smoke_test.sql       # SQL commands for smoke test

docker/
└── docker-compose.yml       # Existing SQL Server container (used by CI)

README.md                    # Update compatibility info
```

**Structure Decision**: The workflows live in `.github/workflows/` per GitHub Actions convention. Helper scripts are placed in `scripts/ci/` to organize CI-specific tooling separately from other scripts. SQL files for tests go in `scripts/sql/`. This leverages the existing `docker/docker-compose.yml` for SQL Server container configuration.

## Complexity Tracking

No constitution violations to justify.
