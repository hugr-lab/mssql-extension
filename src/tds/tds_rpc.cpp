#include "tds/tds_rpc.hpp"

#include "tds/encoding/utf16.hpp"

#include <stdexcept>

namespace duckdb {
namespace tds {

RpcRequestBuilder::RpcRequestBuilder(uint16_t proc_id) {
	// ProcIDSwitch 0xFFFF, then the USHORT ProcID, then OptionFlags (USHORT, 0:
	// no WithRecompile, no NoMetaData).
	body_.push_back(0xFF);
	body_.push_back(0xFF);
	body_.push_back(static_cast<uint8_t>(proc_id & 0xFF));
	body_.push_back(static_cast<uint8_t>(proc_id >> 8));
	body_.push_back(0);
	body_.push_back(0);
}

void RpcRequestBuilder::BeginParam(const std::string &name, uint8_t status) {
	// B_VARCHAR: the length in UTF-16 code units, then the UTF-16LE name.
	const auto utf16 = encoding::Utf16LEEncode(name);
	if (utf16.size() / 2 > 255) {
		// A longer name would wrap the length byte and the rest of it would be
		// read as the parameter's flags and type: a malformed request.
		throw std::invalid_argument("RPC parameter name longer than 255 UTF-16 code units: " + name);
	}
	body_.push_back(static_cast<uint8_t>(utf16.size() / 2));
	body_.insert(body_.end(), utf16.begin(), utf16.end());
	body_.push_back(status);
}

void RpcRequestBuilder::Append(const uint8_t *data, size_t length) {
	body_.insert(body_.end(), data, data + length);
}

}  // namespace tds
}  // namespace duckdb
