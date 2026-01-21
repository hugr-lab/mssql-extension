#pragma once

// Windows platform compatibility definitions
// This header provides POSIX types that are not available on Windows MSVC

#ifdef _WIN32
// NOMINMAX prevents Windows.h from defining min/max macros
// which conflict with std::min/std::max
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <BaseTsd.h>
// ssize_t is a POSIX type not defined by MSVC
// SSIZE_T is the Windows equivalent defined in BaseTsd.h
typedef SSIZE_T ssize_t;
#endif
