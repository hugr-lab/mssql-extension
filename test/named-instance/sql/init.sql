-- Spec 045 test data.
--
-- Runs once after SQL Server is reachable on the configured TCP port
-- (11433 in this stack, not the default 1433 — the non-default port is
-- deliberate so a passing test proves the resolver actually translated
-- TESTINST → 11433 instead of falling back to 1433).
--
-- Creates a NamedInstTest database and a probe table with a single row
-- the run-tests.sh smoke test SELECTs back to prove end-to-end.

USE master;
GO

IF DB_ID('NamedInstTest') IS NULL
    CREATE DATABASE NamedInstTest;
GO

USE NamedInstTest;
GO

IF OBJECT_ID('dbo.Probe', 'U') IS NOT NULL DROP TABLE dbo.Probe;
GO

CREATE TABLE dbo.Probe (
    id      INT NOT NULL PRIMARY KEY,
    payload NVARCHAR(100) NOT NULL
);
GO

INSERT INTO dbo.Probe (id, payload) VALUES (1, N'spec045 lives');
GO
