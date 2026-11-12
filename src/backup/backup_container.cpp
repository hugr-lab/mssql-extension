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

//! The seek index's entries at `at` (where the chain ends), or empty when no seek index record is
//! there. Entries must ascend in both offsets and name a block header between the chain's start
//! and `at`; whether each starts its logical offset is the caller's to check.
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
		if ((i > 0 && (entry.logical <= entries.back().logical || entry.physical <= entries.back().physical)) ||
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
		// It must name a chain block whose output starts at its logical offset.
		auto block = FindBlock(file, chain, entry.logical);
		if (block.logical != entry.logical || block.physical != entry.physical) {
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

std::vector<uint8_t> ReadContainerTape(const RandomAccess &file, const BlockDecoder &decoder) {
	ChainBlock block;
	if (!ReadBlockHeader(file, HEADER_TAPE_AT, 0, block) ||
		block.payload > CONTAINER_HEADER - HEADER_TAPE_AT - BLOCK_HEADER) {
		throw FormatError(Where(file, HEADER_TAPE_AT) +
						  ": no block holding the media label where every compressed backup keeps it");
	}
	auto payload = Read(file, HEADER_TAPE_AT + BLOCK_HEADER, block.payload, "the media label block");
	std::vector<uint8_t> out(block.output);
	if (block.Stored()) {
		std::memcpy(out.data(), payload.data(), block.output);
	} else if (!decoder) {
		throw FormatError(Where(file, HEADER_TAPE_AT) + ": a compressed media label, and no decoder for it");
	} else {
		decoder(payload.data(), payload.size(), out.data(), block.output);
	}
	if (out.size() < 4 || std::memcmp(out.data(), "TAPE", 4) != 0) {
		throw FormatError(Where(file, HEADER_TAPE_AT) + ": the media label block does not hold a TAPE block");
	}
	return out;
}

bool OpenChainFromTail(const RandomAccess &file, BlockChain &chain, std::vector<SeekEntry> &seek) {
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
	if (!ReadBlockHeader(file, CHAIN_START, 0, block)) {
		return false;
	}
	located.checkpoints.push_back(block);
	// A block takes at least a header and a byte of payload and outputs at most 64 KB, so the data
	// set's offset at a physical offset is bounded: a crafted entry cannot claim more, or wrap.
	auto max_logical = [](uint64_t physical) { return (physical - CHAIN_START) / (BLOCK_HEADER + 1) * MAX_BLOCK; };
	for (const auto &entry : entries) {
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

ContainerStream::ContainerStream(const RandomAccess &file, BlockChain chain, BlockDecoder decoder)
	: file_(file), chain_(std::move(chain)), decoder_(std::move(decoder)), name_(file.Name() + " (data set)") {}

const ContainerStream::DecodedBlock *ContainerStream::Recent(uint64_t logical) const {
	for (const auto &d : recent_) {
		if (d.valid && logical >= d.block.logical && logical < d.block.logical + d.block.output) {
			return &d;
		}
	}
	return nullptr;
}

const ContainerStream::DecodedBlock &ContainerStream::Decoded(const ChainBlock &block) const {
	DecodedBlock &d = recent_[next_slot_];
	next_slot_ = (next_slot_ + 1) % recent_.size();
	d.valid = false;
	auto payload = Read(file_, block.physical + BLOCK_HEADER, block.payload, "a block's payload");
	d.output.resize(block.output);
	if (block.Stored()) {
		std::memcpy(d.output.data(), payload.data(), block.output);
	} else {
		if (!decoder_) {
			throw FormatError(Where(file_, block.physical) +
							  ": a compressed block, and no decoder for this container's algorithm");
		}
		decoder_(payload.data(), payload.size(), d.output.data(), block.output);
	}
	d.block = block;
	d.valid = true;
	return d;
}

ChainBlock ContainerStream::Locate(uint64_t logical) const {
	if (logical >= chain_.logical_size) {
		throw FormatError("backup file \"" + file_.Name() + "\": data-set offset " + std::to_string(logical) +
						  " is past its " + std::to_string(chain_.logical_size) + " bytes");
	}
	// From the last checkpoint at or before `logical`, walking headers; a checkpoint is added every
	// CHECKPOINT_STRIDE blocks and CHECKPOINT_BYTES of the file walked, as WalkChain keeps them,
	// so a chain located from its tail is walked once, piece by piece, and only where it is read.
	auto it = std::upper_bound(chain_.checkpoints.begin(), chain_.checkpoints.end(), logical,
							   [](uint64_t l, const ChainBlock &b) { return l < b.logical; });
	if (it == chain_.checkpoints.begin()) {
		throw FormatError("backup file \"" + file_.Name() + "\": a block chain with no checkpoint at or before " +
						  std::to_string(logical));
	}
	--it;
	ChainBlock block = *it;
	const uint64_t stop = (it + 1) != chain_.checkpoints.end() ? (it + 1)->physical : chain_.end;
	// From the nearest block located lately that lies in this span at or before `logical` (a block a
	// walk already confirmed, so the walk from it reads only the headers after it). The checkpoints
	// the walk adds keep their spacing from the span's checkpoint, as a walk from it would: in bytes
	// from the checkpoint itself, in blocks from the count the recent block was found at.
	const ChainBlock last_checkpoint_start = block;
	uint64_t since = 0;
	for (const auto &recent : located_) {
		if (recent.block.physical > block.physical && recent.block.physical < stop && recent.block.logical <= logical) {
			block = recent.block;
			since = recent.since;
		}
	}
	ChainBlock last_checkpoint = last_checkpoint_start;
	const ChainBlock start = block;
	std::vector<ChainBlock> added;
	while (block.physical < stop) {
		if (logical < block.logical + block.output) {
			if (!added.empty()) {
				chain_.checkpoints.insert(it + 1, added.begin(), added.end());
			}
			Located found;
			found.block = block;
			found.since = since;
			// Not recorded: a recent block found again without a step (it is in the ring already), and
			// the span's checkpoint itself (a walk starts there anyway; the ring keeps only blocks past
			// one, review of #552).
			if (block.physical == start.physical) {
				return block;
			}
			if (located_.size() < RECENT_LOCATED) {
				located_.push_back(found);
			} else {
				located_[next_located_] = found;
				next_located_ = (next_located_ + 1) % RECENT_LOCATED;
			}
			return block;
		}
		ChainBlock next;
		const uint64_t at = block.physical + BLOCK_HEADER + block.payload;
		if (at >= stop && (it + 1) != chain_.checkpoints.end()) {
			// The walk reached the next checkpoint: it must land on it, at its data-set offset, so
			// any walk across a span confirms the checkpoints at its ends (a seek index entry's
			// logical offset is otherwise trusted).
			if (at != stop || block.logical + block.output != (it + 1)->logical) {
				throw FormatError(Where(file_, at) + ": the chain's blocks do not reach the checkpoint at offset " +
								  std::to_string(stop) + " (data-set offset " + std::to_string((it + 1)->logical) +
								  ") where it says");
			}
			break;
		}
		if (!ReadBlockHeader(file_, at, block.logical + block.output, next)) {
			break;
		}
		block = next;
		if (++since >= BlockChain::CHECKPOINT_STRIDE &&
			block.physical - last_checkpoint.physical >= BlockChain::CHECKPOINT_BYTES && block.physical < stop) {
			added.push_back(block);
			last_checkpoint = block;
			since = 0;
		}
	}
	throw FormatError(Where(file_, block.physical) + ": no block of the chain holds data-set offset " +
					  std::to_string(logical) + " (the file changed since it was opened?)");
}

void ContainerStream::ReadAt(uint64_t offset, uint8_t *dest, size_t count) const {
	if (offset > chain_.logical_size || count > chain_.logical_size - offset) {
		throw FormatError("backup file \"" + file_.Name() + "\": a read past the end of its data set");
	}
	std::lock_guard<std::mutex> guard(lock_);
	while (count > 0) {
		const DecodedBlock *d = Recent(offset);
		if (!d) {
			d = &Decoded(Locate(offset));
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
