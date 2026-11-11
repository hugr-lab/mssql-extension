# E17 — the allocation pages of a later GAM interval

**Question.** Where does a data file put the allocation pages of its second GAM interval (a GAM page
maps 511,232 pages, about 4 GB)? The reader must know them to decide which pages are live past the
first 4 GB, and to read their bitmaps (spec 085 PR 2b, `backup_allocation`).

**Method** (2026-11-10, SQL Server 2022, a throwaway container): a database whose data file is 5 GB,
then `sys.dm_db_page_info(DB_ID(), 1, page, 'DETAILED')` for the pages around 511,232.

**Result.**

| page | type |
|---|---|
| 0, 1, 2, 3 | file header, PFS, GAM, SGAM |
| 6, 7 | DCM (differential map), BCM (ML map) |
| 9 | boot page |
| 511,226 – 511,231 | none (data) |
| **511,232** | **GAM** |
| **511,233** | **SGAM** |
| 511,234 – 511,237 | none |
| **511,238** | **DCM** |
| **511,239** | **BCM** |

So a later interval starts with its GAM at +0 and SGAM at +1 and has its DCM and BCM at +6 and +7,
as in the first interval; it has no file header and no boot page. PFS pages keep their own period
(every 8,088 pages) across intervals. BusinessCentral.DbReader's `PROVENANCE.md` (MIT) records the
same layout.

**Used by.** `AllocationMap` (the GAM of interval k at `k × 511,232`); the catalog fixture's data
file is 4,100 MB so that opening its backup reads the second interval's GAM, on every version of the
oracle lane.
