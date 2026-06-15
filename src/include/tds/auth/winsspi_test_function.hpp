//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// winsspi_test_function.hpp
//
// Registers mssql_winsspi_auth_test() -- the Windows SSPI peer of
// mssql_kerberos_auth_test() (Phase 3). Exercises the SSPI handshake
// (AcquireCredentialsHandleW + first InitializeSecurityContextW round)
// without connecting to SQL Server, so users can diagnose SPN / ticket
// problems before debugging an ATTACH.
//
// Spec 042 Phase 4.
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {
namespace mssql {
namespace winsspi {

void RegisterWinSspiTestFunction(ExtensionLoader &loader);

}  // namespace winsspi
}  // namespace mssql
}  // namespace duckdb
