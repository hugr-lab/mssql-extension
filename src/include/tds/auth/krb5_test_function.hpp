//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// krb5_test_function.hpp
//
// mssql_kerberos_auth_test() scalar function -- exercises the POSIX
// Kerberos authentication path WITHOUT actually connecting to SQL Server.
// Verifies: krb5.conf is reachable, the ccache has a valid TGT, the SPN
// resolves through the KDC, and gss_init_sec_context produces a non-empty
// SPNEGO blob. Same diagnostic surface as the existing mssql_azure_auth_test.
//
// Compiled and registered only when MSSQL_ENABLE_KRB5 is defined. With the
// macro absent, the function returns "Kerberos support not compiled in".
//
// Spec 042.
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {
namespace mssql {
namespace krb5 {

// Register mssql_kerberos_auth_test scalar functions:
//
//   mssql_kerberos_auth_test(host VARCHAR)                    -> VARCHAR
//       Uses port 1433, default ccache, derived SPN MSSQLSvc/<host>:1433.
//
//   mssql_kerberos_auth_test(host VARCHAR, port INTEGER)      -> VARCHAR
//       Explicit port; derived SPN MSSQLSvc/<host>:<port>.
//
//   mssql_kerberos_auth_test_secret(secret_name VARCHAR)      -> VARCHAR
//       Reads host/port/SPN-override/keytab/realm from an MSSQL secret.
//       Exercises the full factory + Krb5Authenticator construction path.
//
// All variants return a one-line status string on success:
//   "OK: principal=<...>, spn=<...>, mech=SPNEGO, token_size=<N> bytes"
// On failure they return the verbatim error message (same wording the
// ATTACH path produces, including the actionable hint from HintForMinor).
void RegisterKrb5TestFunction(ExtensionLoader &loader);

}  // namespace krb5
}  // namespace mssql
}  // namespace duckdb
