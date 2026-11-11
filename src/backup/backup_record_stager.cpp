#include "backup/backup_record_stager.hpp"

#include "backup/backup_collation.hpp"
#include "backup/backup_function_common.hpp"
#include "codec/code_page/code_page.hpp"
#include "codec/staging/row_stager.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/vector/flat_vector.hpp"

#include <cstring>

namespace duckdb {
namespace mssql {
namespace backup {

using codec::staging::AppendArm;
using codec::staging::ColumnStaging;
using codec::staging::FinalizeKernel;
using codec::staging::StagingKind;

namespace {

// A column's TDS form as the codec resolves it: the system type (its xtype is the TDS type id but
// for bit, whose bare TDS type is 0x32, and timestamp, which travels as binary(8)), the declared
// length (MAX as the PLP marker), precision, scale and, for a single-byte text, its code page.
tds::ColumnMetadata MetadataOf(const CatalogColumn &c) {
	tds::ColumnMetadata m{};
	m.name = c.name;
	m.type_id = c.system_type_id;
	if (c.system_type_id == XTYPE_BIT) {
		m.type_id = tds::TDS_TYPE_BIT;
	} else if (c.system_type_id == XTYPE_TIMESTAMP) {
		m.type_id = tds::TDS_TYPE_BIGBINARY;
	}
	m.max_length = c.max_length < 0 ? 0xFFFF : static_cast<uint16_t>(c.max_length);
	m.precision = c.precision;
	m.scale = c.scale;
	m.flags = tds::COL_FLAG_NULLABLE;
	if (c.system_type_id == XTYPE_VARCHAR || c.system_type_id == XTYPE_CHAR) {
		// A UTF-8 collation's bytes are UTF-8 already (no table); every other page ColumnRefusal let
		// through is single-byte and has one.
		const CollationRow *row = CollationById(static_cast<int32_t>(c.collation_id));
		if (row && row->code_page != 65001) {
			m.code_page = codec::code_page::FindCodePage(static_cast<int32_t>(row->code_page));
		}
	}
	return m;
}

template <uint32_t STRIDE>
inline void Fixed(ColumnStaging &st, const uint8_t *p) {
	st.AppendFixed<STRIDE>(p);
}

// A fixed value staged at its column's stride, the copy sized at compile time (column_staging.hpp:
// a runtime-sized memcpy is a call into libc). The widths are the ones IsStageableFixedWidth admits
// and a fixed type of the reader has; RecordStager refuses a column of any other at construction.
void AppendFixed(ColumnStaging &st, const uint8_t *p) {
	switch (st.stride) {
	case 3:
		return Fixed<3>(st, p);
	case 4:
		return Fixed<4>(st, p);
	case 5:
		return Fixed<5>(st, p);
	case 6:
		return Fixed<6>(st, p);
	case 7:
		return Fixed<7>(st, p);
	case 8:
		return Fixed<8>(st, p);
	case 9:
		return Fixed<9>(st, p);
	case 10:
		return Fixed<10>(st, p);
	case 13:
		return Fixed<13>(st, p);
	case 16:
		return Fixed<16>(st, p);
	case 17:
		return Fixed<17>(st, p);
	default:
		throw InternalException("backup: a fixed column staged at stride %d", static_cast<int>(st.stride));
	}
}

bool StageableFixedStride(uint32_t stride) {
	switch (stride) {
	case 3:
	case 4:
	case 5:
	case 6:
	case 7:
	case 8:
	case 9:
	case 10:
	case 13:
	case 16:
	case 17:
		return true;
	default:
		return false;
	}
}

}  // namespace

RecordStager::RecordStager(const CatalogTable &table, const std::vector<int32_t> &column_ids)
	: table_(table.schema_name + "." + table.name) {
	for (int32_t id : column_ids) {
		const CatalogColumn *found = nullptr;
		for (const auto &c : table.columns) {
			if (c.column_id == id) {
				found = &c;
			}
		}
		if (!found) {
			throw InvalidInputException("backup: %s has no column %d", table_, id);
		}
		const CatalogColumn &c = *found;
		Column col;
		col.column_id = id;
		col.name = table_ + "." + c.name;
		const std::string refusal = ColumnRefusal(c);
		if (!refusal.empty()) {
			throw InvalidInputException("backup: %s: %s", col.name, refusal);
		}
		const LogicalType type = ColumnDuckDBType(c);
		col.meta = MetadataOf(c);
		col.ops = codec::staging::ResolveColumnOps(col.meta, type);
		if (col.ops.arm == AppendArm::Unsupported || col.ops.kernel == FinalizeKernel::Text) {
			// The TDS map and D6's would disagree about the column (the text fallback), or the codec
			// has no way in: both are this reader's gap, named, never a value rendered another way.
			throw InvalidInputException("backup: %s (%s) has no staged decode to %s", col.name, c.system_type_name,
										type.ToString());
		}
		switch (c.system_type_id) {
		case XTYPE_BIT:
			col.arm = StageArm::Bit;
			break;
		case XTYPE_DATETIME:
		case XTYPE_SMALLDATETIME:
		case XTYPE_MONEY:
			col.arm = StageArm::SwapHalves;
			break;
		default:
			if (codec::staging::IsDirectKind(col.ops.kind)) {
				col.arm = StageArm::Direct;
			} else if (col.ops.kind == StagingKind::Fixed) {
				col.arm = StageArm::Fixed;
			} else if (col.ops.arm == AppendArm::P2StageString || col.ops.arm == AppendArm::PlpStageString) {
				col.arm = StageArm::Utf16;
			} else {
				col.arm = StageArm::Bytes;
			}
		}
		// What each arm stages must be what the codec resolved: a direct value into a vector slot of
		// its width, a fixed one at a stride AppendFixed has (and for a swap, the 8 or 4 bytes it swaps).
		const bool direct = col.arm == StageArm::Direct || col.arm == StageArm::Bit;
		const bool fixed = col.arm == StageArm::Fixed || col.arm == StageArm::SwapHalves;
		if ((direct && !codec::staging::IsDirectKind(col.ops.kind)) ||
			(col.arm == StageArm::Bit && col.ops.stride != 1) ||
			(fixed && (col.ops.kind != StagingKind::Fixed || !StageableFixedStride(col.ops.stride))) ||
			(col.arm == StageArm::SwapHalves && col.ops.stride != 8 && col.ops.stride != 4)) {
			throw InvalidInputException("backup: %s (%s) is staged by the codec in a form this reader does not fill",
										col.name, c.system_type_name);
		}
		// The live path's budget (RowStager::Configure): its MAX columns, a code-page one twice.
		if (col.ops.arm >= AppendArm::PlpStageString && col.ops.arm <= AppendArm::LobStageBinary) {
			col.budget_weight = col.ops.kernel == FinalizeKernel::CodePage ? 2 : 1;
		}
		columns_.push_back(std::move(col));
		types_.push_back(type);
	}
	arena_.Configure(columns_.size());
	for (size_t i = 0; i < columns_.size(); i++) {
		arena_.Column(i).Configure(columns_[i].ops.kind, columns_[i].ops.stride, columns_[i].ops.max_value_bytes);
	}
}

void RecordStager::UseLayout(const RowsetLayout &layout) {
	// Resolved whole before any of it is kept: a refused layout leaves the stager on the one it had.
	std::vector<std::pair<size_t, uint8_t>> places;
	for (auto &col : columns_) {
		size_t k = 0;
		while (k < layout.columns.size() && (layout.columns[k].rscolid != col.column_id ||
											 layout.columns[k].Dropped() || layout.columns[k].Uniquifier())) {
			k++;
		}
		if (k == layout.columns.size()) {
			throw FormatError("backup: " + col.name + " has no physical column in rowset " +
							  std::to_string(layout.rowset_id));
		}
		const PhysicalColumn &p = layout.columns[k];
		const bool fixed_arm = col.arm == StageArm::Direct || col.arm == StageArm::Fixed ||
							   col.arm == StageArm::SwapHalves || col.arm == StageArm::Bit;
		// A fixed arm copies `stride` bytes from the record (a bit, one): the physical column must be a
		// fixed one of that width, and a default given for it the same width, so no value needs a test.
		const uint32_t width = col.arm == StageArm::Bit ? 1u : col.ops.stride;
		if (fixed_arm && (p.offset <= 0 || p.FixedLength(layout.table) != width)) {
			throw FormatError("backup: " + col.name + " is stored " +
							  (p.offset <= 0 ? std::string("as a variable column")
											 : std::to_string(p.FixedLength(layout.table)) + " bytes wide") +
							  ", where its type takes " + std::to_string(width) + " fixed bytes");
		}
		if (fixed_arm && p.has_default && p.default_bytes.size() != width) {
			throw FormatError("backup: " + col.name + "'s default is " + std::to_string(p.default_bytes.size()) +
							  " bytes, where its type takes " + std::to_string(width));
		}
		places.emplace_back(k, p.bit_position);
	}
	for (size_t i = 0; i < columns_.size(); i++) {
		columns_[i].layout_index = places[i].first;
		columns_[i].bit_position = places[i].second;
	}
	layout_ = &layout;
}

void RecordStager::BeginChunk(DataChunk &out) {
	for (size_t i = 0; i < columns_.size(); i++) {
		uint8_t *direct = columns_[i].ops.direct_write
							  ? reinterpret_cast<uint8_t *>(FlatVector::GetDataMutable(out.data[i]))
							  : nullptr;
		arena_.Column(i).BeginChunk(direct);
	}
	count_ = 0;
	payload_bytes_ = 0;
	spoiled_ = false;
}

void RecordStager::AppendValue(Column &c, ColumnStaging &st, const ValueRef &v) {
	switch (c.arm) {
	case StageArm::Direct:
		switch (st.stride) {
		case 1:
			st.AppendDirect<1>(v.data);
			break;
		case 2:
			st.AppendDirect<2>(v.data);
			break;
		case 4:
			st.AppendDirect<4>(v.data);
			break;
		default:
			st.AppendDirect<8>(v.data);
		}
		return;
	case StageArm::Bit: {
		// A default is a bit value of its own (a sql_variant bit, one byte 0 / 1), not a shared byte.
		const uint8_t bit = v.state == ValueState::Default ? (v.data[0] != 0) : ((v.data[0] >> c.bit_position) & 1);
		st.AppendDirect<1>(&bit);
		return;
	}
	case StageArm::Fixed:
		AppendFixed(st, v.data);
		return;
	case StageArm::SwapHalves: {
		uint8_t wire[8];
		const uint32_t half = st.stride / 2;
		std::memcpy(wire, v.data + half, half);
		std::memcpy(wire + half, v.data, half);
		AppendFixed(st, wire);
		return;
	}
	case StageArm::Utf16:
		if (v.size & 1) {
			throw FormatError("backup: " + c.name + " holds " + std::to_string(v.size) +
							  " bytes, not a whole number of UTF-16 units");
		}
		st.AppendVarDelimited(v.data, v.size);
		payload_bytes_ += c.budget_weight * (v.size + 2);
		return;
	case StageArm::Bytes:
		st.AppendVar(v.data, v.size);
		payload_bytes_ += c.budget_weight * v.size;
		return;
	}
}

void RecordStager::Append(const RecordView &record) {
	if (spoiled_ || !layout_ || count_ == STANDARD_VECTOR_SIZE) {
		throw InternalException("backup: RecordStager::Append on a chunk that is %s", spoiled_	 ? "spoiled by an error"
																					  : !layout_ ? "of no layout"
																								 : "full");
	}
	// Set until the row is whole: a throw below leaves a chunk with part of a row.
	spoiled_ = true;
	const size_t var_count = RowVarCount(record, layout_->table);
	for (size_t i = 0; i < columns_.size(); i++) {
		Column &c = columns_[i];
		ColumnStaging &st = arena_.Column(i);
		const ValueRef v = LocateValue(record, var_count, *layout_, c.layout_index);
		switch (v.state) {
		case ValueState::Null:
			st.AppendNull();
			break;
		case ValueState::OffRow:
			throw FormatError("backup: " + c.name +
							  " holds a value stored off row (row-overflow or a LOB), which the reader reads from spec "
							  "085 PR 3; leave the column out of the projection to read the others");
		case ValueState::Inline:
		case ValueState::Default:
			AppendValue(c, st, v);
			break;
		}
	}
	spoiled_ = false;
	count_++;
}

void RecordStager::FinalizeChunk(DataChunk &out) {
	if (spoiled_) {
		throw InternalException("backup: RecordStager::FinalizeChunk on a chunk spoiled by an error");
	}
	for (size_t i = 0; i < columns_.size(); i++) {
		const ColumnStaging &st = arena_.Column(i);
		codec::staging::FinalizeStagedColumn(columns_[i].ops.kernel, st, count_, columns_[i].meta, out.data[i]);
		codec::staging::PublishStagedValidity(st, count_, out.data[i]);
	}
	out.SetChildCardinality(count_);
	arena_.EndChunk();
}

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb
