// POSIX and deprecated CRT compatibility wrappers for MSVC
//
// When building with vcpkg's x64-windows-static-md triplet (dynamic CRT /MD),
// static libcurl and OpenSSL reference POSIX functions (read, write, close, etc.)
// and deprecated CRT functions (strncpy, wcsncpy) as DLL imports (__imp_read,
// __imp_strncpy, ...). The MSVC UCRT provides underscore-prefixed POSIX versions
// (_read, _write, _close) and secure alternatives (strncpy_s, wcsncpy_s), but
// the OLDNAMES.lib import stubs don't create __imp_ thunks needed for DLL linking.
//
// These wrappers define the missing symbols so the linker can resolve them when
// building the loadable extension (.duckdb_extension = DLL).

#ifdef _MSC_VER

// Suppress deprecation warnings for POSIX names
#define _CRT_NONSTDC_NO_DEPRECATE
#define _CRT_SECURE_NO_WARNINGS

#include <io.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

// Prevent CRT headers from inlining these functions
#pragma function(strncpy)
#pragma function(wcsncpy)

extern "C" {

// POSIX I/O functions (libcurl: warnless.c, fopen.c, file.c)
int read(int fd, void *buf, unsigned int count) {
	return _read(fd, buf, count);
}

int write(int fd, const void *buf, unsigned int count) {
	return _write(fd, buf, count);
}

int close(int fd) {
	return _close(fd);
}

int unlink(const char *path) {
	return _unlink(path);
}

FILE *fdopen(int fd, const char *mode) {
	return _fdopen(fd, mode);
}

int fileno(FILE *f) {
	return _fileno(f);
}

// Deprecated CRT functions (OpenSSL: evp_key.c; libcurl: curl_multibyte.c, curl_sspi.c)
// These are standard C functions but MSVC may not export them from ucrtbase.dll
// under certain configurations. Providing explicit definitions ensures linkability.
char *strncpy(char *dest, const char *src, size_t count) {
	size_t i;
	for (i = 0; i < count && src[i] != '\0'; i++) {
		dest[i] = src[i];
	}
	for (; i < count; i++) {
		dest[i] = '\0';
	}
	return dest;
}

wchar_t *wcsncpy(wchar_t *dest, const wchar_t *src, size_t count) {
	size_t i;
	for (i = 0; i < count && src[i] != L'\0'; i++) {
		dest[i] = src[i];
	}
	for (; i < count; i++) {
		dest[i] = L'\0';
	}
	return dest;
}

}  // extern "C"

#endif  // _MSC_VER
