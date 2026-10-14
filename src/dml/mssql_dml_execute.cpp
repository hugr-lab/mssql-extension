#include "dml/mssql_dml_execute.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include "connection/mssql_settings.hpp"
#include "duckdb/main/client_context.hpp"
#include "query/tds_info_log.hpp"
#include "tds/tds_packet.hpp"
#include "tds/tds_socket.hpp"

namespace duckdb {
namespace mssql {

namespace {

int GetDmlExecuteDebugLevel() {
	static const int level = []() {
		const char *env = std::getenv("MSSQL_DEBUG");
		return env ? std::atoi(env) : 0;
	}();
	return level;
}

#define DML_EXECUTE_DEBUG(lvl, fmt, ...)                             \
	do {                                                             \
		if (GetDmlExecuteDebugLevel() >= lvl)                        \
			fprintf(stderr, "[MSSQL DML] " fmt "\n", ##__VA_ARGS__); \
	} while (0)

//! Cancel the statement: ATTENTION, then its acknowledgement. A connection
//! whose acknowledgement did not come cannot be used again.
void Cancel(tds::TdsConnection &connection) {
	connection.SendAttention();
	connection.WaitForAttentionAck(5000);
	if (connection.GetState() == tds::ConnectionState::Executing) {
		connection.TransitionState(tds::ConnectionState::Executing, tds::ConnectionState::Disconnected);
	}
}

template <class SEND>
DmlBatchResponse Execute(ClientContext &context, tds::TdsConnection &connection, const DmlBatchOptions &options,
						 SEND &&send) {
	using Status = DmlBatchResponse::Status;
	DmlBatchResponse response;
	auto *socket = connection.GetSocket();
	if (!socket) {
		response.status = Status::SEND_FAILED;
		response.error_message = "the connection has no socket";
		return response;
	}
	// Whatever an earlier statement left unread is not this one's response.
	socket->ClearReceiveBuffer();
	if (!send()) {
		response.status = Status::SEND_FAILED;
		response.error_message = "could not send the statement: " + connection.GetLastError();
		return response;
	}

	const int timeout_s = options.no_deadline ? 0 : LoadQueryTimeout(context);
	const bool has_deadline = timeout_s > 0;
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(has_deadline ? timeout_s : 0);
	const int64_t fail_after_tokens = LoadTestFailParseAfterTokens(context);
	int64_t tokens_seen = 0;

	tds::TokenParser parser;
	std::vector<tds::ColumnMetadata> columns;
	bool server_error = false;
	bool parse_error = false;
	bool done = false;
	while (!done) {
		// Wait in slices of at most 1 s and ask between them. A packet half-read
		// when a slice ends stays in the socket's assembly buffer.
		int recv_timeout = 1000;
		if (has_deadline) {
			const auto remaining =
				std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now())
					.count();
			if (remaining <= 0) {
				DML_EXECUTE_DEBUG(1, "%s: no response within %d s, cancelling", options.label, timeout_s);
				Cancel(connection);
				response.status = Status::TIMEOUT;
				response.error_message = "no response within mssql_query_timeout (" + std::to_string(timeout_s) +
										 " s); the statement was cancelled";
				return response;
			}
			recv_timeout = static_cast<int>(std::min<long long>(remaining, recv_timeout));
		}

		tds::TdsPacket packet;
		if (!socket->ReceivePacket(packet, recv_timeout)) {
			// A broken connection marks the socket dead (over TLS too since
			// #456). One still connected answered nothing yet: the slice ended,
			// or a signal cut the wait short (EINTR), which is no reason to give
			// up on the statement. Go round; the deadline check above decides.
			if (!socket->IsConnected()) {
				response.status = Status::RECEIVE_FAILED;
				response.error_message = "Failed to receive TDS packet: " + socket->GetLastError();
				if (connection.GetState() == tds::ConnectionState::Executing) {
					connection.TransitionState(tds::ConnectionState::Executing, tds::ConnectionState::Disconnected);
				}
				return response;
			}
			if (context.IsInterrupted()) {
				DML_EXECUTE_DEBUG(1, "%s: interrupted, cancelling", options.label);
				Cancel(connection);
				response.status = Status::INTERRUPTED;
				response.error_message = "Interrupted";
				return response;
			}
			continue;
		}

		const bool is_eom = packet.IsEndOfMessage();
		// Not into a parser already in Error: nothing consumes from there, so
		// every byte fed would be kept (issue #323). The packets are still read
		// to the end of the message, which is what leaves the socket clean for
		// the next statement.
		const auto &payload = packet.GetPayload();
		if (!payload.empty() && parser.GetState() != tds::ParserState::Error) {
			parser.Feed(payload);
		}

		tds::ParsedTokenType token;
		while ((token = parser.TryParseNext()) != tds::ParsedTokenType::NeedMoreData) {
			if (fail_after_tokens > 0 && ++tokens_seen >= fail_after_tokens) {
				// Test lever (mssql_test_fail_parse_after_tokens): drop this token
				// and desync, as a framing error would.
				parser.InjectParseError("injected parse error (mssql_test_fail_parse_after_tokens)");
				break;
			}
			switch (token) {
			case tds::ParsedTokenType::ColMetadata:
				columns = parser.GetColumnMetadata();
				if (options.on_metadata) {
					options.on_metadata(columns);
				}
				break;
			case tds::ParsedTokenType::Row:
				if (options.on_row) {
					options.on_row(parser.GetRow(), columns);
				}
				break;
			case tds::ParsedTokenType::Done: {
				const tds::DoneToken &done_token = parser.GetDone();
				DML_EXECUTE_DEBUG(2, "%s: DONE status=0x%04x row_count=%llu has_row_count=%d final=%d", options.label,
								  done_token.status, static_cast<unsigned long long>(done_token.row_count),
								  done_token.HasRowCount(), done_token.IsFinal());
				if (done_token.HasRowCount()) {
					response.rows_affected = done_token.row_count;
				}
				if (done_token.IsFinal()) {
					done = true;
					connection.TransitionState(tds::ConnectionState::Executing, tds::ConnectionState::Idle);
				}
				break;
			}
			case tds::ParsedTokenType::Error:
				// The first error is the cause; the server's later ones follow
				// from it. Keep reading: the response is drained either way.
				if (!server_error) {
					const tds::TdsError &error = parser.GetError();
					response.error_number = error.number;
					response.error_message = error.message;
					server_error = true;
				}
				break;
			case tds::ParsedTokenType::Info:
				LogTdsInfo(context, parser.GetInfo());
				break;
			default:
				break;
			}
		}

		// A parser in Error answers NeedMoreData forever and never yields the
		// final DONE, so the end of the message ends the read below. Name the
		// desync, or the statement would read as a success with every token
		// after it dropped, a server ERROR among them. An error the server sent
		// before the desync keeps precedence.
		if (parser.GetState() == tds::ParserState::Error && !server_error && !parse_error) {
			parse_error = true;
			response.error_message = "TDS parse error: " + parser.GetParseError();
			DML_EXECUTE_DEBUG(1, "%s: %s", options.label, response.error_message.c_str());
		}
		if (is_eom && !done) {
			done = true;
			connection.TransitionState(tds::ConnectionState::Executing, tds::ConnectionState::Idle);
		}
	}

	if (server_error) {
		response.status = Status::SERVER_ERROR;
	} else if (parse_error) {
		response.status = Status::PARSE_ERROR;
	}
	DML_EXECUTE_DEBUG(2, "%s: rows_affected=%llu, status=%d", options.label,
					  static_cast<unsigned long long>(response.rows_affected), static_cast<int>(response.status));
	return response;
}

}  // namespace

std::string DmlBatchResponse::DescribeError() const {
	if (status == Status::SERVER_ERROR && error_number != 0) {
		return "SQL Server error " + std::to_string(error_number) + ": " + error_message;
	}
	return error_message;
}

DmlBatchResponse ExecuteDmlBatch(ClientContext &context, tds::TdsConnection &connection, const tds::Request &request,
								 const DmlBatchOptions &options) {
	return Execute(context, connection, options, [&]() { return connection.ExecuteRequest(request, options.label); });
}

DmlBatchResponse ExecuteDmlBatch(ClientContext &context, tds::TdsConnection &connection, const std::string &sql,
								 const DmlBatchOptions &options) {
	return Execute(context, connection, options, [&]() { return connection.ExecuteBatch(sql, options.label); });
}

}  // namespace mssql
}  // namespace duckdb
