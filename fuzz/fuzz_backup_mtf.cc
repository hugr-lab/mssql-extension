// Fuzz harness: a SQL Server backup's MTF layer (spec 085 tasks 3 and 9).
//
// A .bak is untrusted input (spec 085 § Security). The bytes are a plain family file: the strict
// MTF walk (descriptor blocks, checksums, streams to their SPAD), the backup set's header fields
// (TAPE / SSET / VOLB / the trailer's SCIN, SFGI and SFIN records), and the TAPE-only walk a
// compressed container's media label takes. Every malformed input must end in a FormatError;
// any sanitizer report, signal or other exception is a real bug.

#include <cstddef>
#include <cstdint>

#include "backup/backup_bytes.hpp"
#include "backup/backup_mtf.hpp"
#include "backup/backup_set.hpp"

using namespace duckdb::mssql::backup;

static volatile size_t g_sink = 0;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
	MemoryAccess file("fuzz.bak", data, size);
	try {
		auto layout = WalkMtf(file);
		auto set = ReadBackupSet(file, layout);
		g_sink += set.files.size() + set.backup_size + set.database_name.size();
	} catch (FormatError &) {
	}
	try {
		auto tape = WalkMtfTape(file);
		BackupSetHeader set;
		ReadMediaFields(file, tape, set);
		g_sink += set.family_count;
	} catch (FormatError &) {
	}
	try {
		auto layout = WalkMtfDataSet(file);
		g_sink += layout.blocks.size();
	} catch (FormatError &) {
	}
	return 0;
}
