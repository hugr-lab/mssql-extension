#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace duckdb {
namespace tds {

//! Well-known stored procedures an RPC request can name by id instead of by
//! name ([MS-TDS] 2.2.6.6, ProcIDSwitch 0xFFFF).
constexpr uint16_t RPC_PROC_SP_EXECUTESQL = 10;
constexpr uint16_t RPC_PROC_SP_PREPARE = 11;
constexpr uint16_t RPC_PROC_SP_EXECUTE = 12;

//! Builds the body of an RPC request (RPCReqBatch, without ALL_HEADERS, which
//! TdsProtocol::BuildRpcMultiPacket prepends): the procedure, the option
//! flags, and the parameters. A parameter is opened here -- its name and
//! status flags -- and its TYPE_INFO and value are appended by the codec that
//! knows the type.
class RpcRequestBuilder {
public:
	explicit RpcRequestBuilder(uint16_t proc_id);

	//! Open a parameter: B_VARCHAR name (empty for a positional one) and
	//! StatusFlags (0 for input; 0x01 for OUTPUT).
	void BeginParam(const std::string &name, uint8_t status = 0);
	//! Append the parameter's TYPE_INFO and value bytes.
	void Append(const uint8_t *data, size_t length);

	std::vector<uint8_t> Finish() {
		return std::move(body_);
	}

private:
	std::vector<uint8_t> body_;
};

}  // namespace tds
}  // namespace duckdb
