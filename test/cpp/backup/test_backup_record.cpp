// Spec 085 PR 2b: RecordView / ReadRecord / ReadSlot and ReadFileHeaderRecord.
//
// The two file header records are prefixes of real ones, from backups of synthetic databases made on
// throwaway containers (2026-11-10): SQL Server 2022's (60 columns, 59 variable, data from 138) and
// 2025's (66 columns, 65 variable, data from 151). Their variable columns 2 / 3 / 4 -- file id,
// filegroup, Size -- sit 13 bytes apart, which a fixed offset (+167 / +171, read off 2025) missed on
// 2022. The rest are crafted: every part out of the page's bounds is a named error, a record without a
// null bitmap has its variable count at fixed_end, and a forwarding stub is not split.

#include <cstring>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

#include "backup/backup_record.hpp"

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

std::vector<uint8_t> FromHex(const std::string &hex) {
	std::vector<uint8_t> out;
	for (size_t i = 0; i + 1 < hex.size(); i += 2) {
		out.push_back(static_cast<uint8_t>(std::stoi(hex.substr(i, 2), nullptr, 16)));
	}
	return out;
}

//! An 8 KB page with one slot at 96 holding `record`.
std::vector<uint8_t> PageWith(const std::vector<uint8_t> &record, uint16_t slot_offset = 96) {
	std::vector<uint8_t> page(8192, 0);
	page[0] = 1;
	page[8190] = slot_offset & 0xFF;
	page[8191] = slot_offset >> 8;
	std::memcpy(&page[slot_offset], record.data(), std::min(record.size(), page.size() - 2 - slot_offset));
	return page;
}

// The first 192 bytes of each record: header, null bitmap, variable column ends, then the data up
// to and past the Size.
const char *RECORD_2022 =
	"30000800000000003c0000000000000000003b009a009a009c009e00a200a600"
	"aa00ae00b800c200cc00cc00d000d400d800dc00e60002010c01160120013001"
	"3a014a014e01580158016e017e017e017e017e017e017e017e018e018e018e01"
	"9801a201be01c801d801f401fc0154045404540456046004640468046c047404"
	"7e04820486048a048e04951a4faec64e9e45b0bcac82f2ad5730010001000002"
	"0800ffffffff0020000000000000000000000000000000000000000000000000";
const char *RECORD_2025 =
	"300008000000000042000000000000000000004100a700a700a900ab00af00b3"
	"00b700bb00c500cf00d900d900dd00e100e500e900f3000f01190123012d013d"
	"01470157015b0165016501790189018901890189018901890189019901990199"
	"01a301ad01c901d301e301ff0107025f045f045f0461046b046f04730477047f"
	"0489048d04910495049904a304a704ab04af04b304b7045353a9b8f364664b9a"
	"7df6c2c7de830f0100010000040000ffffffff00200000000000000000000000";

}  // namespace

