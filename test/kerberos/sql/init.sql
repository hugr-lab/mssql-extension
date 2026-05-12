-- Spec 042 test data + AD-principal login mapping.
--
-- Runs once after SQL Server is reachable on localhost. Creates:
--   - a TestDB database
--   - dbo.test sample table for the [kerberos] integration tests
--   - a Windows-style login mapped to testuser@EXAMPLE.COM so Kerberos auth
--     can resolve to a real SQL principal
--
-- Note: SQL Server on Linux maps a Kerberos principal to a "Windows
-- authentication" login using the user@REALM form. Without the matching
-- login, Kerberos auth at the protocol layer succeeds but the server still
-- rejects the connection with a "login failed" error.

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

-- Windows-style login mapping. The literal form "EXAMPLE\\testuser" is what
-- SQL Server reports back; it accepts kinit-style "testuser@EXAMPLE.COM" at
-- the auth layer and maps to this login.
USE master;
GO

IF NOT EXISTS (SELECT 1 FROM sys.server_principals WHERE name = 'EXAMPLE.COM\testuser')
    CREATE LOGIN [EXAMPLE.COM\testuser] FROM WINDOWS;
GO

USE TestDB;
GO

IF NOT EXISTS (SELECT 1 FROM sys.database_principals WHERE name = 'EXAMPLE.COM\testuser')
    CREATE USER [EXAMPLE.COM\testuser] FOR LOGIN [EXAMPLE.COM\testuser];
GO

GRANT SELECT, INSERT, UPDATE, DELETE ON SCHEMA::dbo TO [EXAMPLE.COM\testuser];
GO
