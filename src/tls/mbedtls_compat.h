/*
 * mbedtls_compat.h - Compatibility definitions for vcpkg mbedTLS with DuckDB
 *
 * This header provides definitions that are missing when DuckDB's stripped-down
 * mbedtls_config.h is found before vcpkg's full version due to include path ordering.
 * Force-include this header before any other includes to ensure these macros are defined.
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
 * This typedef is in platform_util.h but may not be available if
 * DuckDB's limited config is used.
 */
#ifndef MBEDTLS_F_RNG_T_DEFINED
#define MBEDTLS_F_RNG_T_DEFINED
typedef int mbedtls_f_rng_t(void *p_rng, unsigned char *output, size_t output_size);
#endif

#endif /* MSSQL_MBEDTLS_COMPAT_H */
