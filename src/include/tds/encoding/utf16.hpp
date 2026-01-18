#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace duckdb {
namespace tds {
namespace encoding {

//===----------------------------------------------------------------------===//
// UTF-16 Encoding Utilities
//===----------------------------------------------------------------------===//

/// Encode a UTF-8 string to UTF-16LE bytes
/// @param input UTF-8 encoded string
/// @return UTF-16LE encoded byte vector
std::vector<uint8_t> Utf16LEEncode(const std::string& input);

/// Decode UTF-16LE bytes to a UTF-8 string
/// @param data Pointer to UTF-16LE encoded data
/// @param byte_length Length in bytes (not characters)
/// @return UTF-8 encoded string
std::string Utf16LEDecode(const uint8_t* data, size_t byte_length);

/// Decode UTF-16LE byte vector to a UTF-8 string
/// @param data UTF-16LE encoded byte vector
/// @return UTF-8 encoded string
std::string Utf16LEDecode(const std::vector<uint8_t>& data);

/// Get the byte length of a string when encoded as UTF-16LE
/// @param input UTF-8 encoded string
/// @return Number of bytes needed for UTF-16LE encoding
size_t Utf16LEByteLength(const std::string& input);

}  // namespace encoding
}  // namespace tds
}  // namespace duckdb
