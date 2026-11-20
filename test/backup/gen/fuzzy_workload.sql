-- Spec 085 PR 5: the fuzzy fixture's workload (fuzzy.sql), run in the background by generate.sh -- three sessions,
-- SESSION 0, 1 and 2, as E3's (a) -- until it sets dbo.control.stop_requested: short transactions on busy, heap
-- and lob with no pause, one in twenty rolled back. Each session keeps to keys of its own (a third of busy's and
-- heap's first 240000, the lob rows id % 3 = SESSION, its own appends), so no session waits for another; none
-- reaches busy's rows from 290000 to 291000, which the open transaction (fuzzy_open.sql) holds.
SET NOCOUNT ON;
SET XACT_ABORT ON;
USE bak_fuzzy;
DECLARE @i bigint = 0;
DECLARE @a bigint;
WHILE (SELECT stop_requested FROM dbo.control) = 0
BEGIN
	SET @a = (@i * 37) % 79000 + 1 + $(SESSION) * 80000;
	BEGIN TRY
		BEGIN TRANSACTION;
		UPDATE dbo.busy SET v = v + 1 WHERE id BETWEEN @a AND @a + 40;
		INSERT INTO dbo.busy (v, pad) SELECT TOP (20) @i % 1000, CONCAT('w', @i) FROM sys.all_objects;
		DELETE FROM dbo.busy WHERE id BETWEEN @a + 500 AND @a + 504;
		UPDATE dbo.heap SET v = v + 1, pad = REPLICATE('x', 100 + (@i % 700)) WHERE id BETWEEN @a AND @a + 30;
		INSERT INTO dbo.heap (id, v, pad)
		SELECT TOP (20) CAST(1000000000 AS bigint) * ($(SESSION) + 1) + @i * 20 + ROW_NUMBER() OVER (ORDER BY (SELECT NULL)),
			@i % 1000, 'w'
		FROM sys.all_objects;
		UPDATE dbo.lob SET doc = REPLICATE(CONVERT(varchar(max), CONCAT('U', @i % 10)), 8500 + (@i % 20000))
		WHERE id = (@i * 3 + $(SESSION)) % 3000 + 1;
		UPDATE dbo.control SET ticks = ticks + 1;
		IF @i % 20 = 7
			ROLLBACK;
		ELSE
			COMMIT;
	END TRY
	BEGIN CATCH
		-- A deadlock victim (1205: the heap's page and row locks among three sessions) goes on with the next one.
		IF XACT_STATE() <> 0
			ROLLBACK;
		IF ERROR_NUMBER() <> 1205
			THROW;
	END CATCH;
	SET @i += 1;
END;
GO
