#include "backup/backup_container.hpp"

#include "backup/backup_bytes.hpp"

#include <algorithm>
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
// The seek index record: u32 length at +0, u32 entry count at +28, entries (u64 logical, u64
// physical) from +40. Seen with 3 entries; bounded so a crafted count cannot allocate.
static constexpr size_t SEEK_COUNT_AT = 28;
static constexpr size_t SEEK_ENTRIES_AT = 40;
static constexpr uint32_t MAX_SEEK_ENTRIES = 1024;		  // 3 to 5 seen
static constexpr uint32_t SEEK_RECORD_TAG = 44;			  // u32 @0 (its meaning unknown; constant)
static constexpr uint32_t SEEK_RECORD_KIND = 0x00010001;  // u32 @24 (constant)

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
	// Seen: 3 MS_XPRESS, 0x83 ZSTD, 5 encrypted, 7 encrypted and compressed (E4); read as bits 0x2
	// compressed, 0x4 encrypted, 0x80 ZSTD. Only these four are accepted: any other word, 0x87
	// (encrypted and ZSTD) included until a backup shows it, is refused naming the word.
	const uint32_t f = header.flags;
	if (f != 0x3 && f != 0x83 && f != 0x5 && f != 0x7) {
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
		const uint8_t *thumbprint = r.Bytes(0x18, 20);
		for (size_t i = 0; i < 20; i++) {
			header.encryptor_thumbprint[i] = thumbprint[i];
		}
	}
	return header;
}

//! The chain block whose header is at `at`, or a block with kind 0 when no chain block is there
//! (the chain's end). `logical` is where its output starts.
static bool ReadBlockHeader(const RandomAccess &file, uint64_t at, uint64_t logical, ChainBlock &block) {
	if (at > file.Size() || file.Size() - at < BLOCK_HEADER) {
		return false;
	}
	uint8_t bytes[BLOCK_HEADER];
	file.ReadAt(at, bytes, BLOCK_HEADER);
	ByteReader r(bytes, BLOCK_HEADER, "a container block header", at);
	const uint8_t kind = r.U8(0);
	if (kind != KIND_COMPRESSED && kind != KIND_STORED) {
		return false;
	}
	block.kind = kind;
	block.physical = at;
	block.output = static_cast<uint32_t>(r.U8(1)) * 512;
	const uint16_t length = r.U16(2);
	block.payload = length == 0 ? MAX_BLOCK : length;
	block.logical = logical;
	if (block.output == 0 || block.output > MAX_BLOCK) {
		r.Fail(1, "a block whose output is " + std::to_string(block.output) + " bytes, not 1..65536");
	}
	if (block.payload > file.Size() - at - BLOCK_HEADER) {
		r.Fail(2, "a block whose " + std::to_string(block.payload) + "-byte payload runs past the end of the file");
	}
	if (kind == KIND_STORED && block.payload != block.output) {
		r.Fail(2, "a stored block of " + std::to_string(block.payload) + " bytes that claims an output of " +
					  std::to_string(block.output));
	}
	return true;
}

BlockChain WalkChain(const RandomAccess &file) {
	BlockChain chain;
	uint64_t at = CHAIN_START;
	uint64_t logical = 0;
	ChainBlock block;
	uint64_t since = 0;
	while (ReadBlockHeader(file, at, logical, block)) {
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
		throw FormatError(Where(file, CHAIN_START) + ": a container with no block where its chain starts");
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
	while (block.physical < stop) {
		if (logical < block.logical + block.output) {
			return block;
		}
		ChainBlock next;
		const uint64_t at = block.physical + BLOCK_HEADER + block.payload;
		if (!ReadBlockHeader(file, at, block.logical + block.output, next)) {
			break;
		}
		block = next;
	}
	throw FormatError(Where(file, block.physical) + ": no block of the chain holds data-set offset " +
					  std::to_string(logical) + " (the file changed since it was opened?)");
}

std::vector<SeekEntry> ReadSeekIndex(const RandomAccess &file, const BlockChain &chain) {
	std::vector<SeekEntry> entries;
	if (chain.end > file.Size() || file.Size() - chain.end < SEEK_ENTRIES_AT) {
		return entries;
	}
	auto head = Read(file, chain.end, SEEK_ENTRIES_AT, "the seek index");
	ByteReader r(head.data(), head.size(), "the seek index", chain.end);
	if (r.U32(0) != SEEK_RECORD_TAG || r.U32(24) != SEEK_RECORD_KIND) {
		// Not the record seen after every chain: no index, not a corrupt one.
		return entries;
	}
	const uint32_t count = r.U32(SEEK_COUNT_AT);
	if (count == 0 || count > MAX_SEEK_ENTRIES ||
		static_cast<uint64_t>(count) * 16 > file.Size() - chain.end - SEEK_ENTRIES_AT) {
		return entries;
	}
	auto body = Read(file, chain.end + SEEK_ENTRIES_AT, static_cast<size_t>(count) * 16, "the seek index entries");
	ByteReader e(body.data(), body.size(), "the seek index entries", chain.end + SEEK_ENTRIES_AT);
	uint64_t previous = 0;
	for (uint32_t i = 0; i < count; i++) {
		SeekEntry entry;
		entry.logical = e.U64(16 * i);
		entry.physical = e.U64(16 * i + 8);
		if ((i > 0 && entry.logical <= previous) || entry.logical >= chain.logical_size) {
			e.Fail(16 * i, "a seek index entry out of order or past the data set");
		}
		// It must name a chain block whose output starts at its logical offset.
		auto block = FindBlock(file, chain, entry.logical);
		if (block.logical != entry.logical || block.physical != entry.physical) {
			e.Fail(16 * i, "a seek index entry (logical " + std::to_string(entry.logical) + ", physical " +
							   std::to_string(entry.physical) + ") that names no block of the chain");
		}
		previous = entry.logical;
		entries.push_back(entry);
	}
	return entries;
}

ContainerStream::ContainerStream(const RandomAccess &file, BlockChain chain, BlockDecoder decoder)
	: file_(file), chain_(std::move(chain)), decoder_(std::move(decoder)), name_(file.Name() + " (data set)") {}

void ContainerStream::Decode(const ChainBlock &block) const {
	auto payload = Read(file_, block.physical + BLOCK_HEADER, block.payload, "a block's payload");
	cached_output_.resize(block.output);
	if (block.Stored()) {
		std::memcpy(cached_output_.data(), payload.data(), block.output);
	} else {
		if (!decoder_) {
			throw FormatError(Where(file_, block.physical) +
							  ": a compressed block, and no decoder for this container's algorithm");
		}
		decoder_(payload.data(), payload.size(), cached_output_.data(), block.output);
	}
	cached_ = block;
	has_cached_ = true;
}

void ContainerStream::ReadAt(uint64_t offset, uint8_t *dest, size_t count) const {
	if (offset > chain_.logical_size || count > chain_.logical_size - offset) {
		throw FormatError("backup file \"" + file_.Name() + "\": a read past the end of its data set");
	}
	std::lock_guard<std::mutex> guard(lock_);
	while (count > 0) {
		if (!has_cached_ || offset < cached_.logical || offset >= cached_.logical + cached_.output) {
			has_cached_ = false;
			Decode(FindBlock(file_, chain_, offset));
		}
		const size_t within = static_cast<size_t>(offset - cached_.logical);
		const size_t n = std::min(count, static_cast<size_t>(cached_.output) - within);
		std::memcpy(dest, cached_output_.data() + within, n);
		dest += n;
		offset += n;
		count -= n;
	}
}

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb
