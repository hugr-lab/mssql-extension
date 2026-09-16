//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// dml/mssql_staged_rows.hpp
//
// Rows held back until a statement knows how big it is (spec 062 W2a).
//
// The INSERT sink buffers its first rows and decides at a threshold whether
// they go as statements or as a bulk load -- exact, where a plan-time estimate
// is wrong for exactly the statements that matter (an INSERT ... SELECT behind a
// filter, a VALUES list wrapped in a CTE). Spec 066 makes the same decision one
// operator later: a handful of rowids as one statement, more as a `#temp` fill.
// One buffer type, one decision shape.
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/main/client_context.hpp"

namespace duckdb {

class MSSQLStagedRows {
public:
	//! Allocated from the context, so a buffer that outgrows memory can spill
	//! like any other collection (an `Allocator &` cannot).
	MSSQLStagedRows(ClientContext &context, const vector<LogicalType> &types)
		: collection_(context, types), count_(0) {}

	void Append(DataChunk &chunk) {
		collection_.Append(chunk);
		count_ += chunk.size();
	}

	idx_t Count() const {
		return count_;
	}

	bool Empty() const {
		return count_ == 0;
	}

	//! Past the threshold: the caller stops staging and streams.
	bool Exceeds(idx_t threshold) const {
		return count_ > threshold;
	}

	//! Hand the staged rows to `sink`, chunk by chunk, in arrival order.
	template <class F>
	void Drain(F &&sink) {
		if (count_ == 0) {
			return;
		}
		ColumnDataScanState state;
		collection_.InitializeScan(state);
		DataChunk chunk;
		collection_.InitializeScanChunk(state, chunk);
		while (collection_.Scan(state, chunk)) {
			sink(chunk);
		}
	}

private:
	ColumnDataCollection collection_;
	idx_t count_;
};

}  // namespace duckdb
