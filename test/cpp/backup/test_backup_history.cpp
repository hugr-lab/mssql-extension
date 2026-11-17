// Spec 085 task 24b (PR 4c-2, DQ-48): BuildHistory over synthetic headers -- the cases the committed
// samples do not hold: a restore between two states (history_break), a differential whose full is not
// among the files, a stripe missing a family, files of two databases, no full at all, and the order of
// states finished within one second (by checkpoint LSN).

#include <functional>
#include <iostream>
#include <string>
#include <vector>

#include "backup/backup_bytes.hpp"
#include "backup/backup_history.hpp"

using namespace duckdb::mssql::backup;

namespace {

int failures = 0;

void Check(bool ok, const std::string &what) {
	std::cout << (ok ? "  ok   " : "  FAIL ") << what << std::endl;
	if (!ok) {
		failures++;
	}
}

std::string ErrorOf(const std::function<void()> &f) {
	try {
		f();
	} catch (std::exception &e) {
		return e.what();
	}
	return std::string();
}

Guid G(uint8_t n) {
	Guid g;
	g.bytes[0] = n;
	return g;
}

MtfDate At(int minute, int second = 0) {
	MtfDate d;
	d.year = 2026;
	d.month = 11;
	d.day = 17;
	d.hour = 10;
	d.minute = minute;
	d.second = second;
	return d;
}

HistoryFamily Backup(const std::string &path, BackupKind kind, uint8_t set, MtfDate finish, uint32_t lsn_block,
					 uint8_t base = 0, uint8_t fork = 1, uint8_t database = 7, uint32_t family = 1,
					 uint32_t families = 1) {
	HistoryFamily f;
	f.path = path;
	f.set.kind = kind;
	f.set.backup_set_guid = G(set);
	f.set.differential_base_guid = G(base);
	f.set.recovery_fork_id = G(fork);
	f.set.family_guid = G(database);
	f.set.database_name = database == 7 ? "sales" : "db" + std::to_string(database);
	f.set.backup_finish_date = finish;
	f.set.checkpoint_lsn.vlf = 1;
	f.set.checkpoint_lsn.block = lsn_block;
	f.set.family_sequence_number = family;
	f.set.family_count = families;
	return f;
}

}  // namespace

