// Spec 085 task 26 (PR 5): the log tail's records (backup_log_tail.hpp) on the committed samples, whose
// expected values are fn_dump_dblog's over the same files (SQL Server 2025, 2026-11-19: every record of
// log.trn, 1628, equal on LSN, operation, context, transaction, page and allocation unit), and the
// blocks the reader refuses. DuckDB-free.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "backup/backup_bytes.hpp"
#include "backup/backup_fuzzy.hpp"
#include "backup/backup_log_tail.hpp"
#include "backup/backup_mtf.hpp"

using namespace duckdb::mssql::backup;

namespace {

int failures = 0;

void Check(bool ok, const std::string &what) {
	std::cout << (ok ? "  ok   " : "  FAIL ") << what << std::endl;
	if (!ok) {
		failures++;
	}
}

const std::string DIR = "test/data/backup/";

std::vector<uint8_t> ReadAll(const std::string &path) {
	std::ifstream in(path, std::ios::binary);
	return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::vector<LogRecord> RecordsOf(const std::string &name) {
	const auto bytes = ReadAll(DIR + name);
	MemoryAccess file(name, bytes.data(), bytes.size());
	const auto layout = WalkMtf(file);
	LogTailReader tail(file, layout, LogBlockHeaderBytes(16));
	std::vector<LogRecord> records;
	tail.ForEach([&](const LogRecord &r) { records.push_back(r); });
	return records;
}

std::string LsnOf(const LogRecord &r) {
	char buf[32];
	snprintf(buf, sizeof(buf), "%08X:%08X:%04X", r.lsn.vlf, r.lsn.block, r.lsn.slot);
	return buf;
}

std::string ErrorOf(const std::function<void()> &f) {
	try {
		f();
	} catch (FormatError &e) {
		return e.what();
	}
	return "";
}

//! A 512-byte log block holding `records` empty records of fixed length `fixed` after its header of
//! `header` bytes.
std::vector<uint8_t> Block(uint16_t records, uint16_t fixed, uint16_t header = 72) {
	std::vector<uint8_t> b(512, 0);
	auto put16 = [&](size_t at, uint16_t v) {
		b[at] = static_cast<uint8_t>(v);
		b[at + 1] = static_cast<uint8_t>(v >> 8);
	};
	const uint16_t slot_end = 500;
	put16(2, records);
	put16(4, slot_end);
	put16(6, 512);
	for (uint16_t i = 0; i < records; i++) {
		const uint16_t at = static_cast<uint16_t>(header + i * 64);
		put16(slot_end - 2 * (i + 1), at);
		put16(at + 2, fixed);
		b[at + 22] = log_op::INSERT_ROWS;
	}
	return b;
}

}  // namespace

int main() {
	std::cout << "log.trn (fn_dump_dblog's counts)" << std::endl;
	{
		const auto records = RecordsOf("log.trn");
		Check(records.size() == 1628, "1628 records (fn_dump_dblog: 1628)");
		std::map<uint8_t, size_t> ops;
		for (const auto &r : records) {
			ops[r.op]++;
		}
		Check(ops[log_op::BEGIN_XACT] == 24 && ops[log_op::COMMIT_XACT] == 24, "24 BEGIN_XACT, 24 COMMIT_XACT");
		Check(ops[log_op::INSERT_ROWS] == 537 && ops[log_op::DELETE_ROWS] == 428 && ops[log_op::MODIFY_COLUMNS] == 201,
			  "537 INSERT_ROWS, 428 DELETE_ROWS, 201 MODIFY_COLUMNS");
		Check(ops[log_op::FORMAT_PAGE] == 40 && ops[log_op::MODIFY_ROW] == 23 && ops[log_op::DELETE_SPLIT] == 17,
			  "40 FORMAT_PAGE, 23 MODIFY_ROW, 17 DELETE_SPLIT");
		Check(ops[0x96] == 16 && ops[0x98] == 16 && ops[0x99] == 16, "16 checkpoints (BEGIN / XACT / END_CKPT)");
		Check(!records.empty() && LsnOf(records.front()) == "0000002C:00000818:0001" && records.front().op == 0x96,
			  "the first record: 2C:818:1, BEGIN_CKPT");
		bool found = false;
		for (const auto &r : records) {
			if (LsnOf(r) == "0000002C:00000830:0002") {
				found = r.op == log_op::MODIFY_COLUMNS && r.context == 0x02 && r.transaction == 0x3f5 && r.has_page &&
						r.file == 1 && r.page == 153 && r.has_allocation_unit &&
						r.allocation_unit == 281474978283520ull;
			}
		}
		Check(found,
			  "2C:830:2: MODIFY_COLUMNS, LCX_CLUSTERED, transaction 0000:000003f5, page 1:153, "
			  "allocation unit 281474978283520");
		// Row-log contents, fn_dump_dblog's RowLog Contents 0 / 1 (it joins the elements from the fifth on).
		auto hex = [](const LogElement &e) {
			static const char *digits = "0123456789ABCDEF";
			std::string out;
			for (uint16_t i = 0; i < e.length; i++) {
				out += digits[e.data[i] >> 4];
				out += digits[e.data[i] & 15];
			}
			return out;
		};
		bool modify = false, insert = false, begin = false;
		// A record's bytes live until the cursor's next record: the elements are read as it is visited.
		const auto bytes = ReadAll(DIR + "log.trn");
		MemoryAccess file("log.trn", bytes.data(), bytes.size());
		const auto layout = WalkMtf(file);
		LogTailReader tail(file, layout, LogBlockHeaderBytes(16));
		LogTailCursor cursor(tail);
		LogRecord r;
		while (cursor.Next(r)) {
			const auto lsn = LsnOf(r);
			if (lsn == "0000002C:00000840:0002") {
				const auto e = RowLogElements(r, "test");
				modify = r.op == log_op::MODIFY_ROW && e.size() == 2 && hex(e[0]) == "00000000000000000000" &&
						 hex(e[1]) == "2C000000180800000100";
			}
			if (lsn == "0000002C:00000970:0010") {
				const auto e = RowLogElements(r, "test");
				insert = r.op == log_op::INSERT_ROWS && e.size() == 62 && e[0].length == 86 && e[3].length == 89 &&
						 hex(e[0]).substr(0, 24) == "300019003C000000014C1D00";
			}
			if (lsn == "0000002C:00000830:0001") {
				begin = r.op == log_op::BEGIN_XACT && RowLogElements(r, "test").size() == 2;
			}
		}
		Check(modify, "2C:840:2 MODIFY_ROW: before 00 x 10, after 2C000000180800000100 (RowLog Contents 0 / 1)");
		Check(insert, "2C:970:10 INSERT_ROWS: 62 elements, the first 86 bytes (a split's rows)");
		Check(begin, "2C:830:1 BEGIN_XACT: two elements (its name, its flags)");
		{
			LogRecord r;
			const uint8_t bytes[8] = {0, 0, 4, 0, 5, 0, 0, 0};	// fixed part 4, then n = 5 and nothing
			r.bytes = bytes;
			r.length = 8;
			r.fixed_length = 4;
			Check(ErrorOf([&] { RowLogElements(r, "test"); }).find("past its end") != std::string::npos,
				  "contents listing elements past the record");
		}
		bool ordered = true;
		for (size_t i = 1; i < records.size(); i++) {
			ordered = ordered && records[i - 1].lsn <= records[i].lsn && !(records[i].lsn <= records[i - 1].lsn);
		}
		Check(ordered, "in strictly increasing LSN order");
	}
	std::cout << "full.bak (a quiet backup: its checkpoint only)" << std::endl;
	{
		const auto records = RecordsOf("full.bak");
		Check(records.size() == 3 && records[0].op == 0x96 && records[1].op == 0x98 && records[2].op == 0x99,
			  "BEGIN_CKPT, XACT_CKPT, END_CKPT");
	}
	std::cout << "sector parity" << std::endl;
	{
		std::vector<uint8_t> b(2048, 0);
		b[512] = 0x40;
		b[1024] = 0x48;
		b[1536] = 0x40;
		b[2048 - 2] = 0x11;	 // sector 1's byte
		b[2048 - 3] = 0x22;	 // sector 2's
		b[2048 - 4] = 0x33;	 // sector 3's
		RestoreSectorParity(b.data(), b.size());
		Check(b[512] == 0x11 && b[1024] == 0x22 && b[1536] == 0x33, "each sector's first byte from the block's end");
	}
	std::cout << "blocks refused" << std::endl;
	{
		std::vector<LogRecord> got;
		auto keep = [&](const LogRecord &r) { got.push_back(r); };
		auto b = Block(2, 48);
		ParseLogBlock(b.data(), b.size(), 72, "test", keep);
		Check(got.size() == 2 && got[0].lsn.slot == 1 && got[1].lsn.slot == 2 && got[0].has_allocation_unit,
			  "a well-formed block: its records, slots 1 and 2");
		Check(ErrorOf([&] { ParseLogBlock(b.data(), 500, 72, "test", keep); }).find("multiple of 512") !=
				  std::string::npos,
			  "a size that is no multiple of 512");
		auto c = b;
		c[4] = 0x10;
		c[5] = 0x02;  // the slot array's end: 528, past the 512-byte block
		Check(ErrorOf([&] { ParseLogBlock(c.data(), c.size(), 72, "test", keep); }).find("slot array") !=
				  std::string::npos,
			  "a slot array past the block");
		auto d = b;
		d[498] = 0x10;	// the first record's offset: 16, inside the header
		d[499] = 0x00;
		Check(
			ErrorOf([&] { ParseLogBlock(d.data(), d.size(), 72, "test", keep); }).find("outside") != std::string::npos,
			"a record offset inside the header");
		// SQL Server 2017's blocks: a header of 48 bytes, records from there on (the oracle lane's 2017 backups).
		Check(LogBlockHeaderBytes(14) == 48 && LogBlockHeaderBytes(15) == 72 && LogBlockHeaderBytes(17) == 72,
			  "a block header of 48 bytes before SQL Server 2019 (15), 72 from it on");
		auto old = Block(2, 48, 48);
		got.clear();
		ParseLogBlock(old.data(), old.size(), 48, "test", keep);
		Check(got.size() == 2, "a 2017 block: its records from 48");
		Check(ErrorOf([&] { ParseLogBlock(old.data(), old.size(), 72, "test", keep); }).find("outside") !=
				  std::string::npos,
			  "the same block read as a 2019 one: its first record inside the header");
		auto e = Block(1, 600);
		Check(ErrorOf([&] { ParseLogBlock(e.data(), e.size(), 72, "test", keep); }).find("runs past") !=
				  std::string::npos,
			  "a record's fixed part past the records");

		// Review of #591: a fixed part is bounded by the record's own span (64 bytes here), not the block's
		// records, and two slots may not name one record.
		auto f = Block(2, 70);
		Check(ErrorOf([&] { ParseLogBlock(f.data(), f.size(), 72, "test", keep); }).find("record's end") !=
				  std::string::npos,
			  "a fixed part reaching into the next record");
		auto g = Block(2, 48);
		g[500 - 4] = g[500 - 2];
		g[500 - 3] = g[500 - 1];
		Check(ErrorOf([&] { ParseLogBlock(g.data(), g.size(), 72, "test", keep); }).find("two records at") !=
				  std::string::npos,
			  "two slots at one offset");
	}
	std::cout << "a record's page and slot by its fixed part" << std::endl;
	{
		// LOP_MIGRATE_LOCKS on 2017 (the oracle lane's load_log.trn): a fixed part of 30 bytes, the page at 24 and
		// its file at 28, then the contents -- fn_dump_dblog gives the page (1:43599), and there is no slot.
		std::vector<LogRecord> got;
		auto keep = [&](const LogRecord &r) { got.push_back(r); };
		for (uint16_t fixed : {24, 29, 30, 31, 32}) {
			auto b = Block(1, fixed);
			const uint8_t field[8] = {0x4f, 0xaa, 0x00, 0x00, 0x01, 0x00, 0x07, 0x00};	// 1:43599, slot 7
			std::copy(field, field + 8, b.begin() + 72 + 24);
			got.clear();
			ParseLogBlock(b.data(), b.size(), 72, "test", keep);
			if (got.size() != 1) {
				Check(false, "a fixed part of " + std::to_string(fixed) + " bytes: one record");
				continue;
			}
			const LogRecord &r = got.front();
			const bool page = r.has_page && r.file == 1 && r.page == 43599;
			const bool slot = r.has_slot && r.slot == 7;
			Check(page == (fixed >= 30) && slot == (fixed >= 32) && (r.has_slot || r.slot == 0),
				  "a fixed part of " + std::to_string(fixed) + " bytes: " +
					  (fixed >= 32	 ? "page and slot"
					   : fixed >= 30 ? "the page, no slot"
									 : "neither"));
		}
	}
	std::cout << "D11's rule for one transaction's changes on one page (ChangeShows)" << std::endl;
	{
		auto lsn = [](uint32_t block) {
			Lsn l;
			l.vlf = 1;
			l.block = block;
			return l;
		};
		FuzzySummary::Change c;
		c.oldest = lsn(10);
		c.newest = lsn(20);
		const Lsn checkpoint = lsn(5);
		// Committed: stale when the copy is older than its newest change.
		Check(ChangeShows(false, c, PageState::Present, lsn(15), checkpoint) &&
				  !ChangeShows(false, c, PageState::Present, lsn(20), checkpoint),
			  "committed: stale before its newest change, not at it");
		// On a page not live in the image: rows taken there after it are missing; extents freed on such an IAM
		// page are not (review of #619).
		c.iam = true;
		Check(!ChangeShows(false, c, PageState::NotLive, lsn(0), checkpoint),
			  "committed: extents freed on an IAM page not live in the image: as RESTORE");
		c.iam = false;
		Check(ChangeShows(false, c, PageState::NotLive, lsn(0), checkpoint),
			  "committed: rows on a page taken into use after the image: stale");
		// Rolled back: only a copy between its first change and its compensation holds what was undone.
		c.rolled_back = true;
		Check(!ChangeShows(false, c, PageState::Present, lsn(8), checkpoint),
			  "rolled back, copied before it: as RESTORE");
		Check(ChangeShows(false, c, PageState::Present, lsn(15), checkpoint),
			  "rolled back, copied between a change and its undo: stale");
		Check(!ChangeShows(false, c, PageState::Present, lsn(25), checkpoint),
			  "rolled back, copied after its undo: as RESTORE");
		Check(!ChangeShows(false, c, PageState::Pending, lsn(0), checkpoint),
			  "rolled back, on a page the backup does not hold: as RESTORE");
		// A page the backup holds as not live: a change on it -- rows on a page taken into use after the image, which
		// the rollback removed -- holds nothing a RESTORE keeps; an IAM page whose extents the change freed and the
		// compensation set again (a TRUNCATE's, review of #619) is stale when that came after CheckpointLSN.
		Check(!ChangeShows(false, c, PageState::NotLive, lsn(0), checkpoint),
			  "rolled back, on a page not live in the backup: as RESTORE");
		c.regained = true;
		Check(ChangeShows(false, c, PageState::NotLive, lsn(0), checkpoint) &&
				  !ChangeShows(false, c, PageState::NotLive, lsn(0), lsn(25)),
			  "rolled back TRUNCATE, its IAM page not live in the backup: stale when given back after CheckpointLSN");
		c.regained = false;
		// Open: dirty when the copy holds its oldest change.
		c.rolled_back = false;
		Check(ChangeShows(true, c, PageState::Present, lsn(10), checkpoint) &&
				  !ChangeShows(true, c, PageState::Present, lsn(9), checkpoint),
			  "open: dirty from its oldest change on");
	}
	std::cout << "a tail read to its LastLSN (TailReachesLast)" << std::endl;
	{
		auto lsn = [](uint32_t vlf, uint32_t block, uint16_t slot) {
			Lsn l;
			l.vlf = vlf;
			l.block = block;
			l.slot = slot;
			return l;
		};
		// The committed samples: the last record slot 1 of a 4096-byte block, LastLSN the next block's first.
		Check(TailReachesLast(lsn(44, 2088, 1), 4096, lsn(44, 2096, 1)), "full.bak: 2C:828:1 then 2C:830:1");
		Check(!TailReachesLast(lsn(44, 2080, 1), 4096, lsn(44, 2096, 1)), "a block short of LastLSN: not whole");
		Check(TailReachesLast(lsn(44, 2088, 3), 4096, lsn(44, 2088, 4)) &&
				  !TailReachesLast(lsn(44, 2088, 2), 4096, lsn(44, 2088, 4)),
			  "LastLSN within a block: the record before it");
		Check(TailReachesLast(lsn(44, 9000, 1), 512, lsn(45, 16, 1)) &&
				  !TailReachesLast(lsn(43, 9000, 1), 512, lsn(45, 16, 1)) &&
				  !TailReachesLast(lsn(46, 16, 1), 512, lsn(45, 16, 1)),
			  "LastLSN in the next VLF: taken; two VLFs on, or an earlier one: not");
	}
	std::cout << "a transaction id begun twice (review of #591)" << std::endl;
	{
		auto record = [](uint32_t block, uint8_t op, uint64_t transaction) {
			LogRecord r;
			r.lsn.vlf = 1;
			r.lsn.block = block;
			r.lsn.slot = 1;
			r.op = op;
			r.context = 0x02;  // LCX_CLUSTERED
			r.transaction = transaction;
			r.has_page = r.has_allocation_unit = true;
			r.file = 1;
			r.page = 300;
			r.allocation_unit = 72057594043170816ULL;
			return r;
		};
		Lsn first, last, checkpoint;
		first.vlf = last.vlf = checkpoint.vlf = 1;
		first.block = checkpoint.block = 1;
		last.block = 99;
		// Its committed change would be filed under the second life's open state, the table consistent.
		const std::vector<LogRecord> reused = {record(2, log_op::BEGIN_XACT, 7), record(3, log_op::INSERT_ROWS, 7),
											   record(4, log_op::COMMIT_XACT, 7), record(5, log_op::BEGIN_XACT, 7)};
		Check(
			ErrorOf([&] {
				SummarizeLogRecords(reused, first, last, checkpoint);
			}).find("begins transaction 0000:00000007 at 1000000000500001 after 3 records of it") != std::string::npos,
			"BEGIN, a change, COMMIT, BEGIN of one id: not one log");
		// One begun before the tail (no BEGIN in it), then begun in it.
		const std::vector<LogRecord> late = {record(2, log_op::INSERT_ROWS, 8), record(3, log_op::BEGIN_XACT, 8)};
		Check(ErrorOf([&] { SummarizeLogRecords(late, first, last, checkpoint); }).find("begun twice") !=
				  std::string::npos,
			  "a change of an id, then its BEGIN: not one log");
		const std::vector<LogRecord> once = {record(2, log_op::BEGIN_XACT, 9), record(3, log_op::INSERT_ROWS, 9),
											 record(4, log_op::COMMIT_XACT, 9), record(5, log_op::BEGIN_XACT, 10)};
		Check(ErrorOf([&] { SummarizeLogRecords(once, first, last, checkpoint); }).empty(),
			  "each id begun once: summarised");
	}
	std::cout << "changes a reader sees beside row operations (fixture tail_window)" << std::endl;
	{
		const uint64_t au = 72057594054311936ULL;
		std::vector<std::vector<uint8_t>> keep;	 // each record's bytes, alive through the summary
		auto record = [&](uint32_t block, uint8_t op, uint8_t context, uint32_t page, std::vector<uint8_t> bytes) {
			keep.push_back(std::move(bytes));
			LogRecord r;
			r.lsn.vlf = 1;
			r.lsn.block = block;
			r.lsn.slot = 1;
			r.op = op;
			r.context = context;
			r.transaction = 5;
			r.has_page = r.has_allocation_unit = true;
			r.file = 1;
			r.page = page;
			r.allocation_unit = au;
			r.bytes = keep.back().data();
			r.length = static_cast<uint16_t>(keep.back().size());
			r.fixed_length = op == log_op::SET_BITS ? 54 : 48;
			return r;
		};
		// A FORMAT_PAGE's elements after its fixed part: their count, their lengths, then the data (4-aligned).
		auto format = [](uint16_t image) {
			std::vector<uint8_t> b(48, 0);
			b.push_back(image ? 1 : 0);
			b.push_back(0);
			if (image) {
				b.push_back(static_cast<uint8_t>(image));
				b.push_back(static_cast<uint8_t>(image >> 8));
				b.resize(52 + image, 0xAB);
			}
			return b;
		};
		// A SET_BITS's fixed part: the first bit @48, their count @50, the value @52.
		auto bits = [](uint8_t value) {
			std::vector<uint8_t> b(54, 0);
			b[48] = 0x56;
			b[50] = 1;
			b[52] = value;
			return b;
		};
		Lsn first, last, checkpoint;
		first.vlf = last.vlf = checkpoint.vlf = 1;
		first.block = checkpoint.block = 1;
		last.block = 99;
		std::vector<LogRecord> records = {record(2, log_op::BEGIN_XACT, 0, 0, {}),
										  record(3, log_op::FORMAT_PAGE, 0x01, 432, format(8096)),
										  record(4, log_op::FORMAT_PAGE, 0x01, 433, format(0)),
										  record(5, log_op::SET_BITS, 0x0a, 425, bits(0)),
										  record(6, log_op::SET_BITS, 0x0a, 426, bits(1)),
										  record(7, log_op::COMMIT_XACT, 0, 0, {})};
		records[0].has_page = records[5].has_page = false;
		const FuzzySummary summary = SummarizeLogRecords(records, first, last, checkpoint);
		auto changed = [&](uint32_t page) {
			auto unit = summary.units.find(au);
			return unit != summary.units.end() && unit->second.finished.count(uint64_t(1) << 32 | page) != 0;
		};
		Check(changed(432) && !changed(433),
			  "a FORMAT_PAGE with its page's image (a bulk load's): counted; an empty one: not");
		Check(changed(425) && !changed(426),
			  "extents freed from a unit's IAM page (a TRUNCATE): counted; allocated: not");
		Check(summary.transactions.at(5).row_operations == 2, "the transaction counts the two");
	}
	std::cout << "a rolled-back TRUNCATE's compensation widens its change (review of #619)" << std::endl;
	{
		const uint64_t au = 72057594054377472ULL;
		auto record = [&](uint32_t block, uint8_t op, uint32_t page, uint8_t value) {
			static std::vector<std::vector<uint8_t>> keep;
			std::vector<uint8_t> b(54, 0);
			b[48] = 0x56;
			b[50] = 1;
			b[52] = value;
			keep.push_back(std::move(b));
			LogRecord r;
			r.lsn.vlf = 1;
			r.lsn.block = block;
			r.lsn.slot = 1;
			r.op = op;
			r.context = op == log_op::SET_BITS ? 0x0a : 0;
			r.transaction = 7;
			r.has_page = r.has_allocation_unit = op == log_op::SET_BITS || op == log_op::FORMAT_PAGE;
			r.file = 1;
			r.page = page;
			r.allocation_unit = au;
			r.bytes = keep.back().data();
			r.length = 54;
			r.fixed_length = 54;
			return r;
		};
		Lsn first, last, checkpoint;
		first.vlf = last.vlf = checkpoint.vlf = 1;
		first.block = checkpoint.block = 1;
		last.block = 99;
		// The TRUNCATE frees extents of IAM page 424; the rollback sets them again. A set on page 431 with no free
		// before it (a bulk load's allocation) widens nothing and is not a change of its own. IAM page 602, formatted
		// in the tail, has extents freed and set again: the transaction's own, not regained.
		const std::vector<LogRecord> records = {
			record(2, log_op::BEGIN_XACT, 0, 0), record(3, log_op::SET_BITS, 431, 1),
			record(4, log_op::SET_BITS, 424, 0), record(5, log_op::FORMAT_PAGE, 602, 0),
			record(6, log_op::SET_BITS, 602, 0), record(7, log_op::SET_BITS, 602, 1),
			record(8, log_op::SET_BITS, 424, 1), record(9, log_op::ABORT_XACT, 0, 0)};
		const FuzzySummary summary = SummarizeLogRecords(records, first, last, checkpoint);
		const auto &unit = summary.units.at(au);
		auto truncated = unit.finished.find(uint64_t(1) << 32 | 424);
		Check(truncated != unit.finished.end() && truncated->second.size() == 1 &&
				  truncated->second[0].oldest.block == 4 && truncated->second[0].newest.block == 8 &&
				  truncated->second[0].rolled_back && truncated->second[0].regained,
			  "the free and the set that undoes it: one change, from the free to the set");
		Check(truncated->second[0].iam, "an IAM page's change is marked so");
		Check(unit.finished.count(uint64_t(1) << 32 | 431) == 0 && summary.transactions.at(7).row_operations == 2,
			  "an allocation alone: no change");
		auto grown = unit.finished.find(uint64_t(1) << 32 | 602);
		Check(grown != unit.finished.end() && grown->second.size() == 1 && grown->second[0].newest.block == 7 &&
				  !grown->second[0].regained,
			  "an IAM page formatted in the tail: widened, but not regained");
	}
	std::cout << "each log block follows the one before it, across the streams (review of #591)" << std::endl;
	{
		// Two MQTL streams (each a 2-byte lead-in, then 512-byte blocks); the first ends in a header of zeros.
		auto block = [](uint32_t vlf, uint32_t id) {
			auto b = Block(1, 24);
			for (int i = 0; i < 4; i++) {
				b[12 + i] = static_cast<uint8_t>(vlf >> (8 * i));
				b[16 + i] = static_cast<uint8_t>(id >> (8 * i));
			}
			return b;
		};
		auto read = [&](const std::vector<std::vector<std::vector<uint8_t>>> &streams, size_t &records) {
			std::vector<uint8_t> bytes;
			MtfLayout layout;
			layout.blocks.emplace_back();
			for (const auto &blocks : streams) {
				MtfStream s;
				s.id = "MQTL";
				s.data_offset = bytes.size();
				bytes.insert(bytes.end(), 2, 0);
				for (const auto &b : blocks) {
					bytes.insert(bytes.end(), b.begin(), b.end());
				}
				s.length = bytes.size() - s.data_offset;
				layout.blocks[0].streams.push_back(s);
			}
			MemoryAccess file("crafted", bytes.data(), bytes.size());
			LogTailReader tail(file, layout, 72);
			records = 0;
			return ErrorOf([&] { tail.ForEach([&](const LogRecord &) { records++; }); });
		};
		const std::vector<uint8_t> zeros(512, 0);
		size_t records = 0;
		Check(
			read({{block(1, 10), block(1, 11), zeros}, {block(1, 12), block(2, 400)}}, records).empty() && records == 4,
			"the next block in its VLF, across a stream's unused end, then the next VLF: 4 records");
		const std::string gap = read({{block(1, 10), block(1, 11), zeros}, {block(1, 13)}}, records);
		Check(gap.find("VLF 1 block 13 does not follow VLF 1 block 11 of 512 bytes before it") != std::string::npos,
			  "a block missing between two streams: not read whole");
		Check(read({{block(1, 10), block(3, 1)}}, records).find("does not follow") != std::string::npos,
			  "a VLF skipped: not read whole");
		Check(read({{block(1, 10), block(1, 10)}}, records).find("does not follow") != std::string::npos,
			  "a block twice: not read whole");
	}
	std::cout << "the pages the tail formats (a zero copy in a one-pass backup: Pending)" << std::endl;
	{
		auto record = [](uint32_t block, uint16_t slot, uint8_t op, uint32_t page) {
			LogRecord r;
			r.lsn.vlf = 1;
			r.lsn.block = block;
			r.lsn.slot = slot;
			r.op = op;
			r.has_page = true;
			r.file = 1;
			r.page = page;
			return r;
		};
		Lsn first, last, checkpoint;
		first.vlf = last.vlf = checkpoint.vlf = 1;
		first.block = checkpoint.block = 1;
		last.block = 9;
		const std::vector<LogRecord> records = {record(2, 1, log_op::FORMAT_PAGE, 44586),
												record(3, 1, log_op::INSERT_ROWS, 44586),
												record(9, 2, log_op::FORMAT_PAGE, 50000)};	// past LastLSN
		const FuzzySummary summary = SummarizeLogRecords(records, first, last, checkpoint);
		Check(summary.formatted.count(uint64_t(1) << 32 | 44586) == 1 && summary.formatted.size() == 1,
			  "a FORMAT_PAGE before LastLSN names its page; one past it, none");
	}
	{
		// The summary of a whole log asks its caller as it reads (an interrupt ends it; review of #591).
		const auto bytes = ReadAll(DIR + "log.trn");
		MemoryAccess file("log.trn", bytes.data(), bytes.size());
		const auto layout = WalkMtf(file);
		LogTailReader tail(file, layout, LogBlockHeaderBytes(16));
		const Lsn none;
		uint64_t polls = 0;
		uint64_t first_bytes = 0, last_bytes = 0;
		const FuzzySummary summary = SummarizeLogTail(tail, none, none, none, [&](uint64_t bytes) {
			first_bytes = polls++ ? first_bytes : bytes;
			last_bytes = bytes;
		});
		const FuzzySummary unasked = SummarizeLogTail(tail, none, none, none);
		Check(summary.records == 1628 && unasked.records == 1628 &&
				  polls == (1628 + SUMMARY_POLL_RECORDS - 1) / SUMMARY_POLL_RECORDS,
			  "the summary polls once every SUMMARY_POLL_RECORDS records, from the first");
		// What it says it holds grows as it reads, from the empty summary on (the caller reserves it).
		Check(first_bytes >= sizeof(FuzzySummary) && first_bytes <= last_bytes && last_bytes < (1u << 20),
			  "the poll is given the summary's bytes so far");
		bool stopped = false;
		uint64_t seen = 0;
		try {
			SummarizeLogTail(tail, none, none, none, [&](uint64_t) {
				seen++;
				throw std::runtime_error("interrupted");
			});
		} catch (std::runtime_error &e) {
			stopped = std::string(e.what()) == "interrupted";
		}
		Check(stopped && seen == 1, "what the poll throws ends the read, as thrown");
	}
	std::cout << (failures ? "FAILED" : "all passed") << std::endl;
	return failures ? 1 : 0;
}
