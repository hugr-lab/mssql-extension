// Fuzz harness: a SQL Server backup's log tail (spec 085 tasks 26-28).
//
// A .bak is untrusted input (spec 085 § Security). The bytes are two things at once:
//   - one log block (its size the input's, rounded down to 512 bytes): the sector parity put back,
//     its records through the slot array, each record's row-log elements;
//   - the data of one MQTL stream: the cursor's walk over its blocks (a block header of zeros ends it,
//     a block past the stream is refused), and D11's summary of the records (the transactions, the
//     changes by unit and page).
// Every malformed input must end in a FormatError; any sanitizer report, signal or other exception is
// a real bug.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "backup/backup_bytes.hpp"
#include "backup/backup_fuzzy.hpp"
#include "backup/backup_log_tail.hpp"
#include "backup/backup_mtf.hpp"

using namespace duckdb::mssql::backup;

static volatile size_t g_sink = 0;

static void Visit(const LogRecord &r) {
	g_sink += r.op + r.context + r.length;
	try {
		for (const auto &e : RowLogElements(r, "fuzz")) {
			g_sink += e.length + (e.length ? e.data[0] : 0);
		}
	} catch (FormatError &) {
	}
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
	// One block.
	const size_t block_size = size - size % 512;
	if (block_size >= 512 && block_size <= 0xffff) {
		std::vector<uint8_t> block(data, data + block_size);
		RestoreSectorParity(block.data(), block.size());
		try {
			ParseLogBlock(block.data(), block.size(), (data[0] & 1) ? 48 : 72, "fuzz", Visit);
		} catch (FormatError &) {
		}
	}
	// One stream: the input after a 2-byte lead-in.
	if (size >= 2) {
		MemoryAccess file("fuzz.bak", data, size);
		MtfLayout layout;
		MtfBlock mtf;
		mtf.type = "MSLS";
		MtfStream stream;
		stream.id = "MQTL";
		stream.data_offset = 0;
		stream.length = size;
		mtf.streams.push_back(stream);
		layout.blocks.push_back(mtf);
		try {
			LogTailReader tail(file, layout, LogBlockHeaderBytes(15));
			LogTailCursor cursor(tail);
			LogRecord r;
			while (cursor.Next(r)) {
				Visit(r);
			}
			Lsn first, last, checkpoint;
			last.vlf = 0xffffffffu;
			checkpoint.vlf = 0x40;
			const FuzzySummary summary = SummarizeLogTail(tail, first, last, checkpoint);
			g_sink += summary.records + summary.transactions.size() + summary.units.size();
		} catch (FormatError &) {
		}
	}
	return 0;
}
