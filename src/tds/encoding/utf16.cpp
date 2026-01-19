#include "tds/encoding/utf16.hpp"

namespace duckdb {
namespace tds {
namespace encoding {

std::vector<uint8_t> Utf16LEEncode(const std::string &input) {
	std::vector<uint8_t> result;
	result.reserve(input.size() * 2);  // Minimum size for ASCII

	size_t i = 0;
	while (i < input.size()) {
		uint32_t codepoint = 0;
		uint8_t byte = static_cast<uint8_t>(input[i]);

		// Decode UTF-8 to Unicode codepoint
		if ((byte & 0x80) == 0) {
			// Single byte (ASCII): 0xxxxxxx
			codepoint = byte;
			i += 1;
		} else if ((byte & 0xE0) == 0xC0) {
			// Two bytes: 110xxxxx 10xxxxxx
			if (i + 1 >= input.size())
				break;
			codepoint = ((byte & 0x1F) << 6) | (static_cast<uint8_t>(input[i + 1]) & 0x3F);
			i += 2;
		} else if ((byte & 0xF0) == 0xE0) {
			// Three bytes: 1110xxxx 10xxxxxx 10xxxxxx
			if (i + 2 >= input.size())
				break;
			codepoint = ((byte & 0x0F) << 12) | ((static_cast<uint8_t>(input[i + 1]) & 0x3F) << 6) |
						(static_cast<uint8_t>(input[i + 2]) & 0x3F);
			i += 3;
		} else if ((byte & 0xF8) == 0xF0) {
			// Four bytes: 11110xxx 10xxxxxx 10xxxxxx 10xxxxxx
			if (i + 3 >= input.size())
				break;
			codepoint = ((byte & 0x07) << 18) | ((static_cast<uint8_t>(input[i + 1]) & 0x3F) << 12) |
						((static_cast<uint8_t>(input[i + 2]) & 0x3F) << 6) |
						(static_cast<uint8_t>(input[i + 3]) & 0x3F);
			i += 4;
		} else {
			// Invalid UTF-8 byte, skip it
			i += 1;
			continue;
		}

		// Encode Unicode codepoint to UTF-16LE
		if (codepoint <= 0xFFFF) {
			// Basic Multilingual Plane (BMP) - single 16-bit code unit
			result.push_back(static_cast<uint8_t>(codepoint & 0xFF));		  // Low byte
			result.push_back(static_cast<uint8_t>((codepoint >> 8) & 0xFF));  // High byte
		} else if (codepoint <= 0x10FFFF) {
			// Supplementary planes - surrogate pair
			codepoint -= 0x10000;
			uint16_t high_surrogate = 0xD800 + ((codepoint >> 10) & 0x3FF);
			uint16_t low_surrogate = 0xDC00 + (codepoint & 0x3FF);
			// High surrogate first (in little-endian)
			result.push_back(static_cast<uint8_t>(high_surrogate & 0xFF));
			result.push_back(static_cast<uint8_t>((high_surrogate >> 8) & 0xFF));
			// Low surrogate second
			result.push_back(static_cast<uint8_t>(low_surrogate & 0xFF));
			result.push_back(static_cast<uint8_t>((low_surrogate >> 8) & 0xFF));
		}
		// Codepoints > 0x10FFFF are invalid, skip them
	}

	return result;
}

std::string Utf16LEDecode(const uint8_t *data, size_t byte_length) {
	std::string result;
	result.reserve(byte_length);  // Rough estimate

	size_t i = 0;
	while (i + 1 < byte_length) {
		// Read UTF-16LE code unit (little-endian)
		uint16_t code_unit = static_cast<uint16_t>(data[i]) | (static_cast<uint16_t>(data[i + 1]) << 8);
		i += 2;

		uint32_t codepoint;

		// Check for surrogate pair
		if (code_unit >= 0xD800 && code_unit <= 0xDBFF) {
			// High surrogate - expect low surrogate next
			if (i + 1 >= byte_length)
				break;
			uint16_t low_surrogate = static_cast<uint16_t>(data[i]) | (static_cast<uint16_t>(data[i + 1]) << 8);
			i += 2;

			if (low_surrogate >= 0xDC00 && low_surrogate <= 0xDFFF) {
				// Valid surrogate pair
				codepoint = 0x10000 + ((static_cast<uint32_t>(code_unit - 0xD800) << 10) | (low_surrogate - 0xDC00));
			} else {
				// Invalid surrogate pair, use replacement character
				codepoint = 0xFFFD;
			}
		} else if (code_unit >= 0xDC00 && code_unit <= 0xDFFF) {
			// Unexpected low surrogate, use replacement character
			codepoint = 0xFFFD;
		} else {
			// Regular BMP character
			codepoint = code_unit;
		}

		// Encode codepoint to UTF-8
		if (codepoint <= 0x7F) {
			result.push_back(static_cast<char>(codepoint));
		} else if (codepoint <= 0x7FF) {
			result.push_back(static_cast<char>(0xC0 | ((codepoint >> 6) & 0x1F)));
			result.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
		} else if (codepoint <= 0xFFFF) {
			result.push_back(static_cast<char>(0xE0 | ((codepoint >> 12) & 0x0F)));
			result.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
			result.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
		} else if (codepoint <= 0x10FFFF) {
			result.push_back(static_cast<char>(0xF0 | ((codepoint >> 18) & 0x07)));
			result.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
			result.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
			result.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
		}
	}

	return result;
}

std::string Utf16LEDecode(const std::vector<uint8_t> &data) {
	return Utf16LEDecode(data.data(), data.size());
}

size_t Utf16LEByteLength(const std::string &input) {
	size_t byte_count = 0;

	size_t i = 0;
	while (i < input.size()) {
		uint8_t byte = static_cast<uint8_t>(input[i]);
		uint32_t codepoint = 0;

		// Decode UTF-8 to get codepoint
		if ((byte & 0x80) == 0) {
			codepoint = byte;
			i += 1;
		} else if ((byte & 0xE0) == 0xC0) {
			if (i + 1 >= input.size())
				break;
			codepoint = ((byte & 0x1F) << 6) | (static_cast<uint8_t>(input[i + 1]) & 0x3F);
			i += 2;
		} else if ((byte & 0xF0) == 0xE0) {
			if (i + 2 >= input.size())
				break;
			codepoint = ((byte & 0x0F) << 12) | ((static_cast<uint8_t>(input[i + 1]) & 0x3F) << 6) |
						(static_cast<uint8_t>(input[i + 2]) & 0x3F);
			i += 3;
		} else if ((byte & 0xF8) == 0xF0) {
			if (i + 3 >= input.size())
				break;
			codepoint = ((byte & 0x07) << 18) | ((static_cast<uint8_t>(input[i + 1]) & 0x3F) << 12) |
						((static_cast<uint8_t>(input[i + 2]) & 0x3F) << 6) |
						(static_cast<uint8_t>(input[i + 3]) & 0x3F);
			i += 4;
		} else {
			i += 1;
			continue;
		}

		// Count UTF-16 bytes needed
		if (codepoint <= 0xFFFF) {
			byte_count += 2;  // Single code unit
		} else if (codepoint <= 0x10FFFF) {
			byte_count += 4;  // Surrogate pair
		}
	}

	return byte_count;
}

}  // namespace encoding
}  // namespace tds
}  // namespace duckdb
