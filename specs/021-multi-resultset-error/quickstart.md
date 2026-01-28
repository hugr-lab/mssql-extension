# Quickstart: Multi-Resultset Error Detection

## Change 1: Add DrainRemainingTokens Helper Method

**File**: `src/query/mssql_result_stream.cpp`
**Location**: After `DrainAfterCancel()` method (after line 480)

```cpp
void MSSQLResultStream::DrainRemainingTokens() {
	// Drain remaining TDS tokens after detecting an error condition.
	// Similar to DrainAfterCancel but without sending ATTENTION signal.
	// SQL Server is already sending the remaining data — we just consume it.
	parser_.SetSkipMode(true);

	auto start = std::chrono::steady_clock::now();
	auto timeout = std::chrono::milliseconds(cancel_timeout_ms_);

	while (true) {
		auto elapsed = std::chrono::steady_clock::now() - start;
		if (elapsed > timeout) {
			MSSQL_DEBUG_LOG(1, "DrainRemainingTokens: TIMEOUT, closing connection");
			connection_->Close();
			return;
		}

		tds::ParsedTokenType token = parser_.TryParseNext();

		if (token == tds::ParsedTokenType::Done) {
			auto done = parser_.GetDone();
			if (done.IsFinal()) {
				connection_->TransitionState(tds::ConnectionState::Executing, tds::ConnectionState::Idle);
				return;
			}
		} else if (token == tds::ParsedTokenType::NeedMoreData) {
			if (!ReadMoreData(cancel_read_timeout_ms_)) {
				MSSQL_DEBUG_LOG(1, "DrainRemainingTokens: ReadMoreData failed, closing connection");
				connection_->Close();
				return;
			}
		} else if (token == tds::ParsedTokenType::None) {
			if (parser_.GetState() == tds::ParserState::Complete) {
				connection_->TransitionState(tds::ConnectionState::Executing, tds::ConnectionState::Idle);
				return;
			}
			if (parser_.GetState() == tds::ParserState::Error) {
				connection_->Close();
				return;
			}
		}
		// All other tokens: skip (ROW, ColMetadata, Info, Error, etc.)
	}
}
```

## Change 2: Add ColMetadata Case in FillChunk

**File**: `src/query/mssql_result_stream.cpp`
**Location**: In `FillChunk()` switch statement, before the `default:` case (before line 297)

```cpp
		case tds::ParsedTokenType::ColMetadata: {
			// A second COLMETADATA token means another result set is starting.
			// This is not supported — only one result-producing statement per batch.
			state_ = MSSQLResultStreamState::Error;
			DrainRemainingTokens();
			throw InvalidInputException(
				"MSSQL Error: The SQL batch produced multiple result sets. "
				"Only one result-producing statement is allowed per mssql_scan() call. "
				"Ensure your batch contains only one SELECT or other result-producing statement, "
				"or use separate mssql_scan() calls for multiple result sets.");
		}
```

## Change 3: Declare DrainRemainingTokens in Header

**File**: `src/include/query/mssql_result_stream.hpp`
**Location**: Near `DrainAfterCancel()` declaration (private section)

```cpp
	void DrainRemainingTokens();
```

## Verification

1. Build: `make debug`
2. Run all tests: `make test`
3. Manual test: `FROM mssql_scan('db', 'SELECT * FROM dbo.test; SELECT ''hello''')`
   - Expected: Clear error message about multiple result sets
   - Expected: No internal crash
4. Manual test: `FROM mssql_scan('db', 'SELECT * INTO #t FROM dbo.test; SELECT * FROM #t')`
   - Expected: Works correctly (only one result-producing statement)
5. Manual test after error: Run a normal query on the same connection
   - Expected: Succeeds (connection is clean)
