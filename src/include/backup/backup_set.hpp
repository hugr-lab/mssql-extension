//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// backup/backup_set.hpp
//
// Spec 085 task 3: what one family file says about its backup set, from its MTF blocks -- TAPE
// (and its RAID stream), SSET, VOLB, and the trailer MQCI of MSLS (SCIN, SFGI, SFIN records) --
// at the offsets spec 085 § Rules (header fields) located against RESTORE HEADERONLY and
// FILELISTONLY (2025). The page store, not this, reads the boot page. DuckDB-free (D15).
//===----------------------------------------------------------------------===//

#pragma once

#include "backup/backup_mtf.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

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

//! A GUID in SQL Server's byte order (the first three groups little-endian).
struct Guid {
	std::array<uint8_t, 16> bytes{};

	bool IsZero() const;
	//! The lowercase canonical text, as HEADERONLY prints it.
	std::string ToString() const;
};

//! An MTF_DATE_TIME: 5 bytes, big-endian bit fields (14 year, 4 month, 5 day, 5 hour, 6 minute,
//! 6 second), in the server's local time. The fields are not validated here (a month of 13 is
//! kept as read): whatever converts one to a TIMESTAMP checks them and refuses by name.
struct MtfDate {
	int year = 0;
	int month = 0;
	int day = 0;
	int hour = 0;
	int minute = 0;
	int second = 0;

	bool IsZero() const {
		return year == 0 && month == 0 && day == 0;
	}
	std::string ToString() const;
};

enum class BackupKind : uint8_t { Full, Differential, Log };
enum class RecoveryModel : uint8_t { Full, BulkLogged, Simple };

const char *BackupKindName(BackupKind kind);
const char *RecoveryModelName(RecoveryModel model);

//! One file of the database (FILELISTONLY's row), from the trailer's SFIN / SFGI records.
struct DatabaseFile {
	std::string logical_name;
	std::string physical_name;
	//! 'D' data, 'L' log.
	char type = 'D';
	uint32_t file_id = 0;
	uint32_t filegroup_id = 0;
	//! Empty for the log.
	std::string filegroup_name;
	uint64_t size_bytes = 0;
	uint64_t max_size_bytes = 0;
	Lsn create_lsn;
	Guid unique_id;
};

struct BackupSetHeader {
	// TAPE and its RAID stream: the media set and this file's place in it. A compressed
	// container's data set has no TAPE: then media_known is false and these are not read (where
	// a container keeps them is not located yet).
	bool media_known = false;
	uint32_t media_family_id = 0;
	Guid media_set_guid;
	uint32_t family_count = 0;
	uint32_t family_sequence_number = 0;
	uint32_t soft_filemark_bytes = 0;

	// SSET.
	uint32_t sset_attributes = 0;
	BackupKind kind = BackupKind::Full;
	bool is_copy_only = false;
	uint16_t position = 0;
	std::string backup_name;
	std::string description;
	std::string user_name;
	uint8_t software_major = 0;
	uint8_t software_minor = 0;
	Guid backup_set_guid;

	// VOLB.
	std::string machine_name;

	// The trailer SCIN (MSLS's MQCI).
	//! HEADERONLY's DatabaseName and ServerName: string descriptors at SCIN +104 / +108 (task 9; 8
	//! of 8 against HEADERONLY, and in every sample).
	std::string database_name;
	std::string server_name;
	//! More of the trailer SCIN (task 9, spec 085 § Rules): the server's version (+24: major << 24,
	//! minor << 16, build), the database's version (+94), its compatibility level (+91), its collation
	//! in TDS's form (+216: LCID, flags, version; +220: SortId), and the BackupSize the trailer records
	//! (+96, the set's; 0 when not recorded). 0 where a field is absent.
	uint32_t server_version = 0;
	uint16_t database_version = 0;
	uint8_t compatibility_level = 0;
	uint32_t collation_info = 0;
	uint8_t collation_sort_id = 0;
	uint64_t recorded_backup_size = 0;
	Lsn first_lsn;
	Lsn last_lsn;
	Lsn checkpoint_lsn;
	Lsn database_backup_lsn;
	MtfDate database_creation_date;
	MtfDate backup_start_date;
	MtfDate backup_finish_date;
	Guid binding_id;
	Guid recovery_fork_id;
	//! The fork point and the parent fork, set once the database was restored WITH RECOVERY.
	Lsn fork_point_lsn;
	Guid parent_fork_id;
	Guid family_guid;
	RecoveryModel recovery_model = RecoveryModel::Full;
	//! A differential's base; zero for any other backup.
	Lsn differential_base_lsn;
	Guid differential_base_guid;
	std::vector<DatabaseFile> files;

	//! This file's share of HEADERONLY's BackupSize: the MSLS block's offset less one soft filemark.
	uint64_t backup_size = 0;
	//! The bytes of the log tail (the MQTL streams' data).
	uint64_t log_tail_bytes = 0;
	//! HEADERONLY's CompressedBackupSize: a u64 at trailer SCIN +344 (task 9: on every truth,
	//! compressed and plain, striped included), the whole set's value in every family; for a plain
	//! backup it equals the BackupSize. Unknown on a SCIN too short to hold it.
	uint64_t compressed_backup_size = 0;
	bool has_compressed_backup_size = false;

	//! HEADERONLY's FirstRecoveryForkID: the parent fork for a log backup that spans the fork point,
	//! else the current fork.
	const Guid &FirstRecoveryForkId() const;
	//! HEADERONLY's ForkPointLSN: set only for a log backup that spans the fork point.
	bool HasForkPoint() const;
};

//! The backup set described by one family file's blocks. Refuses by name a file whose blocks or
//! records are not where the format puts them (no SSET, no trailer MQCI, a SCIN too short).
BackupSetHeader ReadBackupSet(const RandomAccess &file, const MtfLayout &layout);
//! The media fields (TAPE and its RAID stream) of `layout`'s TAPE block into `set`: a plain
//! family's own, or a compressed container's from its header (WalkMtfTape over ReadContainerTape).
void ReadMediaFields(const RandomAccess &file, const MtfLayout &layout, BackupSetHeader &set);

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb
