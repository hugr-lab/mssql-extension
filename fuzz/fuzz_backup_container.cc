// Fuzz harness: a compressed SQL Server backup's MSSQLBAK container (spec 085 tasks 4 and 7a, DQ-41).
//
// The bytes are a container file: its header, its block chain walked whole and located from its
// tail (seek index, checkpoints), its media label (the TAPE block in its header), its data set read
// across blocks and walked as MTF, and its page index built both ways (DQ-41: galloping through the
// chain walked from known blocks, and trust_layout's resync search over its blocks), its pages read
// and its allocation map asked. Compressed blocks go to the real MS_XPRESS decoder or, when
// the header says ZSTD, to a decoder that copies (the zstd library is fuzzed upstream). Every
// malformed input must end in a FormatError; any sanitizer report, signal, hang or other exception
// is a real bug.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "backup/backup_allocation.hpp"
#include "backup/backup_bytes.hpp"
#include "backup/backup_container.hpp"
#include "backup/backup_mtf.hpp"
#include "backup/backup_page_index.hpp"
#include "backup/backup_pass_search.hpp"
#include "backup/backup_set.hpp"
#include "backup/backup_xpress.hpp"

using namespace duckdb::mssql::backup;

static volatile size_t g_sink = 0;

static void Copy(const uint8_t *in, size_t n, uint8_t *out, size_t m) {
	std::memset(out, 0, m);
	std::memcpy(out, in, std::min(n, m));
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
	MemoryAccess file("fuzz_comp.bak", data, size);
	BlockDecoder decoder = Copy;
	try {
		if (!IsContainer(file)) {
			return 0;
		}
		auto header = ReadContainerHeader(file);
		if (header.algorithm == CompressionAlgorithm::MsXpress) {
			decoder = [](const uint8_t *in, size_t n, uint8_t *out, size_t m) { XpressHuffmanDecode(in, n, out, m); };
		}
	} catch (FormatError &) {
		return 0;
	}
	try {
		auto tape = ReadContainerTape(file, decoder);
		MemoryAccess label("label", tape.data(), tape.size());
		BackupSetHeader set;
		ReadMediaFields(label, WalkMtfTape(label), set);
		g_sink += set.family_count;
	} catch (FormatError &) {
	}
	for (int located = 0; located < 2; located++) {
		try {
			BlockChain chain;
			std::vector<SeekEntry> seek;
			if (located) {
				if (!OpenChainFromTail(file, chain, seek)) {
					continue;
				}
			} else {
				chain = WalkChain(file);
				seek = ReadSeekIndex(file, chain);
			}
			ContainerStream stream(file, chain, decoder);
			// Reads across blocks, from both ends of the data set.
			std::vector<uint8_t> bytes(std::min<uint64_t>(stream.Size(), 300000));
			if (!bytes.empty()) {
				stream.ReadAt(0, bytes.data(), bytes.size());
				stream.ReadAt(stream.Size() - bytes.size(), bytes.data(), bytes.size());
				g_sink += bytes[0];
			}
			try {
				auto layout = WalkMtfDataSet(stream);
				auto set = ReadBackupSet(stream, layout);
				g_sink += set.files.size();
				// The page index both ways (DQ-41): through the chain walked from known blocks
				// (gallop), and trust_layout's resync search over the container's blocks.
				ContainerFamily container;
				container.file = &file;
				container.stream = &stream;
				container.seek = &seek;
				container.chain_end = chain.end;
				container.decoder = decoder;
				FamilyStream family;
				family.stream = &stream;
				family.layout = &layout;
				family.container = &container;
				for (int trust = 0; trust < 2; trust++) {
					try {
						PageIndexOptions options;
						options.full_backup = set.kind == BackupKind::Full;
						options.trust_layout = trust == 1;
						auto index = BuildPageIndex(std::vector<FamilyStream>{family}, options);
						std::vector<uint8_t> page;
						for (uint32_t p = 0; p < 64; p += 5) {
							try {
								g_sink += index.ReadPage(1, p, page, true) ? page[1] : 0;
							} catch (FormatError &) {
							}
						}
						try {
							AllocationMap allocation(index, {1});
							for (uint32_t p = 0; p < 64; p++) {
								g_sink += allocation.IsLive(1, p) ? 1 : 0;
							}
						} catch (FormatError &) {
						}
					} catch (FormatError &) {
					}
				}
			} catch (FormatError &) {
			}
		} catch (FormatError &) {
		}
	}
	return 0;
}
