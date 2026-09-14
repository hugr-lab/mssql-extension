//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// dml/insert/mssql_insert_bulk_plan.hpp
//
// What PlanInsert settles for the bulk path of an INSERT, once, at plan time
// (spec 062 W1 / W2): whether the sink may load through INSERT BULK at all,
// and everything the load needs that does not change per chunk -- the target,
// the COLMETADATA of the inserted columns (from the catalog: no round trip),
// the target->chunk mapping, the INSERT BULK text, the batch size, the shape
// the TABLOCK policy and the columnstore warm-up gate read, the writer limit
// and the threshold below which the rows go as statements instead.
//===----------------------------------------------------------------------===//

#pragma once

#include "catalog/mssql_index_kind.hpp"
#include "copy/target_resolver.hpp"
#include "duckdb/common/types.hpp"

namespace duckdb {

struct MSSQLInsertBulkPlan {
	//! False keeps today's statement path for the whole INSERT: RETURNING (no
	//! rows come back from INSERT BULK), an explicit identity column (INSERT
	//! BULK keeps its value where a statement lets the server refuse it), or
	//! mssql_insert_use_bcp = false.
	bool enabled = false;
	//! Why not, for EXPLAIN and the debug log.
	string statement_path_reason;

	//! Rows an INSERT may have and still go as statements.
	idx_t threshold = 0;

	mssql::BCPCopyTarget target;
	//! One entry per INSERTED column, in column-list order.
	vector<mssql::BCPColumnMetadata> columns;
	//! columns[i] is fed by chunk column column_mapping[i]: the 2.0 insert
	//! child is full-width in table order, so this is insert_column_indices.
	vector<int32_t> column_mapping;
	string insert_bulk_sql;
	//! mssql_copy_flush_rows: the batch boundary the server sees.
	idx_t flush_rows = 0;
	//! The target's shape: the warm-up gate applies to a clustered columnstore,
	//! and with `tablock` it decides whether writers may run in parallel.
	MSSQLIndexKind shape = MSSQLIndexKind::HEAP;
	//! The resolved TABLOCK decision (mssql_copy_tablock against the shape).
	bool tablock = false;
	//! mssql_copy_parallel_writers as set; 0 derives from the thread count.
	int64_t configured_writers = 0;
};

}  // namespace duckdb
