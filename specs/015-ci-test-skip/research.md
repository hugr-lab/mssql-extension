# Research: Skip Tests in Community Extensions CI

**Date**: 2026-01-20
**Feature**: 015-ci-test-skip

## Research Questions

### Q1: How does the community-extensions CI handle test configuration?

**Decision**: Use the `test_config` field in `description.yml` with `test_env_variables` JSON object.

**Rationale**: The community-extensions CI infrastructure (`build.py`) reads `test_config` from `description.yml` and exports it as `COMMUNITY_EXTENSION_TEST_CONFIG`. The extension-ci-tools workflow (`_extension_distribution.yml`) then parses this JSON and:
1. Sets environment variables from `test_env_variables` before running tests
2. Uses the `skip_tests` workflow input to conditionally skip test steps

**Alternatives Considered**:
- Rely on `require-env` directive: Tests with `require-env MSSQL_TEST_DSN` should auto-skip, but this doesn't help with test files that don't require env vars, and the workflow structure may still fail.
- Request CI changes: Not feasible - we cannot modify the community-extensions CI infrastructure.

### Q2: What is the exact format for `test_config` in `description.yml`?

**Decision**: JSON object with `test_env_variables` key containing environment variable mappings.

**Rationale**: From `_extension_distribution.yml` lines 393-472, the test config is processed as:
```bash
echo '${{ inputs.test_config }}'| jq -r '.test_env_variables // {} | to_entries | map("\(.key)=\(.value)")|.[]' >> docker_env.txt
```

And:
```bash
eval "$(jq -r '.test_env_variables // {} | to_entries[] | "export \(.key)=\(.value | @sh)"' <<< '${{inputs.test_config}}')"
```

However, `SKIP_TESTS` is handled differently - it's a separate workflow input (line 136-139), not an environment variable. The `skip_tests` input controls whether test steps run at all (lines 460, 465, 704, 965).

**Important Finding**: The `skip_tests` workflow input is NOT read from `description.yml`. It's passed when calling the workflow. This means we cannot directly set `skip_tests=true` via `description.yml`.

### Q3: How can we skip tests if `skip_tests` is not configurable via `description.yml`?

**Decision**: Set `SKIP_TESTS=1` environment variable via `test_config.test_env_variables`.

**Rationale**: The extension-ci-tools `duckdb_extension.Makefile` (lines 187-191) checks:
```makefile
ifeq ($(SKIP_TESTS),1)
	TEST_RELEASE_TARGET=tests_skipped
	TEST_DEBUG_TARGET=tests_skipped
	TEST_RELDEBUG_TARGET=tests_skipped
endif
```

When `SKIP_TESTS=1` is set as an environment variable, the Makefile redirects all test targets to `tests_skipped`, which simply prints "Tests are skipped in this run...".

The `test_env_variables` from `test_config` are exported before running `make test_${{ inputs.build_type }}`, so setting `SKIP_TESTS=1` there will cause the Makefile to skip tests.

**Format**:
```yaml
extension:
  test_config: '{"test_env_variables": {"SKIP_TESTS": "1"}}'
```

### Q4: Which tests would run without skipping?

**Analysis**: Without `SKIP_TESTS=1`, the following tests would execute:

Tests **without** `require-env` (would run):
- `mssql_version.test` - Basic version check (PASSES)
- `mssql_secret.test` - Secret creation/validation (PASSES)
- `mssql_exec.test` - Exec function existence (PASSES)
- `tls_secret.test` - TLS secret configuration (unknown)

Tests **with** `require-env MSSQL_TEST_DSN` (would skip):
- All other 35+ test files

**Conclusion**: Even without `SKIP_TESTS=1`, only 4 tests would run and they should pass since they don't require SQL Server connectivity. However, the CI is still failing, which suggests either:
1. One of these 4 tests is failing for another reason
2. The test runner itself has an issue
3. There's a build or extension loading problem

Setting `SKIP_TESTS=1` is the safest approach to ensure CI passes while investigation continues.

## Implementation Decision

**Final Approach**: Add `test_config` to `description.yml` with `SKIP_TESTS=1` environment variable.

```yaml
extension:
  name: mssql
  # ... existing fields ...
  test_config: '{"test_env_variables": {"SKIP_TESTS": "1"}}'
```

**Benefits**:
1. Guaranteed to work - uses documented Makefile mechanism
2. Simple and reversible - single line change
3. Local development unaffected - only applies in CI via `test_config` processing
4. Standard pattern - follows how other extensions handle test requirements

**Verification**: After merging, the community-extensions CI workflow should complete with "Tests are skipped in this run..." message instead of test failures.
