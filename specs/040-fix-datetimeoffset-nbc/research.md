# Research: Fix DATETIMEOFFSET in NBC Row Reader

**Phase 0 Output** | **Date**: 2026-02-18

## Findings

No NEEDS CLARIFICATION items existed in the spec. All technical context was resolved during the code audit phase.

### Decision 1: Fix approach for ReadValueNBC

**Decision**: Add a `case TDS_TYPE_DATETIMEOFFSET` block to `ReadValueNBC()` using the identical 1-byte-length-prefix pattern already used by DATE, TIME, DATETIME2, and UNIQUEIDENTIFIER in the same function.

**Rationale**: The `SkipValueNBC()` function already handles DATETIMEOFFSET with this exact pattern (lines 829-835). The `ReadValue()` function (standard ROW path) delegates to `ReadDateTimeOffsetType()` which also uses the 1-byte length prefix pattern. The NBC read pattern is simpler because NULL is already handled by the bitmap — we only need `length prefix + data copy`.

**Alternatives considered**:
- Refactoring all NBC datetime reads into a shared helper: Rejected — the existing code repeats the same 6-line pattern for each type, and this is consistent with the codebase style. Adding an abstraction for a one-line fix would be over-engineering.

### Decision 2: Test strategy for NBCROW encoding

**Decision**: Create a test table with many nullable columns (including DATETIMEOFFSET at scales 0, 3, 7 and TIME at scales 0, 3, 7) to reliably trigger NBCROW encoding, then verify all values.

**Rationale**: SQL Server uses NBCROW automatically when the estimated size savings justify it. Having ~20+ nullable columns virtually guarantees NBCROW. The existing `NullableTypes` table (docker/init/init.sql) has 22 nullable columns but lacks DATETIMEOFFSET and TIME scale variants. A new table `NullableDatetimeScales` will add the missing datetime types at all scale boundaries.

**Alternatives considered**:
- Extending the existing `NullableTypes` table: Rejected — would require updating all existing tests that reference this table's column list.
- Unit test with synthetic TDS packets: Possible but fragile — constructing valid NBC packets manually is error-prone and doesn't test the full stack. Integration tests against real SQL Server are more reliable.

### Decision 3: Scale coverage boundaries

**Decision**: Test scales 0, 3, and 7 for each scale-dependent type (TIME, DATETIME2, DATETIMEOFFSET). These hit all three byte-length buckets (3, 4, 5 bytes for time component).

**Rationale**: `GetTimeByteLength()` has three branches: scale 0-2 → 3 bytes, scale 3-4 → 4 bytes, scale 5-7 → 5 bytes. Testing at boundaries 0, 3, 7 covers all branches. Scale 0 is the most critical — it was the root cause of a previous DATETIME2 bug (issue #73).

**Alternatives considered**:
- Testing all 8 scales (0-7): Excessive — scales within the same byte-length bucket use identical wire encoding, just different tick multipliers. The `TimeTicksToMicroseconds()` conversion is already tested via existing datetime2_scale tests.
