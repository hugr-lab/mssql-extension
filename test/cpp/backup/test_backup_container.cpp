// Spec 085 task 4: the MSSQLBAK container over a synthetic data set.
//
// The data set (SSET .. MSLS of mtf_builder's image) is packed into stored (0x23) blocks of a
// few 512-byte units, enough of them for several checkpoints (one per 64 blocks, D4a); the seek
// index names three of them. Checked: the chain walk, FindBlock from its checkpoint, the seek
// index (valid, and one that names no block), ContainerStream reads across blocks, the MTF walk
// of the data set and its BackupSize, and the refusals (a compressed block with no decoder, an
// unknown version or flag word, a payload past the end). XPRESS / ZSTD blocks are tasks 5 / 6.

#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "backup/backup_bytes.hpp"
#include "backup/backup_container.hpp"
#include "backup/backup_mtf.hpp"
#include "backup/backup_set.hpp"
#include "mtf_builder.hpp"

using namespace duckdb::mssql::backup;
using namespace mtf_builder;

namespace {

int failures = 0;

void Check(bool ok, const std::string &what) {
	std::cout << (ok ? "  ok   " : "  FAIL ") << what << std::endl;
	if (!ok) {
		failures++;
	}
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

//! The data set of a synthetic backup: its bytes from SSET to the end of MSLS.
Bytes DataSet(size_t msls_pad_to = 0) {
	Options o;
	o.align_blocks = true;
	o.msls_pad_to = msls_pad_to;
	o.mqda_bytes = 3000000;	 // ~6000 blocks of 512 bytes: three checkpoints by the 1 MiB rule
	auto image = BuildImage(o);
	MemoryAccess file("image", image.data(), image.size());
	auto layout = WalkMtf(file);
	const auto *sset = layout.Find("SSET");
	const auto *msls = layout.Find("MSLS");
	return Bytes(image.begin() + sset->offset, image.begin() + msls->end);
}

struct Packed {
	Bytes file;
	std::vector<uint64_t> block_physical;
	std::vector<uint64_t> block_logical;
};

//! The data set in a container of stored blocks of `units` × 512 bytes each (the last shorter),
//! with a seek index naming `index_blocks`.
Packed Pack(const Bytes &data, uint8_t units, const std::vector<size_t> &index_blocks, uint32_t flags = 0x3,
			bool xor_compress = false) {
	Packed p;
	p.file.assign(8192, 0);
	std::memcpy(&p.file[0], "MSSQLBAK", 8);
	Put32(p.file, 8, 2);
	Put32(p.file, 0x0C, flags);
	for (int i = 0; i < 20; i++) {
		p.file[0x18 + i] = static_cast<uint8_t>(0xA0 + i);
	}
	PutTag(p.file, 4096, "SFMB");
	Put16(p.file, 4096 + 50, Xor16(p.file, 4096, 50));
	size_t logical = 0;
	while (logical < data.size()) {
		size_t out = std::min<size_t>(static_cast<size_t>(units) * 512, data.size() - logical);
		Bytes header(28, 0);
		// "Compressed" blocks here are the bytes XOR 0x5A, which the test's decoder reverses.
		header[0] = xor_compress ? 0x21 : 0x23;
		header[1] = static_cast<uint8_t>(out / 512);
		Put16(header, 2, static_cast<uint16_t>(out == 65536 ? 0 : out));
		p.block_physical.push_back(p.file.size());
		p.block_logical.push_back(logical);
		p.file.insert(p.file.end(), header.begin(), header.end());
		for (size_t i = 0; i < out; i++) {
			p.file.push_back(xor_compress ? static_cast<uint8_t>(data[logical + i] ^ 0x5A) : data[logical + i]);
		}
		logical += out;
	}
	if (index_blocks.empty()) {
		p.file.insert(p.file.end(), 4096, 0);  // no seek index: what follows the chain is not one
		return p;
	}
	// The seek index: u32 44, u32 0x00010001 at +24, u32 count at +28, entries from +40.
	Bytes index(40 + 16 * index_blocks.size(), 0);
	Put32(index, 0, 44);
	Put32(index, 24, 0x00010001);
	Put32(index, 28, static_cast<uint32_t>(index_blocks.size()));
	for (size_t i = 0; i < index_blocks.size(); i++) {
		Put64(index, 40 + 16 * i, p.block_logical[index_blocks[i]]);
		Put64(index, 48 + 16 * i, p.block_physical[index_blocks[i]]);
	}
	p.file.insert(p.file.end(), index.begin(), index.end());
	p.file.insert(p.file.end(), 4096, 0);
	return p;
}

}  // namespace

int main() {
	std::cout << "MSSQLBAK container (spec 085 task 4)" << std::endl;
	const Bytes data = DataSet();
	Check(data.size() % 512 == 0, "the synthetic data set is a whole number of 512-byte units");

	auto packed = Pack(data, 1, {10, 70, 140});
	MemoryAccess file("synthetic_comp.bak", packed.file.data(), packed.file.size());
	Check(IsContainer(file), "the magic marks a container");
	auto header = ReadContainerHeader(file);
	Check(header.compressed && !header.encrypted && header.algorithm == CompressionAlgorithm::MsXpress,
		  "flags 0x3: compressed with MS_XPRESS");

	auto chain = WalkChain(file);
	Check(chain.block_count == packed.block_physical.size() && chain.logical_size == data.size(),
		  "the chain walk counts every block and the data set's length");
	bool spaced = chain.checkpoints.size() >= 2 && chain.checkpoints[0].logical == 0;
	for (size_t i = 1; i < chain.checkpoints.size(); i++) {
		spaced = spaced && chain.checkpoints[i].physical - chain.checkpoints[i - 1].physical >= (1u << 20);
	}
	Check(spaced, "checkpoints at least 64 blocks and 1 MiB apart (" + std::to_string(chain.checkpoints.size()) + ")");
	bool found_all = true;
	for (size_t i = 0; i < packed.block_logical.size(); i++) {
		const uint64_t first = packed.block_logical[i];
		const uint64_t last = (i + 1 < packed.block_logical.size() ? packed.block_logical[i + 1] : data.size()) - 1;
		auto a = FindBlock(file, chain, first);
		auto b = FindBlock(file, chain, last);
		found_all = found_all && a.physical == packed.block_physical[i] && b.physical == packed.block_physical[i];
	}
	Check(found_all, "FindBlock finds the first and the last byte of every block from its checkpoint");
	Check(!ErrorOf([&]() { FindBlock(file, chain, data.size()); }).empty(), "an offset past the data set is refused");

	auto index = ReadSeekIndex(file, chain);
	Check(index.size() == 3 && index[1].logical == 70 * 512 && index[1].physical == packed.block_physical[70],
		  "the seek index's three entries, each a block of the chain");

	{
		auto bad = packed;
		const size_t entry = chain.end + 48 + 16;  // the second entry's physical offset
		Put64(bad.file, entry, packed.block_physical[70] + 1);
		MemoryAccess f("bad_index.bak", bad.file.data(), bad.file.size());
		auto c = WalkChain(f);
		Check(ErrorOf([&]() { ReadSeekIndex(f, c); }).find("names no block") != std::string::npos,
			  "a seek index entry that names no block is refused");
	}

	ContainerStream stream(file, chain, BlockDecoder());
	{
		Bytes all(data.size());
		stream.ReadAt(0, all.data(), all.size());
		Bytes middle(1500);
		stream.ReadAt(1000, middle.data(), middle.size());
		Check(all == data && std::memcmp(middle.data(), data.data() + 1000, 1500) == 0,
			  "the stream returns the data set, across block boundaries");
	}
	{
		auto layout = WalkMtfDataSet(stream);
		Check(!layout.has_tape && layout.blocks.front().type == "SSET" && layout.blocks.back().type == "MSLS",
			  "the data set walks from SSET to MSLS");
		auto set = ReadBackupSet(stream, layout);
		Check(set.backup_size == layout.Find("MSLS")->offset + 4096 && set.backup_name == "nightly" &&
				  set.kind == BackupKind::Full,
			  "its backup set: BackupSize is the MSLS offset plus the container's 4 KB filemark");
		Check(ErrorOf([&]() { WalkMtf(stream); }).find("not TAPE") != std::string::npos,
			  "a data set is not a family file");
	}
	{
		// A busy backup's MSLS is longer than the 4 KB filemark (E3 r1: 6 KB): BackupSize is not the
		// data set's length, which runs 2 KB past it.
		const Bytes busy = DataSet(6144);
		auto bp = Pack(busy, 8, {});
		MemoryAccess bf("busy.bak", bp.file.data(), bp.file.size());
		ContainerStream bs(bf, WalkChain(bf), BlockDecoder());
		auto bl = WalkMtfDataSet(bs);
		auto bset = ReadBackupSet(bs, bl);
		Check(bl.Find("MSLS")->end - bl.Find("MSLS")->offset == 6144 && bset.backup_size == busy.size() - 2048,
			  "a 6 KB MSLS: BackupSize is 2 KB short of the data set's length, as HEADERONLY says of E3 r1");
	}
	{
		auto compressed = packed;
		compressed.file[packed.block_physical[3]] = 0x21;
		MemoryAccess f("compressed.bak", compressed.file.data(), compressed.file.size());
		ContainerStream s(f, WalkChain(f), BlockDecoder());
		Bytes buf(16);
		Check(ErrorOf([&]() { s.ReadAt(packed.block_logical[3], buf.data(), buf.size()); }).find("no decoder") !=
				  std::string::npos,
			  "a compressed block with no decoder is refused by name");
	}
	{
		auto v3 = packed;
		Put32(v3.file, 8, 3);
		MemoryAccess f("v3.bak", v3.file.data(), v3.file.size());
		Check(ErrorOf([&]() { ReadContainerHeader(f); }).find("container version 3") != std::string::npos,
			  "an unknown container version is refused by name");
		auto odd = packed;
		Put32(odd.file, 0x0C, 0x13);
		MemoryAccess g("flags.bak", odd.file.data(), odd.file.size());
		Check(ErrorOf([&]() { ReadContainerHeader(g); }).find("0x13") != std::string::npos,
			  "an unknown flag word is refused naming it");
		for (uint32_t flags : {0x85u, 0x87u, 0x1u}) {
			auto x = Pack(data, 8, {}, flags);
			MemoryAccess xf("x.bak", x.file.data(), x.file.size());
			Check(!ErrorOf([&]() { ReadContainerHeader(xf); }).empty(),
				  "flags " + std::to_string(flags) + " (not one of 3, 0x83, 5, 7) are refused");
		}
		auto only_encrypted = Pack(data, 8, {}, 0x5);
		MemoryAccess oe("enc_only.bak", only_encrypted.file.data(), only_encrypted.file.size());
		auto oh = ReadContainerHeader(oe);
		Check(oh.encrypted && !oh.compressed && oh.algorithm == CompressionAlgorithm::None,
			  "flags 0x5: encrypted, not compressed");
		auto zstd = Pack(data, 8, {}, 0x83);
		MemoryAccess z("zstd.bak", zstd.file.data(), zstd.file.size());
		Check(ReadContainerHeader(z).algorithm == CompressionAlgorithm::Zstd, "flags 0x83: ZSTD");
		auto enc = Pack(data, 8, {}, 0x7);
		MemoryAccess en("enc.bak", enc.file.data(), enc.file.size());
		auto eh = ReadContainerHeader(en);
		Check(
			eh.encrypted && eh.compressed && eh.encryptor_thumbprint[0] == 0xA0 && eh.encryptor_thumbprint[19] == 0xB3,
			"flags 0x7: encrypted and compressed, the thumbprint at 0x18");
		Check(ReadSeekIndex(z, WalkChain(z)).empty(), "zeros after the chain: no seek index, no error");
	}
	{
		// The decode path: XOR-"compressed" blocks, and a decoder that fails once.
		auto x = Pack(data, 4, {}, 0x3, true);
		MemoryAccess xf("xor.bak", x.file.data(), x.file.size());
		int calls = 0;
		BlockDecoder decoder = [&calls](const uint8_t *in, size_t n, uint8_t *out, size_t out_size) {
			if (++calls == 1) {
				throw FormatError("decoder: a failure on the first block");
			}
			if (n != out_size) {
				throw FormatError("decoder: sizes differ");
			}
			for (size_t i = 0; i < n; i++) {
				out[i] = in[i] ^ 0x5A;
			}
		};
		ContainerStream xs(xf, WalkChain(xf), decoder);
		Bytes buf(3000);
		auto first = ErrorOf([&]() { xs.ReadAt(100, buf.data(), buf.size()); });
		xs.ReadAt(100, buf.data(), buf.size());
		Check(first.find("first block") != std::string::npos && std::memcmp(buf.data(), data.data() + 100, 3000) == 0,
			  "a decoder's failure caches nothing: the next read decodes and returns the data set");
	}
	{
		auto big = Pack(data, 128, {});	 // 65536-byte blocks: the length field 0 means 65536
		MemoryAccess bf("big.bak", big.file.data(), big.file.size());
		auto bc = WalkChain(bf);
		ContainerStream bs(bf, bc, BlockDecoder());
		Bytes all(data.size());
		bs.ReadAt(0, all.data(), all.size());
		Check(bc.block_count == (data.size() + 65535) / 65536 && all == data,
			  "a payload length of 0 is a 65536-byte block");
	}
	{
		auto cut = packed;
		cut.file.resize(packed.block_physical[5] + 40);
		MemoryAccess f("cut.bak", cut.file.data(), cut.file.size());
		Check(ErrorOf([&]() { WalkChain(f); }).find("runs past the end") != std::string::npos,
			  "a block whose payload runs past the end of the file is refused");
	}

	std::cout << (failures ? "FAILED: " + std::to_string(failures) + " check(s)" : "all checks passed") << std::endl;
	return failures ? 1 : 0;
}
