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
// stream walks the span after a checkpoint as far as it is read, keeping a mark every 256 KiB of
// the file it walked (a chain located from its tail starts with block 0 and the seek index's
// entries). Decompression is the caller's (XPRESS task 5, ZSTD task 6). DuckDB-free.
//===----------------------------------------------------------------------===//

#pragma once

#include "backup/backup_mtf.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
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
//! empty until tasks 5 / 6, then refused by name). Keeps the last decoded blocks per instance: it
//! serves the header walks (SSET .. MSCI, the trailer), and the page reads of the catalog and the
//! scan (PageIndex reads a container's pages through it; one stream a family), from several threads
//! (the scan's tasks, PR 2e). Each thread keeps the last block it read (§ Scan; weakly, the recent
//! blocks owning it), so a run of reads within one block -- a block holds 8 pages -- takes no lock.
//! A block is located from the marks of its span (Span), under that span's lock only to look a mark
//! up or to walk the span further; the walk from the mark (or from the thread's last block located)
//! to the block reads the file through the thread's window with no lock, and so do the payload's
//! read and decoding, so readers decode in parallel. Two readers missing one block at once both
//! decode it (the second's copy is not kept). The open pass decodes blocks on its own threads (D10),
//! each with its own buffer (task 7).
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
	//! The block whose output holds data-set offset `logical`.
	ChainBlock BlockOf(uint64_t logical) const {
		return Locate(logical);
	}
	//! The chain as the stream was made (its checkpoints bound the spans; nothing adds to them).
	BlockChain Chain() const {
		return chain_;
	}
	//! The marks kept, every span's (for the tests: what the reads walked, D4a's bound).
	uint64_t MarkCount() const;
	//! The file bytes walked so far, every span's (for the tests: a read walks only up to its block).
	uint64_t WalkedBytes() const;

private:
	struct DecodedBlock {
		ChainBlock block;
		std::vector<uint8_t> output;
	};
	using Shared = std::shared_ptr<const DecodedBlock>;
	//! A block's start: its header's file offset and its output's data-set offset.
	struct Mark {
		uint64_t physical = 0;
		uint64_t logical = 0;
	};
	//! The span from checkpoint i to the next (or the chain's end), walked as far as it was read:
	//! the walk's frontier (the first block not walked) and a mark at the checkpoint and at the
	//! first block past every MARK_BYTES of the file walked since the last one. 16 bytes per 256 KiB
	//! of the file walked, 64 KB a GB, whatever the blocks' sizes (D4a's tier: resident while open).
	struct Span {
		std::mutex lock;
		std::vector<Mark> marks;
		Mark frontier;
		bool complete = false;
		//! frontier.logical, published for the reads that walk on from the thread's last block with
		//! no lock (only ever grows; written under the lock).
		std::atomic<uint64_t> walked{0};
	};
	static constexpr uint64_t MARK_BYTES = 256 << 10;
	//! How far past the thread's last block (in output) a read walks on from it with no lock: the next
	//! block or so (a block outputs at most 64 KiB).
	static constexpr uint64_t NEAR_OUTPUT = 64 << 10;
	//! `block` read and decoded; touches no member but the file and the decoder (no lock).
	Shared Decode(const ChainBlock &block) const;
	//! `decoded` into the oldest slot, unless a reader kept the same block meanwhile; the lock is held.
	void KeepLocked(Shared decoded) const;
	//! The recent block holding data-set offset `logical`, or null; the lock is held.
	Shared RecentLocked(uint64_t logical) const;
	//! The block of `logical`: the span walked to it when it is past the frontier, then walked to from
	//! the nearest mark, or the thread's last block located, at or before it.
	ChainBlock Locate(uint64_t logical) const;
	//! Span i walked on from its frontier past the block holding `logical`, or to its end; marks kept
	//! as it goes. The span's lock is held.
	void WalkOnLocked(size_t i, Span &span, uint64_t logical) const;
	//! The block header at `at` (output starting at `logical`) from the thread's window; throws when no
	//! chain block is there or it runs past `stop`.
	ChainBlock HeaderAt(uint64_t at, uint64_t logical, uint64_t stop) const;
	//! Where span i ends in the file.
	uint64_t SpanStop(size_t i) const;
	//! `count` bytes of the file at `at`, from the thread's window of it (read anew when it does not
	//! hold them); valid until the thread's next Window.
	const uint8_t *Window(uint64_t at, size_t count) const;

	const RandomAccess &file_;
	const BlockChain chain_;
	BlockDecoder decoder_;
	std::string name_;
	//! This stream among every one made, so a thread's last block (ReadAt) is never another
	//! stream's, even one at a reused address.
	const uint64_t id_;
	//! One a checkpoint; the vector itself never changes after construction (no lock to index it).
	std::vector<std::unique_ptr<Span>> spans_;
	mutable std::mutex lock_;
	//! The last RECENT_BLOCKS blocks decoded (up to 4 MiB). A search reads windows of 16 groups, its
	//! galloping probes overlap, and the pages it found are then read from the same blocks: with
	//! 16 slots every block of a scan was decoded about 5 times (measured, 25k blocks decoded 125k
	//! times on a 200k-page heap); with 64, once. 128 measured no better.
	//! Held as shared pointers: a reader copies out of a block it took after the lock is released,
	//! while another may evict it from its slot.
	static constexpr size_t RECENT_BLOCKS = 64;
	mutable std::vector<Shared> recent_ = std::vector<Shared>(RECENT_BLOCKS);
	mutable size_t next_slot_ = 0;
};

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb
