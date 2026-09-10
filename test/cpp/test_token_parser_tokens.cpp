// test/cpp/test_token_parser_tokens.cpp
//
// Byte-level tests for the token types the TDS parser SKIPS rather than
// decodes (issue #323, spec 072). The skip group at tds_token_parser.cpp
// assumed every member is `type(1) + length(2) + data`; two members were not,
// one was registered under the wrong byte, and the failure was a two-byte
// desync that surfaced as "Unknown token type: 0x0" at the DONEPROC closing
// every stored-procedure call.
//
// Every stream here is a capture from a live SQL Server 2025 (MSSQL_DEBUG=1's
// unknown-token dump, which prints the buffer byte for byte), assembled with a
// COLMETADATA of the exact shape the server sent for `SELECT @v` -- not bytes
// written from the specification. The parser must yield the same token
// sequence a well-formed stream yields and must never enter the Error state.
//
// No SQL Server, no DuckDB runtime. Part of STANDALONE_TEST_SOURCES
// (`make test-cpp`), which CI runs. The live end is
// test/sql/regression/issue_323.test.

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "tds/tds_token_parser.hpp"

using duckdb::tds::ParsedTokenType;
using duckdb::tds::ParserState;
using duckdb::tds::TokenParser;

static int g_failures = 0;

static void Check(bool ok, const std::string &what) {
	if (ok) {
		return;
	}
	std::cerr << "FAIL: " << what << std::endl;
	g_failures++;
}

//! Drive the same loop the connection does, collecting what the parser yields.
static std::vector<ParsedTokenType> Drain(TokenParser &parser) {
	std::vector<ParsedTokenType> seen;
	for (int steps = 0; steps < 1000; steps++) {
		ParsedTokenType t = parser.TryParseNext();
		if (t == ParsedTokenType::NeedMoreData || t == ParsedTokenType::None) {
			break;
		}
		seen.push_back(t);
	}
	return seen;
}

static void Append(std::vector<uint8_t> &to, std::initializer_list<uint8_t> bytes) {
	to.insert(to.end(), bytes.begin(), bytes.end());
}

//===--------------------------------------------------------------------===//
// Captured fragments
//===--------------------------------------------------------------------===//

