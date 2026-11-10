# E18 — the boot page after a rename, a compatibility change and a restore under another name

**Question.** The reader checks the boot page (1:9) against the backup set's trailer: name, version,
compatibility level, family GUID, collation (spec 085 PR 2b, `CheckBootAgainstSet`). Does the boot
page follow the database when it is renamed, its level changed, or it is restored under a new name,
so that the check does not refuse a legitimate backup?

**Method** (2026-11-10, SQL Server 2022, a throwaway container):

1. `CREATE DATABASE e18_old`, `ALTER DATABASE e18_old MODIFY NAME = e18_renamed`,
   `ALTER DATABASE e18_renamed SET COMPATIBILITY_LEVEL = 130`, then a COPY_ONLY full.
2. That backup restored as `e18_restored` (`WITH MOVE`), then a COPY_ONLY full of it.

Both backups read with `ReadBootPage` and checked with `CheckBootAgainstSet`.

**Result.** In both, the boot page's `dbi_dbname` is the database's current name (`e18_renamed`,
`e18_restored`) and `dbi_cmptlevel` is 130, agreeing with the trailer's SCIN; the family GUID is
the same in both (a restore keeps it). The check passes; the logical file names keep the original
(`e18_old`), which the reader does not compare.

**Not tested.** A level changed while a backup runs (the two records could then disagree; the check
would refuse such a backup by name).
