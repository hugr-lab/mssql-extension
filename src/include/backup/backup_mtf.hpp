//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// backup/backup_mtf.hpp
//
// Spec 085 task 3: the Microsoft Tape Format walk of one family file (or of a compressed
// container's logical stream, task 4). A descriptor block (DBLK) is a 52-byte common header with
// an XOR checksum, its block-specific bytes, then streams (22-byte headers, checksummed) up to an
// SPAD; the next DBLK starts where the SPAD's data ends; an SFMB is TAPE's soft-filemark size
// long. Walked strictly by those lengths, with no resync: a DBLK that does not validate where the
// previous one ended is a FormatError (spec 085 § Rules, MTF). DuckDB-free (D15).
//===----------------------------------------------------------------------===//

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace duckdb {
namespace mssql {
namespace backup {

//! Positional reads of one file (or logical stream). DuckDB-free, so the parsers and their fuzz
//! harnesses read memory; the extension adapts ByteSource to it.
class RandomAccess {
public:
	virtual ~RandomAccess() {}
	virtual uint64_t Size() const = 0;
	//! Exactly `count` bytes at `offset` into `dest`; the caller has checked the bounds.
	virtual void ReadAt(uint64_t offset, uint8_t *dest, size_t count) const = 0;
	//! The file's name, for errors.
	virtual const std::string &Name() const = 0;
};

//! RandomAccess over bytes in memory (tests, fuzz harnesses, a decompressed header region).
class MemoryAccess : public RandomAccess {
public:
	MemoryAccess(std::string name, const uint8_t *data, size_t size)
		: name_(std::move(name)), data_(data), size_(size) {}
	uint64_t Size() const override {
		return size_;
	}
	void ReadAt(uint64_t offset, uint8_t *dest, size_t count) const override;
	const std::string &Name() const override {
		return name_;
	}

private:
	std::string name_;
	const uint8_t *data_;
	size_t size_;
};

struct MtfStream {
	std::string id;
	uint16_t fs_attributes = 0;
	uint16_t media_attributes = 0;
	uint16_t encryption = 0;
	uint16_t compression = 0;
	uint64_t header_offset = 0;
	uint64_t data_offset = 0;
	uint64_t length = 0;
};

struct MtfBlock {
	std::string type;
	uint64_t offset = 0;
	uint32_t attributes = 0;
	uint16_t first_event = 0;
	uint8_t string_type = 0;
	//! For TAPE, SSET and VOLB the block's bytes [offset, offset + first_event): the common header
	//! and the block-specific part, where their strings live (tape addresses are block-relative).
	//! Every other block keeps its 52-byte common header only.
	std::vector<uint8_t> header;
	std::vector<MtfStream> streams;
	//! Where the next block starts.
	uint64_t end = 0;

	//! The first stream named `id`, or null.
	const MtfStream *Stream(const char *id) const;
};

struct MtfLayout {
	std::vector<MtfBlock> blocks;
	//! True when the walk read a whole family file (TAPE first); false for a compressed container's
	//! logical stream, the MTF data set, which starts at SSET (spec 085 § Rules, MSSQLBAK).
	bool has_tape = true;
	//! TAPE's soft_filemark_block_size × 512: an SFMB's length.
	uint32_t soft_filemark_bytes = 0;

	//! The first block of `type`, or null.
	const MtfBlock *Find(const char *type) const;
	//! Every block of `type`.
	std::vector<const MtfBlock *> FindAll(const char *type) const;
};

//! The blocks of `file`, from offset 0 to its end exactly. Every header's checksum is checked;
//! the first block must be TAPE; any length that leaves the file, a block that does not advance,
//! or a header that does not validate is a FormatError naming the file and the offset.
MtfLayout WalkMtf(const RandomAccess &file);

//! The blocks of a compressed container's logical stream: the MTF data set, SSET first and no
//! TAPE or SFMB in it (an SFMB in it is refused). The same strict rules otherwise.
MtfLayout WalkMtfDataSet(const RandomAccess &stream);

//! A stream's data, at most `max_bytes` of it (a FormatError when it is longer: a header stream
//! the format keeps small, read whole).
std::vector<uint8_t> ReadStreamData(const RandomAccess &file, const MtfStream &stream, size_t max_bytes);

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb
