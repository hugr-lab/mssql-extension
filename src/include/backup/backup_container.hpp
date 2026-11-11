//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// backup/backup_container.hpp
//
// Spec 085 task 4: the MSSQLBAK container SQL Server writes for a compressed (or encrypted)
// backup. A 4 KB header ("MSSQLBAK", version, flags), a plain SFMB at 4 KB, then from 8 KB a
// chain of blocks back to back -- a 28-byte header (kind, output size in 512-byte units, payload
// length) and its payload -- whose outputs, concatenated, are the MTF data set (SSET .. MSLS) of
// an uncompressed backup. After the chain a seek index names the blocks that start a few MTF
// positions (the end of each data pass, the trailer): spec 085 § Rules (MSSQLBAK), task 4.
//
// The chain index keeps a checkpoint every 64 blocks and at most one per 1 MiB of file (D4a); a
// read walks the headers from the nearest checkpoint before it, adding checkpoints as it goes (a
// chain located from its tail starts with block 0 and the seek index's entries). Decompression
// is the caller's (XPRESS task 5, ZSTD task 6). DuckDB-free.
//===----------------------------------------------------------------------===//

#pragma once

#include "backup/backup_mtf.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace duckdb {
namespace mssql {
namespace backup {

enum class CompressionAlgorithm : uint8_t { None, MsXpress, Zstd };

const char *CompressionAlgorithmName(CompressionAlgorithm algorithm);

struct ContainerHeader {
	uint32_t version = 0;
	uint32_t flags = 0;
	bool compressed = false;
	bool encrypted = false;
	CompressionAlgorithm algorithm = CompressionAlgorithm::None;
	//! An encrypted backup's encryptor thumbprint (20 bytes at 0x18, E4); zeros otherwise.
	std::array<uint8_t, 20> encryptor_thumbprint{};
};

//! True when `file` starts with the container's magic (and so is not plain MTF).
bool IsContainer(const RandomAccess &file);

//! The container's header. An unknown version or flag combination is refused naming the word.
ContainerHeader ReadContainerHeader(const RandomAccess &file);

//! One block of the chain, as its 28-byte header describes it.
struct ChainBlock {
	uint8_t kind = 0;		// 0x21 compressed, 0x23 stored
	uint64_t physical = 0;	// its header's file offset
	uint32_t payload = 0;	// payload bytes after the header
	uint32_t output = 0;	// decompressed bytes: unit × 512
	uint64_t logical = 0;	// where its output starts in the data set
	bool Stored() const {
		return kind == 0x23;
	}
};

//! The chain: a checkpoint every CHECKPOINT_STRIDE blocks and at most one per CHECKPOINT_BYTES
//! of the file, whichever comes later (D4a: about 40 MB for 10 TB instead of an entry per block,
//! and a crafted chain of tiny blocks cannot make it larger than one entry per MiB), the data
//! set's length, and where the chain ends.
struct BlockChain {
	static constexpr uint64_t CHECKPOINT_STRIDE = 64;
	static constexpr uint64_t CHECKPOINT_BYTES = 1 << 20;
	std::vector<ChainBlock> checkpoints;
	//! Every block's count after WalkChain; 0 for a chain located from its tail.
	uint64_t block_count = 0;
	uint64_t logical_size = 0;
	uint64_t end = 0;
};

//! The chain of `file` from 8 KB, walked header by header (the payloads are skipped, not read).
//! Ends at the first header whose kind is neither 0x21 nor 0x23. Every length is checked against
//! the file and the 64 KB block limit.
BlockChain WalkChain(const RandomAccess &file);

//! One entry of the seek index after the chain: a block that starts at `logical`.
struct SeekEntry {
	uint64_t logical = 0;
	uint64_t physical = 0;
};

//! The chain located from the file's tail, no block walked but the trailer's (E13, DQ-41): the seek
//! index found from the tail record, its entries and the first block as the checkpoints, the data
//! set's size from the trailer's blocks. False when the tail is not the layout every sample has
//! (the caller then walks the chain); a tail that has it but disagrees with its blocks throws.
//! An entry's data-set offset is bounded by its physical one but not otherwise checked here: a
//! read that walks up to an entry checks it lands there (ContainerStream), the MTF walk of the data
//! set reads a DBLK at each entry (the pass boundaries) and checks it, and a page read checks the
//! page's header names it (D4).
bool OpenChainFromTail(const RandomAccess &file, BlockChain &chain, std::vector<SeekEntry> &seek);

//! The block holding logical offset `logical`, found from its checkpoint by walking headers.
ChainBlock FindBlock(const RandomAccess &file, const BlockChain &chain, uint64_t logical);

//! The seek index at `chain.end`, or empty when what follows the chain is not one (its layout was
//! inferred from two samples, MS_XPRESS and ZSTD, 2025). Every entry must name a block of the
//! chain whose output starts at its logical offset, else the index is refused.
std::vector<SeekEntry> ReadSeekIndex(const RandomAccess &file, const BlockChain &chain);

//! Decompresses one block's payload into exactly `output` bytes, or throws.
using BlockDecoder =
	std::function<void(const uint8_t *payload, size_t payload_size, uint8_t *output, size_t output_size)>;

//! The container's media label: the TAPE block (with its RAID stream: the media set, its family
//! count and this file's number) that a compressed backup keeps in its header, as a chain block at
//! 0x1E0 (E13 follow-up, 16 of 16 containers), decoded. Throws when it is not there.
std::vector<uint8_t> ReadContainerTape(const RandomAccess &file, const BlockDecoder &decoder);

//! The data set as a RandomAccess over the container: a read finds its blocks through the chain
//! and decodes them (stored blocks are copied; compressed ones go to `decoder`, which may be
//! empty until tasks 5 / 6, then refused by name). Keeps the last decoded blocks per instance and
//! serialises its readers: it serves the header walks (SSET .. MSCI, the trailer). The open pass
//! and the scan decode blocks on their own threads (D10), each with its own buffer (task 7).
class ContainerStream : public RandomAccess {
public:
	ContainerStream(const RandomAccess &file, BlockChain chain, BlockDecoder decoder);

