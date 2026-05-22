# Specification Quality Checklist: DML Rewrite (spec 050)

**Purpose**: Validate specification completeness and quality before proceeding to planning
**Created**: 2026-05-22
**Feature**: [spec.md](../spec.md)

## Content Quality

- [x] No implementation details (languages, frameworks, APIs)
  - **Note**: spec.md deliberately names concrete artifacts:
    `MSSQLDirectDelete`, `MSSQLStagingUpdate`, `mssql_rowid_strategy`,
    `%%physloc%%`, `@@TRANCOUNT`, `SAVE TRANSACTION`, etc. These are
    not "implementation details" in the spec-template sense — they
    are the *subject* of the rewrite (the spec is about replacing one
    set of operators with another, naming the T-SQL the operators
    emit, and exposing two new session settings). A reader cannot
    understand the rewrite without seeing the names.
- [x] Focused on user value and business needs
  - User-facing value: (a) correctness (no more silent MERGE
    corruption, no more "this table has no PK so DML doesn't work
    at all"), (b) performance (≥ 5× speedup on bulk UPDATE/DELETE
    via BCP staging), (c) safety (atomic transaction wrap), (d)
    diagnostic surface (`mssql_table_capabilities`,
    `mssql_dml_log_transactions`).
- [x] Written for non-technical stakeholders
  - The Overview, User Scenarios, and Success Criteria sections are
    readable without C++ expertise. The FR section necessarily names
    symbols and T-SQL constructs, as discussed above.
- [x] All mandatory sections completed
  - User Scenarios & Testing, Requirements, Success Criteria,
    Assumptions, Dependencies, Out of Scope all populated.

## Requirement Completeness

- [x] No [NEEDS CLARIFICATION] markers remain
  - Every ambiguous decision is resolved in research.md (R1–R15) and
    referenced from spec.md / plan.md / data-model.md.
- [x] Requirements are testable and unambiguous
  - Each FR cites a concrete artifact (file path, table function
    name, T-SQL template, exception type). Each SC has a measurable
    yardstick (regex match, row count assertion, wall-clock ratio,
    `tokei` net LOC).
- [x] Success criteria are measurable
  - SC-001 (7-shape fixture assertions), SC-002 (TDS-trace regex
    match on a single SQL_BATCH), SC-003 (rowcount equality after
    JOIN-based UPDATE), SC-004 (≥ 5× wall-clock ratio), SC-005
    (three transaction-wrap sub-tests), SC-006 (savepoint name
    distinctness in SQL Server trace), SC-007 (exception message
    substring match), SC-008 (RETURNING parity test count),
    SC-009 (`tokei` ≤ +800 LOC), SC-010 (existing-test-green
    audit).
- [x] Success criteria are technology-agnostic (no implementation details)
  - **Caveat**: SC-001, SC-002, SC-007 reference specific test file
    paths, regex patterns, and exception types. As with the Content
    Quality caveat, these are inherent to a rewrite spec — the
    "outcome" is "the operator emits this T-SQL" and "the error
    message contains this substring".
- [x] All acceptance scenarios are defined
  - US1: 4 scenarios. US2: 5 scenarios. US3: 6 scenarios. US4: 5
    scenarios. US5: 3 scenarios.
- [x] Edge cases are identified
  - Eight edge cases listed: `%%physloc%%` concurrency, tempdb
    pressure, pass-through `BEGIN TRANSACTION`, savepoint length
    cap, OUTPUT with triggers, OUTPUT of MAX-types, connection drop
    mid-DML, direct-path SET expression referring to another table.
- [x] Scope is clearly bounded
  - Out of Scope explicitly rejects MERGE server-side execution,
    `ON CONFLICT` / upsert, parallel rowid scan, chunked-commit
    large DML, direct-path eligibility expansion, `%%physloc%%`
    auto-isolation hardening.
