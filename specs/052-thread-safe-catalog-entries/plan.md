# Implementation Plan: Thread-Safe Catalog Entry Lifetime

**Branch**: `052-thread-safe-catalog-entries` | **Date**: 2026-05-26 | **Spec**: [spec.md](./spec.md)

**Input**: Feature specification from `/specs/052-thread-safe-catalog-entries/spec.md`

## Summary

`MSSQLTableSet` (and sibling caches `MSSQLSchemaSet` inside `MSSQLCatalog`, `MSSQLMetadataCache`) hand out raw pointers / `optional_ptr<CatalogEntry>` to the DuckDB binder. The underlying objects are owned by `unique_ptr` inside an internal map. Two concurrent paths destroy those objects while the binder still uses the raw pointer: (1) **the primary dbt failure mode** — `LoadSingleEntry` overwrites `entries_[name]` unconditionally on concurrent first-load of the same table (e.g. 4 dbt workers SELECTing the same source on a cold cache), destroying the winner's entry under the second-arriver's foot; (2) `Invalidate()` calls `entries_.clear()` and drops every outstanding pointer — a real UAF but NOT what dbt with default config hits, since dbt serialises DDL per worker connection and doesn't call `mssql_refresh_cache` in its hot loop.

**Technical approach**: change the ownership model from `unique_ptr` (single owner = the map) to `shared_ptr` (co-owned by the map + the binder's bind data). Specifically:

1. `entries_` becomes `unordered_map<string, shared_ptr<MSSQLTableEntry>>`. Same for `schema_entries_` in `MSSQLCatalog`.
2. `LoadSingleEntry` and `LookupSchema` use `emplace`-only semantics — concurrent first-load: winner stays, loser is dropped after construction.
3. `Invalidate()` **moves** the current map contents into a per-catalog `graveyard_` (a `vector<shared_ptr<…>>`) before clearing — outstanding raw pointers stay valid because the graveyard keeps the underlying objects alive.
4. The extension-owned bind data (`MSSQLBindData` produced by `GetScanFunction` and friends) holds a `shared_ptr<MSSQLTableEntry>` so that the bind data's destruction (= end of query execution) is what releases the reference. Plus an in-bind-phase anchor for the gap between `LookupEntry` and `GetScanFunction`.
5. Graveyard is **never auto-collected**. Catalog destruction (DETACH) frees everything in one shot. Memory tax: bounded by (unique tables touched × invalidation events). For realistic dbt workloads single-digit MBs; documented as known limitation, GC deferred to a future spec.

The fix lives entirely inside the extension; no DuckDB upstream API changes. Verified by extending `test/cpp/test_concurrent_reads.cpp` (scenarios 4/5/6) under UBSan and TSan.

## Technical Context

**Language/Version**: C++17 source, C++11-compatible ABI (DuckDB ODR constraint on Linux). See [CLAUDE.md → Build Troubleshooting → ODR errors].

**Primary Dependencies**: DuckDB main branch (`optional_ptr<CatalogEntry>` API surface). No new vcpkg deps. `std::shared_ptr` from `<memory>`.

**Storage**: In-memory only — `MSSQLTableSet::entries_`, `MSSQLCatalog::schema_entries_`, `MSSQLCatalog::graveyard_`. No persistence.

**Testing**: C++ unit tests via DuckDB's `unittest` harness (`test/cpp/test_concurrent_reads.cpp` extended). UBSan + TSan runs as new `make test-concurrent-sanitizer` target. Integration tests (`make integration-test`) for no-regression gate (SC-005).

**Target Platform**: Linux (GCC + Clang), macOS (Clang), Windows (MSVC). All three CI lanes already exercise the catalog path.

**Project Type**: DuckDB extension (single C++ library, dynamically loaded by DuckDB CLI / embedded DuckDB).

**Performance Goals** (per SC-006): single-threaded fast-path (cache hit) within ±10% of v0.2.0 baseline; concurrent fast-path (4 threads × 1000 binds against hot cache) within ±20% of v0.2.0.

**Constraints**: at most one extra atomic op per `GetEntry` cache-hit fast path (per Assumptions in spec). No new mutex contention on the hot path. No DuckDB upstream API change.

**Scale/Scope**: catalog ~1K-65K tables (CLAUDE.md / spec 033 baseline), bind rate up to 10K/s under analytic workload, invalidation rate ≤1/s under dbt-style DDL cadence. Graveyard worst-case (24h dbt server) ≤ 10K entries.

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Status | Notes |
|---|---|---|
| **I. Native and Open** | PASS | No changes to TDS layer; no new MS lib redistribution. |
| **II. Streaming First** | PASS | Scope is catalog metadata only; streaming pipeline unchanged. |
| **III. Correctness over Convenience** | **REINFORCED** | Spec is itself a correctness fix (UAF → defined behaviour). New `D_ASSERT` on TableEntry move-construction (no surprise relocations). |
| **IV. Explicit State Machines** | PASS | TableSet state machine (NOT_LOADED / NAMES_LOADED / FULLY_LOADED) preserved; ownership change is orthogonal. |
| **V. DuckDB-Native UX** | PASS | `optional_ptr<CatalogEntry>` API unchanged; users see no surface difference. |
| **VI. Incremental Delivery** | PASS | US1/US2 land together (single PR per Clarification 2026-05-26 Q2); US3 audit fixes sibling caches in same PR; rollback = revert the PR. |

**Complexity Tracking**: no violations to justify.

## Project Structure

### Documentation (this feature)

```text
specs/052-thread-safe-catalog-entries/
├── plan.md              # This file
├── research.md          # Phase 0 — strategy evaluation, ownership model decision
├── data-model.md        # Phase 1 — ownership graph + lifecycle state machine
├── quickstart.md        # Phase 1 — how to verify the fix locally
├── contracts/
│   └── ownership.md     # Internal contract: who owns entries, who anchors, when destroyed
├── checklists/
│   └── requirements.md  # /speckit-specify quality checklist (already created)
└── tasks.md             # Phase 2 output (/speckit-tasks command — NOT created here)
```

### Source Code (repository root) — files to touch

```text
src/
├── include/catalog/
│   ├── mssql_catalog.hpp           # schema_entries_ → shared_ptr; graveyard_ member
│   ├── mssql_table_set.hpp         # entries_ → shared_ptr
│   ├── mssql_schema_entry.hpp      # (no change — owns TableSet by value)
│   ├── mssql_table_entry.hpp       # (no change — lifetime managed by shared_ptr in TableSet)
│   ├── mssql_metadata_cache.hpp    # audit — GetTableMetadata returns const MSSQLTableMetadata* (raw ptr)
│   └── mssql_statistics.hpp        # audit — GetRowCount returns by value (already safe)
├── catalog/
│   ├── mssql_catalog.cpp           # LookupSchema emplace-only; graveyard append in InvalidateMetadataCache + InvalidateSchemaTableSet + RefreshCache + OnDetach (full drain)
│   ├── mssql_table_set.cpp         # LoadSingleEntry emplace-only; Invalidate moves to graveyard
│   └── mssql_table_function.cpp    # bind data holds shared_ptr<MSSQLTableEntry> anchor
└── table_scan/
    └── (audit bind paths)          # confirm scan-side bind data anchors entry

test/
├── cpp/
│   ├── test_concurrent_reads.cpp   # extend: scenario 5 (concurrent invalidation), scenario 6 (sibling caches)
│   └── test_catalog_graveyard.cpp  # NEW: graveyard append/destroy semantics
└── (no SQL test changes — race is below the SQL surface)

Makefile                            # new target: test-concurrent-sanitizer (UBSan + TSan run)
CHANGELOG.md                        # [Unreleased] entry on merge
```

**Structure Decision**: extension is a single C++ library; the fix is local to the catalog subsystem. Most edits in `src/catalog/`, plus one bind-data hook in `src/catalog/mssql_table_function.cpp`. Test additions in `test/cpp/`. No new directories.

## Complexity Tracking

| Violation | Why Needed | Simpler Alternative Rejected Because |
|-----------|------------|-------------------------------------|
| Graveyard is never auto-GCd | Without a DuckDB binder-unwind hook, we can't safely free outstanding-but-released entries. Catalog-lifetime tax is bounded for realistic workloads. | Manual `mssql_gc_catalog()` function rejected — adds API surface; can be added later if real-world telemetry shows growth concern. |
| `shared_ptr` adds atomic refcount cost | Plan-Q1 (rejected, see research.md): generation counter requires DuckDB binder cooperation that doesn't exist; refcount-graveyard adds same overhead plus a tracking layer. | `unique_ptr` + thread-local anchor stack rejected — DuckDB binder doesn't fire unwind callbacks at bind/execute boundaries, so anchor cleanup would leak per-thread. |
