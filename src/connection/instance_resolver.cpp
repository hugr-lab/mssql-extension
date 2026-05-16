//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// instance_resolver.cpp
//
// Implementation of the MC-SQLR client. See instance_resolver.hpp for the
// public interface and specs/045-named-instance-resolution/ for the design.
//
// Phase 0 (this commit): ParseBrowserResponse only. The UDP transport and
// the InstanceResolver::Resolve entry points land in Phase 1.
//===----------------------------------------------------------------------===//

#include "connection/instance_resolver.hpp"

#include <cctype>
#include <cstdio>
#include <sstream>
#include <stdexcept>

namespace duckdb {
namespace mssql {

namespace {

//===--------------------------------------------------------------------===//
// HexDump - first N bytes as "XX XX XX ..." for diagnostic error messages.
// We never include more than 32 bytes; a hex dump in the error is for support
// triage, not a debugger replacement.
//===--------------------------------------------------------------------===//
std::string HexDump(const uint8_t *data, std::size_t len, std::size_t max_bytes = 32) {
	std::string out;
	const std::size_t n = (len < max_bytes) ? len : max_bytes;
	out.reserve(n * 3);
	for (std::size_t i = 0; i < n; ++i) {
		char buf[4];
		std::snprintf(buf, sizeof(buf), "%02X ", data[i]);
		out += buf;
	}
	if (!out.empty()) {
		out.pop_back();  // trailing space
	}
	if (len > max_bytes) {
		out += " ... (";
		out += std::to_string(len);
		out += " bytes total)";
	}
	return out;
}

//===--------------------------------------------------------------------===//
// Token scanner over the ASCII body of a SVR_RESP.
//
// Body format (NUL-terminated):
//   key;value;key;value;...;tcp;<port>;np;<pipe>;;next_record_first_key;...
//
// Field separator: ';'. Record separator: ";;" or the trailing '\0'.
// We accept either as end-of-record.
//===--------------------------------------------------------------------===//
class TokenScanner {
public:
	TokenScanner(const char *p, std::size_t len) : begin_(p), end_(p + len), cur_(p) {}

	// Reads up to the next ';'. Returns true on success and fills `out`.
	// Returns false at end of input. Throws on overflow (no terminator).
	bool Next(std::string &out) {
		if (cur_ >= end_) {
			return false;
		}
		const char *start = cur_;
		while (cur_ < end_ && *cur_ != ';') {
			++cur_;
		}
		if (cur_ >= end_) {
			// Hit end-of-buffer without finding ';' - tolerate the final
			// trailing token (some real-world responses omit the closing ';'
			// before the NUL).
			out.assign(start, static_cast<std::size_t>(cur_ - start));
			return !out.empty();
		}
		out.assign(start, static_cast<std::size_t>(cur_ - start));
		++cur_;	 // skip ';'
		return true;
	}

	// Returns true if the *next* character is ';' (i.e. we just hit ";;"),
	// indicating end-of-record. Does NOT advance.
	bool AtRecordEnd() const {
		return cur_ < end_ && *cur_ == ';';
	}

	void ConsumeRecordEnd() {
		if (AtRecordEnd()) {
			++cur_;
		}
	}

	bool AtEnd() const {
		return cur_ >= end_;
	}

private:
	const char *begin_;
	const char *end_;
	const char *cur_;
};

//===--------------------------------------------------------------------===//
// ParseRecord - reads alternating key/value tokens until end-of-record or
// end-of-buffer. Returns the populated BrowserInstance.
//
// Unknown fields are silently skipped (forward-compat with new SQL Server
// builds that add fields we don't care about). Order-independent; we key
// off the field name, not the position.
//===--------------------------------------------------------------------===//
BrowserInstance ParseRecord(TokenScanner &scanner) {
	BrowserInstance inst{};
	inst.tcp_port = 0;
	inst.tcp_enabled = false;
	inst.is_clustered = false;

	std::string key;
	std::string value;
	while (scanner.Next(key)) {
		if (scanner.AtRecordEnd()) {
			// Key without value at end-of-record. SQL Server doesn't emit this
			// but be lenient - treat as empty value and stop.
			scanner.ConsumeRecordEnd();
			break;
		}
		if (!scanner.Next(value)) {
			// Key with no value and no terminator - tolerate.
			break;
		}

		// Field dispatch. Field names are case-sensitive per the SQL Browser
		// implementation; we match documented spellings only.
		if (key == "ServerName") {
			inst.server_name = value;
		} else if (key == "InstanceName") {
			inst.instance_name = value;
		} else if (key == "Version") {
			inst.version = value;
		} else if (key == "IsClustered") {
			inst.is_clustered = (value == "Yes" || value == "yes" || value == "YES");
		} else if (key == "tcp") {
			if (!value.empty()) {
				try {
					int port = std::stoi(value);
					if (port > 0 && port <= 65535) {
						inst.tcp_port = static_cast<uint16_t>(port);
						inst.tcp_enabled = true;
					}
				} catch (...) {
					// Malformed port - leave tcp_enabled false. The caller
					// distinguishes "not present" from "garbage" only at the
					// resolver layer, not the parser.
				}
			}
		}
		// All other keys (np, rpc, via, ...) are silently ignored.

		if (scanner.AtRecordEnd()) {
			scanner.ConsumeRecordEnd();
			break;
		}
	}
	return inst;
}

}  // namespace

std::vector<BrowserInstance> ParseBrowserResponse(const uint8_t *data, std::size_t len) {
	// Header: opcode (1 byte) + size (LE u16) + body.
	if (len < 3) {
		std::ostringstream oss;
		oss << "malformed SQL Browser response: too short (" << len << " bytes; need at least 3); hex: " << HexDump(data, len);
		throw std::runtime_error(oss.str());
	}

	if (data[0] != 0x05) {
		std::ostringstream oss;
		oss << "malformed SQL Browser response: unexpected opcode 0x" << std::hex << static_cast<int>(data[0])
		    << " (expected 0x05); hex: " << HexDump(data, len);
		throw std::runtime_error(oss.str());
	}

	const std::size_t advertised = static_cast<std::size_t>(data[1]) | (static_cast<std::size_t>(data[2]) << 8);
	const std::size_t available = len - 3;

	// The size field is the byte count of the body INCLUDING the trailing NUL.
	// Truncated payloads are a hard error - we don't try to recover partial
	// data, because the part we lost may contain the only tcp port we need.
	if (advertised > available) {
		std::ostringstream oss;
		oss << "malformed SQL Browser response: advertised size " << advertised << " exceeds payload " << available
		    << "; hex: " << HexDump(data, len);
		throw std::runtime_error(oss.str());
	}

	// Some real-world payloads include trailing garbage past the advertised
	// size. Trust the size field, not the buffer length.
	std::size_t body_len = advertised;

	// Strip optional trailing NUL from the body before tokenising.
	if (body_len > 0 && data[3 + body_len - 1] == 0x00) {
		body_len -= 1;
	}

	std::vector<BrowserInstance> records;
	if (body_len == 0) {
		return records;	 // empty response - instance not found
	}

	TokenScanner scanner(reinterpret_cast<const char *>(data + 3), body_len);
	while (!scanner.AtEnd()) {
		BrowserInstance inst = ParseRecord(scanner);
		// Require InstanceName to count as a valid record; otherwise the
		// response is structurally broken (or we read past the end into
		// alignment padding).
		if (inst.instance_name.empty()) {
			break;
		}
		records.push_back(std::move(inst));
	}

	return records;
}

}  // namespace mssql
}  // namespace duckdb
