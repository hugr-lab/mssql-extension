#include "backup/backup_set.hpp"

#include "backup/backup_bytes.hpp"

#include <cstdio>

namespace duckdb {
namespace mssql {
namespace backup {

// The trailer MQCI stream is a few KB (2.9 KB seen); bounded so a corrupt length cannot allocate.
static constexpr size_t MAX_HEADER_STREAM = 1 << 20;
// The RAID stream of TAPE: 32 bytes seen.
static constexpr size_t MAX_RAID_STREAM = 4096;
// The SCIN fields read below end at +270.
static constexpr size_t SCIN_MIN = 270;
// CompressedBackupSize, a u64 past the fixed part (444-byte SCIN records in 2025; a shorter one has none).
static constexpr size_t SCIN_COMPRESSED_SIZE_AT = 344;
// An SFIN's fixed part ends at +202 (UniqueId).
static constexpr size_t SFIN_MIN = 202;
static constexpr uint32_t SFIN_REMOVED = 0x20;
// A compressed container's soft filemark (spec 085 § Rules, MSSQLBAK).
static constexpr uint64_t CONTAINER_SOFT_FILEMARK = 4096;

std::string Lsn::ToDecimal() const {
	if (IsZero()) {
		return "0";
	}
	char buf[48];
	snprintf(buf, sizeof(buf), "%u%010u%05u", vlf, block, static_cast<unsigned>(slot));
	// A VLF of 0 would print a leading 0 before the block's digits: the number has none.
	const char *digits = buf;
	while (digits[0] == '0' && digits[1] != '\0') {
		digits++;
	}
	return digits;
}

bool Lsn::operator<=(const Lsn &other) const {
	if (vlf != other.vlf) {
		return vlf < other.vlf;
	}
	if (block != other.block) {
		return block < other.block;
	}
	return slot <= other.slot;
}

bool Guid::IsZero() const {
	for (auto b : bytes) {
		if (b != 0) {
			return false;
		}
	}
	return true;
}

std::string Guid::ToString() const {
	const auto &b = bytes;
	char buf[40];
	snprintf(buf, sizeof(buf), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", b[3], b[2], b[1],
			 b[0], b[5], b[4], b[7], b[6], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
	return buf;
}

std::string MtfDate::ToString() const {
	char buf[32];
	snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d", year, month, day, hour, minute, second);
	return buf;
}

const char *BackupKindName(BackupKind kind) {
	switch (kind) {
	case BackupKind::Full:
		return "full";
	case BackupKind::Differential:
		return "differential";
	default:
		return "log";
	}
}

const char *RecoveryModelName(RecoveryModel model) {
	switch (model) {
	case RecoveryModel::Simple:
		return "SIMPLE";
	case RecoveryModel::BulkLogged:
		return "BULK_LOGGED";
	default:
		return "FULL";
	}
}

const Guid &BackupSetHeader::FirstRecoveryForkId() const {
	return HasForkPoint() ? parent_fork_id : recovery_fork_id;
}

bool BackupSetHeader::HasForkPoint() const {
	// A log backup whose first LSN is not past the fork point spans it (one case, spec 085 § Rules).
	return kind == BackupKind::Log && !fork_point_lsn.IsZero() && !first_lsn.IsZero() && first_lsn <= fork_point_lsn;
}

static Lsn ReadLsn(const ByteReader &r, size_t at) {
	Lsn lsn;
	lsn.vlf = r.U32(at);
	lsn.block = r.U32(at + 4);
	lsn.slot = r.U16(at + 8);
	return lsn;
}

static Guid ReadGuid(const ByteReader &r, size_t at) {
	Guid guid;
	const uint8_t *bytes = r.Bytes(at, 16);
	for (size_t i = 0; i < 16; i++) {
		guid.bytes[i] = bytes[i];
	}
	return guid;
}

static MtfDate ReadMtfDate(const ByteReader &r, size_t at) {
	const uint8_t *b = r.Bytes(at, 5);
	uint64_t v = 0;
	for (size_t i = 0; i < 5; i++) {
		v = (v << 8) | b[i];
	}
	MtfDate date;
	date.second = static_cast<int>(v & 0x3F);
	v >>= 6;
	date.minute = static_cast<int>(v & 0x3F);
	v >>= 6;
	date.hour = static_cast<int>(v & 0x1F);
	v >>= 5;
	date.day = static_cast<int>(v & 0x1F);
	v >>= 5;
	date.month = static_cast<int>(v & 0x0F);
	v >>= 4;
	date.year = static_cast<int>(v & 0x3FFF);
	return date;
}

//! An MTF tape address at `at` of a block: u16 size, u16 offset from the block's start.
static std::string ReadTapeString(const ByteReader &block, size_t at, uint8_t string_type) {
	const uint16_t size = block.U16(at);
	const uint16_t offset = block.U16(at + 2);
	if (size == 0) {
		return std::string();
	}
	const uint8_t *bytes = block.Bytes(offset, size);
	if (string_type == 2) {
		return Utf16LeToUtf8(bytes, size);
	}
	if (string_type != 1) {
		block.Fail(at, "a string of type " + std::to_string(string_type) +
						   ", neither ANSI (1) nor UTF-16 (2): a string type the reader does not know");
	}
	// Type 1: ANSI, read as Windows-1252 (its 0x80-0x9F are not Latin-1's C1 controls), trailing
	// NULs dropped; the five bytes 1252 leaves unassigned become U+FFFD.
	static const uint16_t CP1252_80[32] = {0x20AC, 0xFFFD, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
										   0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0xFFFD, 0x017D, 0xFFFD,
										   0xFFFD, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
										   0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0xFFFD, 0x017E, 0x0178};
	std::string out;
	size_t n = size;
	while (n > 0 && bytes[n - 1] == 0) {
		n--;
	}
	for (size_t i = 0; i < n; i++) {
		const uint8_t c = bytes[i];
		uint8_t units[2];
		const uint32_t cp = (c >= 0x80 && c < 0xA0) ? CP1252_80[c - 0x80] : c;
		units[0] = static_cast<uint8_t>(cp & 0xFF);
		units[1] = static_cast<uint8_t>(cp >> 8);
		out += Utf16LeToUtf8(units, 2);
	}
	return out;
}

//! A UTF-16LE string of a trailer record: a descriptor (u16 byte length, u16 offset) at `at`.
static std::string ReadRecordString(const ByteReader &record, size_t at) {
	const uint16_t size = record.U16(at);
	const uint16_t offset = record.U16(at + 2);
	if (size == 0) {
		return std::string();
	}
	return Utf16LeToUtf8(record.Bytes(offset, size), size);
}

static const MtfBlock &Require(const RandomAccess &file, const MtfLayout &layout, const char *type) {
	auto block = layout.Find(type);
	if (!block) {
		throw FormatError("backup file \"" + file.Name() + "\" has no " + type + " block");
	}
	return *block;
}

//! The MQCI stream of `block` (the trailer's MSLS, or the MSCI before the data) into `set`: its SFGI
//! and SFIN records, and in the trailer (`trailer`) its SCIN, which it must hold.
static void ReadTrailer(const RandomAccess &file, const MtfBlock &block, BackupSetHeader &set, bool trailer = true) {
	auto stream = block.Stream("MQCI");
	if (!stream) {
		throw FormatError("backup file \"" + file.Name() + "\": its " + block.type + " block carries no MQCI stream");
	}
	auto data = ReadStreamData(file, *stream, MAX_HEADER_STREAM);
	ByteReader mqci(data.data(), data.size(), trailer ? "the trailer MQCI stream" : "the MSCI block's MQCI stream",
					stream->data_offset);
	size_t at = 0;
	std::vector<std::pair<uint32_t, std::string>> filegroups;
	bool scin = false;
	while (at < mqci.Size()) {
		const std::string tag(reinterpret_cast<const char *>(mqci.Bytes(at, 4)), 4);
		const uint32_t length = mqci.U32(at + 4);
		if (length < 8) {
			mqci.Fail(at, "a " + tag + " record of " + std::to_string(length) + " bytes");
		}
		auto record = mqci.Sub(at, length, "a trailer record");
		if (tag == "SCIN" && trailer) {
			if (length < SCIN_MIN) {
				mqci.Fail(at, "a SCIN record of " + std::to_string(length) + " bytes, shorter than its fixed part");
			}
			if (length >= SCIN_COMPRESSED_SIZE_AT + 8) {
				set.compressed_backup_size = record.U64(SCIN_COMPRESSED_SIZE_AT);
				set.has_compressed_backup_size = true;
			}
			set.server_version = record.U32(24);
			set.compatibility_level = record.U8(91);
			set.database_version = record.U16(94);
			set.recorded_backup_size = record.U64(96);
			set.collation_info = record.U32(216);
			set.collation_sort_id = record.U8(220);
			set.database_name = ReadRecordString(record, 104);
			set.server_name = ReadRecordString(record, 108);
			set.first_lsn = ReadLsn(record, 32);
			set.last_lsn = ReadLsn(record, 42);
			set.checkpoint_lsn = ReadLsn(record, 52);
			set.database_backup_lsn = ReadLsn(record, 62);
			set.database_creation_date = ReadMtfDate(record, 72);
			set.backup_start_date = ReadMtfDate(record, 77);
			set.backup_finish_date = ReadMtfDate(record, 82);
			set.binding_id = ReadGuid(record, 124);
			set.recovery_fork_id = ReadGuid(record, 156);
			set.fork_point_lsn = ReadLsn(record, 172);
			set.parent_fork_id = ReadGuid(record, 184);
			set.family_guid = ReadGuid(record, 224);
			const uint8_t model = record.U8(244);
			set.recovery_model = (model & 0x08)	  ? RecoveryModel::Simple
								 : (model & 0x04) ? RecoveryModel::BulkLogged
												  : RecoveryModel::Full;
			if (set.kind == BackupKind::Differential) {
				set.differential_base_guid = ReadGuid(record, 200);
				set.differential_base_lsn = ReadLsn(record, 260);
			}
			scin = true;
		} else if (tag == "SFGI") {
			filegroups.emplace_back(record.U32(12), ReadRecordString(record, 16));
		} else if (tag == "SFIN") {
			if (length < SFIN_MIN) {
				mqci.Fail(at, "an SFIN record of " + std::to_string(length) + " bytes, shorter than its fixed part");
			}
			DatabaseFile f;
			// Flags at +8: 0x20 on a file emptied and removed in the files fixture (2022); a hint, not
			// a fact (DatabaseFile::marked_removed).
			f.marked_removed = (record.U32(8) & SFIN_REMOVED) != 0;
			f.file_id = record.U32(20);
			const uint32_t type = record.U32(28);
			if (type != 1 && type != 2) {
				record.Fail(28, "file " + std::to_string(f.file_id) + " has type " + std::to_string(type) +
									", neither data (1) nor log (2): a file type the reader does not know");
			}
			f.type = type == 1 ? 'D' : 'L';
			f.filegroup_id = record.U32(36);
			f.size_bytes = record.U64(40);
			f.logical_name = ReadRecordString(record, 48);
			f.physical_name = ReadRecordString(record, 52);
			f.max_size_bytes = record.U64(86);
			f.create_lsn = ReadLsn(record, 138);
			f.unique_id = ReadGuid(record, 186);
			set.files.push_back(std::move(f));
		}
		at += length;
	}
	if (trailer && !scin) {
		throw FormatError("backup file \"" + file.Name() + "\": its trailer MQCI stream has no SCIN record");
	}
	for (auto &f : set.files) {
		for (const auto &fg : filegroups) {
			if (f.filegroup_id != 0 && fg.first == f.filegroup_id) {
				f.filegroup_name = fg.second;
			}
		}
	}
}

void ReadMediaFields(const RandomAccess &file, const MtfLayout &layout, BackupSetHeader &set) {
	const auto &tape = Require(file, layout, "TAPE");
	ByteReader tape_header(tape.header.data(), tape.header.size(), "the TAPE block", tape.offset);
	set.media_family_id = tape_header.U32(52);
	if (auto raid = tape.Stream("RAID")) {
		auto data = ReadStreamData(file, *raid, MAX_RAID_STREAM);
		ByteReader r(data.data(), data.size(), "the TAPE block's RAID stream", raid->data_offset);
		set.media_set_guid = ReadGuid(r, 0);
		// u16, as E2 checked against LABELONLY: the u16 after it is 1 in every sample; read
		// as one u32 the two said 65537 for a single family.
		set.family_count = r.U16(20);
		set.family_sequence_number = r.U16(24);
	}
}

BackupSetHeader ReadBackupSet(const RandomAccess &file, const MtfLayout &layout) {
	BackupSetHeader set;
	set.soft_filemark_bytes = layout.soft_filemark_bytes;

	// A compressed container's data set has no TAPE: its media fields are read from the
	// container's header (ReadContainerTape, ReadMediaFields) by the caller.
	if (layout.has_tape) {
		ReadMediaFields(file, layout, set);
	}

	const auto &sset = Require(file, layout, "SSET");
	ByteReader s(sset.header.data(), sset.header.size(), "the SSET block", sset.offset);
	set.sset_attributes = s.U32(52);
	// 0x8 marks a differential; a full and a log backup both carry 0x4, and a log backup is the one
	// with no data stream (no MSDA block, spec 085 § Rules, MTF).
	bool has_log_stream = false;
	for (const auto &block : layout.blocks) {
		has_log_stream = has_log_stream || block.Stream("MQTL") != nullptr;
	}
	if (set.sset_attributes & 0x8) {
		set.kind = BackupKind::Differential;
	} else if (layout.Find("MSDA")) {
		set.kind = BackupKind::Full;
	} else if (has_log_stream) {
		// No data stream, and a log stream: what a log backup is (its SSET carries 0x4 like a full).
		set.kind = BackupKind::Log;
	} else {
		throw FormatError("backup file \"" + file.Name() +
						  "\" carries neither a data stream (MSDA) nor a log stream (MQTL): not a data or log "
						  "backup the reader knows");
	}
	set.is_copy_only = (set.sset_attributes & 0x2) != 0;
	set.position = s.U16(62);
	set.backup_name = ReadTapeString(s, 64, sset.string_type);
	set.description = ReadTapeString(s, 68, sset.string_type);
	set.user_name = ReadTapeString(s, 76, sset.string_type);
	set.software_major = s.U8(93);
	set.software_minor = s.U8(94);
	set.backup_set_guid = ReadGuid(s, 100);

	if (auto volb = layout.Find("VOLB")) {
		ByteReader v(volb->header.data(), volb->header.size(), "the VOLB block", volb->offset);
		set.machine_name = ReadTapeString(v, 64, volb->string_type);
	}

	const auto &msls = Require(file, layout, "MSLS");
	if (!layout.has_tape) {
		// A compressed container's soft filemark is 4 KB (its plain SFMB at 0x1000, the chain at
		// 0x2000): BackupSize is the MSLS offset in the data set plus it, the same rule as a
		// family file's (MSLS - SSET + filemark, SSET at two filemarks). Not the data set's length:
		// a busy backup's MSLS block is longer than 4 KB (E3 r1: 6 KB).
		set.backup_size = msls.offset + CONTAINER_SOFT_FILEMARK;
	} else {
		if (msls.offset < layout.soft_filemark_bytes) {
			throw FormatError("backup file \"" + file.Name() + "\": its MSLS block lies inside the first filemark");
		}
		set.backup_size = msls.offset - layout.soft_filemark_bytes;
	}
	ReadTrailer(file, msls, set);
	// The files as the backup began: what FILELISTONLY lists. Their sizes are the start's; the
	// trailer's are the end's, larger once a file grew while the backup ran (load fixture: 46080 pages
	// at the start, 47104 at the end, FILELISTONLY 46080).
	if (auto msci = layout.Find("MSCI")) {
		if (msci->Stream("MQCI")) {
			BackupSetHeader start;
			ReadTrailer(file, *msci, start, false);
			set.start_files = std::move(start.files);
		}
	}

	for (const auto &block : layout.blocks) {
		for (const auto &stream : block.streams) {
			if (stream.id == "MQTL" && stream.length > 2) {
				// The log's bytes: a stream's 2-byte lead-in is not log (§ Rules, MTF).
				set.log_tail_bytes += stream.length - 2;
			}
		}
	}
	return set;
}

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb
