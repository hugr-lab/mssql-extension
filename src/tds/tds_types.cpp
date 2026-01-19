#include "tds/tds_types.hpp"

namespace duckdb {
namespace tds {

const char *ConnectionStateToString(ConnectionState state) {
	switch (state) {
	case ConnectionState::Disconnected:
		return "Disconnected";
	case ConnectionState::Authenticating:
		return "Authenticating";
	case ConnectionState::Idle:
		return "Idle";
	case ConnectionState::Executing:
		return "Executing";
	case ConnectionState::Cancelling:
		return "Cancelling";
	default:
		return "Unknown";
	}
}

}  // namespace tds
}  // namespace duckdb
