// Spec 085 task 3: WalkMtf and ReadBackupSet over a synthetic MTF image built in memory.
//
// The image has SQL Server's block sequence (TAPE + RAID, SFMB, SSET, VOLB, MSDA, MSLS with the
// trailer MQCI: SCIN, SFGI, SFIN, then SFMB), so the walk, every located field (spec 085 § Rules,
// header fields), the backup kind, the fork rule and the refusals run in CI with no sample file.
// The offsets were located against HEADERONLY / FILELISTONLY on SQL Server 2025 (task 3).

#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "backup/backup_bytes.hpp"
#include "backup/backup_mtf.hpp"
#include "backup/backup_set.hpp"

using namespace duckdb::mssql::backup;

namespace {

int failures = 0;

void Check(bool ok, const std::string &what) {
	std::cout << (ok ? "  ok   " : "  FAIL ") << what << std::endl;
	if (!ok) {
		failures++;
	}
}

using Bytes = std::vector<uint8_t>;

void Put16(Bytes &b, size_t at, uint16_t v) {
	b[at] = v & 0xFF;
	b[at + 1] = v >> 8;
}
void Put32(Bytes &b, size_t at, uint32_t v) {
	for (int i = 0; i < 4; i++) {
		b[at + i] = (v >> (8 * i)) & 0xFF;
	}
}
void Put64(Bytes &b, size_t at, uint64_t v) {
	for (int i = 0; i < 8; i++) {
		b[at + i] = (v >> (8 * i)) & 0xFF;
	}
}
void PutLsn(Bytes &b, size_t at, uint32_t vlf, uint32_t block, uint16_t slot) {
	Put32(b, at, vlf);
	Put32(b, at + 4, block);
	Put16(b, at + 8, slot);
}
void PutGuid(Bytes &b, size_t at, uint8_t seed) {
	for (int i = 0; i < 16; i++) {
		b[at + i] = static_cast<uint8_t>(seed + i);
	}
}
void PutTag(Bytes &b, size_t at, const char *tag) {
	std::memcpy(&b[at], tag, 4);
}
Bytes Utf16(const std::string &ascii) {
	Bytes out;
	for (char c : ascii) {
		out.push_back(static_cast<uint8_t>(c));
		out.push_back(0);
	}
	return out;
}
uint16_t Xor16(const Bytes &b, size_t at, size_t count) {
	uint16_t sum = 0;
	for (size_t i = 0; i < count; i += 2) {
		sum ^= static_cast<uint16_t>(b[at + i] | (b[at + i + 1] << 8));
	}
	return sum;
}
//! An MTF date: 14 bits year, 4 month, 5 day, 5 hour, 6 minute, 6 second, big-endian.
void PutDate(Bytes &b, size_t at, int y, int mo, int d, int h, int mi, int s) {
	uint64_t v = static_cast<uint64_t>(y);
	v = (v << 4) | mo;
	v = (v << 5) | d;
	v = (v << 5) | h;
	v = (v << 6) | mi;
	v = (v << 6) | s;
	for (int i = 4; i >= 0; i--) {
		b[at + i] = v & 0xFF;
		v >>= 8;
	}
}

//! Appends a DBLK of `type` whose block-specific part is `specific` (from +52), its strings at the
//! end of the header, then its streams and an SPAD that pads the block to `pad_to` bytes (0: none).
struct Stream {
	std::string id;
	Bytes data;
	uint16_t media_attributes = 0;
};
void AppendBlock(Bytes &image, const char *type, Bytes header, const std::vector<Stream> &streams, size_t pad_to) {
	const size_t start = image.size();
	PutTag(header, 0, type);
	Put16(header, 8, static_cast<uint16_t>(header.size()));
	if (header[48] == 0) {
		header[48] = 2;	 // UTF-16 strings unless the caller set another type
	}
	Put16(header, 50, Xor16(header, 0, 50));
	image.insert(image.end(), header.begin(), header.end());
	auto add_stream = [&](const std::string &id, const Bytes &data, uint16_t media) {
		Bytes h(22, 0);
		PutTag(h, 0, id.c_str());
		Put16(h, 6, media);
		Put64(h, 8, data.size());
		Put16(h, 20, Xor16(h, 0, 20));
		image.insert(image.end(), h.begin(), h.end());
		image.insert(image.end(), data.begin(), data.end());
		while (image.size() % 4) {
			image.push_back(0);
		}
	};
	for (const auto &s : streams) {
		add_stream(s.id, s.data, s.media_attributes);
	}
	// SPAD: its data fills to pad_to (or is empty).
	size_t used = image.size() - start + 22;
	size_t spad = pad_to > used ? pad_to - used : 0;
	add_stream("SPAD", Bytes(spad, 0), 0);
}
void AppendSfmb(Bytes &image, size_t soft_filemark) {
	Bytes header(soft_filemark, 0);
	PutTag(header, 0, "SFMB");
	Put32(header, 4, 0x80000000u);
	Put16(header, 8, 0);  // no streams
	Put16(header, 50, Xor16(header, 0, 50));
	image.insert(image.end(), header.begin(), header.end());
}

struct Options {
	uint32_t sset_attributes = 0x4;
	bool with_msda = true;
	bool with_log = true;
	uint8_t string_type = 2;
	size_t scin_size = 300;
	uint16_t sfgi_name_offset = 224;
	uint16_t soft_filemark_units = 8;
	bool forked = false;
	uint32_t sfin_type = 1;
};

Bytes BuildImage(const Options &o) {
	Bytes image;
	// TAPE: soft filemark 8 × 512 = 4096 at +64; a RAID stream (media set, family count 2, seq 1).
	Bytes tape(96, 0);
	Put32(tape, 52, 0xA010F2F3u);
	Put16(tape, 64, o.soft_filemark_units);
	Bytes raid(32, 0);
	PutGuid(raid, 0, 0x10);
	Put32(raid, 20, 2);
	Put32(raid, 24, 1);
	const size_t soft_filemark = static_cast<size_t>(o.soft_filemark_units) * 512;
	AppendBlock(image, "TAPE", tape, {{"RAID", raid}}, soft_filemark);
	AppendSfmb(image, soft_filemark);
	// SSET: attributes, data set number, three strings, the software version, the backup set GUID.
	Bytes sset(128, 0);
	sset[48] = o.string_type;
	Put32(sset, 52, o.sset_attributes);
	Put16(sset, 62, 1);
	sset[93] = 17;
	sset[94] = 0;
	PutGuid(sset, 100, 0x20);
	const bool ansi = o.string_type == 1;
	auto put_string = [ansi](Bytes &h, size_t at, const std::string &s) {
		Bytes u = ansi ? Bytes(s.begin(), s.end()) : Utf16(s);
		Put16(h, at, static_cast<uint16_t>(u.size()));
		Put16(h, at + 2, static_cast<uint16_t>(h.size()));
		h.insert(h.end(), u.begin(), u.end());
	};
	put_string(sset, 64, ansi ? std::string("night\x80") : std::string("nightly"));
	put_string(sset, 68, "a description");
	put_string(sset, 76, "sa");
	AppendBlock(image, "SSET", sset, {}, 1024);
	Bytes volb(80, 0);
	volb[48] = o.string_type;
	put_string(volb, 64, "host-1");
	AppendBlock(image, "VOLB", volb, {}, 1024);
	if (o.with_msda) {
		AppendBlock(image, "MSDA", Bytes(60, 0), {{"MQDA", Bytes(8194, 0x5A), 0x2}}, 0);
	}
	if (o.with_log) {
		AppendBlock(image, "MSTL", Bytes(596, 0), {{"MQTL", Bytes(4098, 0x11), 0}}, 0);
	}
	// The trailer MQCI: SCIN (fixed part 270+), SFGI PRIMARY, SFIN for one data file.
	Bytes scin(o.scin_size, 0);
	PutTag(scin, 0, "SCIN");
	Put32(scin, 4, static_cast<uint32_t>(scin.size()));
	PutLsn(scin, 32, 47, 400, 1);
	PutLsn(scin, 42, 47, 480, 1);
	PutLsn(scin, 52, 47, 400, 1);
	PutLsn(scin, 62, 46, 100, 1);
	PutDate(scin, 72, 2026, 11, 9, 8, 40, 35);
	PutDate(scin, 77, 2026, 11, 9, 8, 40, 38);
	PutDate(scin, 82, 2026, 11, 9, 8, 40, 39);
	PutGuid(scin, 124, 0x30);
	PutGuid(scin, 156, 0x40);
	if (o.forked) {
		PutLsn(scin, 172, 47, 400, 1);
		PutGuid(scin, 184, 0x50);
	}
	PutGuid(scin, 200, 0x60);
	PutGuid(scin, 224, 0x70);
	scin[244] = 0x08;
	PutLsn(scin, 260, 46, 100, 1);
	Bytes sfgi(240, 0);
	PutTag(sfgi, 0, "SFGI");
	Put32(sfgi, 4, static_cast<uint32_t>(sfgi.size()));
	Put32(sfgi, 12, 1);
	{
		Bytes u = Utf16("PRIMARY");
		Put16(sfgi, 16, static_cast<uint16_t>(u.size()));
		Put16(sfgi, 18, o.sfgi_name_offset);
		if (o.sfgi_name_offset + u.size() <= sfgi.size()) {
			std::memcpy(&sfgi[o.sfgi_name_offset], u.data(), u.size());
		}
	}
	Bytes sfin(260, 0);
	PutTag(sfin, 0, "SFIN");
	Put32(sfin, 20, 1);
	Put32(sfin, 28, o.sfin_type);
	Put32(sfin, 36, 1);
	Put64(sfin, 40, 16777216);
	Put64(sfin, 86, 209715200);
	PutLsn(sfin, 138, 0, 0, 0);
	PutGuid(sfin, 186, 0x80);
	{
		Bytes logical = Utf16("db_data");
		Bytes physical = Utf16("/data/db.mdf");
		Put16(sfin, 48, static_cast<uint16_t>(logical.size()));
		Put16(sfin, 50, static_cast<uint16_t>(sfin.size()));
		sfin.insert(sfin.end(), logical.begin(), logical.end());
		Put16(sfin, 52, static_cast<uint16_t>(physical.size()));
		Put16(sfin, 54, static_cast<uint16_t>(sfin.size()));
		sfin.insert(sfin.end(), physical.begin(), physical.end());
		Put32(sfin, 4, static_cast<uint32_t>(sfin.size()));
	}
	Bytes mqci;
	mqci.insert(mqci.end(), scin.begin(), scin.end());
	mqci.insert(mqci.end(), sfgi.begin(), sfgi.end());
	mqci.insert(mqci.end(), sfin.begin(), sfin.end());
	AppendBlock(image, "MSLS", Bytes(56, 0), {{"MQCI", mqci}}, 0);
	AppendSfmb(image, soft_filemark);
	return image;
}

template <class F>
std::string ErrorOf(F f) {
	try {
		f();
	} catch (FormatError &e) {
		return e.what();
	}
	return "";
}

std::string Hex(const Guid &g) {
	return g.ToString();
}

}  // namespace