int main() {
	std::cout << "FixedVar records and the file header record (spec 085 PR 2b)" << std::endl;

	{
		auto page = PageWith(FromHex(RECORD_2022));
		auto r = ReadSlot(page.data(), page.size(), 1, 0, "the 2022 record");
		Check(r.Type() == 0 && r.column_count == 60 && r.var_count == 59 && r.var_start == 138,
			  "2022: a primary record of 60 columns, 59 variable, data from 138");
		auto h = ReadFileHeaderRecord(page.data(), page.size());
		Check(h.file_id == 1 && h.filegroup_id == 1 && h.size == 524800,
			  "2022: file 1, filegroup 1, Size 524800 (" + std::to_string(h.size) + ")");
	}
	{
		auto page = PageWith(FromHex(RECORD_2025));
		auto r = ReadSlot(page.data(), page.size(), 1, 0, "the 2025 record");
		Check(r.Type() == 0 && r.column_count == 66 && r.var_count == 65 && r.var_start == 151,
			  "2025: a primary record of 66 columns, 65 variable, data from 151");
		auto h = ReadFileHeaderRecord(page.data(), page.size());
		Check(h.file_id == 1 && h.filegroup_id == 1 && h.size == 1024,
			  "2025: file 1, filegroup 1, Size 1024 (" + std::to_string(h.size) + ")");
		auto moved = PageWith(FromHex(RECORD_2025), 2494);
		Check(ErrorOf([&]() { ReadFileHeaderRecord(moved.data(), moved.size()); }).empty() &&
				  ReadFileHeaderRecord(moved.data(), moved.size()).size == 1024,
			  "a header record at 2494 (a slot offset seen on a server) reads alike");
	}

	// Crafted records: a 4-byte header, then as the cases say.
	auto bad = [](std::vector<uint8_t> record, uint16_t slot = 96) {
		auto page = PageWith(record, slot);
		return ErrorOf([&]() { ReadSlot(page.data(), page.size(), 1, 0, "a crafted record"); });
	};
	Check(!bad({0x30, 0, 2, 0}).empty(), "a fixed part ending before its own header is refused");
	Check(!bad({0x30, 0, 0xFF, 0x7F}).empty(), "a fixed part ending past the page is refused");
	// statusA 0x30, fixed_end 4, 2 columns, bitmap 1 byte, 2 variable columns ending 20 then 16.
	Check(!bad({0x30, 0, 4, 0, 2, 0, 0, 2, 0, 20, 0, 16, 0}).empty(), "variable column ends that descend are refused");
	Check(!bad({0x30, 0, 4, 0, 2, 0, 0, 2, 0, 13, 0, 0xF0, 0x7F}).empty(),
		  "a variable column ending past the page is refused");
	Check(!bad({0x30, 0, 4, 0}, 40).empty(), "a slot pointing into the page header is refused");
	{
		// A primary record without a null bitmap (statusA 0x20 alone): its variable count right at
		// fixed_end (here 6, after two fixed bytes).
		std::vector<uint8_t> record = {0x20, 0, 6, 0, 0xAB, 0xCD, 1, 0, 13, 0, 'x', 'y', 'z'};
		auto page = PageWith(record);
		auto r = ReadSlot(page.data(), page.size(), 1, 0, "a record without a null bitmap");
		auto v = r.Var(0, "its variable column");
		Check(r.Type() == 0 && r.column_count == 0 && r.var_count == 1 && v.Size() == 3 && v.U8(0) == 'x' &&
				  r.Fixed("its fixed part").Size() == 2,
			  "a record without a null bitmap: its variable count at fixed_end");
		Check(!ErrorOf([&]() { r.Var(1, "a column it does not have"); }).empty(),
			  "a variable column past the record's count is refused");
	}
	{
		// A NULL bit: column 1 of 3 NULL.
		std::vector<uint8_t> record = {0x10, 0, 4, 0, 3, 0, 0x02};
		auto page = PageWith(record);
		auto r = ReadSlot(page.data(), page.size(), 1, 0, "a record with a NULL");
		Check(!r.IsNull(0) && r.IsNull(1) && !r.IsNull(2) && !r.IsNull(3), "the null bitmap, bit per column");
	}
	{
		// A forwarding stub (type 2): 9 bytes, its bytes 2..3 part of the pointer, never split.
		std::vector<uint8_t> stub = {0x04, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
		auto page = PageWith(stub);
		auto r = ReadSlot(page.data(), page.size(), 1, 0, "a forwarding stub");
		Check(r.Type() == 2 && r.fixed_end == 0 && r.var_count == 0, "a forwarding stub is not split as FixedVar");
		Check(!ErrorOf([&]() { r.Fixed("a stub's fixed part"); }).empty(), "a stub has no fixed part to read");
		Check(!ErrorOf([&]() { ReadFileHeaderRecord(page.data(), page.size()); }).empty(),
			  "a file header that is not a primary record is refused");
	}

	{
		// An index record (type 3): status byte, keys packed to the page's pminlen, a child pointer --
		// no fixed end @2 (DbReader's sysrscols index slot starts 06 00 00 b5 09: read as FixedVar it
		// would be a fixed part to 2485). Not split.
		std::vector<uint8_t> index = {0x06, 0x00, 0x00, 0xb5, 0x09, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00};
		auto page = PageWith(index);
		auto r = ReadSlot(page.data(), page.size(), 1, 0, "an index record");
		Check(r.Type() == 3 && r.fixed_end == 0 && r.var_count == 0 &&
				  !ErrorOf([&]() { r.Fixed("an index record's fixed part"); }).empty(),
			  "an index record is not split as FixedVar");
	}

	std::cout << (failures ? "FAILED" : "PASSED") << " (" << failures << " failure(s))" << std::endl;
	return failures ? 1 : 0;
}
