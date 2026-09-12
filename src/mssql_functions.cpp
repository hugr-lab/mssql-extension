#include "mssql_functions.hpp"
#include <chrono>
#include <climits>
#include <cstdlib>
#include "catalog/mssql_catalog.hpp"
#include "connection/mssql_connection_provider.hpp"
#include "connection/mssql_settings.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector_operations/unary_executor.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/function/function_set.hpp"
#include "duckdb/logging/logger.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/secret/secret_manager.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "mssql_storage.hpp"
#include "query/mssql_query_executor.hpp"
#include "query/mssql_simple_query.hpp"
#include "query/mssql_sql_params.hpp"
#include "query/tds_info_log.hpp"
#include "tds/encoding/type_converter.hpp"
#include "tds/tds_connection.hpp"
#include "tds/tds_types.hpp"

// Debug logging controlled by MSSQL_DEBUG environment variable
static int GetFunctionDebugLevel() {
	static const int level = []() {
		const char *env = std::getenv("MSSQL_DEBUG");
		return env ? std::atoi(env) : 0;
	}();
	return level;
}

#define MSSQL_FN_DEBUG_LOG(level, fmt, ...)                         \
	do {                                                            \
		if (GetFunctionDebugLevel() >= level) {                     \
			fprintf(stderr, "[MSSQL FN] " fmt "\n", ##__VA_ARGS__); \
		}                                                           \
	} while (0)

namespace duckdb {

//===----------------------------------------------------------------------===//
// mssql_scan implementation
//===----------------------------------------------------------------------===//

unique_ptr<FunctionData> MSSQLScanBindData::Copy() const {
	auto result = make_uniq<MSSQLScanBindData>();
	result->context_name = context_name;
	result->query = query;
	result->return_types = return_types;
	result->column_names = column_names;
	result->result_stream_id = result_stream_id;
	// Shared, not copied: the rows are the same rows (issue #316).
	result->materialized = materialized;
	result->execute_sql = execute_sql;
	result->prepared = prepared;
	result->prepared_session = prepared_session;
	result->executed_at_bind = executed_at_bind;
	return std::move(result);
}

bool MSSQLScanBindData::Equals(const FunctionData &other) const {
	auto &other_data = other.Cast<MSSQLScanBindData>();
	// execute_sql carries the parameter values of mssql_scan_params: two
	// calls with one text and different values must not be merged into one scan.
	return context_name == other_data.context_name && query == other_data.query &&
		   execute_sql == other_data.execute_sql && prepared == other_data.prepared;
}

MSSQLScanGlobalState::~MSSQLScanGlobalState() {
	// Connection is automatically returned to pool when shared_ptr in result_stream is released
	// This may trigger Cancel() if stream is still active
	result_stream.reset();

	// Log total scan time (from first call to destruction, including cancel/cleanup)
	if (timing_started) {
		auto end = std::chrono::steady_clock::now();
		auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - scan_start).count();
		MSSQL_FN_DEBUG_LOG(1, "MSSQLScanGlobalState::~dtor - total scan time: %ldms (including cancel)",
						   (long)total_ms);
	}
}

idx_t MSSQLScanGlobalState::MaxThreads() const {
	// Single-threaded streaming
	return 1;
}

//===----------------------------------------------------------------------===//
// Spec 075: describe at Bind, execute at InitGlobal
//===----------------------------------------------------------------------===//

MSSQLPreparedSession::~MSSQLPreparedSession() {
	if (!connection) {
		return;
	}
	// A connection that is not Idle here was cut off mid-stream: close it rather
	// than pool it. Mirrors ~MSSQLResultStream, and like it touches no
	// ClientContext -- bind data can die on a worker thread.
	auto conn_state = connection->GetState();
	if (conn_state != tds::ConnectionState::Idle && conn_state != tds::ConnectionState::Disconnected) {
		connection->Close();
	}
	if (auto pool = pool_handle.lock()) {
		try {
			connection->SetNeedsReset(reset_on_release);
			pool->Release(std::move(connection));
		} catch (...) {
			connection.reset();
		}
	}
	connection.reset();
}

// What Bind learned about a statement's first result set.
struct MSSQLDescribedShape {
	vector<LogicalType> types;
	vector<string> names;
	bool ok = false;
	string reason;	// why not, for the debug log and the prepared error
};

static int FindResultColumn(const std::vector<std::string> &names, const char *wanted) {
	for (size_t i = 0; i < names.size(); i++) {
		if (StringUtil::Lower(names[i]) == wanted) {
			return static_cast<int>(i);
		}
	}
	return -1;
}

static int QueryTimeoutMs(ClientContext &context) {
	int query_timeout_s = LoadQueryTimeout(context);
	if (query_timeout_s <= 0 || query_timeout_s > INT_MAX / 1000) {
		return 0;
	}
	return query_timeout_s * 1000;
}

static string TypeListToString(const vector<LogicalType> &types) {
	string out;
	for (const auto &t : types) {
		if (!out.empty()) {
			out += ", ";
		}
		out += t.ToString();
	}
	return out;
}

// The TDS type the server sends for a column sp_describe_first_result_set
// names -- `nvarchar(50)`, `decimal(10,2)`, `timestamp` -- with the length,
// precision and scale beside it. The described shape is then whatever
// TypeConverter::GetDuckDBType makes of that COLMETADATA, i.e. the STREAM's
// type by construction: the catalog's own mapping (spec 045) differs from it
// for datetime2 scales, for `rowversion`, and for the types the stream refuses
// (sql_variant, the UDTs), and mssql_scan has always reported the stream's.
// Unknown names return false and the statement is run at bind, as before.
static bool DescribedTypeToTdsMetadata(const string &base, int16_t max_length, uint8_t precision, uint8_t scale,
									   tds::ColumnMetadata &column) {
	column.precision = precision;
	column.scale = scale;
	// -1 is the describe's MAX; the converter does not read the length of a
	// variable type, only of the fixed-width N variants set below.
	column.max_length = max_length < 0 ? 0xFFFF : static_cast<uint16_t>(max_length);
	if (base == "tinyint") {
		column.type_id = tds::TDS_TYPE_INTN;
		column.max_length = 1;
	} else if (base == "smallint") {
		column.type_id = tds::TDS_TYPE_INTN;
		column.max_length = 2;
	} else if (base == "int") {
		column.type_id = tds::TDS_TYPE_INTN;
		column.max_length = 4;
	} else if (base == "bigint") {
		column.type_id = tds::TDS_TYPE_INTN;
		column.max_length = 8;
	} else if (base == "bit") {
		column.type_id = tds::TDS_TYPE_BITN;
	} else if (base == "real") {
		column.type_id = tds::TDS_TYPE_FLOATN;
		column.max_length = 4;
	} else if (base == "float") {
		column.type_id = tds::TDS_TYPE_FLOATN;
		column.max_length = 8;
	} else if (base == "decimal" || base == "numeric") {
		column.type_id = tds::TDS_TYPE_DECIMAL;
	} else if (base == "money") {
		column.type_id = tds::TDS_TYPE_MONEYN;
		column.max_length = 8;
	} else if (base == "smallmoney") {
		column.type_id = tds::TDS_TYPE_MONEYN;
		column.max_length = 4;
	} else if (base == "char") {
		column.type_id = tds::TDS_TYPE_BIGCHAR;
	} else if (base == "varchar") {
		column.type_id = tds::TDS_TYPE_BIGVARCHAR;
	} else if (base == "nchar") {
		column.type_id = tds::TDS_TYPE_NCHAR;
	} else if (base == "nvarchar" || base == "sysname") {
		column.type_id = tds::TDS_TYPE_NVARCHAR;
	} else if (base == "text") {
		column.type_id = tds::TDS_TYPE_TEXT;
	} else if (base == "ntext") {
		column.type_id = tds::TDS_TYPE_NTEXT;
	} else if (base == "binary" || base == "timestamp" || base == "rowversion") {
		column.type_id = tds::TDS_TYPE_BIGBINARY;
	} else if (base == "varbinary") {
		column.type_id = tds::TDS_TYPE_BIGVARBINARY;
	} else if (base == "image") {
		column.type_id = tds::TDS_TYPE_IMAGE;
	} else if (base == "uniqueidentifier") {
		column.type_id = tds::TDS_TYPE_UNIQUEIDENTIFIER;
	} else if (base == "date") {
		column.type_id = tds::TDS_TYPE_DATE;
	} else if (base == "time") {
		column.type_id = tds::TDS_TYPE_TIME;
	} else if (base == "datetime2") {
		column.type_id = tds::TDS_TYPE_DATETIME2;
	} else if (base == "datetimeoffset") {
		column.type_id = tds::TDS_TYPE_DATETIMEOFFSET;
	} else if (base == "datetime") {
		column.type_id = tds::TDS_TYPE_DATETIMEN;
		column.max_length = 8;
	} else if (base == "smalldatetime") {
		column.type_id = tds::TDS_TYPE_DATETIMEN;
		column.max_length = 4;
	} else if (base == "xml") {
		column.type_id = tds::TDS_TYPE_XML;
	} else if (base == "sql_variant") {
		column.type_id = tds::TDS_TYPE_SQL_VARIANT;
	} else if (base == "hierarchyid" || base == "geography" || base == "geometry") {
		column.type_id = tds::TDS_TYPE_UDT;
	} else {
		return false;
	}
	return true;
}

// sp_describe_first_result_set: one row per column of the FIRST result set,
// which is also the one MSSQLResultStream serves (a second COLMETADATA is an
// error there). Each row is turned into the COLMETADATA the server would send
// for it and mapped by the stream's own converter, so a type the stream
// refuses (sql_variant, a UDT) is refused here, at bind, with the same message
// it always had.
static MSSQLDescribedShape DescribeFirstResultSet(tds::TdsConnection &connection, const string &statement,
												  const string &declarations, int timeout_ms) {
	MSSQLDescribedShape shape;
	string batch = "EXEC sp_describe_first_result_set " + mssql::NVarcharLiteral(statement) + ", " +
				   (declarations.empty() ? string("NULL") : mssql::NVarcharLiteral(declarations)) + ", 0";
	auto result = MSSQLSimpleQuery::Execute(connection, batch, timeout_ms);
	if (!result.success) {
		shape.reason = result.DescribeError();
		return shape;
	}
	const int name_idx = FindResultColumn(result.column_names, "name");
	const int type_idx = FindResultColumn(result.column_names, "system_type_name");
	const int len_idx = FindResultColumn(result.column_names, "max_length");
	const int prec_idx = FindResultColumn(result.column_names, "precision");
	const int scale_idx = FindResultColumn(result.column_names, "scale");
	const int hidden_idx = FindResultColumn(result.column_names, "is_hidden");
	const int err_idx = FindResultColumn(result.column_names, "error_number");
	if (name_idx < 0 || type_idx < 0 || len_idx < 0 || prec_idx < 0 || scale_idx < 0) {
		shape.reason = "sp_describe_first_result_set answered with an unexpected shape";
		return shape;
	}
	if (result.rows.empty()) {
		shape.reason = "the statement returns no result set";
		return shape;
	}
	for (const auto &row : result.rows) {
		if (err_idx >= 0) {
			const string &err = row[err_idx];
			if (!err.empty() && err != "NULL" && err != "0") {
				shape.reason = "sp_describe_first_result_set could not determine the shape (error " + err + ")";
				return shape;
			}
		}
		if (hidden_idx >= 0 && row[hidden_idx] == "1") {
			continue;
		}
		const string &type_name = row[type_idx];
		if (type_name.empty() || type_name == "NULL") {
			shape.reason = "sp_describe_first_result_set reported a column without a type";
			return shape;
		}
		string base = StringUtil::Lower(type_name.substr(0, type_name.find('(')));
		auto max_length = static_cast<int16_t>(std::atoi(row[len_idx].c_str()));
		auto precision = static_cast<uint8_t>(std::atoi(row[prec_idx].c_str()));
		auto scale = static_cast<uint8_t>(std::atoi(row[scale_idx].c_str()));
		const string &name = row[name_idx];
		tds::ColumnMetadata column;
		column.name = name == "NULL" ? string() : name;
		if (!DescribedTypeToTdsMetadata(base, max_length, precision, scale, column)) {
			shape.reason = "the describe names a type this extension does not map: " + type_name;
			return shape;
		}
		shape.types.push_back(tds::encoding::TypeConverter::GetDuckDBType(column));
		shape.names.push_back(column.name);
	}
	if (shape.types.empty()) {
		shape.reason = "the statement returns no visible column";
		return shape;
	}
	shape.ok = true;
	return shape;
}

// sp_prepare: the server compiles once and answers with the statement's
// COLMETADATA (a zero-row result set) before the handle. The types are read
// off that token exactly as the stream reads them at execution.
static MSSQLDescribedShape PrepareStatement(tds::TdsConnection &connection, const string &statement,
											const string &declarations, int timeout_ms, int32_t &handle) {
	MSSQLDescribedShape shape;
	string batch = "DECLARE @h int;\nEXEC sp_prepare @h OUTPUT, " +
				   (declarations.empty() ? string("NULL") : mssql::NVarcharLiteral(declarations)) + ", " +
				   mssql::NVarcharLiteral(statement) + ";\nSELECT @h";
	auto result = MSSQLSimpleQuery::Execute(connection, batch, timeout_ms);
	if (!result.success) {
		shape.reason = result.DescribeError();
		return shape;
	}
	if (result.rows.empty() || result.rows.back().empty() || result.rows.back()[0].empty()) {
		shape.reason = "sp_prepare returned no handle";
		return shape;
	}
	handle = std::atoi(result.rows.back()[0].c_str());
	if (result.result_sets.size() < 2) {
		shape.reason = "the statement returns no result set";
		return shape;
	}
	for (const auto &col : result.result_sets.front()) {
		shape.types.push_back(tds::encoding::TypeConverter::GetDuckDBType(col));
		shape.names.push_back(col.name);
	}
	shape.ok = true;
	return shape;
}

static void ValidateScanContext(ClientContext &context, const string &context_name) {
	// Spec 047: per-catalog ownership via DuckDB catalog lookup
	try {
		auto &catalog = Catalog::GetCatalog(context, Identifier(context_name));
		if (catalog.GetCatalogType() != "mssql") {
			throw InvalidInputException(
				"MSSQL Error: Unknown context '%s'. Attach a database first with: ATTACH '' AS %s (TYPE mssql, SECRET "
				"...)",
				context_name, context_name);
		}
	} catch (const std::exception &) {
		throw InvalidInputException(
			"MSSQL Error: Unknown context '%s'. Attach a database first with: ATTACH '' AS %s (TYPE mssql, SECRET ...)",
			context_name, context_name);
	}
}

static bool ReadPreparedOption(const TableFunctionBindInput &input) {
	auto it = input.named_parameters.find("prepared");
	if (it == input.named_parameters.end() || it->second.IsNull()) {
		return false;
	}
	return BooleanValue::Get(it->second);
}

// The part of Bind shared by mssql_scan and mssql_scan_params: settle the
// shape without running the query, or -- when the server cannot describe it --
// run it at Bind as this function always had (spec 075 F1). `bind_data` arrives
// with context_name, query, execute_sql and prepared set; `params` is empty for
// mssql_scan.
static void BindDescribedScan(ClientContext &context, MSSQLScanBindData &bind_data, const mssql::SqlParamSet &params,
							  vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto bind_start = std::chrono::steady_clock::now();
	auto &catalog = Catalog::GetCatalog(context, Identifier(bind_data.context_name));
	auto &mssql_catalog = catalog.Cast<MSSQLCatalog>();
	const bool in_transaction = !context.transaction.IsAutoCommit();
	const int timeout_ms = QueryTimeoutMs(context);
	const string declarations = params.Declarations();

	MSSQLDescribedShape shape;
	auto connection = ConnectionProvider::GetConnection(context, mssql_catalog);
	if (!connection) {
		throw IOException("mssql_scan: Failed to acquire connection from pool for '%s'", bind_data.context_name);
	}
	bool held = false;
	const bool wanted_prepared = bind_data.prepared;
	try {
		if (wanted_prepared) {
			int32_t handle = 0;
			shape = PrepareStatement(*connection, bind_data.query, declarations, timeout_ms, handle);
			if (shape.ok) {
				auto session = make_shared_ptr<MSSQLPreparedSession>();
				session->handle = handle;
				if (!in_transaction) {
					// The handle lives in this session: keep the connection until the
					// bind data dies. In a transaction it is the pinned one, which the
					// transaction keeps for us.
					session->connection = connection;
					session->pool_handle = mssql_catalog.GetConnectionPoolHandle();
					session->reset_on_release = ConnectionProvider::ShouldResetOnRelease(context);
					held = true;
				}
				bind_data.prepared_session = std::move(session);
				bind_data.execute_sql = params.ExecuteByHandleBatch(handle);
				MSSQL_FN_DEBUG_LOG(1, "MSSQLScanBind: prepared handle %d", (int)handle);
			} else {
				// The server took the statement but did not settle its shape (a
				// batch of more than one statement), or refused it: degrade to the
				// default path -- describe, and failing that run it at Bind as a
				// plain batch, where a real error surfaces as such. A handle it did
				// hand out dies with the session reset.
				MSSQL_FN_DEBUG_LOG(1, "MSSQLScanBind: sp_prepare gave no shape (%s), describing instead",
								   shape.reason.c_str());
				bind_data.prepared = false;
				shape = DescribeFirstResultSet(*connection, bind_data.query, declarations, timeout_ms);
			}
		} else {
			shape = DescribeFirstResultSet(*connection, bind_data.query, declarations, timeout_ms);
		}
	} catch (...) {
		ConnectionProvider::ReleaseConnection(context, mssql_catalog, std::move(connection));
		throw;
	}
	if (!held) {
		ConnectionProvider::ReleaseConnection(context, mssql_catalog, std::move(connection));
	}

	if (shape.ok) {
		return_types = shape.types;
		names.clear();
		for (const auto &name : shape.names) {
			names.push_back(Identifier(name));
		}
		bind_data.return_types = shape.types;
		bind_data.column_names = shape.names;
		auto bind_ms =
			std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - bind_start)
				.count();
		MSSQL_FN_DEBUG_LOG(1, "MSSQLScanBind: described %llu column(s) in %ldms, query deferred to InitGlobal",
						   (unsigned long long)shape.types.size(), (long)bind_ms);
		return;
	}

	// F1 fallback: the server would not describe it, so learn the shape the way
	// this function always had -- by running it.
	MSSQL_FN_DEBUG_LOG(1, "MSSQLScanBind: describe fell back to execution: %s", shape.reason.c_str());
	bind_data.executed_at_bind = true;
	auto exec_start = std::chrono::steady_clock::now();
	MSSQLQueryExecutor executor(bind_data.context_name);
	auto result_stream = executor.Execute(context, bind_data.execute_sql);
	auto exec_ms =
		std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - exec_start).count();
	MSSQL_FN_DEBUG_LOG(1, "MSSQLScanBind: query executed in %ldms", (long)exec_ms);

	return_types = result_stream->GetColumnTypes();
	names.clear();
	for (const auto &name : result_stream->GetColumnNames()) {
		names.push_back(Identifier(name));
	}
	bind_data.return_types = return_types;
	bind_data.column_names = result_stream->GetColumnNames();

	if (in_transaction) {
		// Issue #316: this connection is the transaction's ONE pinned connection.
		// Holding it open until execution makes the NEXT mssql_scan fail in its own
		// Bind, before any InitGlobal runs. Drain here and close it. See the
		// `materialized` comment on MSSQLScanBindData for why the trigger is the
		// transaction and not a count of scans.
		MSSQL_FN_DEBUG_LOG(1, "MSSQLScanBind: in a transaction — draining to release the pinned connection");
		auto collection = make_shared_ptr<ColumnDataCollection>(context, bind_data.return_types);
		DataChunk chunk;
		chunk.Initialize(Allocator::Get(context), bind_data.return_types);
		idx_t total = 0;
		for (;;) {
			chunk.Reset();
			const idx_t rows = result_stream->FillChunk(chunk);
			if (rows == 0) {
				break;
			}
			collection->Append(chunk);
			total += rows;
		}
		result_stream->SurfaceWarnings(context);
		// Closing is what returns the connection to Idle; draining alone does not.
		result_stream.reset();
		bind_data.materialized = std::move(collection);
		MSSQL_FN_DEBUG_LOG(1, "MSSQLScanBind: materialized %llu row(s), pinned connection released",
						   (unsigned long long)total);
	} else {
		// Autocommit: this scan holds a pooled connection of its own, so streaming
		// costs nobody anything. Register the stream so execution reuses it instead
		// of running the query twice. Spec 047 / US3: registry lives on MSSQLCatalog.
		bind_data.result_stream_id = mssql_catalog.RegisterStream(std::move(result_stream));
		MSSQL_FN_DEBUG_LOG(1, "MSSQLScanBind: registered result_stream_id=%s", bind_data.result_stream_id.c_str());
	}
	auto bind_ms =
		std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - bind_start).count();
	MSSQL_FN_DEBUG_LOG(1, "MSSQLScanBind: END (total %ldms)", (long)bind_ms);
}

