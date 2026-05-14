# Quickstart: LOGIN7 Non-ASCII Fix + simdutf Foundation

**Feature**: 043-refactoring-foundation
**Audience**: developers implementing or reviewing spec 043

## Prerequisites

- macOS / Linux dev box (Windows CI build is workflow-only)
- DuckDB extension build deps already installed (see `CLAUDE.md`)
- Docker Desktop running for integration tests
- Submodules initialized: `git submodule update --init --recursive`

## 1. Update dependencies

```bash
# Add simdutf to vcpkg.json (will be done as part of spec 043 PR)
#   "dependencies": [ "openssl", "simdutf" ]
# vcpkg baseline may need bumping — check research.md R1.
```

## 2. Build with the new dependency

```bash
# Static linking is the project default; no extra flags required.
GEN=ninja make           # release
GEN=ninja make debug     # debug
```

Sanity-check that simdutf was found:

```bash
# CMake configure output should include a "Found simdutf" line.
# If it doesn't, vcpkg baseline bump is needed.
```

Sanity-check static linking (Linux):

```bash
ldd build/release/extension/mssql/mssql.duckdb_extension | grep -i simdutf
# Expected: no output (simdutf is statically linked).
```

## 3. Run C++ unit tests (no SQL Server required)

```bash
GEN=ninja make
./build/release/test/unittest "[mssql_login7_encoding]"
./build/release/test/unittest "[mssql_simdutf_wrappers]"
./build/release/test/unittest "[mssql_connection_string_parsing]"
```

Each suite should pass with zero failures.

## 4. Bring up the test SQL Server

```bash
make docker-up
make docker-status    # wait for the container to report healthy
```

## 5. Run the LOGIN7 integration tests

```bash
# Requires DSN env vars; see CLAUDE.md > "Test Infrastructure".
# Default for a fresh Docker container started by make docker-up:
export MSSQL_TESTDB_DSN='Server=localhost,1433;Database=TestDB;User Id=sa;Password=TestPassword1'

# Pre-fix expectation: non_ascii_password.test FAILS at the ATTACH
# step with "Login failed for user 'user_ru'."
# Post-fix expectation: every test PASSES.

make integration-test  # runs everything under test/sql/
```

If you want to target only the new tests:

```bash
./build/release/test/unittest test/sql/integration/non_ascii_password.test
./build/release/test/unittest test/sql/integration/non_ascii_connection_formats.test
```

## 6. Manual reproducer (the v0.1.18 failing case)

```bash
# Open DuckDB with the freshly built extension.
./build/release/duckdb

# In DuckDB CLI:
INSTALL mssql FROM local_build_debug;
LOAD mssql;

# Create a login on the Docker SQL Server with a Cyrillic password
# (uses the sa connection):
ATTACH 'Server=localhost,1433;Database=master;User Id=sa;Password=TestPassword1'
    AS mssql_admin (TYPE mssql);

CALL mssql_exec('mssql_admin', '
    IF SUSER_ID(''user_ru'') IS NULL
        CREATE LOGIN [user_ru]
            WITH PASSWORD = N''Тест123!'',
            CHECK_POLICY = OFF;
    USE TestDB;
    IF USER_ID(''user_ru'') IS NULL
        CREATE USER [user_ru] FOR LOGIN [user_ru];
    ALTER ROLE db_datareader ADD MEMBER [user_ru];
');

DETACH mssql_admin;

# This is the failing case in v0.1.18, the passing case post-fix:
ATTACH 'Server=localhost,1433;Database=TestDB;User Id=user_ru;Password=Тест123!'
    AS mssql_ru (TYPE mssql);

SELECT 1;
-- Pre-fix: ERROR: Login failed for user 'user_ru'.
-- Post-fix: ┌───┐
--           │ 1 │
--           ├───┤
--           │ 1 │
--           └───┘
```

Repeat for `"Ünlaut$2024"` (accented Latin), `"🔒secure!"` (emoji
surrogate pair), and for non-ASCII values of the `Database` parameter
to cover Story 2.

## 7. Round-trip the three connection-string formats (Story 3)

```sql
-- URI form (percent-encoded):
ATTACH 'mssql://user_ru:%D0%A2%D0%B5%D1%81%D1%82123%21@localhost:1433/TestDB'
    AS mssql_uri (TYPE mssql);

-- ADO.NET key/value form:
ATTACH 'Server=localhost,1433;Database=TestDB;User Id=user_ru;Password=Тест123!'
    AS mssql_ado (TYPE mssql);

-- DuckDB secret form:
CREATE SECRET mssql_ru (
    TYPE mssql,
    server 'localhost,1433',
    database 'TestDB',
    "user" 'user_ru',
    password 'Тест123!'
);
ATTACH '' AS mssql_secret (TYPE mssql, SECRET mssql_ru);

-- All three should yield connections that pass SELECT 1.

-- {...} quoting check (FR-012, R6):
ATTACH 'Server=localhost,1433;Database=TestDB;User Id=user_ru;Password={Тест;123!}'
    AS mssql_braces (TYPE mssql);
-- Pre-fix: ParseConnectionString splits on raw ;, gets the wrong password.
-- Post-fix: braces honored, password equals "Тест;123!" (note the semicolon).
```

## 8. Verify SC-008 (no new runtime dependency)

Linux:

```bash
ldd build/release/extension/mssql/mssql.duckdb_extension | wc -l
# Compare to the same number from a main-branch build; should not grow.
```

macOS:

```bash
otool -L build/release/extension/mssql/mssql.duckdb_extension
# No new entries from simdutf.
```

Windows (in a release build):

```powershell
dumpbin /dependents build\release\extension\mssql\mssql.duckdb_extension
# No simdutf*.dll entries.
```

## 9. Build size sanity (SC-009, binary-size half)

```bash
ls -l build/release/extension/mssql/mssql.duckdb_extension
# Should be within +500 KB of the same file from main.
```

## 10. Done

When all the above passes, spec 043 is implemented. Hand off to spec
044 for consumer-side simdutf migration.
