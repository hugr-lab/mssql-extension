# E12: where an extent lies in a full backup's first data pass

Date: 2026-11-09. Server: SQL Server 2025 RTM-CU8 (17.0.4075.5), container `mssql-v2test`. Research
only. No new database was made: the backups are those of E1, E2, E3, E5, E6, E7, the probe samples
and spec 085 task 3's T3 set, read locally. Scripts (git-ignored, beside the backups):
`design/001-mssql-backup-reader/experiments/E12/e12.py`, `e12b.py`, `e12_rank.py`.

The question (085 D4a): does a full backup's first MSDA pass carry exactly the GAM-allocated extents,
in order? If it does, the place of a page in the pass is computable from the allocation pages alone,
and opening a backup need not read every page header.

## Short answer

1. **Yes, for a backup on one media family: 56 of 56 full backups.** The 49 uncompressed fulls of
   the experiments and the 7 compressed ones (ZSTD, decompressed: the probe, E3 r0 / r1 taken under
   load, E6 F2 / F3 / FC1 / X0) all satisfy the rule exactly:
   - the pass is a run of **groups of 8 units, one group per extent**, its pages at their positions
     (unit k of a group holds the extent's page k, 0 misses);
   - the groups come in **ascending (file, extent) order**;
   - the extents are exactly **the GAM-allocated extents below the file's `Size`, plus the extents
     holding allocation pages** (the PFS pages every 8,088 pages, the GAM / SGAM / DCM / BCM pages
     every 511,232): a later PFS page's extent is not GAM-allocated but is copied (the probe's extent
     1011, PFS page 8088);
   - after the last extent, **only pad groups** (eight `m_type` 101 units) fill the pass to its
     transfer size (6 to 15 groups seen; none when the pass ends on a transfer boundary).

   Unallocated pages of a copied extent are zero units in their place, not missing: a group is
   always 8 units. So the k-th group of the pass is the k-th extent of (GAM ∪ allocation extents),
   and a page's offset in the pass is `rank(extent) × 64 KB + (page mod 8) × 8 KB`.

2. **A striped backup does not follow it per family.** On the two striped sets (E2 `e_stripe_a/b`,
   T3 `s1/s2`) the families' extents are disjoint, their union is exactly GAM ∪ allocation extents,
   and each family's extents ascend; but which family takes an extent is decided at run time: runs of
   16 extents (sometimes 32 or 96) on E2, of one or two on T3. A family's extent map is read from
   its page headers, one per group (085 D4a's explicit-runs path).

3. **A correction to 085 § Rules (MTF):** an MQDA's units follow its 2-byte lead-in directly; they
   are **not** 8 KB-aligned in the file. E2 `e_fg2full` and E7 `a_full` start their units at file
   offset 20,480 (4 KB-aligned). Reading from the next 8 KB boundary misreads every page of those
   two files.

## What this decides (085 D4a)

- **Opening a single-family full backup reads the allocation pages, not the data.** Group 0 of each
  file is its extent 0 (the file header, page 0, gives `Size`; the GAM, page 2, the first 4 GB's
  extents); each later GAM page's group has a rank known from the GAMs before it; the next file
  starts after the previous file's extents. Liveness needs the PFS pages, one per 64 MB, at ranks
  known the same way. A 10 TB backup reads about 1.3 GB of PFS pages and a few MB of GAM pages
  instead of 10 TB, and the resident index is D4a's extent bitmaps (the GAM bits) with their ranks.
- **The second pass, a differential's pass and a striped set** are read by their page headers, one
  per group: a pass 2 holds the pages changed while pass 1 was taken, in no derivable set.
- **A compressed backup** still walks its block chain (task 4: its headers lie between its
  payloads), so the I/O saving is for uncompressed files; for compressed ones the rule saves the
  decompression of pass 1 while the index is built.
- **Checked, not trusted:** the open pass verifies the rule where it reads (a group's page headers
  against its expected extent when it reads that group); a backup that breaks it is refused for the
  rank path and indexed by headers.

## Not covered

- Versions before 2025 (the CI matrix re-runs E12's check, D14).
- A full backup with `FILEGROUP =` / `FILE =` (a partial backup) beyond E2's `fg2full`, read-only
  filegroups, and files with more than one GAM interval (no experiment file reaches 4 GB).
- Differentials (their pass is the DCM's extents: 089).