unique_ptr<FunctionData> MSSQLScanBind(ClientContext &context, TableFunctionBindInput &input,
									   vector<LogicalType> &return_types, vector<Identifier> &names) {
	MSSQL_FN_DEBUG_LOG(1, "MSSQLScanBind: START");
	if (input.inputs.size() != 2) {
		throw InvalidInputException("MSSQL Error: mssql_scan requires 2 arguments: context_name and query");
	}
	auto bind_data = make_uniq<MSSQLScanBindData>();
	bind_data->context_name = input.inputs[0].GetValue<string>();
	bind_data->query = input.inputs[1].GetValue<string>();
	bind_data->execute_sql = bind_data->query;
	bind_data->prepared = ReadPreparedOption(input);
	ValidateScanContext(context, bind_data->context_name);
	mssql::SqlParamSet no_params;
	BindDescribedScan(context, *bind_data, no_params, return_types, names);
	return std::move(bind_data);
}

// mssql_scan_params(context, statement, {name: value, ...} [, declarations])
unique_ptr<FunctionData> MSSQLScanParamsBind(ClientContext &context, TableFunctionBindInput &input,
											 vector<LogicalType> &return_types, vector<Identifier> &names) {
	MSSQL_FN_DEBUG_LOG(1, "MSSQLScanParamsBind: START");
	if (input.inputs.size() < 3 || input.inputs.size() > 4) {
		throw InvalidInputException(
			"mssql_scan_params requires context_name, a statement and a STRUCT of parameters, "
			"with an optional declaration list");
	}
	auto bind_data = make_uniq<MSSQLScanBindData>();
	bind_data->context_name = input.inputs[0].GetValue<string>();
	bind_data->query = input.inputs[1].GetValue<string>();
	string declarations_override;
	if (input.inputs.size() == 4 && !input.inputs[3].IsNull()) {
		declarations_override = input.inputs[3].GetValue<string>();
	}
	bind_data->prepared = ReadPreparedOption(input);
	ValidateScanContext(context, bind_data->context_name);
	auto params = mssql::BuildSqlParams(input.inputs[2], declarations_override);
	bind_data->execute_sql = params.ExecuteSqlBatch(bind_data->query);
	BindDescribedScan(context, *bind_data, params, return_types, names);
	return std::move(bind_data);
}

unique_ptr<GlobalTableFunctionState> MSSQLScanInitGlobal(ClientContext &context, TableFunctionInitInput &input) {
	auto init_start = std::chrono::steady_clock::now();
	MSSQL_FN_DEBUG_LOG(1, "MSSQLScanInitGlobal: START");

	auto &bind_data = input.bind_data->Cast<MSSQLScanBindData>();
	auto result = make_uniq<MSSQLScanGlobalState>();
	result->context_name = bind_data.context_name;

	if (!bind_data.executed_at_bind) {
		// Spec 075 W2: the query runs here, on the shape Bind described.
		auto &catalog = Catalog::GetCatalog(context, Identifier(bind_data.context_name));
		auto &mssql_catalog = catalog.Cast<MSSQLCatalog>();
		// Inside a transaction the stream is on the ONE pinned connection: take the
		// catalog's MaterializeMutex BEFORE sending the batch, so a catalog scan
		// materialising on another thread has drained (or has not started) -- the
		// same order table_scan.cpp keeps. Held through the drain below.
		std::unique_lock<std::mutex> materialize_lock;
		const bool in_transaction = !context.transaction.IsAutoCommit();
		if (in_transaction) {
			materialize_lock = std::unique_lock<std::mutex>(mssql_catalog.MaterializeMutex());
		}
		MSSQLQueryExecutor executor(bind_data.context_name);
		unique_ptr<MSSQLResultStream> stream;
		if (bind_data.prepared_session && bind_data.prepared_session->connection) {
			// The handle lives in the held session; the stream borrows it and gives
			// it back to the session, not to the pool.
			stream = executor.ExecuteOn(context, bind_data.prepared_session->connection, bind_data.execute_sql, false,
										false);
		} else {
			stream = executor.Execute(context, bind_data.execute_sql);
		}
		stream->SurfaceWarnings(context);
		if (stream->GetColumnTypes() != bind_data.return_types) {
			// The described shape is what the plan was built on; serving rows of
			// another shape would be a silent wrong answer.
			throw InvalidInputException(
				"mssql_scan: the statement's result shape changed between bind and execution: bound (%s), got (%s)",
				TypeListToString(bind_data.return_types), TypeListToString(stream->GetColumnTypes()));
		}
		if (in_transaction) {
			// The stream is on the transaction's ONE pinned connection: drain it now
			// so the next scan or sink of this catalog finds the connection Idle
			// (issue #239 / #316).
			auto collection = make_uniq<ColumnDataCollection>(context, bind_data.return_types);
			DataChunk chunk;
			chunk.Initialize(Allocator::Get(context), bind_data.return_types);
			idx_t total = 0;
			for (;;) {
				chunk.Reset();
				const idx_t rows = stream->FillChunk(chunk);
				if (rows == 0) {
					break;
				}
				collection->Append(chunk);
				total += rows;
			}
			stream.reset();
			result->materialized = std::move(collection);
			result->materialized->InitializeScan(result->materialized_scan);
			MSSQL_FN_DEBUG_LOG(1, "MSSQLScanInitGlobal: materialized %llu row(s), pinned connection released",
							   (unsigned long long)total);
		} else {
			result->result_stream = std::move(stream);
		}
		auto init_ms =
			std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - init_start)
				.count();
		MSSQL_FN_DEBUG_LOG(1, "MSSQLScanInitGlobal: executed at init in %ldms", (long)init_ms);
		return std::move(result);
	}

	// Issue #316: Bind already drained this one and released the connection.
	if (bind_data.materialized) {
		result->materialized_shared = bind_data.materialized;
		result->materialized_shared->InitializeScan(result->materialized_scan);
		MSSQL_FN_DEBUG_LOG(1, "MSSQLScanInitGlobal: serving %llu materialized row(s)",
						   (unsigned long long)result->materialized_shared->Count());
		return std::move(result);
	}

	// Try to retrieve pre-initialized result stream from registry
	// This was created in Bind and avoids executing the query twice
	// Spec 047 / US3: registry lives on MSSQLCatalog (previously process-wide singleton).
	if (!bind_data.result_stream_id.empty()) {
		MSSQL_FN_DEBUG_LOG(1, "MSSQLScanInitGlobal: retrieving result_stream_id=%s",
						   bind_data.result_stream_id.c_str());
		auto &catalog = Catalog::GetCatalog(context, Identifier(bind_data.context_name));
		auto &mssql_catalog = catalog.Cast<MSSQLCatalog>();
		result->result_stream = mssql_catalog.RetrieveStream(bind_data.result_stream_id);
		if (result->result_stream) {
			// COLMETADATA is known, so warn before any rows move -- the drain-end
			// call is missed entirely by a query that stops early (issue #224).
			result->result_stream->SurfaceWarnings(context);
			auto init_end = std::chrono::steady_clock::now();
			auto init_ms = std::chrono::duration_cast<std::chrono::milliseconds>(init_end - init_start).count();
			MSSQL_FN_DEBUG_LOG(1, "MSSQLScanInitGlobal: retrieved from registry in %ldms", (long)init_ms);
			return std::move(result);
		}
		MSSQL_FN_DEBUG_LOG(1, "MSSQLScanInitGlobal: result stream not found in registry, re-executing query");
	}

	// Fallback: re-execute the query (this shouldn't happen normally)
	auto exec_start = std::chrono::steady_clock::now();
	MSSQL_FN_DEBUG_LOG(1, "MSSQLScanInitGlobal: executing query for data...");
	MSSQLQueryExecutor executor(bind_data.context_name);
	result->result_stream = executor.Execute(context, bind_data.execute_sql);
	if (result->result_stream) {
		result->result_stream->SurfaceWarnings(context);
	}
	auto exec_end = std::chrono::steady_clock::now();
	auto exec_ms = std::chrono::duration_cast<std::chrono::milliseconds>(exec_end - exec_start).count();
	MSSQL_FN_DEBUG_LOG(1, "MSSQLScanInitGlobal: query executed in %ldms", (long)exec_ms);

	auto init_end = std::chrono::steady_clock::now();
	auto init_ms = std::chrono::duration_cast<std::chrono::milliseconds>(init_end - init_start).count();
	MSSQL_FN_DEBUG_LOG(1, "MSSQLScanInitGlobal: END (total %ldms)", (long)init_ms);

	return std::move(result);
}

unique_ptr<LocalTableFunctionState> MSSQLScanInitLocal(ExecutionContext &context, TableFunctionInitInput &input,
													   GlobalTableFunctionState *global_state) {
	return make_uniq<MSSQLScanLocalState>();
}

void MSSQLScanFunction(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &global_state = data.global_state->Cast<MSSQLScanGlobalState>();

	// Issue #316: materialized at Bind, so there is no stream to read. Must come
	// before the `!result_stream` test below, which would report "done" at once.
	if (global_state.materialized_shared) {
		output.Reset();
		global_state.materialized_shared->Scan(global_state.materialized_scan, output);
		if (output.size() == 0) {
			global_state.done = true;
		}
		return;
	}
	// Spec 075 W2: materialized at InitGlobal (in a transaction).
	if (global_state.materialized) {
		output.Reset();
		global_state.materialized->Scan(global_state.materialized_scan, output);
		if (output.size() == 0) {
			global_state.done = true;
		}
		return;
	}

	// Start timing on first call
	if (!global_state.timing_started) {
		global_state.scan_start = std::chrono::steady_clock::now();
		global_state.timing_started = true;
		MSSQL_FN_DEBUG_LOG(1, "MSSQLScanFunction: FIRST CALL - scan started");
	}

	// Check if we're done
	if (global_state.done || !global_state.result_stream) {
		auto scan_end = std::chrono::steady_clock::now();
		auto total_ms =
			std::chrono::duration_cast<std::chrono::milliseconds>(scan_end - global_state.scan_start).count();
		MSSQL_FN_DEBUG_LOG(1, "MSSQLScanFunction: SCAN COMPLETE - total=%ldms", (long)total_ms);
		output.SetChildCardinality(0);
		return;
	}

	// Check for query cancellation (Ctrl+C)
	if (context.IsInterrupted()) {
		global_state.result_stream->Cancel();
		global_state.done = true;
		output.SetChildCardinality(0);
		return;
	}

	// Fill chunk from result stream
	try {
		idx_t rows = global_state.result_stream->FillChunk(output);
		if (rows == 0) {
			global_state.done = true;
			// Surface any warnings
			global_state.result_stream->SurfaceWarnings(context);
		}
	} catch (const Exception &e) {
		global_state.done = true;
		// The server's own notices about the batch that just failed -- and the ones
		// immediately preceding the failure are the useful ones. Without this they
		// are lost with the stream, which contradicts the rule the mssql_exec path
		// already follows (PR #320 review).
		//
		// Swallowing a throw from here is deliberate: whatever the logger does, it
		// must not replace the exception the caller is waiting for.
		try {
			global_state.result_stream->SurfaceWarnings(context);
		} catch (...) {	 // NOLINT: never mask the original error
		}
		throw;
	}
}

//===----------------------------------------------------------------------===//
// MSSQLCatalogScanBindData Implementation
//===----------------------------------------------------------------------===//

unique_ptr<FunctionData> MSSQLCatalogScanBindData::Copy() const {
	auto result = make_uniq<MSSQLCatalogScanBindData>();
	result->context_name = context_name;
	result->schema_name = schema_name;
	result->table_name = table_name;
	result->all_types = all_types;
	result->all_column_names = all_column_names;
	result->mssql_columns = mssql_columns;
	result->return_types = return_types;
	result->column_names = column_names;
	result->result_stream_id = result_stream_id;
	result->complex_filter_where_clause = complex_filter_where_clause;
	// ORDER BY pushdown fields (Spec 039)
	result->order_by_clause = order_by_clause;
	result->top_n = top_n;
	result->requires_materialization = requires_materialization;
	// RowId support fields
	result->rowid_requested = rowid_requested;
	result->pk_column_names = pk_column_names;
	result->pk_column_types = pk_column_types;
	result->pk_result_indices = pk_result_indices;
	result->pk_is_composite = pk_is_composite;
	result->rowid_type = rowid_type;
	// Spec 052 (Option D): copy the table_entry pointer. Lifetime of the
	// underlying entry is guaranteed by MSSQLBindAnchors (per ClientContext,
	// released at QueryEnd); the bind-data copy inherits the same anchor
	// from the originating LookupEntry call.
	result->table_entry = table_entry;
	return std::move(result);
}

bool MSSQLCatalogScanBindData::Equals(const FunctionData &other) const {
	auto &other_data = other.Cast<MSSQLCatalogScanBindData>();
	return context_name == other_data.context_name && schema_name == other_data.schema_name &&
		   table_name == other_data.table_name;
}

//===----------------------------------------------------------------------===//
// mssql_exec Scalar Function Implementation
//===----------------------------------------------------------------------===//

// Bind data for mssql_exec - stores context name (attached database name)
struct MSSQLExecBindData : public FunctionData {
	string context_name;

	MSSQLExecBindData(string context_name_p) : context_name(std::move(context_name_p)) {}

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<MSSQLExecBindData>(context_name);
	}

	bool Equals(const FunctionData &other) const override {
		auto &other_data = other.Cast<MSSQLExecBindData>();
		return context_name == other_data.context_name;
	}
};

// The context-name half of Bind, shared by mssql_exec and mssql_exec_params.
static string BindExecContextName(ClientContext &context, const Expression &argument, const char *function_name) {
	// First argument is the context name (attached database name, must be constant)
	if (argument.HasParameter()) {
		throw InvalidInputException("%s: context_name must be a constant, not a parameter", function_name);
	}
	string context_name;
	if (argument.IsFoldable()) {
		auto context_val = ExpressionExecutor::EvaluateScalar(context, argument);
		context_name = context_val.ToString();
		// Validate the context exists (Spec 047: per-catalog ownership)
		try {
			auto &catalog = Catalog::GetCatalog(context, Identifier(context_name));
			if (catalog.GetCatalogType() != "mssql") {
				throw BinderException(
					"%s: Unknown context '%s'. Attach a database first with: ATTACH '' AS %s (TYPE mssql, SECRET ...)",
					function_name, context_name, context_name);
			}
		} catch (const std::exception &) {
			throw BinderException(
				"%s: Unknown context '%s'. Attach a database first with: ATTACH '' AS %s (TYPE mssql, SECRET ...)",
				function_name, context_name, context_name);
		}
	}
	return context_name;
}

// Bind function for mssql_exec
static duckdb::unique_ptr<duckdb::FunctionData> MSSQLExecBind(duckdb::BindScalarFunctionInput &input) {
	auto &context = input.GetClientContext();
	auto &arguments = input.GetArguments();
	return make_uniq<MSSQLExecBindData>(BindExecContextName(context, *arguments[0], "mssql_exec"));
}

// mssql_exec_params(context, statement, {name: value, ...} [, declarations])
static duckdb::unique_ptr<duckdb::FunctionData> MSSQLExecParamsBind(duckdb::BindScalarFunctionInput &input) {
	auto &context = input.GetClientContext();
	auto &arguments = input.GetArguments();
	if (arguments[2]->GetReturnType().id() != LogicalTypeId::STRUCT) {
		throw BinderException(
			"mssql_exec_params: the parameters must be a STRUCT of name -> value, e.g. {'a': 1, "
			"'b': 'x'}; got %s",
			arguments[2]->GetReturnType().ToString());
	}
	return make_uniq<MSSQLExecBindData>(BindExecContextName(context, *arguments[0], "mssql_exec_params"));
}

// Heuristic: does this raw T-SQL statement potentially change schema/catalog
// metadata? Used to invalidate the catalog cache after mssql_exec() runs DDL so
// that subsequent catalog operations (CREATE TABLE IF NOT EXISTS, reads) don't
// act on stale existence metadata (issue #151). Over-detection only costs a
// metadata refresh; under-detection would leave the cache stale, so we err
// toward invalidating — including EXEC, since a stored procedure may run DDL.
static bool ExecSqlMayChangeSchema(const string &sql) {
	auto upper = StringUtil::Upper(sql);
	static const char *kSchemaKeywords[] = {"CREATE", "DROP", "ALTER", "TRUNCATE", "RENAME", "EXEC"};
	for (auto keyword : kSchemaKeywords) {
		if (upper.find(keyword) != string::npos) {
			return true;
		}
	}
	return false;
}

// Run one batch on the named catalog and return the DONE row count: the body
// of mssql_exec, shared with mssql_exec_params. `statement` is the caller's
// text for the DDL heuristic -- for mssql_exec_params the batch wraps it in
// sp_executesql, whose name would otherwise trip the EXEC keyword every time.
static int64_t RunExecBatch(ClientContext &client_context, const string &context_name, const string &sql,
							const string &statement, const char *function_name) {
	// Get the MSSQL catalog (Spec 047: per-catalog ownership)
	MSSQLCatalog *catalog_ptr = nullptr;
	try {
		auto &raw_catalog = Catalog::GetCatalog(client_context, Identifier(context_name));
		if (raw_catalog.GetCatalogType() != "mssql") {
			throw InvalidInputException("%s: Context '%s' is attached as a non-MSSQL catalog (type: %s)", function_name,
										context_name, raw_catalog.GetCatalogType());
		}
		catalog_ptr = &raw_catalog.Cast<MSSQLCatalog>();
	} catch (const InvalidInputException &) {
		throw;
	} catch (const std::exception &) {
		throw InvalidInputException(
			"%s: Unknown context '%s'. Attach a database first with: ATTACH '' AS %s (TYPE mssql, SECRET "
			"...)",
			function_name, context_name, context_name);
	}
	auto &catalog = *catalog_ptr;
	if (catalog.IsReadOnly()) {
		throw InvalidInputException("Cannot execute %s: catalog '%s' is attached in read-only mode", function_name,
									context_name);
	}

	// Get connection via ConnectionProvider (handles transaction pinning)
	auto connection = ConnectionProvider::GetConnection(client_context, catalog);

	if (!connection) {
		throw IOException("%s: Failed to acquire connection from pool for '%s'", function_name, context_name);
	}

	// Execute the SQL.
	// Honor the mssql_query_timeout setting (in seconds) the same way mssql_scan
	// does — previously mssql_exec used MSSQLSimpleQuery's hardcoded 30s default and
	// dropped long-running server-side queries regardless of the setting (#90/#145).
	// 0 (or negative / absurdly large) means no timeout.
	int query_timeout_s = LoadQueryTimeout(client_context);
	int timeout_ms;
	if (query_timeout_s <= 0 || query_timeout_s > INT_MAX / 1000) {
		timeout_ms = 0;	 // no timeout (wait indefinitely)
	} else {
		timeout_ms = query_timeout_s * 1000;
	}
	try {
		auto query_result = MSSQLSimpleQuery::Execute(*connection, sql, timeout_ms);

		// PRINT output and RAISERROR below severity 11 from whatever was run
		// here -- a procedure's progress notices, most of all. Logged before
		// the error check below, because a batch that ultimately failed is
		// exactly when its notices are worth reading.
		for (const auto &info : query_result.info_messages) {
			LogTdsInfo(client_context, info);
		}

		// Release connection via ConnectionProvider (no-op if in transaction)
		ConnectionProvider::ReleaseConnection(client_context, catalog, std::move(connection));

		if (!query_result.success) {
			throw InvalidInputException("MSSQL execution error: %s", query_result.DescribeError());
		}

		// Issue #151: raw DDL run through mssql_exec() bypasses the catalog metadata
		// cache. If the statement may have changed schema, invalidate the cache so a
		// subsequent CREATE TABLE IF NOT EXISTS / read sees the real server-side state
		// instead of a stale cached entry (which caused "Invalid object name" after a
		// raw DROP followed by CREATE ... IF NOT EXISTS). InvalidateMetadataCache() is
		// the lazy path: it marks the metadata cache stale AND invalidates each schema's
		// table set (evicting bound table entries), with reload deferred to next access.
		// Gated by mssql_exec_invalidate_cache, which defaults to FALSE (like the Postgres
		// extension's postgres_execute): by default the caller invalidates manually via
		// mssql_invalidate_cache(); set the flag true to auto-invalidate here.
		if (ExecSqlMayChangeSchema(statement) && LoadExecInvalidateCache(client_context)) {
			catalog.InvalidateMetadataCache();
		}

		// Return affected row count from DONE token
		// For DML operations (INSERT/UPDATE/DELETE), this is the number of affected rows
		// For DDL and SELECT statements, this may be 0
		return query_result.rows_affected;

	} catch (...) {
		// Release connection on error
		ConnectionProvider::ReleaseConnection(client_context, catalog, std::move(connection));
		throw;
	}
}

// Execute function for mssql_exec
static void MSSQLExecExecute(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &bind_data = state.expr.Cast<BoundFunctionExpression>().BindInfo()->Cast<MSSQLExecBindData>();

	auto &context_names = args.data[0];
	auto &sql_statements = args.data[1];

	UnaryExecutor::Execute<string_t, int64_t>(sql_statements, result, args.size(), [&](string_t sql_str) -> int64_t {
		// Get the context name from bind data or first argument
		string context_name = bind_data.context_name;
		if (context_name.empty()) {
			// Get from runtime argument
			auto context_val = context_names.GetValue(0);
			context_name = context_val.ToString();
		}

		string sql = sql_str.GetString();

		MSSQL_FN_DEBUG_LOG(1, "mssql_exec: context=%s, sql=%s", context_name.c_str(), sql.c_str());

		return RunExecBatch(state.GetContext(), context_name, sql, sql, "mssql_exec");
	});
}

ScalarFunction MSSQLExecScalarFunction::GetFunction() {
	ScalarFunction func(NAME, {LogicalType::VARCHAR, LogicalType::VARCHAR}, LogicalType::BIGINT, MSSQLExecExecute,
						MSSQLExecBind);
	func.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	// Side-effecting (runs T-SQL on the server): VOLATILE stops the optimizer
	// from constant-folding the call at plan time or caching it per row.
	func.SetVolatile();
	func.SetFallible();
	return func;
}

// Execute function for mssql_exec_params: one server round trip per row, the
// values rendered into a DECLARE block ahead of sp_executesql (see
// query/mssql_sql_params.hpp for why not `@p = <literal>`).
static void MSSQLExecParamsExecute(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &bind_data = state.expr.Cast<BoundFunctionExpression>().BindInfo()->Cast<MSSQLExecBindData>();
	auto &client_context = state.GetContext();
	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto out = FlatVector::GetDataMutable<int64_t>(result);
	auto &validity = FlatVector::ValidityMutable(result);
	const bool has_declarations = args.ColumnCount() > 3;
	for (idx_t i = 0; i < args.size(); i++) {
		Value context_val = args.data[0].GetValue(i);
		Value sql_val = args.data[1].GetValue(i);
		Value params_val = args.data[2].GetValue(i);
		Value decl_val = has_declarations ? args.data[3].GetValue(i) : Value();
		if (context_val.IsNull() || sql_val.IsNull()) {
			validity.SetInvalid(i);
			continue;
		}
		string context_name = bind_data.context_name.empty() ? context_val.ToString() : bind_data.context_name;
		string statement = sql_val.ToString();
		auto params = mssql::BuildSqlParams(params_val, decl_val.IsNull() ? string() : decl_val.ToString());
		string batch = params.ExecuteSqlBatch(statement);
		MSSQL_FN_DEBUG_LOG(1, "mssql_exec_params: context=%s, batch=%s", context_name.c_str(), batch.c_str());
		out[i] = RunExecBatch(client_context, context_name, batch, statement, "mssql_exec_params");
	}
}

void RegisterMSSQLExecFunction(ExtensionLoader &loader) {
	auto func = MSSQLExecScalarFunction::GetFunction();
	loader.RegisterFunction(func);

	// mssql_exec_params(context, statement, STRUCT [, declarations]) -> BIGINT
	ScalarFunctionSet params_set("mssql_exec_params");
	for (int with_declarations = 0; with_declarations < 2; with_declarations++) {
		vector<LogicalType> arguments{LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::ANY};
		if (with_declarations) {
			arguments.push_back(LogicalType::VARCHAR);
		}
		ScalarFunction f("mssql_exec_params", arguments, LogicalType::BIGINT, MSSQLExecParamsExecute,
						 MSSQLExecParamsBind);
		f.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
		f.SetVolatile();
		f.SetFallible();
		params_set.AddFunction(f);
	}
	loader.RegisterFunction(params_set);
}

//===----------------------------------------------------------------------===//
// Registration
//===----------------------------------------------------------------------===//

void RegisterMSSQLFunctions(ExtensionLoader &loader) {
	// mssql_scan(context_name VARCHAR, query VARCHAR)
	// -> dynamic return schema based on query result columns
	TableFunction mssql_scan("mssql_scan", {LogicalType::VARCHAR, LogicalType::VARCHAR}, MSSQLScanFunction,
							 MSSQLScanBind, MSSQLScanInitGlobal, MSSQLScanInitLocal);
	// Spec 075: `prepared := true` compiles once via sp_prepare instead of
	// describing at bind and compiling again at execution.
	mssql_scan.named_parameters["prepared"] = LogicalType::BOOLEAN;
	loader.RegisterFunction(mssql_scan);

	// mssql_scan_params(context, statement, STRUCT [, declarations], prepared := false)
	TableFunctionSet scan_params("mssql_scan_params");
	for (int with_declarations = 0; with_declarations < 2; with_declarations++) {
		vector<LogicalType> arguments{LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::ANY};
		if (with_declarations) {
			arguments.push_back(LogicalType::VARCHAR);
		}
		TableFunction f("mssql_scan_params", arguments, MSSQLScanFunction, MSSQLScanParamsBind, MSSQLScanInitGlobal,
						MSSQLScanInitLocal);
		f.named_parameters["prepared"] = LogicalType::BOOLEAN;
		scan_params.AddFunction(f);
	}
	loader.RegisterFunction(scan_params);
}

}  // namespace duckdb