- [x] Dependencies and assumptions identified
  - Dependencies on spec 044 (Codec Layer — must be merged first)
    and spec 046 (BCP throughput — should be merged first;
    correctness intact without it but SC-004 is gated on it).
    Assumptions cover Docker SQL Server image, `XACT_STATE()`
    availability, DuckDB plan-tree shape stability, mocked
    engine-edition injection for Synapse fixtures.

## Feature Readiness

- [x] All functional requirements have clear acceptance criteria
  - FR-001..FR-005 (catalog enrichment) → SC-001 + US1 scenarios.
  - FR-010..FR-013 (direct DML) → SC-002 + US2 scenarios.
  - FR-020..FR-028 (staging DML) → SC-003 + US3 scenarios.
  - FR-030..FR-035 (transaction wrap) → SC-005, SC-006 + US4
    scenarios.
  - FR-040..FR-043 (MERGE guard + RETURNING) → SC-007, SC-008 +
    US5 scenarios.
  - FR-050..FR-051 (settings) → covered transitively by US2 /
    US3 acceptance tests.
  - FR-060..FR-062 (backward compat) → SC-010.
  - FR-070 (documentation) → manual review at PR time.
- [x] User scenarios cover primary flows
  - US1: capability detection (foundation).
  - US2: direct path (read hot path).
  - US3: staging path (write hot path).
  - US4: transaction wrap (atomicity + safety).
  - US5: MERGE guard + RETURNING generalization (loud error +
    refactor).
- [x] Feature meets measurable outcomes defined in Success Criteria
  - The ten SCs collectively cover correctness (SC-001, SC-003,
    SC-005, SC-007, SC-008), performance (SC-004), code-size budget
    (SC-009), and regression safety (SC-010).
- [x] No implementation details leak into specification
  - With the caveat noted above (this is a rewrite spec; the
    operator names, T-SQL templates, and session setting names are
    the subject of the work, not incidental implementation
    choices).

## Notes

- All items pass on first review.
- Source design doc (`feature-spec/refactoring-dml-050.md`) was used
  verbatim as the basis for spec.md, with the following structural
  adaptations to fit the `.specify/` template:
  - Source's "High-level architecture" + "Catalog enrichment" + "Path
    selection" sections → spec.md Overview + User Stories US1–US3 +
    data-model.md §E1–§E7.
  - Source's "Transaction wrap" section → spec.md US4 +
    data-model.md §E4b (Sink/Finalize description) + research.md §R5
    (decision rationale).
  - Source's "RETURNING support" + "MERGE early-error guard" →
    spec.md US5 + data-model.md §E8, §E10.
  - Source's "Caveats and known limitations" → spec.md Edge Cases +
    Out of Scope.
  - Source's "Acceptance criteria" (40 items) → spec.md Functional
    Requirements (FR-001..FR-070) + Success Criteria
    (SC-001..SC-010), with FRs grouped per User Story for clarity.
- Three decisions documented in research.md that the source doc left
  implicit:
  - **R1**: Two-path architecture (direct + staging) is retained;
    not collapsed to a unified staging path.
  - **R5**: Transaction wrap branch matrix (`@@TRANCOUNT` ×
    `XACT_STATE()`), including the `-1` "doomed" case which the
    source doc did not handle.
  - **R9**: Direct-path eligibility whitelist starts conservative
    (`LogicalGet`, `LogicalFilter`, `LogicalProjection` only);
    expansion is data-driven by real production query shapes.
- Source doc's name "spec 050" is preserved as the directory name
  (`specs/050-dml-rewrite/`) to align with the source doc's
  reference and the v0.2.0 refactor series numbering (043 →
  044 → 046 → 050).
- Spec deliberately defers four items the source doc proposed as
  in-scope: parallel scan with rowid, chunked-commit DML, direct-
  path eligibility expansion, and `%%physloc%%` isolation
  hardening. Rationale documented in Out of Scope.
- Ready for `/speckit-clarify` (likely no questions needed; spec is
  decision-locked) or directly for `/speckit-plan`.
