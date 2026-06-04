-- Spec 042 test data.
--
-- Runs once after SQL Server is reachable on localhost. Creates a TestDB
-- database and dbo.test sample table. The sql container health check
-- depends on TestDB existing, so this init step is what gates downstream
-- services in docker-compose.
--
-- NOTE: This file used to also CREATE LOGIN [EXAMPLE.COM\testuser] FROM
-- WINDOWS so a Kerberos-authenticated ATTACH could resolve to a real SQL
-- principal. SQL Server on Linux needs SSSD/realmd to look up Windows
-- principals, which the test stack does not provide -- the statement
-- always failed with Msg 15401. CI now exercises the POSIX GSSAPI path
-- via mssql_kerberos_auth_test(...) instead of an end-to-end ATTACH, so
-- the LOGIN mapping is no longer needed. See PR #112 (Option A) and
-- specs/042-integrated-authentication/ for the full discussion. End-to-end
-- ATTACH coverage remains documented as a manual test.

USE master;
GO

IF DB_ID('TestDB') IS NULL
    CREATE DATABASE TestDB;
GO

USE TestDB;
GO

IF OBJECT_ID('dbo.test', 'U') IS NOT NULL DROP TABLE dbo.test;
GO

CREATE TABLE dbo.test (
    id INT NOT NULL PRIMARY KEY,
    name NVARCHAR(50) NOT NULL
);
GO

INSERT INTO dbo.test (id, name) VALUES
    (1, 'kerb-row-A'),
    (2, 'kerb-row-B'),
    (3, 'kerb-row-C');
GO
