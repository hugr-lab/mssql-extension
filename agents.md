# Reusable Subagent Definitions

Predefined agent configurations for common development tasks. Launch via the Task tool.

## 1. Build

Build the extension using Ninja generator.

```yaml
subagent_type: Bash
description: Build the extension
prompt: |
  Build the mssql-extension with Ninja generator. Run:
    cd /Users/vgribanov/projects/hugr-lab/mssql-extension && GEN=ninja make release
  This uses CMake with -G "Ninja" via the extension-ci-tools Makefile.
  Build outputs:
    - DuckDB CLI: build/release/duckdb
    - Static extension: build/release/extension/mssql/libmssql_extension.a
    - Loadable extension: build/release/extension/mssql/mssql.duckdb_extension
  Report: success/failure, number of targets built, and any errors.
```

### Debug Build Variant

```yaml
subagent_type: Bash
description: Build debug extension
prompt: |
  Build the mssql-extension in debug mode with Ninja generator. Run:
    cd /Users/vgribanov/projects/hugr-lab/mssql-extension && GEN=ninja make debug
  Report: success/failure, number of targets built, and any errors.
```

## 2. Test

Run tests with SQL Server. Starts Docker container if needed.

Test environment uses these variables (auto-exported by Makefile):
- `MSSQL_TEST_DSN` — ADO.NET connection string for master
- `MSSQL_TESTDB_DSN` — ADO.NET connection string for TestDB
- `MSSQL_TEST_URI` / `MSSQL_TESTDB_URI` — URI connection strings
- `MSSQL_TEST_DSN_TLS` — TLS URI (NOT exported by default, TLS tests skipped without it)

Test groups: `[sql]`, `[integration]`, `[mssql]`, `[dml]`, `[transaction]`

```yaml
subagent_type: Bash
description: Run extension tests
prompt: |
  Run the mssql-extension test suite. Steps:
  1. cd /Users/vgribanov/projects/hugr-lab/mssql-extension
  2. Check SQL Server container: make docker-status
  3. If not running/healthy, start it: make docker-up
     (waits up to 120s for healthy state, then runs init scripts)
  4. Run all tests: make test-all
     This runs: build/release/test/unittest "*mssql*" --force-reload
     Environment variables (MSSQL_TEST_DSN, MSSQL_TESTDB_DSN, etc.) are auto-exported.
  5. Report: total tests, passed, failed, skipped, and any failure details.
  NOTE: TLS tests (tls_connection, tls_multipacket, tls_parallel, tls_queries) are
  skipped unless MSSQL_TEST_DSN_TLS is manually exported.
```

### Unit Tests Only (no SQL Server)

```yaml
subagent_type: Bash
description: Run unit tests
prompt: |
  Run unit tests only (no SQL Server required). Run:
    cd /Users/vgribanov/projects/hugr-lab/mssql-extension && make test
  Unit tests are C++ tests in test/cpp/ that test isolated components:
    batch builder, connection pool, DDL translator, insert executor,
    statistics provider, TLS connection, value serializer.
  Report: total tests, passed, failed, and any failure details.
```

### Integration Tests Only

```yaml
subagent_type: Bash
description: Run integration tests
prompt: |
  Run integration tests against SQL Server. Steps:
  1. cd /Users/vgribanov/projects/hugr-lab/mssql-extension
  2. Ensure SQL Server is running: make docker-status || make docker-up
  3. Run: make integration-test
     This runs two test suites sequentially:
       build/release/test/unittest "[integration]" --force-reload
       build/release/test/unittest "[sql]" --force-reload
     All MSSQL_TEST_* env vars are auto-exported by the Makefile.
  4. Report: total tests, passed, failed, skipped, and any failure details.
```

### Run Specific Test Group

```yaml
subagent_type: Bash
description: Run specific test group
prompt: |
  Run a specific test group. Steps:
  1. cd /Users/vgribanov/projects/hugr-lab/mssql-extension
  2. Ensure SQL Server is running: make docker-status
  3. Export env vars and run:
     export MSSQL_TEST_DSN="Server=localhost,1433;Database=master;User Id=sa;Password=TestPassword1"
     export MSSQL_TESTDB_DSN="Server=localhost,1433;Database=TestDB;User Id=sa;Password=TestPassword1"
     build/release/test/unittest "[GROUP]" --force-reload
  Replace [GROUP] with: [sql], [integration], [mssql], [dml], or [transaction].
  For a single file: build/release/test/unittest "test/sql/path/to/file.test" --force-reload
  Report: total tests, passed, failed, skipped, and any failure details.
```

### Debug Test (run SQL with MSSQL_DEBUG)

Run a specific SQL command against the statically linked DuckDB CLI with TDS debug logging enabled.
This agent should be allowed to run by default without additional permission prompts.

```yaml
subagent_type: Bash
description: Debug test SQL command
prompt: |
  Run a debug test with the mssql-extension. Execute the DuckDB CLI with TDS debug logging:
    cd /Users/vgribanov/projects/hugr-lab/mssql-extension && MSSQL_DEBUG=1 ./build/release/duckdb -c "<SQL_COMMAND>"
  The static build at build/release/duckdb has the mssql extension linked in (no LOAD needed).
  Replace <SQL_COMMAND> with the SQL to execute. Example:
    MSSQL_DEBUG=1 ./build/release/duckdb -c "ATTACH 'Server=localhost,1433;Database=TestDB;User Id=sa;Password=TestPassword1' AS db (TYPE mssql); SELECT * FROM db.dbo.TestSimplePK;"
  Debug levels:
    MSSQL_DEBUG=1  Basic TDS protocol debug
    MSSQL_DEBUG=2  Verbose debug
    MSSQL_DEBUG=3  Trace level (all packets)
  Additional debug vars:
    MSSQL_DML_DEBUG=1  DML operation debug (generated SQL, batch sizes, rowid values)
  Report: command output, any debug log lines, errors, and query results.
```

## 3. Architect

Investigate architecture and update documentation.

```yaml
subagent_type: Explore
description: Investigate architecture
prompt: |
  Investigate the mssql-extension architecture in /Users/vgribanov/projects/hugr-lab/mssql-extension.
  Thoroughness: very thorough.

  1. Read all docs in docs/ folder.
  2. Explore source code in src/ to find any undocumented components, classes, or patterns.
  3. Compare documentation against actual code and identify:
     - Missing classes or modules not covered in docs
     - Outdated information (class names, method signatures, file paths)
     - Missing configuration settings or extension functions
     - Architectural patterns not described
  4. Report findings as a structured list of gaps with file references.
```

### Architecture Update Variant

When gaps are known, use this to update docs:

```yaml
subagent_type: general-purpose
description: Update architecture docs
prompt: |
  Update architecture documentation in /Users/vgribanov/projects/hugr-lab/mssql-extension/docs/.

  Read the existing docs and the source code, then update the documentation files
  to reflect the current state of the codebase. Follow the style and structure
  of existing documentation. Do NOT create new files unless a major new component
  has no corresponding doc file.

  Key docs to check:
  - docs/architecture.md (main overview)
  - docs/tds-protocol.md (TDS layer)
  - docs/connection-management.md (pool, provider)
  - docs/catalog-integration.md (catalog, schema, tables)
  - docs/type-mapping.md (type conversion)
  - docs/query-execution.md (scan, DML)
  - docs/transactions.md (transaction management)

  Report what was updated and why.
```