int main() {
	std::cout << "WalkMtf / ReadBackupSet (spec 085 task 3)" << std::endl;

	{
		Options o;
		auto image = BuildImage(o);
		MemoryAccess file("synthetic.bak", image.data(), image.size());
		auto layout = WalkMtf(file);
		std::string types;
		for (auto &b : layout.blocks) {
			types += b.type + " ";
		}
		Check(types == "TAPE SFMB SSET VOLB MSDA MSTL MSLS SFMB " && layout.blocks.back().end == image.size(),
			  "the walk finds every block and ends exactly at the end: " + types);
		Check(layout.soft_filemark_bytes == 4096, "the soft filemark size comes from TAPE");
		auto set = ReadBackupSet(file, layout);
		Check(set.kind == BackupKind::Full && !set.is_copy_only && set.position == 1, "a full backup, position 1");
		Check(set.backup_name == "nightly" && set.description == "a description" && set.user_name == "sa" &&
				  set.machine_name == "host-1",
			  "the SSET and VOLB strings");
		Check(set.family_count == 2 && set.family_sequence_number == 1 && set.media_family_id == 0xA010F2F3u,
			  "the RAID stream: family 1 of 2");
		Check(set.backup_set_guid.ToString() == "23222120-2524-2726-2829-2a2b2c2d2e2f",
			  "the backup set GUID's byte order");
		Check(set.first_lsn.ToDecimal() == "47000000040000001" && set.last_lsn.ToDecimal() == "47000000048000001" &&
				  set.database_backup_lsn.ToDecimal() == "46000000010000001",
			  "the LSNs in HEADERONLY's form");
		Check(set.backup_start_date.ToString() == "2026-11-09 08:40:38" &&
				  set.backup_finish_date.ToString() == "2026-11-09 08:40:39" &&
				  set.database_creation_date.ToString() == "2026-11-09 08:40:35",
			  "the three MTF dates");
		Check(set.recovery_model == RecoveryModel::Simple, "the recovery model bit 0x08 is SIMPLE");
		Check(set.differential_base_lsn.IsZero() && set.differential_base_guid.IsZero(),
			  "a full reports no differential base");
		Check(!set.HasForkPoint() && Hex(set.FirstRecoveryForkId()) == Hex(set.recovery_fork_id),
			  "no fork: FirstRecoveryForkID is the current fork");
		Check(set.files.size() == 1 && set.files[0].logical_name == "db_data" &&
				  set.files[0].physical_name == "/data/db.mdf" && set.files[0].type == 'D' &&
				  set.files[0].filegroup_name == "PRIMARY" && set.files[0].size_bytes == 16777216 &&
				  set.files[0].max_size_bytes == 209715200,
			  "the file list from SFIN and SFGI");
		const auto msls = layout.Find("MSLS");
		Check(msls && set.backup_size == msls->offset - 4096, "BackupSize is MSLS's offset less one soft filemark");
	}
	{
		Options o;
		o.sset_attributes = 0x8;
		auto image = BuildImage(o);
		MemoryAccess file("diff.bak", image.data(), image.size());
		auto set = ReadBackupSet(file, WalkMtf(file));
		Check(set.kind == BackupKind::Differential && set.differential_base_lsn.ToDecimal() == "46000000010000001" &&
				  !set.differential_base_guid.IsZero(),
			  "a differential reports its base from SCIN +260 / +200");
	}
	{
		Options o;
		o.with_msda = false;
		o.forked = true;
		auto image = BuildImage(o);
		MemoryAccess file("log.trn", image.data(), image.size());
		auto set = ReadBackupSet(file, WalkMtf(file));
		Check(set.kind == BackupKind::Log, "a backup with no MSDA is a log backup (its SSET also carries 0x4)");
		Check(set.HasForkPoint() && Hex(set.FirstRecoveryForkId()) == Hex(set.parent_fork_id) &&
				  set.fork_point_lsn.ToDecimal() == "47000000040000001",
			  "a log backup whose first LSN is the fork point spans the fork");
	}
	{
		Options o;
		o.forked = true;
		auto image = BuildImage(o);
		MemoryAccess file("restored_full.bak", image.data(), image.size());
		auto set = ReadBackupSet(file, WalkMtf(file));
		Check(!set.HasForkPoint() && Hex(set.FirstRecoveryForkId()) == Hex(set.recovery_fork_id),
			  "a full after a restore carries a fork point but reports none");
	}

	// Refusals.
	auto image = BuildImage(Options());
	{
		auto bad = image;
		bad[4096 + 50] ^= 0x01;	 // the SFMB's checksum
		MemoryAccess file("bad.bak", bad.data(), bad.size());
		auto e = ErrorOf([&]() { WalkMtf(file); });
		Check(e.find("bad.bak") != std::string::npos && e.find("offset 4096") != std::string::npos &&
				  e.find("after TAPE") != std::string::npos,
			  "a header whose checksum fails is refused, naming the file, the offset and the block before");
	}
	{
		auto bad = image;
		bad.resize(bad.size() - 100);
		MemoryAccess file("short.bak", bad.data(), bad.size());
		Check(!ErrorOf([&]() { WalkMtf(file); }).empty(), "a truncated file is refused");
	}
	{
		auto bad = image;
		// The MQDA stream's length: make it run past the end, and re-checksum the stream header.
		auto layout = WalkMtf(MemoryAccess("x", image.data(), image.size()));
		const auto *msda = layout.Find("MSDA");
		size_t at = static_cast<size_t>(msda->streams[0].header_offset);
		Put64(bad, at + 8, 1ULL << 40);
		Put16(bad, at + 20, Xor16(bad, at, 20));
		MemoryAccess file("long.bak", bad.data(), bad.size());
		auto e = ErrorOf([&]() { WalkMtf(file); });
		Check(e.find("MQDA") != std::string::npos && e.find("past the end") != std::string::npos,
			  "a stream longer than the file is refused by name");
	}
	{
		Bytes junk(8192);
		for (size_t i = 0; i < junk.size(); i++) {
			junk[i] = static_cast<uint8_t>(i * 7 + 3);
		}
		MemoryAccess file("junk.bak", junk.data(), junk.size());
		Check(ErrorOf([&]() { WalkMtf(file); }).find("not an MTF backup file") != std::string::npos,
			  "a file that is not MTF is refused as such");
		// 'AAAA…' passes the checksum (25 equal words XOR to the word itself): refused as not TAPE.
		Bytes letters(8192, 0x41);
		MemoryAccess lettered("letters.bak", letters.data(), letters.size());
		Check(ErrorOf([&]() { WalkMtf(lettered); }).find("is AAAA, not TAPE") != std::string::npos,
			  "a block that validates but is not TAPE first is refused");
	}
	{
		Options o;
		o.sfin_type = 3;
		auto bad = BuildImage(o);
		MemoryAccess file("filestream.bak", bad.data(), bad.size());
		auto e = ErrorOf([&]() { ReadBackupSet(file, WalkMtf(file)); });
		Check(e.find("type 3") != std::string::npos, "a file type the reader does not know is refused by name");
	}
	{
		Options o;
		o.with_msda = false;
		o.with_log = false;
		auto bad = BuildImage(o);
		MemoryAccess file("neither.bak", bad.data(), bad.size());
		Check(ErrorOf([&]() { ReadBackupSet(file, WalkMtf(file)); }).find("neither a data stream") != std::string::npos,
			  "a file with neither a data nor a log stream is refused, not called a log backup");
	}
	{
		Options o;
		o.scin_size = 200;
		auto bad = BuildImage(o);
		MemoryAccess file("short_scin.bak", bad.data(), bad.size());
		Check(ErrorOf([&]() { ReadBackupSet(file, WalkMtf(file)); }).find("shorter than its fixed part") !=
				  std::string::npos,
			  "a SCIN shorter than its fixed part is refused");
	}
	{
		Options o;
		o.sfgi_name_offset = 0xFFF0;
		auto bad = BuildImage(o);
		MemoryAccess file("bad_string.bak", bad.data(), bad.size());
		Check(ErrorOf([&]() { ReadBackupSet(file, WalkMtf(file)); }).find("runs past the end") != std::string::npos,
			  "a string descriptor pointing outside its record is refused");
	}
	{
		Options o;
		o.string_type = 1;
		auto image1 = BuildImage(o);
		MemoryAccess file("ansi.bak", image1.data(), image1.size());
		auto set = ReadBackupSet(file, WalkMtf(file));
		Check(set.backup_name == "night\xE2\x82\xAC" && set.machine_name == "host-1",
			  "an ANSI string is read as Windows-1252 (0x80 is the euro sign)");
		o.string_type = 7;
		auto image7 = BuildImage(o);
		MemoryAccess file7("type7.bak", image7.data(), image7.size());
		Check(ErrorOf([&]() { ReadBackupSet(file7, WalkMtf(file7)); }).find("string of type 7") != std::string::npos,
			  "an unknown string type is refused by name");
	}
	{
		// Crafted: a 512-byte filemark and an SFMB claiming a 64 KB header.
		Options o;
		o.soft_filemark_units = 1;
		auto bad = BuildImage(o);
		const size_t sfmb = 512;
		Put16(bad, sfmb + 8, 0xFFFF);
		Put16(bad, sfmb + 50, Xor16(bad, sfmb, 50));
		MemoryAccess file("sfmb.bak", bad.data(), bad.size());
		Check(ErrorOf([&]() { WalkMtf(file); }).find("past its own 512 bytes") != std::string::npos,
			  "an SFMB whose header claims more than its own length is refused");
	}
	{
		auto layout = WalkMtf(MemoryAccess("x", image.data(), image.size()));
		auto bad = image;
		size_t at = static_cast<size_t>(layout.Find("SSET")->offset);
		Put16(bad, at + 8, 20);	 // first_event inside the common header
		Put16(bad, at + 50, Xor16(bad, at, 50));
		MemoryAccess file("inside.bak", bad.data(), bad.size());
		Check(ErrorOf([&]() { WalkMtf(file); }).find("inside its own header") != std::string::npos,
			  "a first stream inside the block's own header is refused");
		auto bad2 = image;
		size_t stream = static_cast<size_t>(layout.Find("VOLB")->offset + layout.Find("VOLB")->first_event);
		bad2[stream + 20] ^= 0x01;
		MemoryAccess file2("stream.bak", bad2.data(), bad2.size());
		Check(ErrorOf([&]() { WalkMtf(file2); }).find("no valid stream header in the VOLB") != std::string::npos,
			  "a stream header whose checksum fails is refused, naming its block");
	}
	{
		Bytes sfmb_first(1024, 0);
		PutTag(sfmb_first, 0, "SFMB");
		Put16(sfmb_first, 50, Xor16(sfmb_first, 0, 50));
		MemoryAccess file("sfmb_first.bak", sfmb_first.data(), sfmb_first.size());
		Check(ErrorOf([&]() { WalkMtf(file); }).find("is SFMB, not TAPE") != std::string::npos,
			  "a file that starts with anything but TAPE is refused");
	}
	{
		Lsn lsn;
		lsn.vlf = 0;
		lsn.block = 12;
		lsn.slot = 3;
		Check(lsn.ToDecimal() == "1200003", "an LSN with VLF 0 has no leading zero");
		const uint8_t pair[] = {0x3D, 0xD8, 0x00, 0xDE, 0x41};
		Check(Utf16LeToUtf8(pair, 5) == "\xF0\x9F\x98\x80",
			  "a surrogate pair is one code point; an odd byte is dropped");
	}
	{
		Bytes padded = Utf16("abc");
		padded.push_back(0);
		padded.push_back(0);
		Check(Utf16LeToUtf8(padded.data(), padded.size()) == "abc", "UTF-16 strings drop trailing NULs");
		const uint8_t lone[] = {0x00, 0xD8, 0x41, 0x00};
		Check(Utf16LeToUtf8(lone, 4) ==
				  "\xEF\xBF\xBD"
				  "A",
			  "an unpaired surrogate becomes U+FFFD");
	}

	std::cout << (failures ? "FAILED: " + std::to_string(failures) + " check(s)" : "all checks passed") << std::endl;
	return failures ? 1 : 0;
}
