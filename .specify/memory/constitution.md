<!--
  SYNC IMPACT REPORT
  ==================
  Version change: 1.0.0 → 2.0.0

  Modified principles:
    - "I. Native First" → "I. Native and Open" (expanded: no MS lib redistribution)
    - "III. Streaming by Design" → "II. Streaming First" (renumbered, simplified)
    - "IV. Correctness Over Completeness" → "III. Correctness over Convenience" (redefined: PK-based row identity)
    - "V. Deterministic State Machines" → "IV. Explicit State Machines" (renamed, simplified)

  Removed principles:
    - "II. Minimal Dependencies" (vcpkg requirement removed from constitution)
    - "VI. Safe Interruption" (merged into Explicit State Machines)
    - "VII. Reproducible Builds" (moved to operational docs)
    - "VIII. Developer Experience" (moved to operational docs)

  Added principles:
    - "V. DuckDB-Native UX" (catalog integration mandate)
    - "VI. Incremental Delivery" (read-first, writes progressive)

  Added sections:
    - Row Identity Model (PK-based rowid semantics)
    - Version Baseline (SQL Server 2019+, UTF-8/UTF-16LE)

  Removed sections:
    - Scope (In-Scope / Out-of-Scope for MVP)
    - Testing & Development Environment

  Templates requiring updates:
    - .specify/templates/plan-template.md: ✅ no changes required (generic)
    - .specify/templates/spec-template.md: ✅ no changes required (generic)
    - .specify/templates/tasks-template.md: ✅ no changes required (generic)

  Follow-up TODOs: None
-->

# DuckDB × Microsoft SQL Server Extension Constitution

## Purpose

This project provides an open-source DuckDB extension that integrates Microsoft SQL
Server as a remote database using a native TDS client implemented in C++.

The extension enables DuckDB to:

- Stream data from SQL Server without ODBC/JDBC
- Expose SQL Server schemas, tables, and views via the DuckDB catalog
- Execute catalog-driven DDL and controlled DML
- Safely support UPDATE and DELETE via primary keys
- Cancel running queries without dropping connections

## Core Principles

### I. Native and Open

All SQL Server connectivity MUST use a native TDS implementation.

- No ODBC, JDBC, FreeTDS, or proprietary drivers permitted
- No redistribution of Microsoft client libraries
- Rationale: Eliminates external runtime dependencies, licensing concerns, and
  distribution restrictions

### II. Streaming First

Results MUST be streamed directly into DuckDB execution pipelines.

- No full result buffering permitted
- Row data MUST flow from TDS packets into DuckDB vectors without intermediate copies
- Memory allocation MUST be bounded by chunk size, not result size
- Rationale: Enables processing of arbitrarily large tables without memory exhaustion

### III. Correctness over Convenience

Operations MUST fail explicitly when correctness cannot be guaranteed.

- No use of unstable physical identifiers (`%%physloc%%`)
- UPDATE and DELETE MUST require logical row identity (primary keys)
- Unsupported operations MUST fail with clear error messages
- Rationale: Silent data corruption or ambiguous behavior is unacceptable; users must
  know exactly what succeeded and what failed

### IV. Explicit State Machines

Connection lifecycle, protocol handling, and cancellation MUST be explicit and testable.

- TDS connection state MUST be modeled as an explicit state machine
- All state transitions MUST be documented and covered by tests
- Connections MUST remain reusable after cancellation (Attention + drain sequence)
- Rationale: Explicit states simplify debugging, prevent undefined behavior, and
  enable formal reasoning about protocol correctness

### V. DuckDB-Native UX

SQL Server MUST appear as a real DuckDB catalog.

- Schemas, tables, and views MUST be browsable via standard DuckDB catalog queries
- DDL MUST be issued through DuckDB catalog commands, not raw SQL strings
- Type mappings MUST preserve semantics (not just byte compatibility)
- Rationale: Users should not need to learn SQL Server specifics to query data

### VI. Incremental Delivery

Features MUST be delivered incrementally with strict semantics at each stage.

- Read-only access MUST be implemented first
- Writes MUST be enabled progressively with explicit semantic guarantees
- Each milestone MUST be independently usable and testable
- Rationale: Partial correctness is preferable to full incorrectness; users can
  adopt features as they stabilize

## Row Identity Model

SQL Server does not expose a stable physical row identifier.

Therefore:

- DuckDB `rowid` MUST map to primary key values
- Single-column PK → scalar `rowid`
- Composite PK → `STRUCT` rowid
- Tables without PK MUST NOT support catalog-based UPDATE/DELETE

Physical row locators (`%%physloc%%`, `%%lockres%%`) MUST NOT be used under any
circumstances.

## Version Baseline

- SQL Server 2019 or newer
- UTF-8 collations are assumed for optimal text handling
- SQL text and parameters MUST be transmitted as UTF-16LE per TDS protocol

## Governance

- This constitution supersedes all other development practices for this project
- Amendments require:
  1. Written proposal with rationale
  2. Review of impact on existing code and documentation
  3. Version bump following semantic versioning (see below)
- All PRs MUST verify compliance with these principles
- Complexity beyond these principles MUST be justified in PR description

### Versioning Policy

- **MAJOR**: Backward-incompatible principle removal or redefinition
- **MINOR**: New principle/section added or materially expanded guidance
- **PATCH**: Clarifications, wording, typo fixes, non-semantic refinements

**Version**: 2.0.0 | **Ratified**: 2026-01-14 | **Last Amended**: 2026-01-15
