/*
 * mbedtls_prefix.h - Symbol prefixing for loadable extension
 *
 * This header defines macros to prefix all mbedTLS function names with "mssql_"
 * to avoid symbol conflicts with DuckDB's bundled mbedTLS when using the loadable extension.
 *
 * Force-include this header BEFORE any mbedTLS headers when building for the loadable extension.
 */

#ifndef MSSQL_MBEDTLS_PREFIX_H
#define MSSQL_MBEDTLS_PREFIX_H

/* Only apply prefixes if MSSQL_PREFIX_MBEDTLS is defined */
#ifdef MSSQL_PREFIX_MBEDTLS

/* SSL/TLS functions */
#define mbedtls_ssl_init mssql_mbedtls_ssl_init
#define mbedtls_ssl_free mssql_mbedtls_ssl_free
#define mbedtls_ssl_setup mssql_mbedtls_ssl_setup
#define mbedtls_ssl_set_bio mssql_mbedtls_ssl_set_bio
#define mbedtls_ssl_handshake mssql_mbedtls_ssl_handshake
#define mbedtls_ssl_read mssql_mbedtls_ssl_read
#define mbedtls_ssl_write mssql_mbedtls_ssl_write
#define mbedtls_ssl_close_notify mssql_mbedtls_ssl_close_notify
#define mbedtls_ssl_get_ciphersuite mssql_mbedtls_ssl_get_ciphersuite
#define mbedtls_ssl_get_version mssql_mbedtls_ssl_get_version

/* SSL config functions */
#define mbedtls_ssl_config_init mssql_mbedtls_ssl_config_init
#define mbedtls_ssl_config_free mssql_mbedtls_ssl_config_free
#define mbedtls_ssl_config_defaults mssql_mbedtls_ssl_config_defaults
#define mbedtls_ssl_conf_rng mssql_mbedtls_ssl_conf_rng
#define mbedtls_ssl_conf_authmode mssql_mbedtls_ssl_conf_authmode
#define mbedtls_ssl_conf_max_tls_version mssql_mbedtls_ssl_conf_max_tls_version

/* Entropy and RNG functions */
#define mbedtls_entropy_init mssql_mbedtls_entropy_init
#define mbedtls_entropy_free mssql_mbedtls_entropy_free
#define mbedtls_entropy_func mssql_mbedtls_entropy_func
#define mbedtls_ctr_drbg_init mssql_mbedtls_ctr_drbg_init
#define mbedtls_ctr_drbg_free mssql_mbedtls_ctr_drbg_free
#define mbedtls_ctr_drbg_seed mssql_mbedtls_ctr_drbg_seed
#define mbedtls_ctr_drbg_random mssql_mbedtls_ctr_drbg_random

/* Net functions */
#define mbedtls_net_init mssql_mbedtls_net_init
#define mbedtls_net_free mssql_mbedtls_net_free

/* Error functions */
#define mbedtls_strerror mssql_mbedtls_strerror

#endif /* MSSQL_PREFIX_MBEDTLS */

#endif /* MSSQL_MBEDTLS_PREFIX_H */
