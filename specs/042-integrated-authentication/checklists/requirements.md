# Specification Quality Checklist: Integrated Authentication (Kerberos / SSPI)

**Purpose**: Validate specification completeness and quality before proceeding to planning
**Created**: 2026-05-12
**Feature**: [spec.md](../spec.md)

## Content Quality

- [ ] No implementation details leak into the user-visible parts of the spec (protocol specifics may appear in Assumptions and Key Entities — this is a protocol-level feature, like 041)
- [ ] Focused on user value: AD-joined SQL Server users can finally connect
- [ ] Written so a non-technical reviewer can follow the user scenarios
- [ ] All mandatory sections completed (Problem Statement, User Scenarios, Requirements, Success Criteria, Assumptions, Out of Scope, Open Questions)

## Requirement Completeness

- [ ] No [NEEDS CLARIFICATION] markers remain (open questions are flagged separately in the Open Questions section)
- [ ] All FRs are testable and unambiguous
- [ ] Success criteria are measurable (each SC names a concrete observable check)
- [ ] All acceptance scenarios are defined (each user story has 3+ scenarios)
- [ ] Edge cases are identified (clock skew, SPN mismatch, pool reuse, TLS + auth interaction, multi-homed hosts, etc.)
- [ ] Scope is clearly bounded (Out of Scope section enumerates v1 exclusions)
- [ ] Dependencies and assumptions identified (system Kerberos libraries, KDC reachability, SPN registration, no DuckDB-bundled Kerberos client)

## Feature Acceptance Criteria (the user-facing "must work" list)

These are the executable acceptance gates. Each maps to one or more SC-### in spec.md.

- [ ] `kinit user@REALM` followed by `ATTACH 'Server=...;Trusted_Connection=yes;Encrypt=yes' AS sqlserver (TYPE mssql)` succeeds and basic catalog queries return expected rows (maps to SC-001, US1).
- [ ] On a domain-joined Windows host, `ATTACH 'Server=...;Trusted_Connection=yes' AS sqlserver (TYPE mssql)` succeeds without `kinit` (maps to SC-002, US2).
- [ ] Missing or expired ticket produces a clear error containing `"no credentials cache"` or `"ticket expired"` (maps to SC-005).
- [ ] Conflicting options (e.g. `Trusted_Connection=yes` together with `User Id=...`) fail at ATTACH-time validation with a clear message naming the offending keys (maps to FR-008, SC-005).
- [ ] Keytab mode (`authenticator=krb5;krb5-keytabfile=...;User Id=svc@REALM`) works for service-account scenarios (maps to SC-003, US3).
- [ ] Raw-credentials mode (`authenticator=krb5;User Id=alice;Password=...;krb5-realm=...`) works when KDC is reachable but no client config exists (maps to FR-004, US4).
- [ ] All existing SQL-auth and Azure-AD integration tests pass unchanged: `test/sql/attach/`, `test/sql/azure/`, `test/sql/integration/`, `test/sql/transaction/`, `test/sql/insert/`, `test/sql/dml/`, `test/sql/copy/`, `test/sql/ctas/` (maps to SC-004, US5).
- [ ] `mssql_pool_stats()` and connection pooling work identically under Kerberos auth — same lifecycle, same reuse, no per-query re-authentication (maps to SC-006, FR-011).
- [ ] All four target platforms build in CI: Linux x86_64, Linux ARM64, macOS ARM64, Windows x64 (maps to SC-007).
- [ ] README is updated: the `"Only SQL Server authentication is supported"` line is removed; new Integrated Auth section is present; Key Aliases and Secret Fields tables include the new keys.
- [ ] `docs/kerberos.md` exists and the examples in it are runnable against the containerized KDC fixture.
- [ ] `Trusted_Connection=yes` on a build compiled with `ENABLE_KRB5=OFF` produces the documented "compiled without Kerberos support" error rather than a generic failure (maps to FR-012).
- [ ] Wire-level protocol checks: LOGIN7 with `OptionFlags2.fIntSecurity` set, user/password fields empty, SSPI blob present at correct offset; server `0xED` continuation tokens consumed and responded to via SSPI Message packets (maps to FR-006).

## Feature Readiness

- [ ] All functional requirements have clear acceptance criteria above
- [ ] User scenarios cover primary flows (US1 = POSIX cred-cache, US2 = Windows SSPI, US3 = keytab, US4 = raw, US5 = regression)
- [ ] Feature meets measurable outcomes defined in Success Criteria
- [ ] Implementation details (GSSAPI calls, SSPI calls, TDS field offsets) are confined to research.md / plan.md / quickstart.md — spec.md describes behavior, not mechanism

## Notes

- Protocol-level details (TDS LOGIN7 bit positions, GSSAPI OID values) appear in spec.md's Assumptions section as context — same convention as 041's XML wire format. This is appropriate because the domain *is* the protocol.
- The Open Questions section in spec.md contains five items that are genuinely unresolved (CMake flag default, Windows-in-v1 scope, EPA timing, SPN override location, error-message wording). These should be confirmed with the maintainers before T006 of tasks.md is scheduled.
- Acceptance gates that require a real on-prem AD (cross-realm trust, EPA-enforcing servers) are explicitly Out of Scope for v1 and are not included in the checklist above.
