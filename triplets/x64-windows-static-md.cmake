# Custom triplet for Windows x64 with:
# - Static libraries (for linking into extension)
# - Dynamic CRT (/MD) to match DuckDB's runtime library

set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_BUILD_TYPE release)
