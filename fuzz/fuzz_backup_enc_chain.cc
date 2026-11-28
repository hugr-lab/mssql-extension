// Fuzz harness: an encrypted MSSQLBAK container (spec 087 task 8) -- BACKUP … WITH ENCRYPTION (flags 5 / 7 /
// 0x87: blocks 0x31 / 0x33 under the backup's key) and a TDE database's compressed backup (E4c, 0x2B / 0xAB /
// 0x2F: blocks 0x61 under the DEK, 0x71 under both). The header's encryptor and DEK fields, the chain from
// ChainStart walked and located from its tail (a seek index naming offsets inside its last block, E4c), the
// media label (at 0x1E0, or 0x438 for TDE; an encrypted stored one running past the 4 KB header), the data set
// read across blocks -- each block decrypted, the backup's layer first, its length on disk checked -- and walked
// as MTF, and the page index built both ways with its pages read (a TDE page's flag 0x0800 deciding whether it is
// decrypted). The keys are fixed (AES-256, 0x11… for the backup's, 0x22… for the DEK: the seeds' blocks are
// encrypted with them; the unwrap is fuzz_backup_keyblob's); compressed blocks go to a decoder that copies (the
// codecs have harnesses of their own). Every malformed input must end in a FormatError; any sanitizer report,
// signal, hang or other exception is a bug.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include "backup/backup_allocation.hpp"
#include "backup/backup_bytes.hpp"
#include "backup/backup_container.hpp"
#include "backup/backup_crypto.hpp"
#include "backup/backup_mtf.hpp"
#include "backup/backup_page_index.hpp"
#include "backup/backup_set.hpp"

using namespace duckdb::mssql::backup;

static volatile size_t g_sink = 0;

static void Copy(const uint8_t *in, size_t n, uint8_t *out, size_t m) {
	std::memset(out, 0, m);
	std::memcpy(out, in, std::min(n, m));
}

static std::shared_ptr<const ContentKey> FixedKey(uint8_t byte) {
	std::shared_ptr<ContentKey> key(new ContentKey());
	key->algorithm = KeyAlgorithm::Aes256;
	std::vector<uint8_t> bytes(32, byte);
	key->key = SecretBytes(bytes.data(), bytes.size());
	return key;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
	static const std::shared_ptr<const ContentKey> backup_key = FixedKey(0x11);
	static const std::shared_ptr<const ContentKey> dek = FixedKey(0x22);
	MemoryAccess file("fuzz_enc.bak", data, size);
	ContainerHeader header;
	try {
		if (!IsContainer(file)) {
			return 0;
		}
		header = ReadContainerHeader(file);
	} catch (FormatError &) {
		return 0;
	}
	ContainerKeys keys;
	keys.backup = header.encrypted ? backup_key : nullptr;
	keys.dek = header.tde ? dek : nullptr;
	const BlockDecoder decoder = Copy;
	const uint64_t start = ChainStart(header);
	try {
		auto tape = ReadContainerTape(file, decoder, keys, header.tde);
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
				if (!OpenChainFromTail(file, chain, seek, start)) {
					continue;
				}
			} else {
				chain = WalkChain(file, start);
				seek = ReadSeekIndex(file, chain);
			}
			ContainerStream stream(file, chain, decoder, keys);
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
				ContainerFamily container;
				container.file = &file;
				container.stream = &stream;
				container.seek = &seek;
				container.chain_end = chain.end;
				container.decoder = decoder;
				container.keys = keys;
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
						// A TDE database's pages: decrypted where their flag says so (the DEK as the boot page's).
						index.SetPageKey(dek);
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