//! DONEPROC closing a procedure call: status 0, curcmd 0xE0, rowcount 0.
//! Captured after every EXEC in the survey ("fe 00 00 e0 00 00 00 00 00 00 00
//! 00 00", thirteen bytes -- TDS 7.2+, where DoneRowCount is 8 bytes).
static const std::initializer_list<uint8_t> DONEPROC_0 = {0xfe, 0x00, 0x00, 0xe0, 0x00, 0x00, 0x00,
														  0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

//! COLMETADATA for one INTN(4) column named "id". Shape captured from
//! `SELECT @v`: count 1, UserType 0, Flags 0x0021, type 0x26 INTN, length 4,
//! then the name as B_VARCHAR.
static const std::initializer_list<uint8_t> COLMETA_INT_ID = {0x81, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x21,
															  0x00, 0x26, 0x04, 0x02, 0x69, 0x00, 0x64, 0x00};

//! One ROW of that column, value 1.
static const std::initializer_list<uint8_t> ROW_INT_1 = {0xd1, 0x04, 0x01, 0x00, 0x00, 0x00};

//! DONE, status DONE_COUNT, curcmd 0xC1 (SELECT), rowcount 1.
static const std::initializer_list<uint8_t> DONE_SELECT_1 = {0xfd, 0x10, 0x00, 0xc1, 0x00, 0x01, 0x00,
															 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

//! TABNAME + COLINFO as captured from `SELECT id FROM dbo.I323T FOR BROWSE`:
//! TABNAME length 0x15, two parts "dbo" and "I323T" in UCS-2; COLINFO length 3.
static const std::initializer_list<uint8_t> TABNAME_COLINFO = {
	0xa4, 0x15, 0x00, 0x02, 0x03, 0x00, 0x64, 0x00, 0x62, 0x00, 0x6f, 0x00, 0x05, 0x00, 0x49,
	0x00, 0x33, 0x00, 0x32, 0x00, 0x33, 0x00, 0x54, 0x00, 0xa5, 0x03, 0x00, 0x01, 0x01, 0x00};

//===--------------------------------------------------------------------===//
// Tests
//===--------------------------------------------------------------------===//

//! RETURN n. The old skip read the low two bytes of the value as a length: 0
//! left two bytes behind, 7 consumed five bytes of the DONEPROC that follows.
//! Either way the parser next saw a 0x00 byte and died.
static void TestReturnStatusValues() {
	const uint32_t values[] = {0, 7, 1, 0x100, 0xFFFFFFFFu};
	for (uint32_t v : values) {
		std::vector<uint8_t> stream;
		Append(stream, {0x79, static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v >> 16),
						static_cast<uint8_t>(v >> 24)});
		Append(stream, DONEPROC_0);

		TokenParser parser;
		parser.Feed(stream.data(), stream.size());
		auto seen = Drain(parser);

		const std::string tag = "RETURN " + std::to_string(v) + ": ";
		Check(parser.GetState() != ParserState::Error,
			  tag + "parser did not enter Error (" + parser.GetParseError() + ")");
		Check(seen.size() == 1 && seen[0] == ParsedTokenType::Done, tag + "exactly one token, DONEPROC");
		if (seen.size() == 1 && seen[0] == ParsedTokenType::Done) {
			const auto &done = parser.GetDone();
			Check(done.status == 0, tag + "DONEPROC status is 0");
			Check(done.cur_cmd == 0xE0, tag + "DONEPROC curcmd is 0xE0");
			Check(done.row_count == 0, tag + "DONEPROC rowcount is 0");
			Check(done.IsFinal(), tag + "DONEPROC is final");
		}
	}
}

//! A RETURNSTATUS split across packets: the first three bytes arrive alone.
//! That is NeedMoreData, never an error, and never a partial consume.
static void TestReturnStatusSplitAcrossPackets() {
	TokenParser parser;
	const uint8_t head[] = {0x79, 0x07, 0x00};
	parser.Feed(head, sizeof(head));
	Check(parser.TryParseNext() == ParsedTokenType::NeedMoreData, "3 of 5 bytes: NeedMoreData");
	Check(parser.GetState() != ParserState::Error, "3 of 5 bytes: not an error");

	std::vector<uint8_t> rest;
	Append(rest, {0x00, 0x00});
	Append(rest, DONEPROC_0);
	parser.Feed(rest.data(), rest.size());
	auto seen = Drain(parser);
	Check(seen.size() == 1 && seen[0] == ParsedTokenType::Done, "after the rest arrives: DONEPROC");
	Check(parser.GetState() != ParserState::Error, "after the rest arrives: not an error");
}

//! A procedure with a result set: COLMETADATA, ROW, DONEINPROC-less (NOCOUNT
//! ON), RETURNSTATUS, DONEPROC. The rows must come through and the DONEPROC
//! must still be found after the status.
static void TestProcedureWithResultSet() {
	std::vector<uint8_t> stream;
	Append(stream, COLMETA_INT_ID);
	Append(stream, ROW_INT_1);
	Append(stream, {0x79, 0x00, 0x00, 0x00, 0x00});
	Append(stream, DONEPROC_0);

	TokenParser parser;
	parser.Feed(stream.data(), stream.size());
	auto seen = Drain(parser);
	Check(parser.GetState() != ParserState::Error, "proc with rows: not an error (" + parser.GetParseError() + ")");
	Check(seen.size() == 3, "proc with rows: three tokens");
	if (seen.size() == 3) {
		Check(seen[0] == ParsedTokenType::ColMetadata, "proc with rows: COLMETADATA first");
		Check(seen[1] == ParsedTokenType::Row, "proc with rows: then ROW");
		Check(seen[2] == ParsedTokenType::Done, "proc with rows: then DONEPROC after the status");
	}
}

//! FOR BROWSE. TABNAME was registered as 0x04 while the wire says 0xA4, so
//! the arm that skips it correctly was never reached and the stream failed
//! with "Unknown token type: 0x164" (decimal 164 -- see the message test).
static void TestForBrowseTabnameColinfo() {
	std::vector<uint8_t> stream;
	Append(stream, COLMETA_INT_ID);
	Append(stream, TABNAME_COLINFO);
	Append(stream, ROW_INT_1);
	Append(stream, DONE_SELECT_1);

	TokenParser parser;
	parser.Feed(stream.data(), stream.size());
	auto seen = Drain(parser);
	Check(parser.GetState() != ParserState::Error, "FOR BROWSE: not an error (" + parser.GetParseError() + ")");
	Check(seen.size() == 3, "FOR BROWSE: three tokens, TABNAME and COLINFO skipped");
	if (seen.size() == 3) {
		Check(seen[0] == ParsedTokenType::ColMetadata, "FOR BROWSE: COLMETADATA");
		Check(seen[1] == ParsedTokenType::Row, "FOR BROWSE: ROW survives the two skipped tokens");
		Check(seen[2] == ParsedTokenType::Done, "FOR BROWSE: DONE");
		Check(parser.GetDone().row_count == 1, "FOR BROWSE: DONE rowcount 1");
	}
}

//! RETURNVALUE has no length field and cannot be skipped. It also cannot
//! arrive over SQL_BATCH. If it ever does, it must fail BY NAME rather than
//! read ParamOrdinal as a length and desync silently, which is what it did.
static void TestReturnValueFailsByName() {
	std::vector<uint8_t> stream;
	Append(stream, {0xac, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
	Append(stream, DONEPROC_0);

	TokenParser parser;
	parser.Feed(stream.data(), stream.size());
	auto seen = Drain(parser);
	Check(parser.GetState() == ParserState::Error, "RETURNVALUE: parser refuses");
	Check(seen.empty(), "RETURNVALUE: nothing yielded past it");
	Check(parser.GetParseError().find("RETURNVALUE") != std::string::npos,
		  "RETURNVALUE: the refusal names the token (" + parser.GetParseError() + ")");
	Check(parser.GetParseError().find("RPC") != std::string::npos, "RETURNVALUE: the refusal says why");
}

//! The unknown-token message printed std::to_string(byte) -- decimal -- after
//! a "0x" prefix, so 0xA4 reported as "0x164". A byte that really is unknown
//! must report in hex.
static void TestUnknownTokenMessageIsHex() {
	const uint8_t stream[] = {0x99, 0x00, 0x00, 0x00};
	TokenParser parser;
	parser.Feed(stream, sizeof(stream));
	Drain(parser);
	Check(parser.GetState() == ParserState::Error, "0x99: parser refuses");
	const std::string &msg = parser.GetParseError();
	Check(msg.find("0x99") != std::string::npos, "0x99: message says 0x99 (" + msg + ")");
	Check(msg.find("153") == std::string::npos, "0x99: message does not say 153 (" + msg + ")");
}

int main() {
	TestReturnStatusValues();
	TestReturnStatusSplitAcrossPackets();
	TestProcedureWithResultSet();
	TestForBrowseTabnameColinfo();
	TestReturnValueFailsByName();
	TestUnknownTokenMessageIsHex();

	if (g_failures > 0) {
		std::cerr << g_failures << " check(s) failed" << std::endl;
		return 1;
	}
	std::cout << "All token parser token tests passed" << std::endl;
	return 0;
}
