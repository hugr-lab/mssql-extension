# Specification Quality Checklist: LOGIN7 Non-ASCII Fix + simdutf Foundation

**Purpose**: Validate specification completeness and quality before proceeding to planning
**Created**: 2026-05-14
**Feature**: [Link to spec.md](../spec.md)

## Content Quality

- [x] No implementation details (languages, frameworks, APIs)
  - Spec references existing functions by name (`BuildLogin7`, `ParseUri`,
    `EncodePassword`, etc.) and specific file paths because that's how this
    codebase's specs are written — these are anchors for the developer, not
    implementation prescriptions. The MS-TDS protocol facts (nibble-swap,
    XOR 0xA5, `cch*` semantics) are protocol-level, not implementation-level.
- [x] Focused on user value and business needs
- [x] Written for non-technical stakeholders
  - Caveat: the audience for an extension-internal protocol fix is necessarily
    technical. User stories are framed in user-observable behavior
    ("authentication fails / succeeds"); the FR list is necessarily technical.
- [x] All mandatory sections completed

## Requirement Completeness

- [x] No [NEEDS CLARIFICATION] markers remain
- [x] Requirements are testable and unambiguous
- [x] Success criteria are measurable
- [x] Success criteria are technology-agnostic (no implementation details)
  - SC-008 mentions `ldd` / `otool -L` / Dependency Walker as verification
    tooling, not as implementation detail. Behavior-level metric: "no new
    runtime shared-library dependency."
- [x] All acceptance scenarios are defined
- [x] Edge cases are identified
- [x] Scope is clearly bounded
- [x] Dependencies and assumptions identified

## Feature Readiness

- [x] All functional requirements have clear acceptance criteria
- [x] User scenarios cover primary flows
  - Story 1 (P1, password); Story 2 (P1, other fields); Story 3 (P2,
    connection-string formats); Story 4 (P2, simdutf foundation).
- [x] Feature meets measurable outcomes defined in Success Criteria
- [x] No implementation details leak into specification
  - File paths and function names are anchor references, not prescriptions.
    The wrapper filename is explicitly left to `/speckit-plan`.

## Notes

- This spec was re-scoped twice during specification based on user direction:
  1. Initial draft put simdutf and LOGIN7 fix together (from source document).
  2. User asked to move simdutf to spec 044, narrowing 043 to LOGIN7 fix +
     connection-string audit.
  3. User then asked to install simdutf in 043 and use it from the LOGIN7
     fix as the first consumer, leaving bulk migration in 044. Final scope
     reflects this last direction.
- The original source document (`feature-spec/refactoring-foundation-043.md`)
  asserted that simdutf is already linked by DuckDB and headers are inside
  `duckdb/third_party/simdutf/`. Verified false against the current
  submodule — simdutf is not present in `third_party/`. The spec uses vcpkg
  as the source of truth instead.
- The proposed wrapper API in the source document used class-based interfaces
  coupled to DuckDB `Vector` and `TdsWriter`. The spec rejects this in
  FR-032 / Out of Scope in favor of free-function primitives matching the
  legacy converter's signatures, deferring all DuckDB-coupled batch APIs to
  spec 044.
- The source document framed §4.8 as "fix unknown root cause via diagnosis
  plan" — five candidate causes (a)–(e). Static review of
  `src/tds/tds_protocol.cpp:153-219` identified the cause unambiguously as
  (c) — `cchPassword = password.size()` treats UTF-8 byte count as UTF-16
  code-unit count, and the same bug repeats for hostname, username, appname,
  servername, and database. Spec 043 fixes all six fields rather than just
  password.
