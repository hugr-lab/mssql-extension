# Quickstart: Windows CI Build Support

## Overview

This feature fixes Windows build errors in the DuckDB community-extensions CI and adds local Windows CI testing capability.

## Implementation

### Step 1: Add Windows Platform Compatibility Header

Create `src/include/tds/tds_platform.hpp`:

```cpp
#pragma once

// Windows platform compatibility definitions
#ifdef _WIN32
#include <BaseTsd.h>
// ssize_t is a POSIX type not defined by MSVC
typedef SSIZE_T ssize_t;
#endif
```

### Step 2: Include Platform Header in Affected Files

Add `#include "tds/tds_platform.hpp"` (or appropriate relative path) at the top of:

1. `src/include/tds/tds_socket.hpp`
2. `src/include/tds/tds_connection.hpp`
3. `src/include/tds/tls/tds_tls_context.hpp`
4. `src/include/tds/tls/tds_tls_impl.hpp`

### Step 3: Add Windows Build Jobs to CI Workflow

Add to `.github/workflows/ci.yml`:

```yaml
build-windows:
  name: Build Windows (${{ matrix.config.name }})
  runs-on: windows-latest
  if: github.event_name == 'workflow_dispatch' && inputs.run_build
  strategy:
    fail-fast: false
    matrix:
      config:
        - name: MSVC
          triplet: x64-windows-static-release
          is_mingw: false
        - name: MinGW
          triplet: x64-mingw-static
          is_mingw: true
  # ... rest of job configuration
```

## How It Works

1. **ssize_t Definition**: MSVC doesn't define `ssize_t` (POSIX type). We define it as `SSIZE_T` from `<BaseTsd.h>`.

2. **MSVC Build**: Uses Visual Studio 2022 with vcpkg triplet `x64-windows-static-release`.

3. **MinGW Build**: Uses Rtools 4.2 GCC with vcpkg triplet `x64-mingw-static`.

## Local Testing

### Trigger Windows Build Manually

1. Go to Actions tab in GitHub
2. Select "CI" workflow
3. Click "Run workflow"
4. Check "Run build and tests"
5. Select branch

### Verify Build Output

After CI completes:
1. Check that both MSVC and MinGW jobs pass
2. Download artifacts to verify extension was produced
3. Load extension in DuckDB to verify it works

## Verification

After changes are deployed:

1. Local CI Windows builds should complete successfully
2. Community-extensions CI Windows builds should pass
3. Extension should load on Windows DuckDB:
   ```sql
   LOAD 'mssql.duckdb_extension';
   SELECT mssql_version();
   ```
