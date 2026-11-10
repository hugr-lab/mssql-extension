// Test helper for spec 085's backup tests: a synthetic MTF image with SQL Server's block
// sequence (TAPE + RAID, SFMB, SSET, VOLB, MSDA, MSTL, MSLS with the trailer MQCI, SFMB), at the
// offsets located against HEADERONLY / FILELISTONLY (spec 085 § Rules, header fields).

#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace mtf_builder {

using Bytes = std::vector<uint8_t>;

inline void Put16(Bytes &b, size_t at, uint16_t v) {
	b[at] = v & 0xFF;
	b[at + 1] = v >> 8;
}
inline void Put32(Bytes &b, size_t at, uint32_t v) {
	for (int i = 0; i < 4; i++) {
		b[at + i] = (v >> (8 * i)) & 0xFF;
	}
}
//! A data file's header record (page 0's slot 0) as SQL Server lays it out: a primary FixedVar
//! record (statusA 0x30) with no fixed columns, a null bitmap, and five variable columns -- a
//! `first_var`-byte value (a GUID on a server: 16), an empty one, the file id (u16), its filegroup
//! (u16) and its Size in pages (u32), the columns ReadFileHeaderRecord reads by number. Returns its
//! bytes; place them at a slot's offset.
inline Bytes FileHeaderRecordBytes(uint16_t file, uint32_t size, uint16_t first_var = 16) {
	const uint16_t columns = 5;
	const uint16_t data = 4 + 2 + 1 + 2 + 2 * columns;	// header, column count, bitmap, var count, ends
	const uint16_t ends[columns] = {static_cast<uint16_t>(data + first_var), static_cast<uint16_t>(data + first_var),
									static_cast<uint16_t>(data + first_var + 2),
									static_cast<uint16_t>(data + first_var + 4),
									static_cast<uint16_t>(data + first_var + 8)};
	Bytes r(ends[columns - 1], 0);
	r[0] = 0x30;
	r[2] = 4;  // fixed part ends at 4: no fixed columns
	r[4] = columns;
	r[7] = columns;	 // variable column count (the null bitmap's one byte at 6 is zero)
	for (uint16_t i = 0; i < columns; i++) {
		r[9 + 2 * i] = static_cast<uint8_t>(ends[i] & 0xFF);
		r[10 + 2 * i] = static_cast<uint8_t>(ends[i] >> 8);
	}
	const size_t at = ends[1];
	r[at] = static_cast<uint8_t>(file & 0xFF);
	r[at + 1] = static_cast<uint8_t>(file >> 8);
	r[at + 2] = 1;	// PRIMARY
	for (int i = 0; i < 4; i++) {
		r[at + 4 + i] = static_cast<uint8_t>((size >> (8 * i)) & 0xFF);
	}
	return r;
}

