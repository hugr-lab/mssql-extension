# Specification Quality Checklist: UTF-16 Codec Consolidation

**Purpose**: Validate specification completeness and quality before proceeding to planning
**Created**: 2026-05-14
**Feature**: [spec.md](../spec.md)

## Content Quality

- [x] No implementation details (languages, frameworks, APIs)
  - **Note**: This spec deliberately names specific source files and function
    symbols (`Utf16LEEncode`, `TypeConverter::ConvertString`, etc.). These are
    not "implementation details" in the spec-template sense — they are the
    *subject* of the migration (the spec is about replacing one symbol with
    another at named call sites). A reader cannot understand the migration
    without seeing the names; the names are user-visible to the developer
    audience (the "users" of this internal refactor).
- [x] Focused on user value and business needs
  - The user-facing value is correctness (unified UTF-16 path with one
    validated implementation), performance (NVARCHAR scan throughput at least
    matching the legacy path, with measurable headroom from SIMD on non-trivial
    inputs), and maintainability (single source of truth). The "user" here is
    both the end user running queries and the developer maintaining the codebase.
- [x] Written for non-technical stakeholders
  - The Overview, User Stories, and Success Criteria are readable without C++
    expertise. The FR section necessarily names symbols, as discussed above.
- [x] All mandatory sections completed
  - User Scenarios & Testing, Requirements, Success Criteria, Assumptions,
    Out of Scope, Dependencies all populated.

## Requirement Completeness

- [x] No [NEEDS CLARIFICATION] markers remain
  - All three potentially-ambiguous decisions (legacy retire vs defer, symbol
    rename, batch-API necessity) were resolved with reasonable defaults
    documented in Clarifications + Assumptions, per the spec-template's
    guidance that reasonable defaults beat blocking on every detail.
- [x] Requirements are testable and unambiguous
  - Each FR cites either a concrete file path + line range (for migration FRs)
    or a concrete artifact (microbenchmark file, regression test file). Each SC
    has a measurable yardstick.
- [x] Success criteria are measurable
  - SC-001 (grep count), SC-002 (file existence check), SC-003 (test suite
    green), SC-004 (benchmark ratio with explicit slack), SC-005 (fixture
    count), SC-006 (round-trip equality), SC-007 (CI green), SC-008 (file
    presence check at the swapped paths).
- [x] Success criteria are technology-agnostic (no implementation details)
  - **Caveat**: SC-001, SC-002, SC-004, SC-008 reference specific paths,
    benchmark targets, and grep patterns. As with the Content Quality caveat,
    these are inherent to a migration spec — the "outcome" is "the symbol is
    gone from these files" and "the new symbol is in this place". The spec
    template's guidance applies primarily to user-facing feature specs; here
    the migration *is* the feature.
- [x] All acceptance scenarios are defined
  - Every user story has 2–4 acceptance scenarios in Given/When/Then form.
- [x] Edge cases are identified
  - Ten edge cases listed (invalid UTF-8/UTF-16LE, empty input, PLP chunks,
    unaligned buffers, CHAR/VARCHAR path unchanged, locale narrowing absent,
    spec 042 collision, Windows builds, post-migration rename).
- [x] Scope is clearly bounded
  - Out of Scope explicitly rejects the source doc's `MssqlTypeCodec` class
    hierarchy, `VariantCodec`, type-mapping holes, filter-pushdown /
    INSERT-VALUES / CTAS-DDL refactors, batch APIs, and CI perf gates.
- [x] Dependencies and assumptions identified
  - Dependencies on spec 043 (must be merged first — confirmed) and spec 042
    (parallel collaborator work) documented. Assumptions cover wrapper
    contract stability, PLP reassembly above the codec, scan-path call shape,
    Azure-token ASCII content, rebase strategy, benchmark-as-local-tool.

## Feature Readiness

- [x] All functional requirements have clear acceptance criteria
  - FR-001..FR-005 → SC-001, SC-003 (migration completeness + correctness).
  - FR-010..FR-013 → SC-002, SC-008 (legacy retirement, file rename).
  - FR-020..FR-024 → SC-004, SC-005 (microbenchmark).
  - FR-030..FR-031 → coordination notes (no direct SC; verified by 042's
    rebase being clean).
  - FR-040..FR-043 → SC-006, SC-007 (regression test + existing suite).
  - FR-050..FR-056 → SC-009, SC-010, SC-011 (end-to-end before/after
    benchmark + recorded artifact).
- [x] User scenarios cover primary flows
  - Story 1: scan decode (read hot path).
  - Story 2: BCP encode (write hot path).
  - Story 3: tail call sites + legacy retirement.
  - Story 4: Azure / FedAuth + spec 042 coordination.
  - Story 5: end-to-end before/after measurement on the integration
    SQL Server (DDL + INSERT + CTAS+BCP + COPY + SELECT workflow).
- [x] Feature meets measurable outcomes defined in Success Criteria
  - The eight SCs collectively cover migration completeness (SC-001, SC-002),
    correctness (SC-003, SC-005, SC-006), performance floor (SC-004), build
    stability (SC-007), and the legacy-retirement file state (SC-008).
- [x] No implementation details leak into specification
  - With the caveat noted above (this is a migration spec; the source-file
    paths and symbol names are the subject of the work, not incidental
    implementation choices).

## Notes

- All items pass on first review.
- Three reasonable-default decisions documented in Clarifications:
  - **Legacy retirement strategy** (two-pass: migrate + rename in same PR)
  - **PLP chunk boundary handling** (defer to row reader; no codec change)
  - **Performance assertion form** (≤ 1.10× legacy on every fixture; no
    hard "≥ N% faster" target)
- The spec deliberately scopes down from the source doc's
  `MssqlTypeCodec` class hierarchy proposal. Rationale documented in the
  spec's Overview and Out of Scope sections.
- The end-to-end before/after benchmark (User Story 5, FR-050..FR-056,
  SC-009..SC-011) was added on user request to produce real
  user-level throughput numbers against the integration SQL Server, in
  addition to the codec-level microbenchmark from FR-020..FR-024. Two
  refinements from the user's proposal:
  (a) Step 6 keeps `SELECT COUNT(*)` as a smoke check but adds a
  `SELECT *` materialization via `COPY ... TO '/dev/null'` to actually
  exercise NVARCHAR scan decode (COUNT alone does not pull NVARCHAR
  bytes over the wire).
  (b) The user requested **100M rows**. Applied at full scale to the
  three steps that exercise the per-cell UTF-16 codec hot path
  (CTAS+BCP, COPY/BCP, SELECT *). The INSERT-via-VALUES step is
  bounded at 100k rows because that path does not exercise the
  per-cell codec — it encodes the whole SQL_BATCH text to UTF-16LE
  once per batch via `tds_protocol.cpp:751,762`, so the codec cost
  is amortized and 100M rows would simply add hours of SQL Server
  insert overhead with no additional codec signal. The asymmetric
  row count is documented in User Story 5 step 3 and in FR-051.
- Ready for `/speckit-clarify` (likely no questions needed; spec is
  decision-locked) or directly for `/speckit-plan`.
