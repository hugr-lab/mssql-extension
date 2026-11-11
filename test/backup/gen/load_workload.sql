-- Spec 085 PR 2b: the load fixture's workload, run in the background by generate.sh until it sets
-- dbo.control.stop_requested: small transactions that insert, update and delete, each committed, and
-- one long transaction left open now and then (so a backup's end can find one in flight).
SET NOCOUNT ON;
SET XACT_ABORT ON;
USE bak_load;
DECLARE @i bigint = 0;
WHILE (SELECT stop_requested FROM dbo.control) = 0
BEGIN
	BEGIN TRANSACTION;
	INSERT INTO dbo.events (at, payload, status)
	SELECT TOP (200) SYSDATETIME(), CONCAT('w', @i, '-', ROW_NUMBER() OVER (ORDER BY (SELECT NULL))), 1 FROM sys.all_objects;
	UPDATE TOP (100) dbo.events SET status = (status + 1) % 4, payload = CONCAT('u', @i) WHERE id % 97 = @i % 97;
	DELETE TOP (50) FROM dbo.events WHERE id % 89 = @i % 89;
	INSERT INTO dbo.journal VALUES (@i, CONCAT('tick ', @i));
	UPDATE dbo.control SET ticks = ticks + 1;
	IF @i % 25 = 0
		WAITFOR DELAY '00:00:00.300';	-- a transaction held open across a moment
	COMMIT;
	SET @i += 1;
END;
GO
