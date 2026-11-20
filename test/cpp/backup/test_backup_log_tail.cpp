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
		Check(ErrorOf([&] { ParseLogBlock(b.data(), 500, 72, "test", keep); }).find("multiple of 512") != std::string::npos,
			  "a size that is no multiple of 512");
		auto c = b;
		c[4] = 0x10;
		c[5] = 0x02;  // the slot array's end: 528, past the 512-byte block
		Check(ErrorOf([&] { ParseLogBlock(c.data(), c.size(), 72, "test", keep); }).find("slot array") != std::string::npos,
			  "a slot array past the block");
		auto d = b;
		d[498] = 0x10;	// the first record's offset: 16, inside the header
		d[499] = 0x00;
		Check(ErrorOf([&] { ParseLogBlock(d.data(), d.size(), 72, "test", keep); }).find("outside") != std::string::npos,
			  "a record offset inside the header");
		// SQL Server 2017's blocks: a header of 48 bytes, records from there on (the oracle lane's 2017 backups).
		Check(LogBlockHeaderBytes(14) == 48 && LogBlockHeaderBytes(15) == 72 && LogBlockHeaderBytes(17) == 72,
			  "a block header of 48 bytes before SQL Server 2019 (15), 72 from it on");
		auto old = Block(2, 48, 48);
		got.clear();
		ParseLogBlock(old.data(), old.size(), 48, "test", keep);
		Check(got.size() == 2, "a 2017 block: its records from 48");
		Check(ErrorOf([&] { ParseLogBlock(old.data(), old.size(), 72, "test", keep); }).find("outside") != std::string::npos,
			  "the same block read as a 2019 one: its first record inside the header");
		auto e = Block(1, 600);
		Check(ErrorOf([&] { ParseLogBlock(e.data(), e.size(), 72, "test", keep); }).find("runs past") != std::string::npos,
			  "a record's fixed part past the records");
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
	std::cout << (failures ? "FAILED" : "all passed") << std::endl;
	return failures ? 1 : 0;
}
