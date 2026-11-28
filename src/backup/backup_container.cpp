#include "backup/backup_container.hpp"

#include "backup/backup_bytes.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>

namespace duckdb {
namespace mssql {
namespace backup {

static constexpr uint64_t CONTAINER_HEADER = 4096;
static constexpr uint64_t CHAIN_START = 8192;
static constexpr size_t BLOCK_HEADER = 28;
static constexpr uint32_t MAX_BLOCK = 65536;
static constexpr uint8_t KIND_COMPRESSED = 0x21;
static constexpr uint8_t KIND_STORED = 0x23;
// Spec 087 (E4b): an encrypted container's blocks, the same two with bit 0x10: the payload is the
// ciphertext of `clen` plaintext bytes, its length on disk the header's u32 at 20, the IV at 4.
static constexpr uint8_t KIND_ENCRYPTED_COMPRESSED = 0x31;
static constexpr uint8_t KIND_ENCRYPTED_STORED = 0x33;
// E4c: a TDE database's compressed backup -- its payload the DEK's ciphertext (0x61), and under the backup's key
// too when the backup is encrypted (0x71). Only compressed ones were seen.
static constexpr uint8_t KIND_TDE_COMPRESSED = 0x61;
static constexpr uint8_t KIND_TDE_ENCRYPTED_COMPRESSED = 0x71;
// E4c: where such a header keeps the wrapped DEK, and its media label.
static constexpr size_t TDE_DEK_AT = 0x214;
static constexpr uint64_t TDE_TAPE_AT = 0x438;
static constexpr size_t ENCRYPTED_LENGTH_AT = 20;
static constexpr size_t IV_AT = 4;
static constexpr uint64_t ENCRYPTED_STORED_CHAIN_START = 0x3000;
// The seek index record: u32 length at +0, u32 entry count at +28, entries (u64 logical, u64
// physical) from +40. Seen with 3 entries; bounded so a crafted count cannot allocate.
static constexpr size_t SEEK_COUNT_AT = 28;
static constexpr size_t SEEK_ENTRIES_AT = 40;
static constexpr uint32_t MAX_SEEK_ENTRIES = 1024;		  // 3 to 5 seen
static constexpr uint32_t SEEK_RECORD_TAG = 44;			  // u32 @0 (its meaning unknown; constant)
static constexpr uint32_t SEEK_RECORD_KIND = 0x00010001;  // u32 @24 (constant)
// The container's tail (E13, 16 of 16 containers, 2025): the file is whole 4 KB pages; a 16-byte
// record at EOF - 12304 (u16 1, u16 1, u32 the seek index's entry count, u32 the distance back
// from the record to the seek index, less 8, u32 unknown), then a plain SFMB at EOF - 12288, a
// block at EOF - 8192 (the ESET), and a plain SFMB at EOF - 4096.
static constexpr uint64_t TAIL_RECORD_FROM_END = 12304;
static constexpr uint64_t TAIL_SFMB_FROM_END = 12288;
static constexpr uint64_t TAIL_LAST_SFMB_FROM_END = 4096;
// The blocks after the last seek entry (the trailer) that OpenChainFromTail walks to the chain's
// end: the trailer is a few blocks (2 to 6 seen); a crafted tail must not make it a full walk.
static constexpr uint32_t MAX_TRAILER_BLOCKS = 4096;

const char *CompressionAlgorithmName(CompressionAlgorithm algorithm) {
	switch (algorithm) {
	case CompressionAlgorithm::MsXpress:
		return "MS_XPRESS";
	case CompressionAlgorithm::Zstd:
		return "ZSTD";
	default:
		return "NONE";
	}
}

static std::string Where(const RandomAccess &file, uint64_t offset) {
	return "backup file \"" + file.Name() + "\" at offset " + std::to_string(offset);
}

static std::vector<uint8_t> Read(const RandomAccess &file, uint64_t offset, size_t count, const char *what) {
	if (offset > file.Size() || count > file.Size() - offset) {
		throw FormatError(Where(file, offset) + ": " + what + " (" + std::to_string(count) +
						  " bytes) runs past the end of the " + std::to_string(file.Size()) + "-byte file");
	}
	std::vector<uint8_t> bytes(count);
	if (count) {
		file.ReadAt(offset, bytes.data(), count);
	}
	return bytes;
}

bool IsContainer(const RandomAccess &file) {
	if (file.Size() < 8) {
		return false;
	}
	uint8_t magic[8];
	file.ReadAt(0, magic, 8);
	return std::memcmp(magic, "MSSQLBAK", 8) == 0;
}

ContainerHeader ReadContainerHeader(const RandomAccess &file) {
	auto bytes = Read(file, 0, CONTAINER_HEADER, "the container header");
	ByteReader r(bytes.data(), bytes.size(), "the MSSQLBAK header");
	if (std::memcmp(bytes.data(), "MSSQLBAK", 8) != 0) {
		throw FormatError(Where(file, 0) + ": not an MSSQLBAK container");
	}
	ContainerHeader header;
	header.version = r.U32(8);
	if (header.version != 2) {
		r.Fail(8,
			   "container version " + std::to_string(header.version) + ", not 2: a version the reader does not know");
	}
	header.flags = r.U32(0x0C);
	// Seen: 3 MS_XPRESS, 0x83 ZSTD, 5 encrypted, 7 encrypted and MS_XPRESS (E4), 0x87 encrypted and ZSTD
	// (E4b); read as bits 0x2 compressed, 0x4 encrypted, 0x80 ZSTD. A TDE database's compressed backup adds
	// bits 0x8 and 0x20 (E4c): 0x2B MS_XPRESS, 0xAB ZSTD, 0x2F encrypted too (MS_XPRESS). Only these eight are
	// accepted: 0xAF (TDE, encrypted, ZSTD), not seen, and any other word are refused naming it.
	const uint32_t f = header.flags;
	if (f != 0x3 && f != 0x83 && f != 0x5 && f != 0x7 && f != 0x87 && f != 0x2B && f != 0xAB && f != 0x2F) {
		char word[16];
		snprintf(word, sizeof(word), "0x%X", header.flags);
		r.Fail(0x0C, std::string("container flags ") + word + ": a combination the reader does not know");
	}
	header.compressed = (header.flags & 0x2) != 0;
	header.encrypted = (header.flags & 0x4) != 0;
	header.algorithm = !header.compressed			? CompressionAlgorithm::None
					   : (header.flags & 0x80) != 0 ? CompressionAlgorithm::Zstd
													: CompressionAlgorithm::MsXpress;
	if (header.encrypted) {
		// Spec 087 (E4): u32 the thumbprint's length (20) at 0x14, the thumbprint, u32 the wrapped key's
		// length (the certificate's modulus size), then the wrapped key.
		if (r.U32(0x14) != 20) {
			r.Fail(0x14, "an encryptor thumbprint of " + std::to_string(r.U32(0x14)) + " bytes, not 20");
		}
		const uint8_t *thumbprint = r.Bytes(0x18, 20);
		for (size_t i = 0; i < 20; i++) {
			header.encryptor_thumbprint[i] = thumbprint[i];
		}
		const uint32_t wrapped = r.U32(0x2C);
		if (wrapped < 128 || wrapped > 1024) {
			r.Fail(0x2C, "a wrapped key of " + std::to_string(wrapped) + " bytes (an RSA modulus is 128 to 1024)");
		}
		const uint8_t *blob = r.Bytes(0x30, wrapped);
		header.wrapped_key.assign(blob, blob + wrapped);
	}
	header.tde = (f & 0x28) == 0x28;
	if (header.tde) {
		if (r.U32(TDE_DEK_AT) != 20) {
			r.Fail(TDE_DEK_AT,
				   "a TDE certificate's thumbprint of " + std::to_string(r.U32(TDE_DEK_AT)) + " bytes, not 20");
		}
		const uint8_t *thumbprint = r.Bytes(TDE_DEK_AT + 4, 20);
		std::memcpy(header.tde_thumbprint.data(), thumbprint, 20);
		const uint32_t wrapped = r.U32(TDE_DEK_AT + 24);
		if (wrapped < 128 || wrapped > 1024) {
			r.Fail(TDE_DEK_AT + 24,
				   "a wrapped DEK of " + std::to_string(wrapped) + " bytes (an RSA modulus is 128 to 1024)");
		}
		const uint8_t *blob = r.Bytes(TDE_DEK_AT + 28, wrapped);
		header.wrapped_dek.assign(blob, blob + wrapped);
	}
	return header;
}

uint64_t ChainStart(const ContainerHeader &header) {
	return header.encrypted && !header.compressed ? ENCRYPTED_STORED_CHAIN_START : CHAIN_START;
}

//! The chain block whose header's BLOCK_HEADER bytes are `bytes`, at `at` of `file`: false when no
//! chain block is there (the chain's end). `logical` is where its output starts.
static bool ParseBlockHeader(const RandomAccess &file, const uint8_t *bytes, uint64_t at, uint64_t logical,
							 ChainBlock &block) {
	ByteReader r(bytes, BLOCK_HEADER, "a container block header", at);
	const uint8_t kind = r.U8(0);
	if (kind != KIND_COMPRESSED && kind != KIND_STORED && kind != KIND_ENCRYPTED_COMPRESSED &&
		kind != KIND_ENCRYPTED_STORED && kind != KIND_TDE_COMPRESSED && kind != KIND_TDE_ENCRYPTED_COMPRESSED) {
		return false;
	}
	block.kind = kind;
	block.physical = at;
	block.output = static_cast<uint32_t>(r.U8(1)) * 512;
	const uint16_t length = r.U16(2);
	block.plain = length == 0 ? MAX_BLOCK : length;
	block.payload = block.plain;
	if (block.Encrypted()) {
		// E4b: the ciphertext's length on disk, the plaintext's rounded up to 16 (for 3DES too).
		block.payload = r.U32(ENCRYPTED_LENGTH_AT);
		if (block.payload != (block.plain + 15) / 16 * 16) {
			r.Fail(ENCRYPTED_LENGTH_AT, "an encrypted block of " + std::to_string(block.plain) +
											" plaintext bytes whose ciphertext is " + std::to_string(block.payload) +
											" bytes, not the plaintext rounded up to 16");
		}
	}
	block.logical = logical;
	if (block.output == 0 || block.output > MAX_BLOCK) {
		r.Fail(1, "a block whose output is " + std::to_string(block.output) + " bytes, not 1..65536");
	}
	if (block.payload > file.Size() - at - BLOCK_HEADER) {
		r.Fail(2, "a block whose " + std::to_string(block.payload) + "-byte payload runs past the end of the file");
	}
	if (block.Stored() && block.plain != block.output) {
		r.Fail(2, "a stored block of " + std::to_string(block.plain) + " bytes that claims an output of " +
					  std::to_string(block.output));
	}
	return true;
}

//! The chain block whose header is at `at`, or false when no chain block is there (the chain's end).
//! `logical` is where its output starts.
static bool ReadBlockHeader(const RandomAccess &file, uint64_t at, uint64_t logical, ChainBlock &block) {
	if (at > file.Size() || file.Size() - at < BLOCK_HEADER) {
		return false;
	}
	uint8_t bytes[BLOCK_HEADER];
	file.ReadAt(at, bytes, BLOCK_HEADER);
	return ParseBlockHeader(file, bytes, at, logical, block);
}

//! A chain's headers walked forward, read CHAIN_WINDOW bytes at a time up to `limit` (PR 1's review,
//! task 16c: one 28-byte read a header was one range request each on a remote file, 16M for 1 TB of
//! 64 KB blocks; a window holds 64 of those headers, thousands of well-compressed ones).
class HeaderWindow {
public:
	static constexpr uint64_t CHAIN_WINDOW = 4 << 20;
	HeaderWindow(const RandomAccess &file, uint64_t limit) : file_(file), limit_(std::min(limit, file.Size())) {}
	//! As ReadBlockHeader, from the window.
	bool Read(uint64_t at, uint64_t logical, ChainBlock &block) {
		if (at > file_.Size() || file_.Size() - at < BLOCK_HEADER) {
			return false;
		}
		if (at < base_ || at - base_ > bytes_.size() || bytes_.size() - (at - base_) < BLOCK_HEADER) {
			const uint64_t until = std::max(limit_, at + BLOCK_HEADER);
			const uint64_t n = std::min<uint64_t>(CHAIN_WINDOW, until - at);
			bytes_.resize(static_cast<size_t>(std::max<uint64_t>(n, BLOCK_HEADER)));
			base_ = at;
			file_.ReadAt(at, bytes_.data(), bytes_.size());
		}
		return ParseBlockHeader(file_, bytes_.data() + (at - base_), at, logical, block);
	}

private:
	const RandomAccess &file_;
	const uint64_t limit_;
	uint64_t base_ = 0;
	std::vector<uint8_t> bytes_;
};

BlockChain WalkChain(const RandomAccess &file, uint64_t start) {
	BlockChain chain;
	uint64_t at = start;
	uint64_t logical = 0;
	ChainBlock block;
	uint64_t since = 0;
	HeaderWindow headers(file, file.Size());
	while (headers.Read(at, logical, block)) {
		if (chain.checkpoints.empty() ||
			(since >= BlockChain::CHECKPOINT_STRIDE &&
			 block.physical - chain.checkpoints.back().physical >= BlockChain::CHECKPOINT_BYTES)) {
			chain.checkpoints.push_back(block);
			since = 0;
		}
		since++;
		chain.block_count++;
		logical += block.output;
		at += BLOCK_HEADER + block.payload;
	}
	if (chain.block_count == 0) {
		throw FormatError(Where(file, start) + ": a container with no block where its chain starts");
	}
	chain.logical_size = logical;
	chain.end = at;
	return chain;
}

ChainBlock FindBlock(const RandomAccess &file, const BlockChain &chain, uint64_t logical) {
	if (logical >= chain.logical_size) {
		throw FormatError("backup file \"" + file.Name() + "\": data-set offset " + std::to_string(logical) +
						  " is past its " + std::to_string(chain.logical_size) + " bytes");
	}
	if (chain.checkpoints.empty()) {
		throw FormatError("backup file \"" + file.Name() + "\": a block chain with no checkpoint");
	}
	// The last checkpoint at or before `logical` (checkpoints ascend by logical offset; the first is
	// the chain's first block, at logical 0).
	size_t lo = 0;
	size_t hi = chain.checkpoints.size();
	while (hi - lo > 1) {
		size_t mid = lo + (hi - lo) / 2;
		if (chain.checkpoints[mid].logical <= logical) {
			lo = mid;
		} else {
			hi = mid;
		}
	}
	ChainBlock block = chain.checkpoints[lo];
	// The walk ends at the next checkpoint (or the chain's end): within a checkpoint's span.
	const uint64_t stop = lo + 1 < chain.checkpoints.size() ? chain.checkpoints[lo + 1].physical : chain.end;
	HeaderWindow headers(file, stop);
	while (block.physical < stop) {
		if (logical < block.logical + block.output) {
			return block;
		}
		ChainBlock next;
		const uint64_t at = block.physical + BLOCK_HEADER + block.payload;
		if (!headers.Read(at, block.logical + block.output, next)) {
			break;
		}
		block = next;
	}
	throw FormatError(Where(file, block.physical) + ": no block of the chain holds data-set offset " +
					  std::to_string(logical) + " (the file changed since it was opened?)");
}

//! The seek index's entries at `at` (where the chain ends), or empty when no seek index record is
//! there. Entries must ascend in their logical offsets, not descend in their physical ones, and name a
//! block header between the chain's start and `at`; whether each starts its logical offset is the
//! caller's to check. Spec 087 E4c: a TDE database's compressed backup also names offsets inside its
//! last block (0x344000 the block's start, then 0x345254, 0x346254, 0x347254 at the same physical offset,
//! 4 of 4 such files): an entry at the previous one's physical offset is one of those.
static std::vector<SeekEntry> ParseSeekIndex(const RandomAccess &file, uint64_t at) {
	std::vector<SeekEntry> entries;
	if (at > file.Size() || file.Size() - at < SEEK_ENTRIES_AT) {
		return entries;
	}
	auto head = Read(file, at, SEEK_ENTRIES_AT, "the seek index");
	ByteReader r(head.data(), head.size(), "the seek index", at);
	if (r.U32(0) != SEEK_RECORD_TAG || r.U32(24) != SEEK_RECORD_KIND) {
		// Not the record seen after every chain: no index, not a corrupt one.
		return entries;
	}
	const uint32_t count = r.U32(SEEK_COUNT_AT);
	if (count == 0 || count > MAX_SEEK_ENTRIES ||
		static_cast<uint64_t>(count) * 16 > file.Size() - at - SEEK_ENTRIES_AT) {
		return entries;
	}
	auto body = Read(file, at + SEEK_ENTRIES_AT, static_cast<size_t>(count) * 16, "the seek index entries");
	ByteReader e(body.data(), body.size(), "the seek index entries", at + SEEK_ENTRIES_AT);
	for (uint32_t i = 0; i < count; i++) {
		SeekEntry entry;
		entry.logical = e.U64(16 * i);
		entry.physical = e.U64(16 * i + 8);
		if ((i > 0 && (entry.logical <= entries.back().logical || entry.physical < entries.back().physical)) ||
			entry.physical < CHAIN_START || entry.physical >= at) {
			e.Fail(16 * i, "a seek index entry out of order or outside the chain");
		}
		entries.push_back(entry);
	}
	return entries;
}

std::vector<SeekEntry> ReadSeekIndex(const RandomAccess &file, const BlockChain &chain) {
	auto entries = ParseSeekIndex(file, chain.end);
	for (size_t i = 0; i < entries.size(); i++) {
		const auto &entry = entries[i];
		if (entry.logical >= chain.logical_size) {
			throw FormatError(Where(file, chain.end) + ": a seek index entry past the data set");
		}
		// It must name a chain block whose output starts at its logical offset -- or, at the previous
		// entry's physical offset (E4c), the block that holds it.
		auto block = FindBlock(file, chain, entry.logical);
		const bool inside = i > 0 && entries[i - 1].physical == entry.physical;
		if ((block.logical != entry.logical && !inside) || block.physical != entry.physical) {
			throw FormatError(Where(file, chain.end) + ": a seek index entry (logical " +
							  std::to_string(entry.logical) + ", physical " + std::to_string(entry.physical) +
							  ") that names no block of the chain");
		}
	}
	return entries;
}

// The container's own TAPE block (E13 follow-up, 16 of 16 containers, 2025): a chain block at 0x1E0
// of its header, 4 KB of output, whose bytes are the TAPE DBLK an uncompressed family starts with.
static constexpr uint64_t HEADER_TAPE_AT = 0x1E0;

//! `block`'s payload (`payload`, its bytes on disk) decrypted with `keys` when it is encrypted -- the backup's
//! layer, then the DEK's (E4c), each with the header's IV --: its first `block.plain` bytes, in `scratch`; else
//! `payload` itself.
static const uint8_t *Plaintext(const RandomAccess &file, const ChainBlock &block, const uint8_t *header,
								const uint8_t *payload, const ContainerKeys &keys, std::vector<uint8_t> &scratch) {
	if (!block.Encrypted()) {
		return payload;
	}
	if ((block.BackupLayer() && !keys.backup) || (block.DekLayer() && !keys.dek)) {
		throw FormatError(Where(file, block.physical) + ": an encrypted block, and no key to decrypt it");
	}
	scratch.assign(payload, payload + block.payload);
	if (block.BackupLayer()) {
		DecryptCbc(*keys.backup, header + IV_AT, scratch.data(), scratch.size());
	}
	if (block.DekLayer()) {
		DecryptCbc(*keys.dek, header + IV_AT, scratch.data(), scratch.size());
	}
	return scratch.data();
}

std::vector<uint8_t> ReadContainerTape(const RandomAccess &file, const BlockDecoder &decoder, const ContainerKeys &keys,
									   bool tde) {
	const uint64_t label_at = tde ? TDE_TAPE_AT : HEADER_TAPE_AT;
	ChainBlock block;
	// An encrypted, stored container's label is stored too: 4 KB of payload, which runs past the 4 KB header
	// into the 4 KB before its SFMB at 0x2000 (E4b); every other's fits the header.
	if (!ReadBlockHeader(file, label_at, 0, block) ||
		block.payload >
			(block.Encrypted() && block.Stored() ? ENCRYPTED_STORED_CHAIN_START - CONTAINER_HEADER : CONTAINER_HEADER) -
				label_at - BLOCK_HEADER) {
		throw FormatError(Where(file, label_at) +
						  ": no block holding the media label where every compressed backup keeps it");
	}
	auto bytes = Read(file, label_at, BLOCK_HEADER + block.payload, "the media label block");
	std::vector<uint8_t> scratch;
	const uint8_t *payload = Plaintext(file, block, bytes.data(), bytes.data() + BLOCK_HEADER, keys, scratch);
	std::vector<uint8_t> out(block.output);
	if (block.Stored()) {
		std::memcpy(out.data(), payload, block.output);
	} else if (!decoder) {
		throw FormatError(Where(file, label_at) + ": a compressed media label, and no decoder for it");
	} else {
		decoder(payload, block.plain, out.data(), block.output);
	}
	if (out.size() < 4 || std::memcmp(out.data(), "TAPE", 4) != 0) {
		throw FormatError(Where(file, label_at) + ": the media label block does not hold a TAPE block");
	}
	return out;
}

bool OpenChainFromTail(const RandomAccess &file, BlockChain &chain, std::vector<SeekEntry> &seek, uint64_t start) {
	const uint64_t size = file.Size();
	if (size % 4096 != 0 || size < CHAIN_START + TAIL_RECORD_FROM_END) {
		return false;
	}
	const uint64_t record_at = size - TAIL_RECORD_FROM_END;
	auto record = Read(file, record_at, 16, "the container's tail record");
	ByteReader r(record.data(), record.size(), "the container's tail record", record_at);
	uint8_t magic[4];
	file.ReadAt(size - TAIL_SFMB_FROM_END, magic, 4);
	const bool sfmb = std::memcmp(magic, "SFMB", 4) == 0;
	file.ReadAt(size - TAIL_LAST_SFMB_FROM_END, magic, 4);
	if (!sfmb || std::memcmp(magic, "SFMB", 4) != 0 || r.U16(0) != 1 || r.U16(2) != 1) {
		return false;
	}
	const uint64_t back = static_cast<uint64_t>(r.U32(8)) + 8;
	if (back > record_at - CHAIN_START) {
		return false;
	}
	const uint64_t end = record_at - back;
	auto entries = ParseSeekIndex(file, end);
	if (entries.empty() || entries.size() != r.U32(4)) {
		return false;
	}
	// The checkpoints: the first block (logical 0) and each entry's block, whose headers must be
	// blocks; the data set's size from walking the trailer, the blocks after the last entry.
	BlockChain located;
	ChainBlock block;
	if (!ReadBlockHeader(file, start, 0, block)) {
		return false;
	}
	located.checkpoints.push_back(block);
	// A block takes at least a header and a byte of payload and outputs at most 64 KB, so the data
	// set's offset at a physical offset is bounded: a crafted entry cannot claim more, or wrap.
	auto max_logical = [](uint64_t physical) { return (physical - CHAIN_START) / (BLOCK_HEADER + 1) * MAX_BLOCK; };
	for (const auto &entry : entries) {
		if (entry.physical == block.physical && located.checkpoints.size() > 1) {
			// An offset inside the last entry's block (E4c): no checkpoint of its own.
			if (entry.logical <= block.logical || entry.logical >= block.logical + block.output) {
				throw FormatError(Where(file, entry.physical) + ": a seek index entry at data-set offset " +
								  std::to_string(entry.logical) + ", outside the block it names");
			}
			continue;
		}
		if (entry.logical == 0) {
			throw FormatError(Where(file, entry.physical) +
							  ": a seek index entry at data-set offset 0, where the chain's first block is");
		}
		if (entry.logical > max_logical(entry.physical)) {
			throw FormatError(Where(file, entry.physical) + ": a seek index entry at data-set offset " +
							  std::to_string(entry.logical) + ", more than the blocks before it can hold");
		}
		if (!ReadBlockHeader(file, entry.physical, entry.logical, block)) {
			throw FormatError(Where(file, entry.physical) + ": a seek index entry that names no block");
		}
		located.checkpoints.push_back(block);
	}
	uint64_t at = block.physical + BLOCK_HEADER + block.payload;
	uint64_t logical = block.logical + block.output;
	for (uint32_t n = 0; at < end; n++) {
		if (n == MAX_TRAILER_BLOCKS) {
			// Not the layout of the samples (2 to 6 blocks): walk the chain instead.
			return false;
		}
		if (!ReadBlockHeader(file, at, logical, block)) {
			throw FormatError(Where(file, at) + ": the chain's trailer does not end where its tail says");
		}
		logical += block.output;
		at += BLOCK_HEADER + block.payload;
	}
	if (at != end) {
		throw FormatError(Where(file, at) + ": the chain's last block runs past where its tail says it ends");
	}
	if (logical > max_logical(end)) {
		throw FormatError(Where(file, end) + ": a data set larger than its chain's blocks can hold");
	}
	located.logical_size = logical;
	located.end = end;
	chain = std::move(located);
	seek = std::move(entries);
	return true;
}

static uint64_t NextStreamId() {
	static std::atomic<uint64_t> next{1};
	return next.fetch_add(1);
}

ContainerStream::ContainerStream(const RandomAccess &file, BlockChain chain, BlockDecoder decoder, ContainerKeys keys)
	: file_(file),
	  chain_(std::move(chain)),
	  decoder_(std::move(decoder)),
	  keys_(std::move(keys)),
	  name_(file.Name() + " (data set)"),
	  id_(NextStreamId()) {
	spans_.reserve(chain_.checkpoints.size());
	for (const auto &checkpoint : chain_.checkpoints) {
		std::unique_ptr<Span> span(new Span());
		Mark start;
		start.physical = checkpoint.physical;
		start.logical = checkpoint.logical;
		span->marks.push_back(start);
		span->frontier = start;
		span->walked.store(start.logical, std::memory_order_relaxed);
		spans_.push_back(std::move(span));
	}
}

//! The bytes of `file` a thread read last, by the stream's id (§ Scan: a thread keeps what it read):
//! READ_WINDOW bytes read at once, so the headers a walk reads and the payloads of the blocks that
//! follow one another are read in a few reads, not one a header and one a block. At most READ_WINDOW
//! a thread (a block is at most 64 KiB): its bytes the stream's until the thread reads another's,
//! its capacity the thread's for its life (spec 085 § Resources).
static constexpr size_t READ_WINDOW = 256 << 10;

const uint8_t *ContainerStream::Window(uint64_t at, size_t count) const {
	struct ThreadWindow {
		uint64_t stream = 0;
		uint64_t base = 0;
		std::vector<uint8_t> bytes;
	};
	static thread_local ThreadWindow window;
	if (window.stream == id_ && at >= window.base && count <= window.bytes.size() &&
		at - window.base <= window.bytes.size() - count) {
		return window.bytes.data() + (at - window.base);
	}
	if (at > file_.Size() || count > file_.Size() - at) {
		throw FormatError(Where(file_, at) + ": " + std::to_string(count) + " bytes that run past the end of the " +
						  std::to_string(file_.Size()) + "-byte file");
	}
	const size_t n = static_cast<size_t>(std::max<uint64_t>(count, std::min<uint64_t>(READ_WINDOW, file_.Size() - at)));
	window.stream = 0;	// not this stream's while the read may throw
	window.bytes.resize(n);
	file_.ReadAt(at, window.bytes.data(), n);
	window.stream = id_;
	window.base = at;
	return window.bytes.data();
}

ContainerStream::Shared ContainerStream::RecentLocked(uint64_t logical) const {
	for (const auto &d : recent_) {
		if (d && logical >= d->block.logical && logical < d->block.logical + d->block.output) {
			return d;
		}
	}
	return nullptr;
}

ContainerStream::Shared ContainerStream::Decode(const ChainBlock &block) const {
	auto d = std::make_shared<DecodedBlock>();
	const uint8_t *bytes = Window(block.physical, BLOCK_HEADER + block.payload);
	// An encrypted block's plaintext in a buffer of the thread's own (the window's bytes are kept for the
	// next read and must stay the file's).
	static thread_local std::vector<uint8_t> scratch;
	const uint8_t *payload = Plaintext(file_, block, bytes, bytes + BLOCK_HEADER, keys_, scratch);
	d->output.resize(block.output);
	if (block.Stored()) {
		std::memcpy(d->output.data(), payload, block.output);
	} else {
		if (!decoder_) {
			throw FormatError(Where(file_, block.physical) +
							  ": a compressed block, and no decoder for this container's algorithm");
		}
		decoder_(payload, block.plain, d->output.data(), block.output);
	}
	d->block = block;
	return d;
}

void ContainerStream::KeepLocked(Shared decoded) const {
	for (const auto &d : recent_) {
		if (d && d->block.physical == decoded->block.physical) {
			return;
		}
	}
	recent_[next_slot_] = std::move(decoded);
	next_slot_ = (next_slot_ + 1) % recent_.size();
}

uint64_t ContainerStream::SpanStop(size_t i) const {
	return i + 1 == chain_.checkpoints.size() ? chain_.end : chain_.checkpoints[i + 1].physical;
}

ChainBlock ContainerStream::HeaderAt(uint64_t at, uint64_t logical, uint64_t stop) const {
	ChainBlock block;
	if (at >= stop || stop - at < BLOCK_HEADER ||
		!ParseBlockHeader(file_, Window(at, BLOCK_HEADER), at, logical, block)) {
		throw FormatError(Where(file_, at) + ": no block of the chain where its span (to offset " +
						  std::to_string(stop) + ") goes on");
	}
	return block;
}

void ContainerStream::WalkOnLocked(size_t i, Span &span, uint64_t logical) const {
	// On from the frontier, a header at a time from the thread's window, until the block holding
	// `logical` is walked or the span ends. The walk must land on the next checkpoint at its data-set
	// offset, so a span walked to its end confirms the checkpoint after it (a seek index entry's
	// logical offset is otherwise trusted). The frontier moves a block at a time: a walk that throws
	// leaves the span as walked so far, and the next read throws at the same header.
	const uint64_t stop = SpanStop(i);
	const bool last = i + 1 == chain_.checkpoints.size();
	while (!span.complete && span.frontier.logical <= logical) {
		const Mark at = span.frontier;
		const ChainBlock block = HeaderAt(at.physical, at.logical, stop);
		if (at.physical - span.marks.back().physical >= MARK_BYTES) {
			span.marks.push_back(at);
		}
		Mark next;
		next.physical = at.physical + BLOCK_HEADER + block.payload;
		next.logical = at.logical + block.output;
		if (next.physical > stop ||
			(next.physical == stop && !last && next.logical != chain_.checkpoints[i + 1].logical)) {
			throw FormatError(Where(file_, next.physical) +
							  ": the chain's blocks do not reach the checkpoint at offset " + std::to_string(stop) +
							  " where it says");
		}
		span.frontier = next;
		span.complete = next.physical == stop;
		span.walked.store(next.logical, std::memory_order_release);
	}
}

ChainBlock ContainerStream::Locate(uint64_t logical) const {
	if (logical >= chain_.logical_size) {
		throw FormatError("backup file \"" + file_.Name() + "\": data-set offset " + std::to_string(logical) +
						  " is past its " + std::to_string(chain_.logical_size) + " bytes");
	}
	auto it = std::upper_bound(chain_.checkpoints.begin(), chain_.checkpoints.end(), logical,
							   [](uint64_t l, const ChainBlock &b) { return l < b.logical; });
	if (it == chain_.checkpoints.begin()) {
		throw FormatError("backup file \"" + file_.Name() + "\": a block chain with no checkpoint at or before " +
						  std::to_string(logical));
	}
	const size_t i = static_cast<size_t>(it - chain_.checkpoints.begin()) - 1;
	Span &span = *spans_[i];
	const uint64_t stop = SpanStop(i);
	// The thread's last block located, when it lies in this span at or before `logical`: a scan reads
	// its blocks one after another, so the walk on from it to the next block or so is a header or two,
	// and needs no lock while `logical` is behind the frontier (every block before it was walked and
	// confirmed). Farther on (the thread's next unit, past the units the others claimed) the nearest
	// mark is looked up instead: a walk from the last block would read every byte between through
	// the window (measured: 1 s more of the kernel's time on a 16-task scan of an 8 GB heap).
	struct LastLocated {
		uint64_t stream = 0;
		ChainBlock block;
	};
	static thread_local LastLocated last;
	const bool last_here = last.stream == id_ && last.block.physical >= chain_.checkpoints[i].physical &&
						   last.block.physical < stop && last.block.logical <= logical;
	if (last_here && logical < last.block.logical + last.block.output) {
		return last.block;
	}
	Mark from;
	if (last_here && logical - last.block.logical < static_cast<uint64_t>(last.block.output) + NEAR_OUTPUT &&
		logical < span.walked.load(std::memory_order_acquire)) {
		from.physical = last.block.physical + BLOCK_HEADER + last.block.payload;
		from.logical = last.block.logical + last.block.output;
	} else {
		{
			std::lock_guard<std::mutex> guard(span.lock);
			if (logical >= span.frontier.logical) {
				WalkOnLocked(i, span, logical);
				if (logical >= span.frontier.logical) {
					throw FormatError(Where(file_, span.frontier.physical) +
									  ": no block of the chain holds data-set offset " + std::to_string(logical) +
									  " (the file changed since it was opened?)");
				}
			}
			auto mark = std::upper_bound(span.marks.begin(), span.marks.end(), logical,
										 [](uint64_t l, const Mark &m) { return l < m.logical; });
			from = *(mark - 1);	 // marks[0] is the checkpoint, at or before `logical`
		}
		if (last_here && last.block.physical >= from.physical) {
			from.physical = last.block.physical + BLOCK_HEADER + last.block.payload;
			from.logical = last.block.logical + last.block.output;
		}
	}
	for (;;) {
		const ChainBlock block = HeaderAt(from.physical, from.logical, stop);
		if (logical < from.logical + block.output) {
			last.stream = id_;
			last.block = block;
			return block;
		}
		from.physical += BLOCK_HEADER + block.payload;
		from.logical += block.output;
	}
}

uint64_t ContainerStream::MarkCount() const {
	uint64_t n = 0;
	for (const auto &span : spans_) {
		std::lock_guard<std::mutex> guard(span->lock);
		n += span->marks.size();
	}
	return n;
}

uint64_t ContainerStream::WalkedBytes() const {
	uint64_t n = 0;
	for (const auto &span : spans_) {
		std::lock_guard<std::mutex> guard(span->lock);
		n += span->frontier.physical - span->marks.front().physical;
	}
	return n;
}

void ContainerStream::ReadAt(uint64_t offset, uint8_t *dest, size_t count) const {
	if (offset > chain_.logical_size || count > chain_.logical_size - offset) {
		throw FormatError("backup file \"" + file_.Name() + "\": a read past the end of its data set");
	}
	// The thread's last block (§ Scan: each thread keeps its last block, at most 64 KB), by this
	// stream's id: a read in the block the thread read last -- the next page of a run, a page of the
	// group its search just probed -- takes no lock. Held weakly: the recent blocks own it, so it goes
	// with the stream (or when evicted), never kept by a worker thread past its backup (review).
	struct LastBlock {
		uint64_t stream = 0;
		std::weak_ptr<const DecodedBlock> block;
	};
	static thread_local LastBlock last;
	while (count > 0) {
		Shared d;
		if (last.stream == id_) {
			d = last.block.lock();
			if (d && (offset < d->block.logical || offset >= d->block.logical + d->block.output)) {
				d.reset();
			}
		}
		if (!d) {
			{
				std::lock_guard<std::mutex> guard(lock_);
				d = RecentLocked(offset);
			}
			if (!d) {
				// Located (its span's lock held only to look a mark up or walk on), read and decoded with no lock.
				d = Decode(Locate(offset));
				std::lock_guard<std::mutex> guard(lock_);
				KeepLocked(d);
			}
			last.stream = id_;
			last.block = d;
		}
		const size_t within = static_cast<size_t>(offset - d->block.logical);
		const size_t n = std::min(count, static_cast<size_t>(d->block.output) - within);
		std::memcpy(dest, d->output.data() + within, n);
		dest += n;
		offset += n;
		count -= n;
	}
}

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb
