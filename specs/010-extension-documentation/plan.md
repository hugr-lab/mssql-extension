# Implementation Plan: Extension Documentation

**Branch**: `010-extension-documentation` | **Date**: 2026-01-19 | **Spec**: [spec.md](./spec.md)
**Input**: Feature specification from `/specs/010-extension-documentation/spec.md`

## Summary

Create comprehensive user documentation for the mssql-extension covering installation, connection configuration, function reference, catalog integration, type mapping, INSERT operations, build instructions, and troubleshooting. Documentation will be written primarily in README.md with a structure optimized for quick start and reference lookup.

## Technical Context

**Language/Version**: Markdown (GitHub-flavored)
**Primary Dependencies**: N/A (documentation only)
**Storage**: N/A
**Testing**: Manual verification against actual extension behavior
**Target Platform**: GitHub repository (README.md), any markdown viewer
**Project Type**: Documentation
**Performance Goals**: Users complete Quick Start in under 5 minutes
**Constraints**: Must be accurate against current implementation (spec 009)
**Scale/Scope**: Single README.md file with ~1000-1500 lines

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle                        | Relevance                                             | Compliance |
| -------------------------------- | ----------------------------------------------------- | ---------- |
| I. Native and Open               | Documentation must not reference ODBC/JDBC/FreeTDS    | ✅ Pass    |
| II. Streaming First              | Documentation must explain streaming behavior         | ✅ Pass    |
| III. Correctness over Convenience | Document PK requirements for UPDATE/DELETE           | ✅ Pass    |
| IV. Explicit State Machines      | Document connection lifecycle if relevant             | ✅ Pass    |
| V. DuckDB-Native UX              | Emphasize catalog integration, three-part naming      | ✅ Pass    |
| VI. Incremental Delivery         | Document current feature set accurately (no vaporware) | ✅ Pass    |

**Gate Status**: ✅ PASSED - No violations. Documentation accurately represents the extension's constitution-compliant implementation.

## Project Structure

### Documentation (this feature)

```text
specs/010-extension-documentation/
├── plan.md              # This file
├── research.md          # Phase 0 output (documentation structure research)
├── data-model.md        # Phase 1 output (documentation entity model)
├── quickstart.md        # Phase 1 output (documentation quickstart guide)
├── contracts/           # Phase 1 output (not applicable for docs)
└── tasks.md             # Phase 2 output (/speckit.tasks command)
```

### Source Code (repository root)

```text
# Documentation deliverable location
README.md                # Primary documentation file (to be created/updated)

# Reference sources for documentation content
src/
├── include/
│   ├── mssql_extension.hpp
│   ├── mssql_functions.hpp
│   ├── mssql_secret.hpp
│   ├── mssql_storage.hpp
│   ├── catalog/
│   ├── connection/
│   ├── tds/
│   ├── query/
│   ├── insert/
│   └── pushdown/

test/sql/                # Example queries for documentation
docker/                  # Docker setup for examples
```

**Structure Decision**: Documentation-only feature. Output is README.md at repository root. No source code changes required.

## Complexity Tracking

> No violations to justify. Documentation feature does not introduce architectural complexity.

## Phase 0: Research Summary

Research tasks:

1. Document best practices for DuckDB extension READMEs
2. Verify all function signatures against current implementation
3. Verify all configuration settings against current implementation
4. Verify type mapping against current implementation
5. Document TLS build differences (static vs loadable)

See [research.md](./research.md) for detailed findings.

## Phase 1: Design Artifacts

### Documentation Structure (data-model.md)

See [data-model.md](./data-model.md) for:

- README.md section hierarchy
- Content requirements per section
- Cross-reference requirements

### Quick Reference (quickstart.md)

See [quickstart.md](./quickstart.md) for:

- Minimal viable documentation flow
- Copy-paste examples
- Common error solutions

## Next Steps

1. Run `/speckit.tasks` to generate implementation tasks
2. Execute tasks to write README.md content
3. Verify all examples against running SQL Server
4. Review for accuracy and completeness
