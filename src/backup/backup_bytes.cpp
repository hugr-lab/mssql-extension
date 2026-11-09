#include "backup/backup_bytes.hpp"

namespace duckdb {
namespace mssql {
namespace backup {

void ByteReader::Fail(size_t offset, const std::string &message) const {
	// An offset past the view is not a place in the file (base_ + offset can wrap): name it in the view.
	const std::string where =
		offset <= size_ ? "at file offset " + std::to_string(base_ + offset)
						: "at offset " + std::to_string(offset) + " of its " + std::to_string(size_) + "-byte span";
	throw FormatError(std::string("backup file: ") + what_ + " " + where + ": " + message);
}

void ByteReader::ThrowOutOfBounds(size_t offset, size_t count) const {
	Fail(offset, "a read of " + std::to_string(count) + " byte(s) runs past the end of its " + std::to_string(size_) +
					 "-byte span");
}

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb
