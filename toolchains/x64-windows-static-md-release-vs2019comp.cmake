# Custom vcpkg triplet for MSVC: static libraries + static CRT (/MT) + VS2019 compatibility
# Must use VCPKG_CRT_LINKAGE static to match DuckDB's CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded" (/MT)
# The _DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR flag ensures VS2019 ABI compatibility
set(VCPKG_CXX_FLAGS "/D_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR")
set(VCPKG_C_FLAGS "")

set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE static)
set(VCPKG_LIBRARY_LINKAGE static)

set(VCPKG_BUILD_TYPE release)