int main() {
	std::cout << "backup history (spec 085 task 24b)" << std::endl;

	{
		// Two days: a full a day and differentials over each; a log; a stripe given whole; a diff whose
		// full is gone; one second shared by two states.
		auto h = BuildHistory({
			Backup("d2_1400.bak", BackupKind::Differential, 22, At(40), 9, 20),
			Backup("full1.bak", BackupKind::Full, 10, At(0), 1),
			Backup("d1_1100.bak", BackupKind::Differential, 11, At(10), 2, 10),
			Backup("log.trn", BackupKind::Log, 12, At(15), 3),
			Backup("full2_b.bak", BackupKind::Full, 20, At(30), 5, 0, 1, 7, 2, 2),
			Backup("full2_a.bak", BackupKind::Full, 20, At(30), 5, 0, 1, 7, 1, 2),
			Backup("d2_1300.bak", BackupKind::Differential, 21, At(30), 6, 20),
			Backup("orphan.bak", BackupKind::Differential, 30, At(50), 10, 99),
		});
		Check(h.versions.size() == 5, "five versions: two fulls, three differentials over them");
		std::vector<std::string> last;
		for (const auto &v : h.versions) {
			last.push_back(h.Last(v).paths.front());
		}
		Check(
			last == std::vector<std::string>({"full1.bak", "d1_1100.bak", "full2_a.bak", "d2_1300.bak", "d2_1400.bak"}),
			"ordered by finish, a full before its differential finished in the same second (LSN)");
		Check(h.versions[2].number == 3 &&
				  h.backups[h.versions[2].full].paths == std::vector<std::string>({"full2_a.bak", "full2_b.bak"}),
			  "a stripe's families by sequence number");
		Check(h.versions[4].IsDifferential() && h.backups[h.versions[4].full].paths.front() == "full2_a.bak",
			  "a differential over its own full");
		Check(h.skipped.size() == 2, "the log and the differential with no full left out");
		Check(!h.versions[1].history_break, "one fork: no break");
		Check(h.Version(0) == nullptr && h.Version(6) == nullptr && h.Version(5) == &h.versions[4], "versions 1 to 5");
		Check(h.FinishedBy(At(30)) == &h.versions[3], "finished by 10:30: the last of that second");
		Check(h.FinishedBy(At(29, 59)) == &h.versions[1], "finished by 10:29:59: the differential of day one");
		Check(h.FinishedBy(At(0, 0)) == &h.versions[0] && h.FinishedBy(MtfDate()) == nullptr,
			  "nothing finished before the first");
	}
	{
		auto h = BuildHistory({Backup("full1.bak", BackupKind::Full, 10, At(0), 1),
							   Backup("full2.bak", BackupKind::Full, 20, At(30), 5, 0, 2)});
		Check(h.versions[1].history_break, "a full on another recovery fork: the history breaks there");
	}
	{
		// Recreated under its name: another family GUID, one database name.
		auto other = Backup("full2.bak", BackupKind::Full, 20, At(30), 5, 0, 1, 8);
		other.set.database_name = "sales";
		auto h = BuildHistory({Backup("full1.bak", BackupKind::Full, 10, At(0), 1), other});
		Check(h.versions.size() == 2 && h.versions[1].history_break,
			  "the database recreated under its name: one history, broken there");
	}
	{
		// Two differentials in one second, given in the reverse order: the LSN decides.
		auto h = BuildHistory({Backup("full1.bak", BackupKind::Full, 10, At(0), 1),
							   Backup("d_late.bak", BackupKind::Differential, 12, At(10), 9, 10),
							   Backup("d_early.bak", BackupKind::Differential, 11, At(10), 4, 10)});
		Check(
			h.Last(h.versions[1]).paths.front() == "d_early.bak" && h.Last(h.versions[2]).paths.front() == "d_late.bak",
			"states within one second ordered by checkpoint LSN");
	}
	{
		// A copy of a file beside it: read once, the copy named.
		auto h = BuildHistory({Backup("full_copy.bak", BackupKind::Full, 10, At(0), 1),
							   Backup("full.bak", BackupKind::Full, 10, At(0), 1),
							   Backup("diff.bak", BackupKind::Differential, 11, At(10), 2, 10)});
		Check(h.versions.size() == 2 && h.backups[h.versions[0].full].paths == std::vector<std::string>({"full.bak"}),
			  "a copy of a backup is read once");
		Check(h.skipped.size() == 1 && h.skipped[0].path == "full_copy.bak" &&
				  h.skipped[0].reason.find("a copy of \"full.bak\"") != std::string::npos,
			  "the copy left out, named");
	}
	{
		// Families 1 and 3 of a stripe of 2.
		auto h = BuildHistory({Backup("full1.bak", BackupKind::Full, 10, At(0), 1),
							   Backup("s_1.bak", BackupKind::Full, 20, At(30), 5, 0, 1, 7, 1, 2),
							   Backup("s_3.bak", BackupKind::Full, 20, At(30), 5, 0, 1, 7, 3, 2)});
		Check(h.versions.size() == 1 && h.skipped.size() == 1, "a stripe whose families are not 1..n left out");
	}
	{
		auto h = BuildHistory({Backup("full1.bak", BackupKind::Full, 10, At(0), 1),
							   Backup("half_1.bak", BackupKind::Full, 20, At(30), 5, 0, 1, 7, 1, 2)});
		Check(h.versions.size() == 1 && h.skipped.size() == 1 &&
				  h.skipped[0].reason.find("2 files, 1 of them") != std::string::npos,
			  "a stripe missing a family left out, named");
	}
	Check(ErrorOf([] {
			  BuildHistory({Backup("a.bak", BackupKind::Full, 10, At(0), 1),
							Backup("b.bak", BackupKind::Full, 20, At(1), 2, 0, 1, 8)});
		  }).find("different databases") != std::string::npos,
		  "files of two databases refused, named");
	Check(ErrorOf([] { BuildHistory({Backup("log.trn", BackupKind::Log, 12, At(15), 3)}); }).find("no full backup") !=
			  std::string::npos,
		  "no full among the files refused");

	if (failures) {
		std::cout << failures << " FAILED" << std::endl;
		return 1;
	}
	std::cout << "all passed" << std::endl;
	return 0;
}
