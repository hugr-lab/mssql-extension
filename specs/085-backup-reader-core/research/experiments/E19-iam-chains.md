# E19 — IAM chains across two files, and after a file is removed

**Question.** A table's pages come from its allocation unit's IAM chain (DQ-40). What does a chain
of more than one page look like, and what happens to it when a data file is emptied and removed?

**Method** (2026-11-10, SQL Server 2022, a throwaway container): a database of two 64 MB data files
in PRIMARY; `dbo.wide (id int PRIMARY KEY, pad char(4000))` with 6,000 rows, spread over both files
by proportional fill; a COPY_ONLY full; then `DBCC SHRINKFILE (e19_2, EMPTYFILE)` and
`ALTER DATABASE e19 REMOVE FILE e19_2`, and another full. The chains read with `ReadIamChain`.

**Result.**

- Two files: the table's chain has one IAM page per (file, GAM interval) it has extents in, here two
  -- the second page mapping file 3 (the second data file; file 2 is the log) --, linked by the page
  header's next pointer, the sequence ascending, the single-page slots on the first page only. Every
  live data, index, LOB and IAM page lies in exactly one chain, under the AU of its own header.
- After the removal: the chains lose the removed file's IAM pages and are **not renumbered**: five
  chains start at sequence 1. So the reader requires sequence numbers to ascend, not to start at 0
  or be gapless.
- The removed file is still listed in the backup set's trailer, as an SFIN record with flag `0x20`
  at +8, which `RESTORE FILELISTONLY` leaves out and of which the backup carries no page (the files
  fixture, `test/backup/gen/files.sql` / `files_remove.sql`, made the same observation).

**Used by.** `ReadIamChain` (sequence ascending, from any number); `backup_set` (an SFIN record with
0x20 left out, its id kept, a backup carrying a page of it refused).
