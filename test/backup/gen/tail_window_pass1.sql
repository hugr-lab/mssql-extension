-- Spec 085 D11: run while generate.sh holds tail_window's backup in its first data pass. Every filler
-- page changed (a small change: the log stays small), so the second pass copies its extents again and
-- lasts long enough to be held in.
SET NOCOUNT ON;
GO
UPDATE bak_tail_window.dbo.filler SET n = n + 1;
GO
