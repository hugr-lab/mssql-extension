# Matches community-extensions CI triplet (from extension-ci-tools v1.4-andium)
# Static libraries + dynamic CRT (/MD) + VS2019 compatibility
set(VCPKG_CXX_FLAGS "/D_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR")
set(VCPKG_C_FLAGS "")

set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)

set(VCPKG_BUILD_TYPE release)
