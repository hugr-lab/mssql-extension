//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// backup/backup_lsn.hpp
//
// Spec 085: a log sequence number as SQL Server stores it -- in a backup set's records and in every
// page header (m_lsn) -- on its own so that both read it. DuckDB-free (D15).
//===----------------------------------------------------------------------===//

#pragma once

#include <cstdint>
#include <string>

namespace duckdb {
namespace mssql {
namespace backup {

//! A log sequence number as SQL Server stores it: u32 VLF, u32 block, u16 slot.
struct Lsn {
	uint32_t vlf = 0;
	uint32_t block = 0;
	uint16_t slot = 0;

	bool IsZero() const {
		return vlf == 0 && block == 0 && slot == 0;
	}
	//! RESTORE HEADERONLY's numeric form: vlf, then block in 10 digits, then slot in 5.
	std::string ToDecimal() const;
	bool operator<=(const Lsn &other) const;
};

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb
