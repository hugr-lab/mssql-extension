#include "query/mssql_result_stream.hpp"
#include <chrono>
#include <climits>
#include <cstdlib>
#include <iostream>
#include "catalog/mssql_catalog.hpp"
#include "codec/code_page/code_page_codec.hpp"
#include "codec/type_family.hpp"
#include "connection/mssql_connection_provider.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/logging/logger.hpp"
#include "duckdb/main/client_context.hpp"
#include "mssql_counters.hpp"
#include "query/tds_info_log.hpp"
#include "tds/encoding/type_converter.hpp"
#include "tds/encoding/utf16.hpp"
#include "tds/tds_packet.hpp"
#include "tds/tds_socket.hpp"

// Debug logging controlled by MSSQL_DEBUG environment variable
// Set MSSQL_DEBUG=1 to enable, MSSQL_DEBUG=2 for verbose row-level logging
static int GetDebugLevel() {
	static const int level = []() {
		const char *env = std::getenv("MSSQL_DEBUG");
		return env ? std::atoi(env) : 0;
	}();
	return level;
}

#define MSSQL_DEBUG_LOG(level, fmt, ...)                               \
	do {                                                               \
		if (GetDebugLevel() >= level) {                                \
			fprintf(stderr, "[MSSQL DEBUG] " fmt "\n", ##__VA_ARGS__); \
		}                                                              \
	} while (0)

namespace duckdb {

//! A statement's server errors as the user's error: the message the first one's, as it always was, and
//! the numbers of all of them, in order, in the exception's extra info (MSSQL_SERVER_ERROR_NUMBERS) for a
//! caller that answers some of them (issue #570: a catalog scan's 207 / 208, and the 4413 after them when
//! the object read is a view).
[[noreturn]] static void ThrowServerError(const std::vector<tds::TdsError> &errors) {
	string numbers;
	for (const auto &e : errors) {
		numbers += (numbers.empty() ? "" : ",") + std::to_string(e.number);
	}
	const auto &err = errors[0];
	throw InvalidInputException(
		{{MSSQL_SERVER_ERROR_NUMBERS, numbers}},
		StringUtil::Format("SQL Server error [%d, severity %d]: %s", err.number, err.severity, err.message));
}

//===----------------------------------------------------------------------===//
// MSSQLResultStream Implementation
//===----------------------------------------------------------------------===//

MSSQLResultStream::MSSQLResultStream(std::shared_ptr<tds::TdsConnection> connection, const tds::Request &request,
									 const string &context_name, bool client_transcode, tds::GeometryAs geometry_as,
									 weak_ptr<tds::ConnectionPool> pool_handle, bool transaction_pinned,
									 int query_timeout_seconds, bool reset_on_release)
	: connection_(std::move(connection)),
	  context_name_(context_name),
	  pool_handle_(std::move(pool_handle)),
	  transaction_pinned_(transaction_pinned),
	  reset_on_release_(reset_on_release),
	  client_transcode_(client_transcode),
	  geometry_as_(geometry_as),
	  sql_(request.sql),
	  request_(request.IsRpc() ? request : tds::Request()),
	  state_(MSSQLResultStreamState::Initializing),
	  is_cancelled_(false),
	  rows_read_(0),
	  // MSSQL_COUNTERS, not MSSQL_DEBUG — see mssql_counters.hpp for the two
	  // measurements that forced the split. MSSQL_DEBUG>=1 still enables them so
	  // existing scripts keep working, but every phase number taken that way is
	  // measuring the logging as well as the work.
	  counters_enabled_(mssql::CountersEnabled()),
	  timing_enabled_(mssql::CountersEnabled()) {
	// Convert timeout from seconds to milliseconds
	// 0 = no timeout (use INT_MAX for effectively infinite wait)
	// Otherwise, multiply by 1000 to convert to milliseconds
	if (query_timeout_seconds <= 0) {
		read_timeout_ms_ = INT_MAX;	 // Effectively infinite timeout
	} else {
		read_timeout_ms_ = query_timeout_seconds * 1000;
	}
	if (counters_enabled_) {
		// D10: latched here, before any COLMETADATA, because the stager counts
		// per-column sizing decisions as it resolves them.
		stager_.EnableCounters();
	}
}

MSSQLResultStream::~MSSQLResultStream() {
	// D4 (spec 054): close summary. fprintf only — must stay safe on a worker
	// thread (see the connection-release comment below).
	if (counters_enabled_) {
		PrintDebugCounters();
	}

	// If we're still in streaming state, try to cancel
	if (state_ == MSSQLResultStreamState::Streaming) {
		Cancel();
	}

	// Return connection to the pool.
	//
	// Issue #178 review: this destructor can run on a WORKER thread during query
	// teardown while the client thread commits the query's transaction — the old
	// `Catalog::GetCatalog(*client_context_, ...)` path called
	// MetaTransaction::Get on that context and raced
	// TransactionContext::Commit's unique_ptr move (TSan-confirmed). The pool
	// weak_ptr and pinned-ness were captured at construction instead; no
	// ClientContext is touched here, and a failed lock() (catalog torn down)
	// degrades to dropping the connection — never a dangling dereference
	// (PR #179 review).
	if (connection_) {
		auto conn_state = connection_->GetState();
		if (batch_sent_ && conn_state != tds::ConnectionState::Idle &&
			conn_state != tds::ConnectionState::Disconnected) {
			// Connection is in unexpected state - close it to prevent pool corruption.
			// Only when this stream is the one that put it there: an Initialize
			// refused because the pinned connection was busy must leave the
			// connection to the stream that is using it (spec 075).
			connection_->Close();
		}

		if (transaction_pinned_) {
			// The MSSQLTransaction owns the pin; just drop our reference.
			connection_.reset();
		} else if (auto pool = pool_handle_.lock()) {
			try {
				// Flag session reset (temp tables, SET options) for the next
				// reuse — same as ConnectionProvider's autocommit release
				// (the reset is a header bit on the next SQL_BATCH, not an
				// extra round trip).
				//
				// `mssql_reset_connection = false` skips it (issue #189), and
				// THIS is the path that decides whether a `##temp` table survives:
				// every mssql_scan and every table scan returns its connection
				// here, so leaving it unconditional made the setting look
				// implemented while changing nothing a user could observe.
				connection_->SetNeedsReset(reset_on_release_);
				pool->Release(std::move(connection_));
			} catch (...) {
				// Release failed — drop the connection.
				// shared_ptr destructor closes the socket.
				connection_.reset();
			}
		} else {
			connection_.reset();
		}
	}
}

bool MSSQLResultStream::Initialize() {
	if (state_ != MSSQLResultStreamState::Initializing) {
		throw InvalidInputException("MSSQLResultStream already initialized");
	}

	// Send the SQL batch
	const bool sent = request_.IsRpc() ? connection_->ExecuteRequest(request_, "scan result stream")
									   : connection_->ExecuteBatch(sql_, "scan result stream");
	if (!sent) {
		throw IOException("Failed to execute SQL batch: " + connection_->GetLastError());
	}
	batch_sent_ = true;

	// Read and parse until we get COLMETADATA
	while (state_ == MSSQLResultStreamState::Initializing) {
		if (!ReadMoreData(read_timeout_ms_)) {
			if (IsTimeoutError()) {
				throw IOException(GetTimeoutErrorMessage());
			}
			throw IOException("Connection closed while waiting for COLMETADATA");
		}

		tds::ParsedTokenType token;
		while ((token = parser_.TryParseNext()) != tds::ParsedTokenType::NeedMoreData) {
			if (ConsumeInitialToken(token)) {
				return true;
			}
		}
	}

	return state_ == MSSQLResultStreamState::Streaming || state_ == MSSQLResultStreamState::Complete;
}

bool MSSQLResultStream::ConsumeInitialToken(tds::ParsedTokenType token) {
	switch (token) {
	case tds::ParsedTokenType::ColMetadata:
		// Got column metadata - transition to streaming
		AdoptColumnMetadata();
		state_ = MSSQLResultStreamState::Streaming;
		return true;

	case tds::ParsedTokenType::Error:
		NoteServerError();
		return false;

	case tds::ParsedTokenType::Info:
		info_messages_.push_back(parser_.GetInfo());
		return false;

	case tds::ParsedTokenType::Done: {
		auto done = parser_.GetDone();
		// Check for errors accumulated from previous ERROR tokens
		if (!errors_.empty()) {
			state_ = MSSQLResultStreamState::Error;
			ThrowServerError(errors_);
		}
		// If more results follow, continue looking for COLMETADATA
		if (!done.IsFinal()) {
			return false;
		}
		// Final DONE with no columns — empty result set
		state_ = MSSQLResultStreamState::Complete;
		return true;
	}

	case tds::ParsedTokenType::None:
		if (parser_.GetState() == tds::ParserState::Error) {
			throw IOException("TDS parse error: " + parser_.GetParseError());
		}
		return false;

	default:
		// Skip other tokens
		return false;
	}
}

void MSSQLResultStream::AdoptColumnMetadata() {
	const auto &parsed_columns = parser_.GetColumnMetadata();
	column_metadata_.clear();
	column_metadata_.reserve(parsed_columns.size());
	for (const auto &col : parsed_columns) {
		column_metadata_.push_back(col);
	}
	if (client_transcode_) {
		mssql::codec::code_page::ResolveWireCodePages(column_metadata_);
	}
	// Spec 093 T5: what a geometry / geography column is read as, the catalog's, before its
	// type is asked below.
	for (auto &col : column_metadata_) {
		col.geometry_as = geometry_as_;
	}
	column_names_.clear();
	column_types_.clear();

	for (const auto &col : column_metadata_) {
		column_names_.push_back(col.name);
		column_types_.push_back(tds::encoding::TypeConverter::GetDuckDBType(col));
	}

	// D4 (spec 054): precompute per-column decode family + PLP-ness
	// for the debug counters (FamilyFromTdsType is the same dispatch
	// TypeConverter::ConvertValue uses per value).
	if (counters_enabled_) {
		counter_col_family_.clear();
		counter_col_plp_.clear();
		for (const auto &col : column_metadata_) {
			uint8_t family = 0xFF;
			try {
				family = static_cast<uint8_t>(mssql::codec::FamilyFromTdsType(col.type_id));
			} catch (...) {
				// Unknown wire type — counted as unknown_family_values.
			}
			counter_col_family_.push_back(family);
			counter_col_plp_.push_back(col.max_length == 0xFFFF);
		}
	}
}

void MSSQLResultStream::NoteServerError() {
	auto error = parser_.GetError();
	errors_.push_back(error);
	// Fatal errors (severity >= 20) throw immediately
	if (error.IsFatal()) {
		state_ = MSSQLResultStreamState::Error;
		throw IOException("SQL Server fatal error [%d, severity %d]: %s", error.number, error.severity, error.message);
	}
}

namespace {

// Opt-in phase timer. steady_clock::now() is ~15 ns per call here, and the fill
// loop below runs two of these per row, so an ungated version costs about as
// much as the work it measures. When disabled it is a predictable branch and
// no clock read at all.
struct PhaseTimer {
	const bool enabled;
	std::chrono::steady_clock::time_point start;

	explicit PhaseTimer(bool enabled_p) : enabled(enabled_p) {
		if (enabled) {
			start = std::chrono::steady_clock::now();
		}
	}
	// Accumulate in nanoseconds: these intervals are per row (~100 ns), so
	// truncating each to whole microseconds would report zero for all of them.
	void Stop(uint64_t &accum_ns) const {
		if (!enabled) {
			return;
		}
		accum_ns += static_cast<uint64_t>(
			std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
	}
};

}  // namespace

idx_t MSSQLResultStream::FillChunk(DataChunk &chunk) {
	PhaseTimer chunk_timer(timing_enabled_);

	if (!ReadyToFill()) {
		return 0;
	}

	// Reset chunk for new data - DuckDB already initialized it with correct types
	chunk.Reset();

	const bool staged = BeginStagedChunk(chunk);

	FillPhaseTimes times;
	// D4: snapshot the thread-local fallback counter; the delta at exit is
	// this call's fallbacks (FillChunk runs entirely on one thread).
	const uint64_t utf16_fallbacks_at_entry = counters_enabled_ ? tds::encoding::Utf16FallbackCount() : 0;

	const idx_t row_count = ReadRows(chunk, staged, times);

	if (staged) {
		FinishStagedChunk(chunk, row_count);
	}
	// 2.0 vectors carry their own size, and every consumer keyed on
	// Vector::size() — IsNull, filters, the hash join — sees an empty vector
	// unless the producer sets it. The deprecated SetCardinality sets only the
	// chunk's logical count. Target vectors (rowid STRUCT children) are not in
	// chunk.data, so they are sized explicitly.
	for (auto *tv : target_vectors_) {
		if (tv) {
			FlatVector::SetSize(*tv, row_count);
		}
	}
	chunk.SetChildCardinality(row_count);

	// Log timing summary
	uint64_t total_ns = 0;
	chunk_timer.Stop(total_ns);
	MSSQL_DEBUG_LOG(1, "FillChunk: %llu rows, total=%lluns, parse=%lluns, read=%lluns, process=%lluns",
					(unsigned long long)row_count, (unsigned long long)total_ns, (unsigned long long)times.parse_ns,
					(unsigned long long)times.read_ns, (unsigned long long)times.process_ns);

	if (counters_enabled_) {
		if (row_count > 0) {
			counters_.chunks++;
		}
		counters_.fill_total_ns += total_ns;
		counters_.fill_parse_ns += times.parse_ns;
		counters_.fill_read_ns += times.read_ns;
		counters_.fill_process_ns += times.process_ns;
		counters_.utf16_fallbacks += tds::encoding::Utf16FallbackCount() - utf16_fallbacks_at_entry;
	}

	return row_count;
}

bool MSSQLResultStream::ReadyToFill() {
	if (state_ == MSSQLResultStreamState::Complete || state_ == MSSQLResultStreamState::Error) {
		return false;
	}

	if (state_ != MSSQLResultStreamState::Streaming) {
		throw InvalidInputException("MSSQLResultStream not in streaming state");
	}

	// Check for cancellation
	if (is_cancelled_.load(std::memory_order_acquire)) {
		DrainAfterCancel();
		return false;
	}
	return true;
}

bool MSSQLResultStream::BeginStagedChunk(DataChunk &chunk) {
	// Staged read path (spec 055 T5). Not used with target_vectors_: those are
	// STRUCT children owned by the caller, and publishing a whole column's
	// validity mask at once would overwrite what the caller already wrote there.
	const bool staged = target_vectors_.empty() && !column_metadata_.empty();
	if (staged) {
		ResolveStagedTargets(chunk);
		if (!stager_.IsConfigured()) {
			stager_.Configure(column_metadata_, staged_targets_);
			parser_.SetRawRowMode(true);
		}
		stager_.BeginChunk(staged_targets_);
	}
	return staged;
}

idx_t MSSQLResultStream::ReadRows(DataChunk &chunk, bool staged, FillPhaseTimes &times) {
	const idx_t max_rows = STANDARD_VECTOR_SIZE;  // 2048
	idx_t row_count = 0;

	while (row_count < max_rows && state_ == MSSQLResultStreamState::Streaming) {
		// Check for cancellation periodically
		if (is_cancelled_.load(std::memory_order_acquire)) {
			break;
		}

		PhaseTimer parse_timer(timing_enabled_);
		tds::ParsedTokenType token = parser_.TryParseNext();
		parse_timer.Stop(times.parse_ns);

		// A row is handled here, in the loop: this is the path every row of every scan takes.
		if (token != tds::ParsedTokenType::Row) {
			if (ConsumeStreamToken(token, row_count, times.read_ns)) {
				break;
			}
			continue;
		}

		PhaseTimer process_timer(timing_enabled_);
		if (staged) {
			const uint8_t *row = parser_.GetRawRow();
			const size_t row_length = parser_.GetRawRowLength();
			if (counters_enabled_) {
				counters_.wire_bytes_in += row_length;
			}
			if (parser_.IsRawRowNBC()) {
				stager_.StageNBCRow(row, row_length, row_count);
			} else {
				stager_.StageRow(row, row_length, row_count);
			}
			row_count++;
			// A MAX-typed column can stage megabytes per row, so a chunk is
			// closed on staged BYTES as well as on rows. DataChunk is allowed
			// to be short; without this, 2048 rows of large values would be
			// buffered in full before DuckDB saw any of them.
			// The loop is left before this row is timed and counted in rows_read_, as it always was.
			if (stager_.StagedBytesExceed(mssql::codec::staging::STAGING_CHUNK_PAYLOAD_BUDGET_BYTES)) {
				break;
			}
		} else {
			ProcessRow(chunk, row_count++);
		}
		process_timer.Stop(times.process_ns);
		rows_read_++;
	}
	return row_count;
}

bool MSSQLResultStream::ConsumeStreamToken(tds::ParsedTokenType token, idx_t row_count, uint64_t &read_time_ns) {
	switch (token) {
	case tds::ParsedTokenType::Done:
		ConsumeDone();
		return false;

	case tds::ParsedTokenType::Error:
		NoteServerError();
		return false;

	case tds::ParsedTokenType::Info:
		info_messages_.push_back(parser_.GetInfo());
		return false;

	case tds::ParsedTokenType::NeedMoreData:
		return ReadForMoreRows(row_count, read_time_ns);

	case tds::ParsedTokenType::None:
		if (parser_.GetState() == tds::ParserState::Error) {
			FailOnParseError();
		}
		if (parser_.GetState() == tds::ParserState::Complete) {
			FinishStream();
		}
		return false;

	case tds::ParsedTokenType::ColMetadata:
		RefuseSecondResultSet();

	default:
		// Skip other tokens
		return false;
	}
}

void MSSQLResultStream::ConsumeDone() {
	auto done = parser_.GetDone();
	if (done.HasError()) {
		// Error indicated in DONE token
		state_ = MSSQLResultStreamState::Error;
		if (!errors_.empty()) {
			ThrowServerError(errors_);
		}
		throw InvalidInputException("SQL Server returned error status");
	}
	if (done.IsFinal()) {
		FinishStream();
	}
}

bool MSSQLResultStream::ReadForMoreRows(idx_t row_count, uint64_t &read_time_ns) {
	// Check if parser is in terminal state (Complete or Error)
	if (parser_.GetState() == tds::ParserState::Complete) {
		FinishStream();
		return true;
	}
	if (parser_.GetState() == tds::ParserState::Error) {
		FailOnParseError();
	}
	PhaseTimer read_timer(timing_enabled_);
	bool read_ok = ReadMoreData(read_timeout_ms_);
	read_timer.Stop(read_time_ns);
	if (!read_ok) {
		if (row_count > 0) {
			// Return what we have
			return true;
		}
		if (IsTimeoutError()) {
			throw IOException(GetTimeoutErrorMessage());
		}
		throw IOException("Connection closed unexpectedly");
	}
	return false;
}

void MSSQLResultStream::FinishStream() {
	state_ = MSSQLResultStreamState::Complete;
	// Transition connection back to Idle
	connection_->TransitionState(tds::ConnectionState::Executing, tds::ConnectionState::Idle);
}

void MSSQLResultStream::FailOnParseError() {
	state_ = MSSQLResultStreamState::Error;
	throw IOException("TDS parse error: " + parser_.GetParseError());
}

void MSSQLResultStream::RefuseSecondResultSet() {
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

void MSSQLResultStream::FinishStagedChunk(DataChunk &chunk, idx_t row_count) {
	stager_.FinalizeChunk(row_count);
	for (idx_t c = 0; c < column_metadata_.size(); c++) {
		NoteStagedColumn(c);
	}
	if (counters_enabled_) {
		CountChunkForDebug(chunk, row_count);
	}
}

void MSSQLResultStream::NoteStagedColumn(idx_t c) {
	if (stager_.ChunkOutOfRange(c) > 0) {
		NoteOutOfRange(c);
	}
	if (stager_.KernelOf(c) == mssql::codec::staging::FinalizeKernel::Vector && column_metadata_[c].IsVectorText()) {
		NoteVectorText(c);
	}
	// Spec 093 T1: a sql_variant value read as its bytes is said as soon as the scan meets
	// one, like D8's out-of-range values, so a query that stops early still says it.
	if (column_metadata_[c].type_id == tds::TDS_TYPE_SQL_VARIANT &&
		(c >= variant_notes_warned_.size() || !variant_notes_warned_[c])) {
		const auto &notes = stager_.VariantNotes(c);
		if (notes.unknown_collation + notes.invalid_utf8 > 0) {
			warnings_pending_ = true;
		}
	}
	// Spec 093 T5: a spatial value read as NULL, likewise.
	if (column_metadata_[c].IsSpatialUdt() && SpatialWarningPending(c)) {
		warnings_pending_ = true;
	}
}

idx_t MSSQLResultStream::ColumnsToFill(const DataChunk &chunk) const {
	// Determine how many columns to fill:
	// - If target_vectors_ is set, use those vectors instead of chunk.data
	// - If columns_to_fill_ was explicitly set (e.g., for COUNT(*)), use that
	// - Otherwise, fill up to chunk's column count (but not more than we have data for)
	if (!target_vectors_.empty()) {
		// Use target vectors - columns_to_fill_ should match target_vectors_.size()
		return std::min(target_vectors_.size(), column_metadata_.size());
	}
	if (columns_to_fill_ != static_cast<idx_t>(-1)) {
		// Explicitly set - use this value (may be 0 for COUNT(*))
		return std::min(columns_to_fill_, static_cast<idx_t>(column_metadata_.size()));
	}
	// Default behavior - fill columns that exist in both SQL result and chunk
	return std::min(static_cast<idx_t>(column_metadata_.size()), chunk.ColumnCount());
}

void MSSQLResultStream::NoteDecodeNotes(idx_t col_idx, const Vector &target_vector,
										const mssql::codec::variant::VariantColumnNotes *variant_notes,
										const mssql::codec::spatial::SpatialColumnNotes *spatial_notes) {
	if (spatial_notes && SpatialWarningPending(col_idx)) {
		warnings_pending_ = true;
	}
	if (column_metadata_[col_idx].IsVectorText() && target_vector.GetType().id() == LogicalTypeId::ARRAY) {
		NoteVectorText(col_idx);
	}
	// Spec 093 T1: as the staged path, said as soon as the scan meets one.
	if (variant_notes && variant_notes->unknown_collation + variant_notes->invalid_utf8 > 0 &&
		(col_idx >= variant_notes_warned_.size() || !variant_notes_warned_[col_idx])) {
		warnings_pending_ = true;
	}
}

void MSSQLResultStream::ProcessRow(DataChunk &chunk, idx_t row_idx) {
	const auto &row = parser_.GetRow();

	const idx_t cols_to_fill = ColumnsToFill(chunk);

	// Debug: log column count info on first row
	if (row_idx == 0) {
		MSSQL_DEBUG_LOG(1,
						"ProcessRow: sql_columns=%zu, chunk_columns=%llu, columns_to_fill_=%llu, cols_to_fill=%llu, "
						"target_vectors=%zu",
						column_metadata_.size(), (unsigned long long)chunk.ColumnCount(),
						(unsigned long long)columns_to_fill_, (unsigned long long)cols_to_fill, target_vectors_.size());
	}

	for (idx_t col_idx = 0; col_idx < cols_to_fill; col_idx++) {
		// Get the target vector: either from target_vectors_ or from chunk.data
		Vector *target_vector;
		if (!target_vectors_.empty()) {
			// Write to target vectors (e.g., STRUCT children)
			target_vector = target_vectors_[col_idx];
		} else if (!output_column_mapping_.empty()) {
			// Map SQL column index to output chunk column index
			target_vector = &chunk.data[output_column_mapping_[col_idx]];
		} else {
			// Default: SQL column i goes to output i
			target_vector = &chunk.data[col_idx];
		}

		mssql::codec::variant::VariantColumnNotes *variant_notes = nullptr;
		if (column_metadata_[col_idx].type_id == tds::TDS_TYPE_SQL_VARIANT) {
			row_variant_notes_.resize(column_metadata_.size());
			variant_notes = &row_variant_notes_[col_idx];
		}
		mssql::codec::spatial::SpatialColumnNotes *spatial_notes = nullptr;
		if (column_metadata_[col_idx].IsSpatialUdt()) {
			row_spatial_notes_.resize(column_metadata_.size());
			spatial_notes = &row_spatial_notes_[col_idx];
		}
		tds::encoding::TypeConverter::ConvertValue(row.values[col_idx], row.null_mask[col_idx],
												   column_metadata_[col_idx], *target_vector, row_idx, variant_notes,
												   spatial_notes);
		// Only a spatial, a sql_variant or a vector-as-text column has anything to note; for any other
		// the three tests are all there is, as they were.
		if (spatial_notes || variant_notes || column_metadata_[col_idx].IsVectorText()) {
			NoteDecodeNotes(col_idx, *target_vector, variant_notes, spatial_notes);
		}
		// A datetime2 value read as NULL for not fitting TIMESTAMP_NS (issue #168): spec 093 D8's warning.
		const auto &meta = column_metadata_[col_idx];
		if (!row.null_mask[col_idx] && meta.type_id == tds::TDS_TYPE_DATETIME2 &&
			target_vector->GetType().id() == LogicalTypeId::TIMESTAMP_NS &&
			FlatVector::IsNull(*target_vector, row_idx)) {
			NoteOutOfRange(col_idx);
		}
	}

	if (counters_enabled_) {
		CountRowForDebug(chunk, row_idx, cols_to_fill);
	}
}

void MSSQLResultStream::ResolveStagedTargets(DataChunk &chunk) {
	const idx_t column_count = column_metadata_.size();
	// Same rules ProcessRow applies, hoisted out of the row loop. target_vectors_
	// is excluded by the caller, so only two of the three cases can arise here.
	idx_t cols_to_fill;
	if (columns_to_fill_ != static_cast<idx_t>(-1)) {
		cols_to_fill = std::min(columns_to_fill_, column_count);
	} else {
		cols_to_fill = std::min(column_count, chunk.ColumnCount());
	}

	staged_targets_.assign(column_count, nullptr);
	for (idx_t col_idx = 0; col_idx < cols_to_fill; col_idx++) {
		staged_targets_[col_idx] =
			output_column_mapping_.empty() ? &chunk.data[col_idx] : &chunk.data[output_column_mapping_[col_idx]];
	}
}

void MSSQLResultStream::CountChunkForDebug(DataChunk &chunk, idx_t row_count) {
	(void)chunk;
	const auto string_family = static_cast<uint8_t>(mssql::codec::TypeFamily::String);
	const idx_t counted = std::min(counter_col_family_.size(), staged_targets_.size());
	for (idx_t col_idx = 0; col_idx < counted; col_idx++) {
		Vector *target = staged_targets_[col_idx];
		if (target == nullptr) {
			continue;
		}
		const idx_t nulls = stager_.ChunkNulls(col_idx);
		const idx_t values = row_count - nulls;
		counters_.nulls += nulls;

		const uint8_t family = counter_col_family_[col_idx];
		if (family < mssql::codec::TYPE_FAMILY_COUNT) {
			counters_.values_per_family[family] += values;
		} else {
			counters_.unknown_family_values += values;
		}
		if (counter_col_plp_[col_idx]) {
			counters_.plp_values += values;
		}
		if (family == string_family && target->GetType().InternalType() == PhysicalType::VARCHAR) {
			// UnifiedVectorFormat, NOT FlatVector: the stager publishes a uniform
			// column-chunk as a CONSTANT vector (spec 056, #221), and
			// FlatVector::GetData / FlatVector::Validity assert a flat one. This
			// counter therefore KILLED the query it was measuring — every scan of a
			// column whose 2048-row chunk held one repeated value died with
			// `INTERNAL Error: Operation requires a flat vector`, at MSSQL_DEBUG=2,
			// the level the docs name for debugging. The partial `rows=2048
			// chunks=0` dump that issue #233 reports is this crash seen from the
			// destructor during unwinding, not a sampling bug.
			//
			// The count stays LOGICAL — one entry per row, so `str_out / rows`
			// remains an average value size and stays comparable with wire_in. For
			// a CONSTANT vector that is not the number of bytes converted (one
			// value was), which is a representation question and belongs to a
			// representation counter, not to this one.
			UnifiedVectorFormat format;
			target->ToUnifiedFormat(format);
			const string_t *strings = UnifiedVectorFormat::GetData<string_t>(format);
			for (idx_t row = 0; row < row_count; row++) {
				const idx_t idx = format.sel->get_index(row);
				if (format.validity.RowIsValid(idx)) {
					counters_.string_bytes_out += strings[idx].GetSize();
				}
			}
		}
	}
}

void MSSQLResultStream::CountRowForDebug(DataChunk &chunk, idx_t row_idx, idx_t cols_to_fill) {
	const auto &row = parser_.GetRow();
	const auto string_family = static_cast<uint8_t>(mssql::codec::TypeFamily::String);
	for (idx_t col_idx = 0; col_idx < cols_to_fill && col_idx < counter_col_family_.size(); col_idx++) {
		counters_.wire_bytes_in += row.values[col_idx].size();
		if (row.null_mask[col_idx]) {
			counters_.nulls++;
			continue;
		}
		const uint8_t family = counter_col_family_[col_idx];
		if (family < 9) {
			counters_.values_per_family[family]++;
		} else {
			counters_.unknown_family_values++;
		}
		if (counter_col_plp_[col_idx]) {
			counters_.plp_values++;
		}
		if (family == string_family) {
			// Same 3-way target resolution as the conversion loop above.
			Vector *target_vector;
			if (!target_vectors_.empty()) {
				target_vector = target_vectors_[col_idx];
			} else if (!output_column_mapping_.empty()) {
				target_vector = &chunk.data[output_column_mapping_[col_idx]];
			} else {
				target_vector = &chunk.data[col_idx];
			}
			if (target_vector->GetType().InternalType() == PhysicalType::VARCHAR) {
				counters_.string_bytes_out += FlatVector::GetData<string_t>(*target_vector)[row_idx].GetSize();
			}
		}
	}
}

void MSSQLResultStream::PrintDebugCounters() {
	if (mssql::CountersConfoundedByLogging()) {
		// Say it where the numbers are read, not only in a comment. Level 1 already
		// logs a line per chunk from inside the timed fill; level 2 adds one per
		// token from inside the timed parse. Either way the phase split below is
		// measuring stderr as well as the work.
		fprintf(stderr,
				"[MSSQL COUNTERS] NOTE: MSSQL_DEBUG is set, and its logging runs inside the phases timed "
				"below — these numbers include it. For timings use MSSQL_COUNTERS=1 with MSSQL_DEBUG unset.\n");
	}
	fprintf(stderr,
			"[MSSQL COUNTERS] stream close: rows=%llu chunks=%llu nulls=%llu wire_in=%lluB str_out=%lluB "
			"plp=%llu utf16_fallback=%llu fill=%lluus (parse=%lluus read=%lluus process=%lluus)\n",
			(unsigned long long)rows_read_, (unsigned long long)counters_.chunks, (unsigned long long)counters_.nulls,
			(unsigned long long)counters_.wire_bytes_in, (unsigned long long)counters_.string_bytes_out,
			(unsigned long long)counters_.plp_values, (unsigned long long)counters_.utf16_fallbacks,
			(unsigned long long)(counters_.fill_total_ns / 1000), (unsigned long long)(counters_.fill_parse_ns / 1000),
			(unsigned long long)(counters_.fill_read_ns / 1000),
			(unsigned long long)(counters_.fill_process_ns / 1000));

	// Per-row breakdown — the form spec 055 D0 actually needs. `unaccounted` is
	// total minus the three measured phases: chunk setup/teardown, the loop
	// itself, and the timing calls that remain when timing is on.
	if (rows_read_ > 0) {
		const double rows = static_cast<double>(rows_read_);
		const uint64_t measured_ns = counters_.fill_parse_ns + counters_.fill_read_ns + counters_.fill_process_ns;
		const double unaccounted_ns =
			counters_.fill_total_ns > measured_ns ? static_cast<double>(counters_.fill_total_ns - measured_ns) : 0.0;
		fprintf(stderr, "[MSSQL COUNTERS]   ns/row: total=%.1f parse=%.1f read=%.1f process=%.1f unaccounted=%.1f\n",
				counters_.fill_total_ns / rows, counters_.fill_parse_ns / rows, counters_.fill_read_ns / rows,
				counters_.fill_process_ns / rows, unaccounted_ns / rows);
	}
	std::string families;
	for (uint8_t f = 0; f < mssql::codec::TYPE_FAMILY_COUNT; f++) {
		if (counters_.values_per_family[f] > 0) {
			families += " ";
			families += mssql::codec::FamilyName(static_cast<mssql::codec::TypeFamily>(f));
			families += "=" + std::to_string(counters_.values_per_family[f]);
		}
	}
	if (counters_.unknown_family_values > 0) {
		families += " unknown=" + std::to_string(counters_.unknown_family_values);
	}
	if (!families.empty()) {
		fprintf(stderr, "[MSSQL COUNTERS]   values/family:%s\n", families.c_str());
	}
	PrintStagingCounters();
}

//! D10 (spec 055): what the staged path itself did. Printed only for a stream
//! that took it — the legacy per-value path leaves every one of these at zero,
//! and three empty lines are worse than none.
void MSSQLResultStream::PrintStagingCounters() {
	const auto &sc = stager_.Counters();
	const auto &arena = stager_.Arena();

	std::string kernels;
	for (uint8_t k = 0; k < mssql::codec::staging::FINALIZE_KERNEL_COUNT; k++) {
		if (sc.kernel_values[k] == 0) {
			continue;
		}
		// ns/value is the number to read: it is directly comparable with the
		// micro-benchmark's per-family cells.
		char buf[192];
		snprintf(buf, sizeof(buf), " %s=%lluv/%lluB/%.1fns",
				 mssql::codec::staging::FinalizeKernelName(static_cast<mssql::codec::staging::FinalizeKernel>(k)),
				 (unsigned long long)sc.kernel_values[k], (unsigned long long)sc.staged_bytes[k],
				 static_cast<double>(sc.kernel_ns[k]) / static_cast<double>(sc.kernel_values[k]));
		kernels += buf;
	}
	if (kernels.empty() && sc.direct_values == 0 && sc.nbc_rows == 0) {
		return;
	}
	fprintf(stderr,
			"[MSSQL COUNTERS]   staging: direct_bypass=%llu nbc_rows=%llu grow=%llu shrink=%llu peak_payload=%lluB\n",
			(unsigned long long)sc.direct_values, (unsigned long long)sc.nbc_rows,
			(unsigned long long)arena.GrowEvents(), (unsigned long long)arena.ShrinkEvents(),
			(unsigned long long)arena.PeakPayloadBytes());
	if (sc.constant_columns > 0 || sc.constant_null_columns > 0) {
		fprintf(stderr, "[MSSQL COUNTERS]   constant column-chunks: uniform=%llu all_null=%llu\n",
				(unsigned long long)sc.constant_columns, (unsigned long long)sc.constant_null_columns);
	}
	fprintf(stderr, "[MSSQL COUNTERS]   columns: prealloc_bounded=%llu prealloc_capped=%llu unbounded=%llu\n",
			(unsigned long long)sc.prealloc_bounded_columns, (unsigned long long)sc.prealloc_capped_columns,
			(unsigned long long)sc.unbounded_columns);
	if (!kernels.empty()) {
		fprintf(stderr, "[MSSQL COUNTERS]   kernels (values/staged bytes/ns per value):%s\n", kernels.c_str());
	}

	std::string boundaries;
	for (uint8_t b = 0; b < mssql::codec::staging::BOUNDARY_STRATEGY_COUNT; b++) {
		if (sc.boundary[b] == 0) {
			continue;
		}
		boundaries += " ";
		boundaries +=
			mssql::codec::staging::BoundaryStrategyName(static_cast<mssql::codec::staging::BoundaryStrategy>(b));
		boundaries += "=" + std::to_string(sc.boundary[b]);
	}
	if (!boundaries.empty() || sc.replaced_units > 0) {
		fprintf(stderr, "[MSSQL COUNTERS]   string boundaries (column-chunks):%s replaced_units=%llu\n",
				boundaries.c_str(), (unsigned long long)sc.replaced_units);
	}
}

bool MSSQLResultStream::ReadMoreData(int timeout_ms) {
	// Read TDS packet from connection (packet includes 8-byte header)
	// We use the socket's ReceivePacket method to properly parse the header
	auto *socket = connection_->GetSocket();
	if (!socket) {
		last_socket_error_ = "Socket is null";
		return false;
	}

	// Feed from a view into the socket's assembly buffer. The payload used to be
	// copied into a TdsPacket first and copied into the parser second — two
	// passes over every byte to move it eight bytes to the left.
	const uint8_t *payload = nullptr;
	size_t payload_length = 0;
	if (!socket->ReceivePayloadView(payload, payload_length, timeout_ms)) {
		last_socket_error_ = socket->GetLastError();
		return false;
	}
	if (payload_length > 0) {
		parser_.Feed(payload, payload_length);
	}

	return true;
}

void MSSQLResultStream::Cancel() {
	if (is_cancelled_.load(std::memory_order_acquire)) {
		MSSQL_DEBUG_LOG(1, "Cancel: already cancelled, skipping");
		return;	 // Already cancelled
	}

	is_cancelled_.store(true, std::memory_order_release);

	// Send ATTENTION signal if we're in streaming state
	if (state_ == MSSQLResultStreamState::Streaming) {
		MSSQL_DEBUG_LOG(1, "Cancel: sending ATTENTION (state=Streaming, rows_read=%llu)",
						(unsigned long long)rows_read_);
		state_ = MSSQLResultStreamState::Draining;

		if (connection_->SendAttention()) {
			MSSQL_DEBUG_LOG(1, "Cancel: ATTENTION sent, starting drain");
			// Wait for attention acknowledgment
			DrainAfterCancel();
		} else {
			MSSQL_DEBUG_LOG(1, "Cancel: SendAttention FAILED");
		}
	} else {
		MSSQL_DEBUG_LOG(1, "Cancel: not in Streaming state (state=%d)", (int)state_);
	}
}

void MSSQLResultStream::DrainAfterCancel() {
	// After sending ATTENTION:
	// 1. Read data from socket as fast as possible (data may already be buffered)
	// 2. Cancel timeout runs in parallel
	// 3. Race: DONE+ATTN arrives first → reuse connection
	//          Timeout fires first → close connection
	//
	// Use very short per-read timeout (10ms) to quickly consume any buffered data
	// without blocking. Overall timeout determines when we give up.
	auto start = std::chrono::steady_clock::now();
	auto timeout = std::chrono::milliseconds(cancel_timeout_ms_);

	// NOTE: Don't reset parser! We need column metadata to skip ROW tokens.
	// SQL Server may have data in TCP buffer that arrives after ATTENTION.
	// Enable skip mode - ROW tokens skipped without parsing values (much faster)
	parser_.SetSkipMode(true);

	int read_count = 0;
	int token_count = 0;

	while (state_ == MSSQLResultStreamState::Draining) {
		auto elapsed = std::chrono::steady_clock::now() - start;
		if (elapsed > timeout) {
			// Overall timeout - close connection, don't try to reuse
			MSSQL_DEBUG_LOG(1, "DrainAfterCancel: TIMEOUT after %ldms (reads=%d, tokens=%d), closing connection",
							(long)std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), read_count,
							token_count);
			connection_->Close();
			state_ = MSSQLResultStreamState::Error;
			return;
		}

		// Try to read with short timeout (poll for data)
		if (!ReadMoreData(cancel_read_timeout_ms_)) {
			// A dead connection will not answer: stop now rather than read it
			// again at once until the overall timeout (issue #456).
			auto *socket = connection_->GetSocket();
			if (!socket || !socket->IsConnected()) {
				MSSQL_DEBUG_LOG(1, "DrainAfterCancel: connection lost (reads=%d, tokens=%d), closing", read_count,
								token_count);
				connection_->Close();
				state_ = MSSQLResultStreamState::Error;
				return;
			}
			// No data available, keep trying until overall timeout
			continue;
		}
		read_count++;

		tds::ParsedTokenType token;
		while ((token = parser_.TryParseNext()) != tds::ParsedTokenType::NeedMoreData) {
			token_count++;

			if (token == tds::ParsedTokenType::Done) {
				auto done = parser_.GetDone();
				MSSQL_DEBUG_LOG(1, "DrainAfterCancel: DONE token - status=0x%04x, IsFinal=%d, IsAttentionAck=%d",
								done.status, done.IsFinal(), done.IsAttentionAck());
				if (done.IsAttentionAck()) {
					// Got ATTENTION acknowledgment - connection is clean
					MSSQL_DEBUG_LOG(1, "DrainAfterCancel: SUCCESS - got ATTN ack in %ldms, connection reusable",
									(long)std::chrono::duration_cast<std::chrono::milliseconds>(
										std::chrono::steady_clock::now() - start)
										.count());
					state_ = MSSQLResultStreamState::Complete;
					connection_->TransitionState(tds::ConnectionState::Cancelling, tds::ConnectionState::Idle);
					return;
				}
				// Got DONE but not ATTN - parser may have set state to Complete
				// Reset to WaitingForToken to keep parsing until ATTN
				parser_.ResetState();
			}
			// Discard other tokens while draining
		}
	}
}

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

mssql::codec::spatial::SpatialColumnNotes MSSQLResultStream::SpatialNotesOf(idx_t c) const {
	mssql::codec::spatial::SpatialColumnNotes notes;
	if (stager_.IsConfigured()) {
		notes = stager_.SpatialNotes(c);
	}
	if (c < row_spatial_notes_.size()) {
		const auto &row = row_spatial_notes_[c];
		notes.not_geometry += row.not_geometry;
		if (notes.malformed == 0) {
			notes.first_malformed = row.first_malformed;
		}
		notes.malformed += row.malformed;
	}
	return notes;
}

bool MSSQLResultStream::SpatialWarningPending(idx_t c) const {
	const uint8_t warned = c < spatial_warned_.size() ? spatial_warned_[c] : 0;
	const auto notes = SpatialNotesOf(c);
	return (notes.not_geometry > 0 && !(warned & mssql::SPATIAL_WARNED_NOT_GEOMETRY)) ||
		   (notes.malformed > 0 && !(warned & mssql::SPATIAL_WARNED_MALFORMED));
}

void MSSQLResultStream::SurfaceSpatialWarnings(ClientContext &context) {
	spatial_warned_.resize(column_metadata_.size(), 0);
	for (idx_t c = 0; c < column_metadata_.size(); c++) {
		if (column_metadata_[c].IsSpatialUdt()) {
			spatial_warned_[c] =
				mssql::LogSpatialNotes(context, column_metadata_[c], SpatialNotesOf(c), spatial_warned_[c]);
		}
	}
}

namespace mssql {

uint8_t LogSpatialNotes(ClientContext &context, const tds::ColumnMetadata &column,
						const codec::spatial::SpatialColumnNotes &notes, uint8_t warned) {
	// No count in either: on a streaming scan the WARNING is logged as soon as the scan meets the first
	// such value, so a count would be the count of the chunks read so far, not the column's.
	// The ATTACH option first: it is read at ATTACH. A column_types override to GEOMETRY decodes whatever
	// the option says, so it has to go too.
	if (notes.not_geometry > 0 && !(warned & SPATIAL_WARNED_NOT_GEOMETRY)) {
		warned |= SPATIAL_WARNED_NOT_GEOMETRY;
		DUCKDB_LOG_WARNING(context,
						   "mssql: column \"%s\" (%s) holds values a GEOMETRY cannot hold (a CircularString, "
						   "CompoundCurve, CurvePolygon or FullGlobe, or collections nested deeper than 15), "
						   "read as NULL; attach with geometry_as 'wkb' (ISO WKB, the curves included) or 'native' "
						   "(the stored bytes), and drop any column_types override to GEOMETRY (it still decodes "
						   "into one), to read them",
						   column.name.c_str(), column.udt_type_name.c_str());
	}
	// A value that is not MS-SSCLRT is NULL, never an error: one value does not fail the scan of its
	// table (owner, 2026-11-20).
	if (notes.malformed > 0 && !(warned & SPATIAL_WARNED_MALFORMED)) {
		warned |= SPATIAL_WARNED_MALFORMED;
		DUCKDB_LOG_WARNING(context,
						   "mssql: column \"%s\" (%s) holds values whose stored bytes cannot be read as %s (the "
						   "first: %s), read as NULL; attach with geometry_as 'native', and drop any column_types "
						   "override to GEOMETRY (it still decodes into one), to read the stored bytes",
						   column.name.c_str(), column.udt_type_name.c_str(), column.udt_type_name.c_str(),
						   codec::ssclrt::DescribeStatus(notes.first_malformed));
	}
	return warned;
}

bool LogVariantNotes(ClientContext &context, const tds::ColumnMetadata &column,
					 const codec::variant::VariantColumnNotes &notes) {
	if (notes.unknown_collation + notes.invalid_utf8 == 0) {
		return false;
	}
	// No count: logged at the first chunk (or batch) that meets one, a count would be that chunk's alone.
	const char *why = notes.unknown_collation && notes.invalid_utf8
						  ? "under a collation this build has no code page for, and under a UTF-8 collation but "
							"not UTF-8"
					  : notes.unknown_collation ? "under a collation this build has no code page for"
												: "under a UTF-8 collation but not UTF-8";
	DUCKDB_LOG_WARNING(context,
					   "mssql: column \"%s\": sql_variant string values read as their bytes (a BLOB inside the "
					   "VARIANT): %s",
					   column.name.c_str(), why);
	return true;
}

}  // namespace mssql

void MSSQLResultStream::NoteOutOfRange(idx_t c) {
	if (c < refuse_out_of_range_.size() && refuse_out_of_range_[c]) {
		// Issue #559: under TIMESTAMP_NS a value outside 1677-2262 reads as NULL, and a table with no usable
		// key is matched by all its columns NULL-safely, so the row would update the one that really holds
		// NULL. Refused by name before any row is written.
		throw InvalidInputException(
			"mssql: column \"%s\" identifies the rows of %s (a table with no usable key, matched by all its "
			"columns) and holds a datetime2(7) value outside 1677-2262, which TIMESTAMP_NS cannot hold and reads "
			"as NULL, so the row would match another one (issue #559). Attach with datetime2_7_as 'timestamp' "
			"(such a table's row-by-row UPDATE / DELETE is then refused by name), narrow the statement to rows "
			"inside the range, or give the table a primary key",
			column_metadata_[c].name, refuse_table_);
	}
	if (out_of_range_.size() < column_metadata_.size()) {
		out_of_range_.resize(column_metadata_.size(), 0);
	}
	if (out_of_range_[c] == 0) {
		out_of_range_[c] = 1;
		warnings_pending_ = true;
	}
}

void MSSQLResultStream::NoteVectorText(idx_t c) {
	if (vector_text_.size() < column_metadata_.size()) {
		vector_text_.resize(column_metadata_.size(), 0);
	}
	if (vector_text_[c] == 0) {
		vector_text_[c] = 1;
		warnings_pending_ = true;
	}
}

void MSSQLResultStream::SurfaceWarnings(ClientContext &context) {
	// Called when the stream has its COLMETADATA and again when it is drained --
	// never per row, never per chunk. Both loops below walk metadata, not data,
	// and both are written to be safe under repeated calls.

	// SQL Server INFO tokens: PRINT output, RAISERROR at severity <= 10, the
	// "Changed database context" notices, and anything a stored procedure says
	// about its own progress. The stream has collected these since the token
	// parser was written and this function dropped every one of them on the
	// floor -- the comment claimed DuckDB had no warning API, which was true
	// when it was written and is not now.
	// Resume where the last call stopped: a stream is visited at least twice and
	// these accumulate as the batch runs.
	for (; info_surfaced_ < info_messages_.size(); info_surfaced_++) {
		LogTdsInfo(context, info_messages_[info_surfaced_]);
	}

	// Spec 093 D8: a datetime2 column read as TIMESTAMP_NS whose values outside 1677-2262 were read
	// as NULL (issue #168), once per column per statement, as soon as the scan meets one; T5: a
	// geometry / geography column's values read as NULL, likewise (SurfaceSpatialWarnings).
	if (warnings_pending_) {
		warnings_pending_ = false;
		SurfaceSpatialWarnings(context);
		SurfaceOutOfRangeWarnings(context);
	}

	SurfaceVectorTextWarnings(context);
	SurfaceVariantWarnings(context);
	SurfaceCollationWarnings(context);
}

void MSSQLResultStream::SurfaceOutOfRangeWarnings(ClientContext &context) {
	for (idx_t c = 0; c < out_of_range_.size(); c++) {
		if (out_of_range_[c] != 1) {
			continue;
		}
		out_of_range_[c] = 2;
		// The ATTACH form first: the setting is read at ATTACH, and whoever reads this has an attached
		// catalog, on which a bare SET changes nothing (review of #560).
		DUCKDB_LOG_WARNING(context,
						   "mssql: column \"%s\" holds datetime2(7) values outside 1677-2262 (a temporal "
						   "table's 9999-12-31, say), read as NULL as TIMESTAMP_NS cannot hold them; attach with "
						   "datetime2_7_as 'timestamp' (or SET mssql_datetime2_7_as = 'timestamp' before ATTACH) "
						   "to read them as TIMESTAMP, the 100 ns digit truncated",
						   column_metadata_[c].name.c_str());
	}
}

void MSSQLResultStream::SurfaceVectorTextWarnings(ClientContext &context) {
	// Spec 093 D4: a vector column read from its JSON text on a connection that was not granted
	// VECTORSUPPORT, where a float32 column arrives that way too and its 8 significant digits can put an
	// element one ulp from the stored one (V2: about 1.5 % of values). Under the feature only a float16
	// column is text, and exact, so it is not said. Once per column per statement.
	auto connection = GetConnection();
	const bool granted = connection && connection->VectorSupportVersion() > 0;
	for (idx_t c = 0; c < vector_text_.size(); c++) {
		if (vector_text_[c] != 1) {
			continue;
		}
		vector_text_[c] = 2;
		if (granted) {
			continue;
		}
		DUCKDB_LOG_WARNING(context,
						   "mssql: column \"%s\" is a vector read from its JSON text, as a session without "
						   "VECTORSUPPORT receives it (mssql_vector_support false at ATTACH, or a server that does "
						   "not grant it): a float32 element can read one ulp from its stored value (8 significant "
						   "digits); with the feature (SQL Server 2025) a float32 vector streams its bytes",
						   column_metadata_[c].name.c_str());
	}
}

void MSSQLResultStream::SurfaceVariantWarnings(ClientContext &context) {
	// Spec 093 T1: sql_variant values the scan read as their bytes (a collation this build has no code
	// page for, UTF-8 bytes that are not UTF-8), once a column.
	if (stager_.IsConfigured() || !row_variant_notes_.empty()) {
		variant_notes_warned_.resize(column_metadata_.size(), false);
		for (idx_t c = 0; c < column_metadata_.size(); c++) {
			if (column_metadata_[c].type_id != tds::TDS_TYPE_SQL_VARIANT || variant_notes_warned_[c]) {
				continue;
			}
			mssql::codec::variant::VariantColumnNotes notes;
			if (stager_.IsConfigured()) {
				notes = stager_.VariantNotes(c);
			}
			if (c < row_variant_notes_.size()) {
				notes.unknown_collation += row_variant_notes_[c].unknown_collation;
				notes.invalid_utf8 += row_variant_notes_[c].invalid_utf8;
			}
			if (mssql::LogVariantNotes(context, column_metadata_[c], notes)) {
				variant_notes_warned_[c] = true;
			}
		}
	}
}

void MSSQLResultStream::SurfaceCollationWarnings(ClientContext &context) {
	// Issue #224, narrowed by spec 092 D9: a CHAR / VARCHAR / TEXT column under a
	// code page is transcoded on the client (ColumnMetadata::code_page), except on
	// a catalog with client_transcode off and for a collation newer than the
	// tables. There its bytes reach a DuckDB VARCHAR, which is UTF-8 by contract,
	// verbatim. Documented and deliberately NOT enforced: validating would cost the
	// scan's hot path to make a hand-written query fail loudly.
	//
	// So say it once per query instead of once per value. The check is over
	// COLMETADATA, which is a handful of columns, and it fires only when such a
	// column actually arrived.
	// One shot per stream. COLMETADATA does not change under us, and this is
	// reached from both the init-time and the drain-time call.
	if (collations_warned_) {
		return;
	}
	collations_warned_ = true;

	// The off switch (mssql_warn_non_utf8_collation). Read here rather than
	// cached because the answer is the asking session's, and read after the
	// one-shot flag is set so flipping it mid-session cannot make an already
	// visited stream warn late.
	Value warn_setting;
	if (context.TryGetCurrentSetting("mssql_warn_non_utf8_collation", warn_setting) && !warn_setting.GetValue<bool>()) {
		return;
	}

	for (const auto &col : column_metadata_) {
		if (!col.IsSingleByteTextColumn() || col.IsUtf8Collation() || col.code_page != nullptr) {
			continue;
		}
		DUCKDB_LOG_WARNING(context,
						   "mssql: column \"%s\" has a non-UTF-8 collation (LCID 0x%04X, SortId %u); its bytes are "
						   "returned verbatim and may not be valid UTF-8. %s",
						   col.name.c_str(), (unsigned)(col.collation & 0xFFFFFu), (unsigned)col.collation_sort_id,
						   VerbatimBytesFix());
	}
}

const char *MSSQLResultStream::VerbatimBytesFix() const {
	// What the caller can do depends on why it was not transcoded and on the
	// path. A raw mssql_scan() caller wrote the T-SQL and can cast in it; a
	// catalog scan's SQL is generated, and under client_transcode off such a
	// column reached here because mssql_convert_varchar_max was turned off too.
	if (client_transcode_) {
		return is_catalog_scan_ ? "The collation is newer than this build's code-page tables; set "
								  "mssql_convert_varchar_max=true so the server converts it."
								: "The collation is newer than this build's code-page tables; CAST it to NVARCHAR "
								  "in the query to convert.";
	}
	if (is_catalog_scan_) {
		return "Attach with client_transcode true, or set mssql_convert_varchar_max=true so the server converts it.";
	}
	return "Attach with client_transcode true, or CAST it to NVARCHAR in the query to convert.";
}

}  // namespace duckdb
