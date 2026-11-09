#include "backup/backup_mtf.hpp"

#include "backup/backup_bytes.hpp"

#include <cstring>

namespace duckdb {
namespace mssql {
namespace backup {

static constexpr size_t DBLK_HEADER = 52;
static constexpr size_t STREAM_HEADER = 22;
// Real backups have tens of blocks (16-44 in every experiment); a crafted file of minimal blocks
// would otherwise grow the layout without bound.
static constexpr size_t MAX_BLOCKS = 1000000;

//! The blocks whose block-specific bytes a reader needs (their strings and fields); every other
//! block keeps only its 52-byte common header, so a crafted file cannot make the walk hold
//! 64 KB per block (spec 085 § Security).
static bool KeepsHeader(const std::string &type) {
	return type == "TAPE" || type == "SSET" || type == "VOLB";
}

void MemoryAccess::ReadAt(uint64_t offset, uint8_t *dest, size_t count) const {
	if (offset > size_ || count > size_ - offset) {
		throw FormatError("backup file \"" + name_ + "\": a read past its end");
	}
	std::memcpy(dest, data_ + offset, count);
}

const MtfStream *MtfBlock::Stream(const char *id) const {
	for (const auto &stream : streams) {
		if (stream.id == id) {
			return &stream;
		}
	}
	return nullptr;
}

const MtfBlock *MtfLayout::Find(const char *type) const {
	for (const auto &block : blocks) {
		if (block.type == type) {
			return &block;
		}
	}
	return nullptr;
}

std::vector<const MtfBlock *> MtfLayout::FindAll(const char *type) const {
	std::vector<const MtfBlock *> found;
	for (const auto &block : blocks) {
		if (block.type == type) {
			found.push_back(&block);
		}
	}
	return found;
}

static std::string Where(const RandomAccess &file, uint64_t offset) {
	return "backup file \"" + file.Name() + "\" at offset " + std::to_string(offset);
}

//! XOR of the 16-bit little-endian words of `bytes` (MTF's header checksum).
static uint16_t Xor16(const uint8_t *bytes, size_t count) {
	uint16_t sum = 0;
	for (size_t i = 0; i + 1 < count; i += 2) {
		sum ^= static_cast<uint16_t>(bytes[i] | (bytes[i + 1] << 8));
	}
	return sum;
}

static bool IsTypeChar(uint8_t c) {
	return c >= 0x30 && c <= 0x5A;
}

static std::string TypeOf(const uint8_t *bytes) {
	return std::string(reinterpret_cast<const char *>(bytes), 4);
}

static std::vector<uint8_t> ReadExact(const RandomAccess &file, uint64_t offset, size_t count, const char *what) {
	if (offset > file.Size() || count > file.Size() - offset) {
		throw FormatError(Where(file, offset) + ": " + what + " (" + std::to_string(count) +
						  " bytes) runs past the end of the " + std::to_string(file.Size()) + "-byte file");
	}
	std::vector<uint8_t> bytes(count);
	if (count > 0) {
		file.ReadAt(offset, bytes.data(), count);
	}
	return bytes;
}

static MtfStream ReadStreamHeader(const RandomAccess &file, uint64_t offset, const std::string &block) {
	auto bytes = ReadExact(file, offset, STREAM_HEADER, "a stream header");
	ByteReader r(bytes.data(), bytes.size(), "an MTF stream header", offset);
	if (!IsTypeChar(bytes[0]) || !IsTypeChar(bytes[1]) || !IsTypeChar(bytes[2]) || !IsTypeChar(bytes[3]) ||
		Xor16(bytes.data(), 20) != r.U16(20)) {
		throw FormatError(Where(file, offset) + ": no valid stream header in the " + block +
						  " block (its checksum or its id does not match)");
	}
	MtfStream stream;
	stream.id = TypeOf(bytes.data());
	stream.fs_attributes = r.U16(4);
	stream.media_attributes = r.U16(6);
	stream.length = r.U64(8);
	stream.encryption = r.U16(16);
	stream.compression = r.U16(18);
	stream.header_offset = offset;
	stream.data_offset = offset + STREAM_HEADER;
	if (stream.length > file.Size() - stream.data_offset) {
		throw FormatError(Where(file, offset) + ": the " + stream.id + " stream of the " + block + " block claims " +
						  std::to_string(stream.length) + " bytes, past the end of the file");
	}
	return stream;
}

MtfLayout WalkMtf(const RandomAccess &file) {
	MtfLayout layout;
	const uint64_t size = file.Size();
	uint64_t offset = 0;
	while (offset < size) {
		auto common = ReadExact(file, offset, DBLK_HEADER, "a descriptor block header");
		ByteReader r(common.data(), common.size(), "an MTF descriptor block", offset);
		if (!IsTypeChar(common[0]) || !IsTypeChar(common[1]) || !IsTypeChar(common[2]) || !IsTypeChar(common[3]) ||
			Xor16(common.data(), 50) != r.U16(50)) {
			throw FormatError(Where(file, offset) + ": no valid descriptor block where the previous one ended" +
							  (layout.blocks.empty() ? std::string(" (not an MTF backup file?)")
													 : " (after " + layout.blocks.back().type + ")"));
		}
		MtfBlock block;
		block.type = TypeOf(common.data());
		block.offset = offset;
		block.attributes = r.U32(4);
		block.first_event = r.U16(8);
		block.string_type = r.U8(48);
		if (layout.blocks.empty() && block.type != "TAPE") {
			throw FormatError(Where(file, offset) + ": the first descriptor block is " + block.type + ", not TAPE");
		}
		if (block.first_event != 0 && block.first_event < DBLK_HEADER) {
			throw FormatError(Where(file, offset) + ": the " + block.type + " block's first stream (" +
							  std::to_string(block.first_event) + ") lies inside its own header");
		}
		const size_t header_size =
			(block.first_event == 0 || !KeepsHeader(block.type)) ? DBLK_HEADER : block.first_event;
		block.header =
			header_size == DBLK_HEADER ? std::move(common) : ReadExact(file, offset, header_size, "a descriptor block");
		if (block.type == "TAPE") {
			ByteReader tape(block.header.data(), block.header.size(), "the TAPE block", offset);
			layout.soft_filemark_bytes = static_cast<uint32_t>(tape.U16(64)) * 512;
			if (layout.soft_filemark_bytes == 0) {
				throw FormatError(Where(file, offset) + ": the TAPE block gives a soft-filemark size of 0");
			}
		}
		if (block.type == "SFMB") {
			if (layout.soft_filemark_bytes == 0) {
				throw FormatError(Where(file, offset) + ": an SFMB before any TAPE block");
			}
			if (block.first_event > layout.soft_filemark_bytes) {
				throw FormatError(Where(file, offset) + ": an SFMB whose first stream (" +
								  std::to_string(block.first_event) + ") lies past its own " +
								  std::to_string(layout.soft_filemark_bytes) + " bytes");
			}
			block.end = offset + layout.soft_filemark_bytes;
		} else {
			if (block.first_event == 0) {
				throw FormatError(Where(file, offset) + ": the " + block.type + " block has no streams");
			}
			uint64_t at = offset + block.first_event;
			while (true) {
				auto stream = ReadStreamHeader(file, at, block.type);
				// Stream data is padded to 4 bytes; the next stream, or block, follows.
				uint64_t next = stream.data_offset + stream.length;
				next = (next + 3) & ~static_cast<uint64_t>(3);
				const bool spad = stream.id == "SPAD";
				block.streams.push_back(std::move(stream));
				at = next;
				if (spad) {
					break;
				}
				if (at >= size) {
					throw FormatError(Where(file, offset) + ": the " + block.type +
									  " block's streams run to the end of the file with no SPAD");
				}
			}
			block.end = at;
		}
		if (block.end <= offset) {
			throw FormatError(Where(file, offset) + ": the " + block.type + " block does not advance");
		}
		if (block.end > size) {
			throw FormatError(Where(file, offset) + ": the " + block.type + " block ends at " +
							  std::to_string(block.end) + ", past the end of the " + std::to_string(size) +
							  "-byte file");
		}
		offset = block.end;
		layout.blocks.push_back(std::move(block));
		if (layout.blocks.size() > MAX_BLOCKS) {
			throw FormatError(Where(file, offset) + ": more than " + std::to_string(MAX_BLOCKS) +
							  " descriptor blocks: not a backup SQL Server wrote");
		}
	}
	if (layout.blocks.empty()) {
		throw FormatError("backup file \"" + file.Name() + "\" is empty");
	}
	return layout;
}

std::vector<uint8_t> ReadStreamData(const RandomAccess &file, const MtfStream &stream, size_t max_bytes) {
	if (stream.length > max_bytes) {
		throw FormatError(Where(file, stream.header_offset) + ": the " + stream.id + " stream is " +
						  std::to_string(stream.length) + " bytes, more than the " + std::to_string(max_bytes) +
						  " a header stream may hold");
	}
	return ReadExact(file, stream.data_offset, static_cast<size_t>(stream.length), "a stream's data");
}

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb
