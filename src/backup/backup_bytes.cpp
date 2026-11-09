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

static void AppendUtf8(std::string &out, uint32_t cp) {
	if (cp < 0x80) {
		out += static_cast<char>(cp);
	} else if (cp < 0x800) {
		out += static_cast<char>(0xC0 | (cp >> 6));
		out += static_cast<char>(0x80 | (cp & 0x3F));
	} else if (cp < 0x10000) {
		out += static_cast<char>(0xE0 | (cp >> 12));
		out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
		out += static_cast<char>(0x80 | (cp & 0x3F));
	} else {
		out += static_cast<char>(0xF0 | (cp >> 18));
		out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
		out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
		out += static_cast<char>(0x80 | (cp & 0x3F));
	}
}

std::string Utf16LeToUtf8(const uint8_t *data, size_t bytes) {
	size_t units = bytes / 2;
	while (units > 0 && data[2 * (units - 1)] == 0 && data[2 * (units - 1) + 1] == 0) {
		units--;
	}
	std::string out;
	out.reserve(units);
	for (size_t i = 0; i < units; i++) {
		uint32_t u = static_cast<uint32_t>(data[2 * i] | (data[2 * i + 1] << 8));
		if (u >= 0xD800 && u <= 0xDBFF && i + 1 < units) {
			uint32_t v = static_cast<uint32_t>(data[2 * i + 2] | (data[2 * i + 3] << 8));
			if (v >= 0xDC00 && v <= 0xDFFF) {
				AppendUtf8(out, 0x10000 + ((u - 0xD800) << 10) + (v - 0xDC00));
				i++;
				continue;
			}
		}
		AppendUtf8(out, (u >= 0xD800 && u <= 0xDFFF) ? 0xFFFD : u);
	}
	return out;
}

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb
