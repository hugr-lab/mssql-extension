//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// backup/backup_bytes.hpp
//
// Spec 085 task 1: the bounded reads every backup parser makes. A backup file is untrusted
// input (spec 085, Security): every length, offset and count in it is checked against its
// container before use, and a read past a span is a named error, never a crash. DuckDB-free
// (D15), so the parsers' fuzz harnesses run without libduckdb.
//===----------------------------------------------------------------------===//

#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace duckdb {
namespace mssql {
namespace backup {

//! A backup file that does not parse: a structure out of bounds, a checksum that does not
//! match, a value the format does not allow. The message names the structure and the offset.
class FormatError : public std::runtime_error {
public:
	explicit FormatError(const std::string &message) : std::runtime_error(message) {}
};

//! A read-only view of `size` bytes at `data`, named `what` for errors, at `base` in the file
//! (so an error names the file offset, not the offset in the view). Every accessor checks its
//! bounds and throws FormatError past them. Little-endian, as everything SQL Server writes
//! except the MTF date. `what` is a string literal (never null): the view keeps the pointer.
//! The signed reads convert the unsigned value, which every supported compiler wraps (two's
//! complement; implementation-defined before C++20, not undefined).
class ByteReader {
public:
	ByteReader(const uint8_t *data, size_t size, const char *what, uint64_t base = 0)
		: data_(data), size_(size), what_(what), base_(base) {}

	size_t Size() const {
		return size_;
	}
	const uint8_t *Data() const {
		return data_;
	}
	const char *What() const {
		return what_;
	}
	uint64_t Base() const {
		return base_;
	}

	//! True when [offset, offset + count) lies inside the view (overflow-safe).
	bool Has(size_t offset, size_t count) const {
		return offset <= size_ && count <= size_ - offset;
	}
	//! Throws unless [offset, offset + count) lies inside the view.
	void Require(size_t offset, size_t count) const {
		if (!Has(offset, count)) {
			ThrowOutOfBounds(offset, count);
		}
	}

	uint8_t U8(size_t offset) const {
		Require(offset, 1);
		return data_[offset];
	}
	uint16_t U16(size_t offset) const {
		Require(offset, 2);
		return static_cast<uint16_t>(data_[offset] | (data_[offset + 1] << 8));
	}
	uint32_t U32(size_t offset) const {
		Require(offset, 4);
		return static_cast<uint32_t>(data_[offset]) | (static_cast<uint32_t>(data_[offset + 1]) << 8) |
			   (static_cast<uint32_t>(data_[offset + 2]) << 16) | (static_cast<uint32_t>(data_[offset + 3]) << 24);
	}
	uint64_t U64(size_t offset) const {
		Require(offset, 8);
		return static_cast<uint64_t>(U32(offset)) | (static_cast<uint64_t>(U32(offset + 4)) << 32);
	}
	int16_t I16(size_t offset) const {
		return static_cast<int16_t>(U16(offset));
	}
	int32_t I32(size_t offset) const {
		return static_cast<int32_t>(U32(offset));
	}
	int64_t I64(size_t offset) const {
		return static_cast<int64_t>(U64(offset));
	}
	//! `count` bytes at `offset`, as a pointer into the view.
	const uint8_t *Bytes(size_t offset, size_t count) const {
		Require(offset, count);
		return data_ + offset;
	}
	//! The `count` bytes at `offset` as a view of their own, named `what`.
	ByteReader Sub(size_t offset, size_t count, const char *what) const {
		Require(offset, count);
		return ByteReader(data_ + offset, count, what, base_ + offset);
	}

	//! A FormatError naming this view, the file offset and `message` (an offset past the view, which
	//! has no file offset, is named as one in the view).
	[[noreturn]] void Fail(size_t offset, const std::string &message) const;

private:
	[[noreturn]] void ThrowOutOfBounds(size_t offset, size_t count) const;

	const uint8_t *data_;
	size_t size_;
	const char *what_;
	uint64_t base_;
};

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb
