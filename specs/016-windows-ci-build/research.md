# Research: Windows CI Build Support

**Date**: 2026-01-21
**Feature**: 016-windows-ci-build

## Research Questions

### Q1: What is causing the "unknown override specifier" errors on Windows?

**Decision**: The errors are caused by missing `ssize_t` type definition in header files.

**Rationale**:
- `ssize_t` is a POSIX type defined in `<sys/types.h>` on Unix systems
- Windows MSVC does not define `ssize_t` by default
- When MSVC encounters `ssize_t` in a function return type, it fails to parse the declaration
- This cascades into "unknown override specifier" errors because the parser gets confused

**Evidence from code**:
- `src/include/tds/tds_socket.hpp:56` - `ssize_t Receive(...)`
- `src/include/tds/tds_connection.hpp:68` - `ssize_t ReceiveData(...)`
- `src/include/tds/tls/tds_tls_context.hpp:84,88` - `ssize_t Send(...)`, `ssize_t Receive(...)`
- `src/include/tds/tls/tds_tls_impl.hpp:65,68` - `ssize_t Send(...)`, `ssize_t Receive(...)`

**Solution**: Add Windows-specific typedef for `ssize_t` in a common header:
```cpp
#ifdef _WIN32
#include <BaseTsd.h>
typedef SSIZE_T ssize_t;
#endif
```

### Q2: How does community-extensions CI configure Windows builds?

**Decision**: Community-extensions uses TWO Windows build configurations - MSVC and MinGW.

**Rationale**: From analysis of `extension-ci-tools/config/distribution_matrix.json`:

| Build | Runner | vcpkg Triplet | Compiler |
|-------|--------|---------------|----------|
| windows_amd64 | windows-latest | x64-windows-static-release | MSVC (VS2022) |
| windows_amd64_mingw | windows-latest | x64-mingw-static | MinGW (Rtools 4.2) |

**MSVC Configuration**:
- Uses `vcvars64.bat` from VS2022 Enterprise
- Static linking via vcpkg
- No special compiler flags needed beyond vcpkg toolchain

**MinGW Configuration**:
- Uses Rtools version 4.2 (version 43 has linker bugs)
- Compiler binaries copied to vcpkg-compatible names:
  - `gcc.exe` → `x86_64-w64-mingw32-gcc.exe`
  - `g++.exe` → `x86_64-w64-mingw32-g++.exe`
- Environment variables: `CC=gcc`, `CXX=g++`

### Q3: What vcpkg triplets are required for Windows builds?

**Decision**: Use `x64-windows-static-release` for MSVC and `x64-mingw-static` for MinGW.

**Rationale**: These are the exact triplets used by community-extensions CI, ensuring binary compatibility.

**vcpkg.json modifications**: None required - vcpkg automatically selects dependencies based on triplet.

### Q4: Are there additional Windows-specific code fixes needed?

**Decision**: The `ssize_t` fix is the primary requirement. Socket code already has Windows handling.

**Rationale**: Analysis of existing code shows:

1. **Socket includes** - Already handled in `tds_socket.cpp`:
   ```cpp
   #ifdef _WIN32
   #include <winsock2.h>
   #include <ws2tcpip.h>
   #pragma comment(lib, "ws2_32.lib")
   #define CLOSE_SOCKET closesocket
   // ... other Windows-specific macros
   #endif
   ```

2. **Socket type** - Uses `int fd_` which works on Windows (SOCKET is typedef'd to UINT_PTR, but most WinSock functions accept int)

3. **TLS code** - Already has Windows handling in `tds_tls_impl.cpp`

**Potential additional fixes**:
- Signed/unsigned warnings (noted in CI output but non-blocking)
- May need to verify WinSock initialization (`WSAStartup`) is called

### Q5: How should the local CI workflow be structured?

**Decision**: Add Windows build jobs to `.github/workflows/ci.yml` with manual trigger support.

**Rationale**: Match community-extensions structure while allowing PR validation:

1. **Workflow trigger**: Add `pull_request` to workflow triggers with manual dispatch option
2. **Matrix strategy**: Include both MSVC and MinGW configurations
3. **Caching**: Cache vcpkg packages per triplet
4. **Artifact upload**: Upload built extensions for verification

**Workflow structure**:
```yaml
jobs:
  build-windows:
    strategy:
      matrix:
        include:
          - name: MSVC
            triplet: x64-windows-static-release
            compiler: msvc
          - name: MinGW
            triplet: x64-mingw-static
            compiler: mingw
```

### Q6: What is the MinGW/Rtools setup process?

**Decision**: Use `r-lib/actions/setup-r@v2` with Rtools 4.2.

**Rationale**: From community-extensions workflow:

```yaml
- uses: r-lib/actions/setup-r@v2
  with:
    r-version: 'devel'
    update-rtools: true
    rtools-version: '42'  # version 43 has linker bug

- name: Setup rtools gcc for vcpkg
  run: |
    cp C:/rtools42/x86_64-w64-mingw32.static.posix/bin/gcc.exe \
       C:/rtools42/x86_64-w64-mingw32.static.posix/bin/x86_64-w64-mingw32-gcc.exe
    # ... similar for g++ and gfortran
```

**Important notes**:
- Rtools 4.2 is required (4.3 has linker bugs)
- Compiler binaries must be copied with triple-prefixed names for vcpkg compatibility
- `PATH` must include Rtools bin directory

## Implementation Decision

### Phase 1: Source Code Fixes (for both MSVC and MinGW)

1. **Add Windows type compatibility header** (`src/include/tds/tds_platform.hpp`):
   ```cpp
   #pragma once

   #ifdef _WIN32
   #include <BaseTsd.h>
   typedef SSIZE_T ssize_t;
   #endif
   ```

2. **Include compatibility header** in all files using `ssize_t`:
   - `tds_socket.hpp`
   - `tds_connection.hpp`
   - `tds_tls_context.hpp`
   - `tds_tls_impl.hpp`

### Phase 2: CI Workflow Updates

1. **Add Windows build jobs** to `ci.yml`:
   - MSVC job using VS2022
   - MinGW job using Rtools 4.2

2. **Configure vcpkg caching** per triplet

3. **Add smoke test step** to verify extension loads

### Verification

After implementation:
1. Push changes to branch
2. Trigger local CI workflow manually
3. Verify both MSVC and MinGW builds pass
4. Update community-extensions PR with new tag
5. Verify community-extensions CI passes
