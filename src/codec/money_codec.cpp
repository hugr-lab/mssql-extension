//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension — spec 045
//
// codec/money_codec.cpp
//
// Phase-2 stub. Definitions for codec::money::* land during US3
// (Phase 6 sub-phase 4). Money is scan-decode-only by design: only
// DecodeFromTds will be defined; the other 3 ops stay declaration-only.
//===----------------------------------------------------------------------===//

#include "codec/money_codec.hpp"
