#pragma once

// =============================================================================
// DuckDB API Compatibility
// =============================================================================
// DuckDB nightly (main branch) renamed PhysicalOperator::GetData to GetDataInternal.
// This header provides macros to conditionally compile against either API.
//
// Build Configuration:
//   -DMSSQL_DUCKDB_API_NIGHTLY=ON  -> Uses GetDataInternal (nightly/main)
//   -DMSSQL_DUCKDB_API_NIGHTLY=OFF -> Uses GetData (stable 1.4.x)
//
// The macro MSSQL_DUCKDB_NIGHTLY is set by CMake when MSSQL_DUCKDB_API_NIGHTLY=ON

#ifdef MSSQL_DUCKDB_NIGHTLY
#define MSSQL_GETDATA_METHOD GetDataInternal
#else
#define MSSQL_GETDATA_METHOD GetData
#endif