inline void Put64(Bytes &b, size_t at, uint64_t v) {
	for (int i = 0; i < 8; i++) {
		b[at + i] = (v >> (8 * i)) & 0xFF;
	}
}
inline void PutLsn(Bytes &b, size_t at, uint32_t vlf, uint32_t block, uint16_t slot) {
	Put32(b, at, vlf);
	Put32(b, at + 4, block);
	Put16(b, at + 8, slot);
}
inline void PutGuid(Bytes &b, size_t at, uint8_t seed) {
	for (int i = 0; i < 16; i++) {
		b[at + i] = static_cast<uint8_t>(seed + i);
	}
}
inline void PutTag(Bytes &b, size_t at, const char *tag) {
	std::memcpy(&b[at], tag, 4);
}
inline Bytes Utf16(const std::string &ascii) {
	Bytes out;
	for (char c : ascii) {
		out.push_back(static_cast<uint8_t>(c));
		out.push_back(0);
	}
	return out;
}
inline uint16_t Xor16(const Bytes &b, size_t at, size_t count) {
	uint16_t sum = 0;
	for (size_t i = 0; i < count; i += 2) {
		sum ^= static_cast<uint16_t>(b[at + i] | (b[at + i + 1] << 8));
	}
	return sum;
}
//! An MTF date: 14 bits year, 4 month, 5 day, 5 hour, 6 minute, 6 second, big-endian.
inline void PutDate(Bytes &b, size_t at, int y, int mo, int d, int h, int mi, int s) {
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
inline void AppendBlock(Bytes &image, const char *type, Bytes header, const std::vector<Stream> &streams,
						size_t pad_to) {
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
	// SPAD: its data fills to pad_to (or is empty; pad_to 1 aligns the block's end to 512 bytes).
	size_t used = image.size() - start + 22;
	if (pad_to == 1) {
		pad_to = (used + 511) / 512 * 512;
	}
	size_t spad = pad_to > used ? pad_to - used : 0;
	add_stream("SPAD", Bytes(spad, 0), 0);
}
inline void AppendSfmb(Bytes &image, size_t soft_filemark) {
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
	//! Pad MSDA / MSTL / MSLS to 512 bytes, as a compressed container's data set needs (its
	//! blocks' outputs are 512-byte units).
	bool align_blocks = false;
	//! The MQDA stream's bytes (its 2-byte lead-in and the pages).
	size_t mqda_bytes = 8194;
	//! Pad the MSLS block to this many bytes (0: as align_blocks says); a busy backup's MSLS is
	//! longer than 4 KB (E3 r1: 6 KB).
	size_t msls_pad_to = 0;
	bool forked = false;
	uint32_t sfin_type = 1;
	//! This file's FamilySequenceNumber in the RAID stream (the media set has two families).
	uint16_t family_sequence = 1;
	//! The MSDA block's header bytes (52 common + its specific part): a larger header moves the MQDA
	//! data, which is how a seed puts the pass's units on a block boundary of a container.
	size_t msda_header = 60;
	//! The MQDA stream's bytes, when not empty (else mqda_bytes of 0x5A).
	Bytes mqda_content;
};

inline Bytes BuildImage(const Options &o) {
	Bytes image;
	// TAPE: soft filemark 8 × 512 = 4096 at +64; a RAID stream (media set, family count 2, seq 1).
	Bytes tape(96, 0);
	Put32(tape, 52, 0xA010F2F3u);
	Put16(tape, 64, o.soft_filemark_units);
	Bytes raid(32, 0);
	PutGuid(raid, 0, 0x10);
	Put16(raid, 20, 2);
	Put16(raid, 22, 1);	 // as every sample has it: a u32 read of +20 would say 65538
	Put16(raid, 24, o.family_sequence);
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
		const Bytes mqda = o.mqda_content.empty() ? Bytes(o.mqda_bytes, 0x5A) : o.mqda_content;
		AppendBlock(image, "MSDA", Bytes(o.msda_header, 0), {{"MQDA", mqda, 0x2}}, o.align_blocks ? 1 : 0);
	}
	if (o.with_log) {
		AppendBlock(image, "MSTL", Bytes(596, 0), {{"MQTL", Bytes(4098, 0x11), 0}}, o.align_blocks ? 1 : 0);
	}
	// The trailer MQCI: SCIN (fixed part 270+), SFGI PRIMARY, SFIN for one data file.
	// Filled at full size, then cut to scin_size (a test of a short SCIN), so no field is
	// written past the buffer.
	Bytes scin(o.scin_size > 300 ? o.scin_size : 300, 0);
	PutTag(scin, 0, "SCIN");
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
	{
		// DatabaseName and ServerName: descriptors at +104 / +108, the strings past the fixed part.
		Bytes db = Utf16("salesdb");
		Bytes server = Utf16("srv1");
		Put16(scin, 104, static_cast<uint16_t>(db.size()));
		Put16(scin, 106, 272);
		std::memcpy(&scin[272], db.data(), db.size());
		Put16(scin, 108, static_cast<uint16_t>(server.size()));
		Put16(scin, 110, static_cast<uint16_t>(272 + db.size()));
		std::memcpy(&scin[272 + db.size()], server.data(), server.size());
	}
	scin.resize(o.scin_size);
	Put32(scin, 4, static_cast<uint32_t>(scin.size()));
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
	AppendBlock(image, "MSLS", Bytes(56, 0), {{"MQCI", mqci}},
				o.msls_pad_to ? o.msls_pad_to : (o.align_blocks ? 1 : 0));
	AppendSfmb(image, soft_filemark);
	return image;
}

}  // namespace mtf_builder
