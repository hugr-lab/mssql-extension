//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// backup/backup_history.hpp
//
// Spec 085 task 24b (PR 4c-2, DQ-48): the history of a database's backups -- every file of a set read
// for its header only, grouped into backups (a media set's families) and ordered into versions: each
// full backup, and each differential over a full among them, a state of the database at its finish.
// DuckDB-free.
//===----------------------------------------------------------------------===//

#pragma once

#include "backup/backup_set.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace duckdb {
namespace mssql {
namespace backup {

//! One family file as a history reads it: its path and its header.
struct HistoryFamily {
	std::string path;
	//! The file as the history read it (ByteSource's size and version tag): a version opened later is
	//! checked against it.
	uint64_t size = 0;
	std::string version_tag;
	BackupSetHeader set;
};

//! One backup of the history: the families of one backup set, by family sequence number.
struct HistoryBackup {
	std::vector<std::string> paths;
	std::vector<uint64_t> sizes;			// of `paths`
	std::vector<std::string> version_tags;	// of `paths`
	BackupSetHeader set;					// its first family's
};

//! A state of the database: a full backup, or a differential over it.
struct HistoryVersion {
	uint64_t number = 0;  // 1..N, in the order of the states
	size_t full = 0;	  // BackupHistory::backups
	static constexpr size_t NONE = static_cast<size_t>(-1);
	size_t diff = NONE;
	//! The state's recovery fork, or its database (family GUID: recreated under its name), is not the
	//! previous one's: the database was restored or recreated meanwhile, and the two compare only as a
	//! reload (DQ-48). False for the first.
	bool history_break = false;

	bool IsDifferential() const {
		return diff != NONE;
	}
};

//! A file the history leaves out, and why (a log backup, a differential whose full is not among the
//! files, a striped backup missing a family).
struct HistorySkipped {
	std::string path;
	std::string reason;
};

struct BackupHistory {
	std::vector<HistoryBackup> backups;
	std::vector<HistoryVersion> versions;
	std::vector<HistorySkipped> skipped;

	//! The backup a version's state ends with: its differential, else its full.
	const HistoryBackup &Last(const HistoryVersion &version) const {
		return backups[version.IsDifferential() ? version.diff : version.full];
	}
	//! The version numbered `number`, or none.
	const HistoryVersion *Version(uint64_t number) const;
	//! The last version finished at or before `when`, or none.
	const HistoryVersion *FinishedBy(const MtfDate &when) const;
};

//! `families` (each file's header, any order) as a history: grouped by backup set, ordered by finish
//! (then checkpoint LSN), numbered. A second file of one backup set and family (a copy) is left out. A
//! FormatError naming the files when they are of two databases (another name and family GUID; one
//! name with another family GUID is the database recreated: a break) or hold no full backup.
BackupHistory BuildHistory(std::vector<HistoryFamily> families);

//! The order of two dates, year to second: negative, 0, positive.
int CompareDates(const MtfDate &a, const MtfDate &b);

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb
