# Implementation Plan: DuckDB Extension CI Tools Integration

**Branch**: `001-extension-ci-tools-integration` | **Date**: 2026-01-20 | **Spec**: [spec.md](./spec.md)
**Input**: Feature specification from `/specs/001-extension-ci-tools-integration/spec.md`

## Summary

Integrate the `duckdb/extension-ci-tools` repository as a git submodule to make the mssql-extension compatible with DuckDB Community Extensions CI workflows. The primary deliverable is exposing the `set_duckdb_version` target required by Community CI, while preserving existing developer-oriented Makefile targets using a hybrid approach.

## Technical Context

**Language/Version**: Makefile (GNU Make), CMake 3.20+, Bash
**Primary Dependencies**: duckdb/extension-ci-tools (git submodule), duckdb (existing submodule)
**Storage**: N/A (build system only)
**Testing**: Manual verification via `make set_duckdb_version`, `make release`, `make test`
**Target Platform**: Linux, macOS, Windows (cross-platform CI)
**Project Type**: Build system integration (no source code changes)
**Performance Goals**: N/A (build infrastructure)
**Constraints**: Must preserve existing vcpkg-based OpenSSL integration
**Scale/Scope**: 4 files modified/added (submodule, Makefile, extension_config.cmake, README)

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Status | Notes |
|-----------|--------|-------|
| I. Native and Open | ✅ PASS | No new dependencies on proprietary drivers |
| II. Streaming First | ✅ N/A | Build system only, no runtime impact |
| III. Correctness over Convenience | ✅ PASS | CI integration ensures reproducible builds |
| IV. Explicit State Machines | ✅ N/A | No protocol changes |
| V. DuckDB-Native UX | ✅ PASS | Aligns with DuckDB ecosystem standards |
| VI. Incremental Delivery | ✅ PASS | Minimal change, independently testable |

**Gate Result**: PASS - No violations. Proceed with implementation.

## Project Structure

### Documentation (this feature)

```text
specs/001-extension-ci-tools-integration/
├── spec.md              # Feature specification
├── plan.md              # This file
├── research.md          # Phase 0 research findings
├── checklists/
│   └── requirements.md  # Spec quality checklist
└── quickstart.md        # Developer quickstart guide
```

### Source Code (repository root)

```text
# Files to be modified/added
./
├── Makefile                 # MODIFY: Hybrid approach (include ci-tools + custom targets)
├── extension_config.cmake   # VERIFY: Already exists, may need adjustment
├── extension-ci-tools/      # ADD: Git submodule
├── duckdb/                  # EXISTS: Git submodule (unchanged)
├── vcpkg.json               # EXISTS: Verify compatibility
└── README.md                # MODIFY: Add build instructions section
```

**Structure Decision**: Single project with build system integration only. No changes to src/ or tests/ directories.

## Complexity Tracking

> No violations to justify. Implementation is minimal and follows established patterns.

---

## Phase 0: Research Findings

### Decision 1: Makefile Include Pattern

**Decision**: Use standard include pattern from extension-ci-tools

**Pattern**:
```makefile
PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
EXT_NAME=mssql
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

include extension-ci-tools/makefiles/duckdb_extension.Makefile

# Custom targets follow...
```

**Rationale**: This is the exact pattern used by all DuckDB community extensions (spatial, httpfs, etc.). The included Makefile provides `set_duckdb_version`, `release`, `debug`, `test` targets.

**Alternatives considered**:
- Custom Makefile with only needed targets: Rejected - would diverge from ecosystem standards and require maintenance
- Wrapper script: Rejected - unnecessary complexity, standard Makefile include is cleaner

### Decision 2: set_duckdb_version Implementation

**Decision**: Rely on extension-ci-tools provided target

**Implementation**:
```makefile
# Provided by extension-ci-tools/makefiles/duckdb_extension.Makefile:
set_duckdb_version:
	cd duckdb && git checkout $(DUCKDB_GIT_VERSION)
```

**Rationale**: The `duckdb/` submodule already exists in the repository. The CI tool's target simply performs a git checkout to the requested version.

**Prerequisites**:
- `duckdb/` must be a git submodule (already is)
- Submodule must be initialized before running

### Decision 3: Hybrid Makefile Approach

**Decision**: Include extension-ci-tools first, then append custom targets

**Preserved custom targets**:
- `docker-up`, `docker-down`, `docker-status` - SQL Server container management
- `integration-test` - Tests requiring SQL Server
- `vcpkg-setup` - Bootstrap vcpkg if missing
- `test-all`, `test-debug`, `test-simple-query` - Extended test targets
- `help` - Developer documentation

**Overridden behavior**:
- `test` target from ci-tools will be used (runs `[sql]` tagged tests)
- `release` and `debug` from ci-tools will be used (may need EXT_FLAGS for vcpkg)

**Rationale**: Preserves valuable developer tooling while gaining CI compatibility.

### Decision 4: vcpkg Integration

**Decision**: Pass vcpkg toolchain via EXT_FLAGS

**Implementation**:
```makefile
VCPKG_DIR := $(shell pwd)/vcpkg
VCPKG_TOOLCHAIN := $(VCPKG_DIR)/scripts/buildsystems/vcpkg.cmake

# Pass vcpkg to extension-ci-tools builds
EXT_FLAGS := -DCMAKE_TOOLCHAIN_FILE="$(VCPKG_TOOLCHAIN)" -DVCPKG_MANIFEST_DIR="$(shell pwd)"
```

**Rationale**: The extension-ci-tools Makefile passes `$(EXT_FLAGS)` to all CMake invocations. This allows seamless vcpkg integration.

**Alternatives considered**:
- Using extension-ci-tools native vcpkg support: Rejected - requires specific directory structure and merged manifests
- Removing vcpkg dependency: Not possible - OpenSSL is required for TLS support

### Decision 5: extension_config.cmake Format

**Decision**: Keep existing format, verify compatibility

**Current content** (verified):
```cmake
duckdb_extension_load(mssql
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
    LOAD_TESTS
)
```

**Rationale**: This format is already compatible with DuckDB's extension build system. The `duckdb_extension_load()` function is provided by DuckDB's CMake infrastructure.

### Decision 6: Test Target Behavior

**Decision**: CI test target skips integration tests by default

**Implementation**: The existing `test` target in the custom section runs `[sql]` tagged tests only, which don't require SQL Server. This behavior should be preserved or the ci-tools `test` target configured similarly.

**Note**: The extension-ci-tools `test` target runs `make test_release` which executes the DuckDB test runner. Tests tagged `[integration]` will be skipped when SQL Server is unavailable (tests themselves check for connection).

---

## Phase 1: Implementation Design

### File Changes

#### 1. Add extension-ci-tools submodule

```bash
git submodule add https://github.com/duckdb/extension-ci-tools.git extension-ci-tools
```

#### 2. Makefile (complete replacement)

```makefile
# Makefile for DuckDB MSSQL Extension
# Compatible with DuckDB Community Extensions CI

PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Extension configuration
EXT_NAME=mssql
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# vcpkg integration
VCPKG_DIR := $(PROJ_DIR)vcpkg
VCPKG_TOOLCHAIN := $(VCPKG_DIR)/scripts/buildsystems/vcpkg.cmake

# Pass vcpkg toolchain to all builds (if vcpkg exists)
ifneq ($(wildcard $(VCPKG_TOOLCHAIN)),)
    EXT_FLAGS := -DCMAKE_TOOLCHAIN_FILE="$(VCPKG_TOOLCHAIN)" -DVCPKG_MANIFEST_DIR="$(PROJ_DIR)"
endif

# Include DuckDB extension CI tools (provides: set_duckdb_version, release, debug, test, etc.)
include extension-ci-tools/makefiles/duckdb_extension.Makefile

#
# Custom targets (preserved from original Makefile)
#

.PHONY: vcpkg-setup docker-up docker-down docker-status integration-test test-all help

# Bootstrap vcpkg if not present
vcpkg-setup:
	@if [ ! -d "$(VCPKG_DIR)" ]; then \
		echo "Bootstrapping vcpkg..."; \
		git clone https://github.com/microsoft/vcpkg.git $(VCPKG_DIR); \
		$(VCPKG_DIR)/bootstrap-vcpkg.sh; \
	fi

# Docker targets for SQL Server test container
DOCKER_COMPOSE := docker/docker-compose.yml

docker-up:
	@echo "Starting SQL Server test container..."
	docker compose -f $(DOCKER_COMPOSE) up -d sqlserver
	@echo "Waiting for SQL Server to be healthy..."
	@timeout=120; while [ $$timeout -gt 0 ]; do \
		if docker compose -f $(DOCKER_COMPOSE) ps sqlserver | grep -q "healthy"; then \
			echo "SQL Server is ready!"; \
			break; \
		fi; \
		echo "Waiting... ($$timeout seconds remaining)"; \
		sleep 5; \
		timeout=$$((timeout - 5)); \
	done
	@echo "Running init scripts..."
	docker compose -f $(DOCKER_COMPOSE) up sqlserver-init
	@echo "SQL Server is ready for testing!"

docker-down:
	@echo "Stopping SQL Server test container..."
	docker compose -f $(DOCKER_COMPOSE) down

docker-status:
	@echo "SQL Server container status:"
	@docker compose -f $(DOCKER_COMPOSE) ps sqlserver 2>/dev/null || echo "Container not running"

# Load environment from .env file if it exists
-include .env

# Default test environment variables
MSSQL_TEST_HOST ?= localhost
MSSQL_TEST_PORT ?= 1433
MSSQL_TEST_USER ?= sa
MSSQL_TEST_PASS ?= TestPassword1
MSSQL_TEST_DB ?= master

# Export test environment variables
export MSSQL_TEST_HOST MSSQL_TEST_PORT MSSQL_TEST_USER MSSQL_TEST_PASS MSSQL_TEST_DB

# Integration tests - requires SQL Server running
integration-test: release
	@echo "Running integration tests (requires SQL Server)..."
	@if ! docker compose -f $(DOCKER_COMPOSE) ps sqlserver 2>/dev/null | grep -q "healthy"; then \
		echo "WARNING: SQL Server container not detected. Run 'make docker-up' first."; \
	fi
	build/release/test/unittest "[integration]" --force-reload

# Run all tests (unit + integration)
test-all: release
	@echo "Running all tests..."
	build/release/test/unittest "*mssql*" --force-reload

# Show help
help:
	@echo "DuckDB MSSQL Extension Build System"
	@echo ""
	@echo "Standard CI targets (from extension-ci-tools):"
	@echo "  make release              - Build release version"
	@echo "  make debug                - Build debug version"
	@echo "  make test                 - Run unit tests"
	@echo "  make set_duckdb_version   - Set DuckDB version (use DUCKDB_GIT_VERSION=v1.x.x)"
	@echo ""
	@echo "Custom targets:"
	@echo "  make vcpkg-setup          - Bootstrap vcpkg (required for TLS support)"
	@echo "  make integration-test     - Run integration tests (requires SQL Server)"
	@echo "  make test-all             - Run all tests"
	@echo "  make docker-up            - Start SQL Server test container"
	@echo "  make docker-down          - Stop SQL Server test container"
	@echo "  make docker-status        - Check SQL Server container status"
	@echo "  make help                 - Show this help"
	@echo ""
	@echo "Examples:"
	@echo "  make vcpkg-setup && make release"
	@echo "  DUCKDB_GIT_VERSION=v1.4.3 make set_duckdb_version"
	@echo "  make docker-up && make integration-test"
```

#### 3. extension_config.cmake (verify/keep as-is)

Current content is already compatible:
```cmake
duckdb_extension_load(mssql
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
    LOAD_TESTS
)
```

#### 4. README.md update (add section)

```markdown
## Building with DuckDB Extension CI Tools

This extension is compatible with DuckDB Community Extensions CI.

### Prerequisites

```bash
# Clone with submodules
git clone --recurse-submodules https://github.com/hugr-lab/mssql-extension.git
cd mssql-extension

# Or initialize submodules after clone
git submodule update --init --recursive
```

### CI Build (Community Extensions compatible)

```bash
# Set DuckDB version (required by Community CI)
DUCKDB_GIT_VERSION=v1.4.3 make set_duckdb_version

# Build release
make release

# Run tests
make test
```

### Local Development Build

```bash
# Bootstrap vcpkg (required for TLS/OpenSSL support)
make vcpkg-setup

# Build
make release   # or: make debug

# Load extension in DuckDB
./build/release/duckdb
> LOAD mssql;
```

### Running Integration Tests

```bash
# Start SQL Server container
make docker-up

# Run integration tests
make integration-test

# Stop container when done
make docker-down
```
```

### Verification Checklist

- [ ] `git submodule add https://github.com/duckdb/extension-ci-tools.git extension-ci-tools`
- [ ] `DUCKDB_GIT_VERSION=v1.4.3 make set_duckdb_version` succeeds
- [ ] `make vcpkg-setup` bootstraps vcpkg
- [ ] `make release` produces extension at `build/release/extension/mssql/mssql.duckdb_extension`
- [ ] `make test` completes without failure (skips integration tests)
- [ ] `./build/release/duckdb -c "LOAD mssql; SELECT 1;"` works
- [ ] Custom targets (`docker-up`, `integration-test`, `help`) still work

---

## Next Steps

Run `/speckit.tasks` to generate the implementation task list.
