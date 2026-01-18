/*
 * mbedtls_compat.h - Compatibility definitions for vcpkg mbedTLS with DuckDB
 *
 * This header was originally created to provide missing definitions when DuckDB's
 * stripped-down mbedtls_config.h was found before vcpkg's full version. Now that
 * the TLS build filters out DuckDB's mbedtls include path, most of these are no
 * longer needed but are kept for robustness.
 */

#ifndef MSSQL_MBEDTLS_COMPAT_H
#define MSSQL_MBEDTLS_COMPAT_H

#include <stddef.h>
#include <stdint.h>

/* PSA_HASH_MAX_SIZE - Maximum hash size for PSA crypto
 * This value is 64 bytes for SHA-512/SHA3-512 which is the largest supported hash.
 * The vcpkg mbedTLS ssl.h uses this for TLS 1.3 secrets buffers.
 */
#ifndef PSA_HASH_MAX_SIZE
#define PSA_HASH_MAX_SIZE 64u
#endif

/* PSA_MAC_MAX_SIZE - Maximum MAC size */
#ifndef PSA_MAC_MAX_SIZE
#define PSA_MAC_MAX_SIZE PSA_HASH_MAX_SIZE
#endif

/* mbedtls_f_rng_t - Random number generator function type
 * This typedef is in platform_util.h but may not be available in all configs.
 */
#ifndef MBEDTLS_F_RNG_T_DEFINED
#define MBEDTLS_F_RNG_T_DEFINED
typedef int mbedtls_f_rng_t(void *p_rng, unsigned char *output, size_t output_size);
#endif

#endif /* MSSQL_MBEDTLS_COMPAT_H */