	uint64_t Size() const override {
		return chain_.logical_size;
	}
	void ReadAt(uint64_t offset, uint8_t *dest, size_t count) const override;
	const std::string &Name() const override {
		return name_;
	}
	//! The block whose output holds data-set offset `logical`, the chain walked to it.
	ChainBlock BlockOf(uint64_t logical) const {
		std::lock_guard<std::mutex> guard(lock_);
		return Locate(logical);
	}
	//! A copy: readers add checkpoints to the chain as they walk it.
	BlockChain Chain() const {
		std::lock_guard<std::mutex> guard(lock_);
		return chain_;
	}

private:
	struct DecodedBlock {
		ChainBlock block;
		bool valid = false;
		std::vector<uint8_t> output;
	};
	//! `block` decoded, from the recent blocks when it is one of them, else into the oldest slot.
	const DecodedBlock &Decoded(const ChainBlock &block) const;
	//! The recent block holding data-set offset `logical`, or null.
	const DecodedBlock *Recent(uint64_t logical) const;
	//! The block of `logical`: walked from its checkpoint, adding checkpoints as WalkChain would.
	ChainBlock Locate(uint64_t logical) const;

	const RandomAccess &file_;
	mutable BlockChain chain_;
	BlockDecoder decoder_;
	std::string name_;
	mutable std::mutex lock_;
	//! The last RECENT_BLOCKS blocks decoded (1 MiB): a search reads a window of 16 groups and the
	//! pages it found are then read from the same blocks, which would otherwise be decoded twice
	//! (measured on the large fixture's MS_XPRESS heap: decoding was then the scan's whole cost).
	static constexpr size_t RECENT_BLOCKS = 16;
	mutable std::vector<DecodedBlock> recent_ = std::vector<DecodedBlock>(RECENT_BLOCKS);
	mutable size_t next_slot_ = 0;
	//! The blocks the last RECENT_LOCATED Locates returned: a later offset in the same span between
	//! checkpoints is walked to from the nearest of them at or before it, not from the checkpoint.
	//! In a well-compressed container a checkpoint's span holds ~1600 blocks (one checkpoint per MiB
	//! of the file, a 64 KB block of pages compressing to ~600 bytes), and a search's probes step back
	//! and forth within it (measured: a 530k-row heap of the large fixture, 527 s, 94% of it walking
	//! headers). Kept apart from the chain's checkpoints, whose count D4a bounds.
	static constexpr size_t RECENT_LOCATED = 64;
	struct Located {
		ChainBlock block;
		uint64_t since = 0;	 // blocks walked past the checkpoint before it, for CHECKPOINT_STRIDE
	};
	mutable std::vector<Located> located_;
	mutable size_t next_located_ = 0;
};

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb
