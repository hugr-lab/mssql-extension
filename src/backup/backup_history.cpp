//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// backup/backup_history.cpp
//
// Spec 085 task 24b (PR 4c-2, DQ-48): a database's backups ordered into versions.
//===----------------------------------------------------------------------===//

#include "backup/backup_history.hpp"

#include "backup/backup_bytes.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <set>
#include <tuple>

namespace duckdb {
namespace mssql {
namespace backup {

int CompareDates(const MtfDate &a, const MtfDate &b) {
	const auto ta = std::make_tuple(a.year, a.month, a.day, a.hour, a.minute, a.second);
	const auto tb = std::make_tuple(b.year, b.month, b.day, b.hour, b.minute, b.second);
	return ta < tb ? -1 : tb < ta ? 1 : 0;
}

static bool LsnLess(const Lsn &a, const Lsn &b) {
	return std::make_tuple(a.vlf, a.block, a.slot) < std::make_tuple(b.vlf, b.block, b.slot);
}

const HistoryVersion *BackupHistory::Version(uint64_t number) const {
	if (number == 0 || number > versions.size()) {
		return nullptr;
	}
	return &versions[number - 1];
}

const HistoryVersion *BackupHistory::FinishedBy(const MtfDate &when) const {
	const HistoryVersion *found = nullptr;
	for (const auto &v : versions) {
		// A backup whose finish is not recorded is finished by no time (review of #579).
		const auto &finish = Last(v).set.backup_finish_date;
		if (!finish.IsZero() && CompareDates(finish, when) <= 0) {
			found = &v;
		}
	}
	return found;
}

BackupHistory BuildHistory(std::vector<HistoryFamily> families) {
	BackupHistory history;
	history.families = families;
	const std::string first_path = families.empty() ? std::string() : families.front().path;
	// One backup per backup set GUID, its families by sequence number.
	std::map<std::array<uint8_t, 16>, std::vector<HistoryFamily>> by_set;
	std::vector<std::array<uint8_t, 16>> order;	 // the sets in the order their first file came
	for (auto &f : families) {
		auto &group = by_set[f.set.backup_set_guid.bytes];
		if (group.empty()) {
			order.push_back(f.set.backup_set_guid.bytes);
		}
		group.push_back(std::move(f));
	}
	for (const auto &guid : order) {
		auto &all = by_set[guid];
		std::sort(all.begin(), all.end(), [](const HistoryFamily &a, const HistoryFamily &b) {
			return std::make_tuple(a.set.family_sequence_number, a.path) <
				   std::make_tuple(b.set.family_sequence_number, b.path);
		});
		// A copy of a file (an archive keeps both): the same backup set and family, read once.
		std::vector<HistoryFamily> group;
		for (auto &f : all) {
			if (!group.empty() && group.back().set.family_sequence_number == f.set.family_sequence_number) {
				history.skipped.push_back(
					{f.path, "a copy of \"" + group.back().path + "\" (the same backup set and family)"});
				continue;
			}
			group.push_back(std::move(f));
		}
		const auto &first = group.front();
		bool whole = first.set.family_count == group.size();
		for (size_t i = 0; whole && i < group.size(); i++) {
			whole = group[i].set.family_sequence_number == i + 1 &&
					group[i].set.media_set_guid.bytes == first.set.media_set_guid.bytes;
		}
		if (!whole) {
			history.skipped.push_back({first.path, "a backup striped over " + std::to_string(first.set.family_count) +
													   " files, " + std::to_string(group.size()) +
													   " of them among the files"});
			continue;
		}
		if (first.set.kind == BackupKind::Log) {
			history.skipped.push_back({first.path,
									   "a log backup (the history's states are full and differential "
									   "backups)"});
			continue;
		}
		const std::string partial = first.set.NotWholeDatabase();
		if (!partial.empty()) {
			history.skipped.push_back({first.path, partial});
			continue;
		}
		HistoryBackup backup;
		for (const auto &f : group) {
			backup.paths.push_back(f.path);
			backup.sizes.push_back(f.size);
			backup.version_tags.push_back(f.version_tag);
		}
		backup.set = first.set;
		history.backups.push_back(std::move(backup));
	}
	// One database: a file of another is a mistake in the set, not a version. The database recreated
	// under its name (another family GUID) is one history, broken there.
	for (const auto &a : history.backups) {
		for (const auto &b : history.backups) {
			if (!a.set.family_guid.IsZero() && !b.set.family_guid.IsZero() &&
				a.set.family_guid.bytes != b.set.family_guid.bytes && a.set.database_name != b.set.database_name) {
				throw FormatError("backup files \"" + a.paths.front() + "\" and \"" + b.paths.front() +
								  "\" are of different databases (" + a.set.database_name + ", " + b.set.database_name +
								  "): a history is one database's");
			}
		}
	}
	// Each full is a state; a differential is one over its base, when the base is among the files.
	std::map<std::array<uint8_t, 16>, size_t> fulls;
	for (size_t i = 0; i < history.backups.size(); i++) {
		if (history.backups[i].set.kind == BackupKind::Full) {
			fulls[history.backups[i].set.backup_set_guid.bytes] = i;
			HistoryVersion v;
			v.full = i;
			history.versions.push_back(v);
		}
	}
	for (size_t i = 0; i < history.backups.size(); i++) {
		const auto &set = history.backups[i].set;
		if (set.kind != BackupKind::Differential) {
			continue;
		}
		auto base = fulls.find(set.differential_base_guid.bytes);
		if (base == fulls.end()) {
			history.skipped.push_back({history.backups[i].paths.front(),
									   "a differential whose full backup (backup set " +
										   set.differential_base_guid.ToString() + ") is not among the files"});
			continue;
		}
		HistoryVersion v;
		v.full = base->second;
		v.diff = i;
		history.versions.push_back(v);
	}
	if (history.versions.empty()) {
		throw FormatError("backup files from \"" + first_path +
						  "\": no full backup among them, so no state of the database to read");
	}
	std::stable_sort(history.versions.begin(), history.versions.end(),
					 [&](const HistoryVersion &a, const HistoryVersion &b) {
						 const auto &sa = history.Last(a).set;
						 const auto &sb = history.Last(b).set;
						 const int by_date = CompareDates(sa.backup_finish_date, sb.backup_finish_date);
						 if (by_date != 0) {
							 return by_date < 0;
						 }
						 return LsnLess(sa.checkpoint_lsn, sb.checkpoint_lsn);
					 });
	for (size_t i = 0; i < history.versions.size(); i++) {
		auto &v = history.versions[i];
		v.number = i + 1;
		if (i > 0) {
			const auto &previous = history.Last(history.versions[i - 1]).set;
			const auto &now = history.Last(v).set;
			v.history_break = previous.recovery_fork_id.bytes != now.recovery_fork_id.bytes ||
							  previous.family_guid.bytes != now.family_guid.bytes;
		}
	}
	return history;
}

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb
