# Quickstart: Skip Tests in Community Extensions CI

## Overview

This feature configures the MSSQL extension to skip tests in the DuckDB community-extensions CI environment, where SQL Server is not available.

## Implementation

### Step 1: Update description.yml

Add the `test_config` field to the extension section in `description.yml`:

```yaml
extension:
  name: mssql
  description: "Connect DuckDB to Microsoft SQL Server via native TDS (including TLS)."
  version: "0.1.0"
  language: "C++"
  build: "cmake"
  licence: "MIT"
  maintainers:
    - name: "Vladimir Gribanov"
      github: "VGSML"
  excluded_platforms:
    - "osx_amd64"
    - "windows_arm64"
  test_config: '{"test_env_variables": {"SKIP_TESTS": "1"}}'
```

### Step 2: Update community-extensions PR

After updating `description.yml` in the main mssql-extension repository:

1. Create a new tag for the release (e.g., `v0.1.1`)
2. Update the `ref` in the community-extensions PR to point to the new tag
3. Re-run the CI workflow

## How It Works

1. The community-extensions CI reads `test_config` from `description.yml`
2. The JSON is parsed and `SKIP_TESTS=1` is exported as an environment variable
3. The extension-ci-tools Makefile checks `SKIP_TESTS` and skips all test execution
4. The CI completes successfully without running any tests

## Local Development

This change does NOT affect local development:

- Running `make test` locally does NOT read from `description.yml`
- Integration tests continue to work with `make integration-test` when SQL Server is available
- The `SKIP_TESTS` environment variable is only set by the community-extensions CI

## Verification

After the changes are deployed:

1. The community-extensions CI should complete successfully
2. The test step should output: "Tests are skipped in this run..."
3. No test failures should be reported
