#include "backup/backup_record_stager.hpp"

#include "backup/backup_collation.hpp"
#include "backup/backup_function_common.hpp"
#include "backup/backup_lob_map.hpp"
#include "codec/code_page/code_page.hpp"
#include "codec/staging/row_stager.hpp"
#include "codec/string_codec.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/string_vector.hpp"

#include <algorithm>
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
	if (c.system_type_id == XTYPE_VARCHAR || c.system_type_id == XTYPE_CHAR || c.system_type_id == XTYPE_TEXT) {
		// A UTF-8 collation's bytes are UTF-8 already (no table); every other page has one, double-byte
		// ones included (spec 091), which the code-page kernel decodes on the client: a backup has no
		// server to convert it, where the TDS path keeps the server's cast for them only for speed.
		const CollationRow *row = CollationById(static_cast<int32_t>(c.collation_id));
		if (row && row->code_page != 65001) {
			m.code_page = codec::code_page::FindCodePage(static_cast<int32_t>(row->code_page));
		}
	}
	return m;
}

// Where a value walked off row goes: straight into its column's staging, leaf by leaf, the value
// begun at its first byte (a NULL root has none), and its nodes kept when the row's map wants them.
class StagingSink : public LobSink {
public:
	explicit StagingSink(ColumnStaging &st_p) : st(st_p) {}
	ColumnStaging &st;
	bool begun = false;
	void Data(const uint8_t *data, size_t size) override {
		if (!begun) {
			st.BeginVar();
			begun = true;
		}
		std::memcpy(st.ExtendVar(static_cast<uint32_t>(size)), data, size);
	}
};

// Where a value written straight into an output vector goes: its string_t's bytes, never past the
// length its pointer (or root record) said.
class DirectSink : public LobSink {
public:
	DirectSink(char *data_p, uint64_t length_p, const std::string &what_p)
		: data(data_p), length(length_p), what(what_p) {}
	char *data;
	uint64_t length;
	const std::string &what;
	uint64_t at = 0;
	void Data(const uint8_t *bytes, size_t size) override {
		if (size > length - at) {
			throw FormatError("backup: " + what + " holds more bytes off row than its " + std::to_string(length));
		}
		std::memcpy(data + at, bytes, size);
		at += size;
	}
};

// Where a long text value goes (PR 3 S4): its fragments gathered into a block, each block decoded
// into the output vector at once -- by the batch kernels' own functions, so the text is theirs -- with
// a character the block's end would cut carried into the next one: the second half of a UTF-16
// surrogate pair, or a double-byte lead byte. Every byte is decoded once.
class StreamSink : public LobSink {
public:
	StreamSink(std::vector<uint8_t> &block_p, char *dst_p, const codec::code_page::tables::CodePageTable *page_p,
			   idx_t &replaced_p)
		: block(block_p), dst(dst_p), page(page_p), replaced(replaced_p) {}
	std::vector<uint8_t> &block;
	char *dst;
	const codec::code_page::tables::CodePageTable *page;  // nullptr: UTF-16
	idx_t &replaced;
	size_t used = 0;
	size_t written = 0;
	void Data(const uint8_t *bytes, size_t size) override {
		while (size > 0) {
			const size_t take = std::min(size, block.size() - used);
			std::memcpy(block.data() + used, bytes, take);
			used += take;
			bytes += take;
			size -= take;
			if (used == block.size()) {
				Flush(false);
			}
		}
	}
	//! Decodes what the block holds; on the last block (`last`) all of it, else up to a character's cut.
	//! Returns the bytes left undecoded (a UTF-16 value of an odd length leaves its last byte).
	size_t Flush(bool last) {
		size_t keep = 0;
		if (!page) {
			size_t units = used / 2;
			keep = used - units * 2;
			if (!last && units > 0) {
				const uint16_t unit = static_cast<uint16_t>(block[units * 2 - 2] | (block[units * 2 - 1] << 8));
				if (unit >= 0xD800 && unit <= 0xDBFF) {
					units--;  // a high surrogate: its pair's low half is in the next block
					keep += 2;
				}
			}
			written += codec::string::DecodeUtf16Block(block.data(), units, dst + written, replaced);
		} else if (last) {
			written += codec::code_page::DecodeToUtf8(*page, block.data(), used, dst + written);
		} else {
			size_t consumed = used;
			written += codec::code_page::DecodeToUtf8Block(*page, block.data(), used, dst + written, consumed);
			keep = used - consumed;
		}
		if (keep > 0) {
			std::memmove(block.data(), block.data() + used - keep, keep);
		}
		used = keep;
		return keep;
	}
};

// A walk whose bytes and nodes go nowhere: a root pointer's length and NULL, from its root record.
class NoSink : public LobSink {
public:
	void Data(const uint8_t *, size_t) override {}
};

// A column read as its stored bytes (ReadingOf's `raw`): a varbinary of its declared length, which the
// binary kernel publishes as they are.
tds::ColumnMetadata RawMetadataOf(const CatalogColumn &c) {
	tds::ColumnMetadata m{};
	m.name = c.name;
	m.type_id = tds::TDS_TYPE_BIGVARBINARY;
	m.max_length = c.max_length <= 0 ? 0xFFFF : static_cast<uint16_t>(c.max_length);
	m.flags = tds::COL_FLAG_NULLABLE;
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
		// text / ntext / image declare 16 bytes (their pointer), but text in row keeps up to 7,000 in the row.
		const bool legacy_lob =
			c.system_type_id == XTYPE_TEXT || c.system_type_id == XTYPE_NTEXT || c.system_type_id == XTYPE_IMAGE;
		col.declared_bytes = c.max_length < 0 || legacy_lob ? IN_ROW_BYTES : static_cast<uint32_t>(c.max_length);
		const ColumnReading reading = ReadingOf(c);
		if (!reading.readable) {
			throw InvalidInputException("backup: %s: %s", col.name, reading.note);
		}
		const LogicalType type = reading.type;
		col.meta = reading.raw ? RawMetadataOf(c) : MetadataOf(c);
		col.ops = codec::staging::ResolveColumnOps(col.meta, type);
		col.binary_kernel = col.ops.kernel == FinalizeKernel::Binary && col.meta.type_id != tds::TDS_TYPE_BIGCHAR;
		col.utf16_kernel = col.ops.kernel == FinalizeKernel::String && col.meta.type_id != tds::TDS_TYPE_NCHAR;
		col.code_page_kernel = col.ops.kernel == FinalizeKernel::CodePage && col.meta.code_page != nullptr &&
							   col.meta.type_id != tds::TDS_TYPE_BIGCHAR;
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
			} else if (col.ops.arm == AppendArm::P2StageString || col.ops.arm == AppendArm::PlpStageString ||
					   col.ops.arm == AppendArm::LobStageString) {
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
	prepared_.assign(columns_.size(), PreparedValue());
	for (size_t i = 0; i < columns_.size(); i++) {
		if (columns_[i].arm == StageArm::Utf16 || columns_[i].arm == StageArm::Bytes) {
			var_columns_.push_back(i);
		}
	}
	arena_.Configure(columns_.size());
	for (size_t i = 0; i < columns_.size(); i++) {
		arena_.Column(i).Configure(columns_[i].ops.kind, columns_[i].ops.stride, columns_[i].ops.max_value_bytes);
		// A value stored off row runs to 2^31 - 1 bytes (DQ-46: past the wire's 2047 MiB cap, on this path
		// only), within the offsets' 32 bits.
		arena_.Column(i).payload_cap = 0xFFFFFFFFull;
	}
	// The staging's bound (FixedStagingBytes, VarRowBytes): a direct value or a bit goes into its vector
	// and stages nothing; a fixed one a vector of strides; a variable one its offset and length per row
	// and a first buffer, and its stored bytes (+2 for UTF-16's delimiter), a row's in-row part at most.
	uint64_t declared = 0;
	uint64_t delimiters = 0;
	for (const auto &col : columns_) {
		if (col.arm == StageArm::Fixed || col.arm == StageArm::SwapHalves) {
			fixed_staging_ += static_cast<uint64_t>(col.ops.stride) * STANDARD_VECTOR_SIZE;
		} else if (col.arm == StageArm::Utf16 || col.arm == StageArm::Bytes) {
			fixed_staging_ +=
				STANDARD_VECTOR_SIZE * 2 * sizeof(uint64_t) + codec::staging::STAGING_INITIAL_PAYLOAD_BYTES;
			declared += col.declared_bytes;
			delimiters += col.arm == StageArm::Utf16 ? 2 : 0;
		}
	}
	var_row_ = std::min<uint64_t>(declared, IN_ROW_BYTES) + delimiters;
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
			throw FormatError("backup: " + col.name + " has no physical column in its partition's layout");
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
	// The map's columns: every variable column of the rowset, projected or not (DQ-46), by catalog id.
	map_columns_.clear();
	for (size_t k = 0; k < layout.columns.size(); k++) {
		const PhysicalColumn &p = layout.columns[k];
		if (p.offset < 0 && !p.Dropped() && !p.Uniquifier()) {
			map_columns_.push_back(MapColumn{k, p.rscolid, table_ + "." + p.name});
		}
	}
	std::sort(map_columns_.begin(), map_columns_.end(),
			  [](const MapColumn &a, const MapColumn &b) { return a.column_id < b.column_id; });
	layout_ = &layout;
}

void RecordStager::UseOffRow(const LobPageFetch *fetch, const LobUnits &units) {
	fetch_ = fetch;
	units_ = units;
}

void RecordStager::MapOffRow(bool on) {
	map_off_row_ = on;
}

const LobPageFetch &RecordStager::Fetch(const std::string &what) const {
	if (!fetch_) {
		throw InternalException("backup: %s is stored off row, and the stager was given no pages to read it from",
								what);
	}
	return *fetch_;
}

OffRowNeed RecordStager::PrepareRow(const RecordView &record) {
	if (!layout_) {
		throw InternalException("backup: RecordStager::PrepareRow with no layout");
	}
	OffRowNeed need;
	prepared_record_ = record.data;
	prepared_valid_ = true;
	// Only the columns that can hold a value stored off row are looked at; a row of none, with no map
	// wanted, costs nothing more (review of PR 3 S4: this runs for every row).
	if (var_columns_.empty() && !map_off_row_) {
		return need;
	}
	prepared_valid_ = false;
	const size_t var_count = RowVarCount(record, layout_->table);
	for (size_t i : var_columns_) {
		Column &c = columns_[i];
		PreparedValue &p = prepared_[i];
		p = PreparedValue();
		const ValueRef v = LocateValue(record, var_count, *layout_, c.layout_index);
		if (v.state != ValueState::OffRow) {
			continue;
		}
		p.off_row = true;
		p.pointer = ParseOffRowPointer(v.data, v.size, c.name);
		if (p.pointer.form == OffRowForm::RootPointer) {
			// Its length (and whether it is NULL) is its root record's: read now, so it is reserved
			// before it is assembled.
			NoSink none;
			const LobValue root =
				WalkLob(Fetch(c.name), p.pointer, units_, LobWalk::MapOnly, MAX_OFF_ROW_BYTES, c.name, none);
			p.length = root.length;
			p.is_null = root.is_null;
		} else {
			p.length = p.pointer.length;
		}
		if (p.is_null) {
			continue;
		}
		// A long value goes to its vector by the shortest way: its bytes (a BLOB, a UTF-8 text), or decoded
		// block by block into room for its longest UTF-8 (3 bytes a UTF-16 unit or a code-page byte); a short
		// one, and any other, is staged for its column's batch kernel.
		if (p.length >= DIRECT_VALUE_BYTES && (c.binary_kernel || c.utf16_kernel || c.code_page_kernel)) {
			p.direct = c.binary_kernel;
			p.stream = !c.binary_kernel;
			p.output = c.binary_kernel	? p.length
					   : c.utf16_kernel ? 3 * (p.length / 2)
										: codec::code_page::Utf8Bound(p.length);
			if (p.output > string_t::MAX_STRING_SIZE) {
				// Room for its longest UTF-8 passes DuckDB's longest string (a code page's 3 bytes a byte past
				// about 1.43 GB): staged instead, where the kernel sizes the output exactly (review of PR 3 S4).
				p.direct = p.stream = false;
				p.output = 0;
			}
		}
		if (p.direct || p.stream) {
			need.direct += p.output;
		} else {
			// AppendOffRow grows the buffer to exactly what the value needs (ReserveMore): it is in memory once.
			need.staged += p.length + (c.arm == StageArm::Utf16 ? 2 : 0);
		}
	}
	if (map_off_row_) {
		MapRow(record, var_count);
		need.map = row_lob_map_.size();
	}
	prepared_valid_ = true;
	return need;
}

std::string RecordStager::DescribePrepared() const {
	std::string out;
	for (size_t i : var_columns_) {
		const PreparedValue &p = prepared_[i];
		if (p.off_row && !p.is_null) {
			out += (out.empty() ? "" : ", ") + columns_[i].name + " (" +
				   StringUtil::BytesToHumanReadableString(p.length) + ")";
		}
	}
	return out;
}

void RecordStager::AppendDirect(size_t column, Column &c, ColumnStaging &st, const PreparedValue &p) {
	string_t value;
	try {
		value = StringVector::EmptyString(*targets_[column], p.length);
	} catch (OutOfMemoryException &e) {
		throw OutOfMemoryException("backup: %s holds a value of %s stored off row, which DuckDB cannot allocate: %s",
								   c.name, StringUtil::BytesToHumanReadableString(p.length), e.what());
	}
	DirectSink sink(value.GetDataWriteable(), p.length, c.name);
	WalkLob(Fetch(c.name), p.pointer, units_, LobWalk::Assemble, MAX_OFF_ROW_BYTES, c.name, sink);
	if (sink.at != p.length) {
		throw FormatError("backup: " + c.name + " holds " + std::to_string(sink.at) + " bytes off row, not its " +
						  std::to_string(p.length));
	}
	value.Finalize();
	st.AppendVar(nullptr, 0);  // a placeholder, set to the value once the kernel has run
	direct_values_.push_back(DirectValue{column, count_, value});
	// Its bytes are the chunk's too, in the vector's heap until the chunk is consumed: they count
	// toward Full(), so a chunk of large values closes on them (review of PR 3 S4).
	payload_bytes_ += p.length;
}

void RecordStager::AppendStream(size_t column, Column &c, ColumnStaging &st, const PreparedValue &p) {
	string_t value;
	try {
		value = StringVector::EmptyString(*targets_[column], p.output);
	} catch (OutOfMemoryException &e) {
		throw OutOfMemoryException(
			"backup: %s holds a value of %s stored off row, which DuckDB cannot allocate "
			"room to decode (%s): %s",
			c.name, StringUtil::BytesToHumanReadableString(p.length), StringUtil::BytesToHumanReadableString(p.output),
			e.what());
	}
	if (stream_block_.size() != STREAM_BLOCK_BYTES) {
		stream_block_.resize(STREAM_BLOCK_BYTES);
	}
	StreamSink sink(stream_block_, value.GetDataWriteable(), c.code_page_kernel ? c.meta.code_page : nullptr,
					st.replaced_units);
	WalkLob(Fetch(c.name), p.pointer, units_, LobWalk::Assemble, MAX_OFF_ROW_BYTES, c.name, sink);
	if (sink.Flush(true) != 0) {
		throw FormatError("backup: " + c.name + " holds " + std::to_string(p.length) +
						  " bytes off row, not a whole number of UTF-16 units");
	}
	value.SetSizeAndFinalize(static_cast<uint32_t>(sink.written), p.output);
	// A placeholder, set to the value once the kernel has run; a UTF-16 column's carries its delimiter,
	// which its kernel splits the column on (review of PR 3 S4: without it the next value lost a byte).
	if (c.arm == StageArm::Utf16) {
		st.BeginVar();
		st.FinishVarDelimited();
	} else {
		st.AppendVar(nullptr, 0);
	}
	direct_values_.push_back(DirectValue{column, count_, value});
	payload_bytes_ += p.output;
}

void RecordStager::AppendOffRow(Column &c, ColumnStaging &st, const PreparedValue &p) {
	if (p.is_null) {
		st.AppendNull();
		return;
	}
	// The buffer grown once, to exactly what the value needs, before its fragments are copied in; a large
	// one closes the chunk after its row (Full).
	st.ReserveMore(p.length + (c.arm == StageArm::Utf16 ? 2 : 0));
	if (p.length >= DIRECT_VALUE_BYTES) {
		large_staged_ = true;
	}
	StagingSink sink(st);
	const LobValue value =
		WalkLob(Fetch(c.name), p.pointer, units_, LobWalk::Assemble, MAX_OFF_ROW_BYTES, c.name, sink);
	if (value.is_null) {
		st.AppendNull();
		return;
	}
	if (!sink.begun) {
		st.BeginVar();	// an empty value
	}
	if (c.arm == StageArm::Utf16) {
		if (value.length & 1) {
			throw FormatError("backup: " + c.name + " holds " + std::to_string(value.length) +
							  " bytes off row, not a whole number of UTF-16 units");
		}
		st.FinishVarDelimited();
		payload_bytes_ += c.budget_weight * (value.length + 2);
		staged_bytes_ += value.length + 2;
		return;
	}
	st.FinishVar();
	payload_bytes_ += c.budget_weight * value.length;
	staged_bytes_ += value.length;
}

void RecordStager::MapRow(const RecordView &record, size_t var_count) {
	row_lob_map_.clear();
	std::vector<LobMapEntry> entries;
	for (const auto &mc : map_columns_) {
		const ValueRef v = LocateValue(record, var_count, *layout_, mc.layout_index);
		if (v.state != ValueState::OffRow) {
			continue;
		}
		const std::string &what = mc.name;
		LobMapEntry e;
		e.column_id = static_cast<uint32_t>(mc.column_id);
		const OffRowPointer pointer = ParseOffRowPointer(v.data, v.size, what);
		e.form = pointer.form;
		e.pointer.assign(reinterpret_cast<const char *>(v.data), v.size);
		LobNodeList nodes;
		const LobValue value = WalkLob(Fetch(what), pointer, units_, LobWalk::MapOnly, MAX_OFF_ROW_BYTES, what, nodes);
		e.is_null = value.is_null;
		e.length = static_cast<uint32_t>(value.length);
		e.nodes = std::move(nodes.nodes);
		entries.push_back(std::move(e));
	}
	if (!entries.empty()) {
		AppendLobMap(entries, table_, row_lob_map_);
	}
}

void RecordStager::BeginChunk(DataChunk &out) {
	if (out.ColumnCount() != columns_.size()) {
		throw InternalException("backup: RecordStager::BeginChunk with a chunk of %d columns for %d",
								static_cast<int>(out.ColumnCount()), static_cast<int>(columns_.size()));
	}
	std::vector<Vector *> targets;
	for (size_t i = 0; i < columns_.size(); i++) {
		targets.push_back(&out.data[i]);
	}
	BeginChunk(targets);
}

void RecordStager::BeginChunk(const std::vector<Vector *> &targets) {
	if (targets.size() != columns_.size()) {
		throw InternalException("backup: RecordStager::BeginChunk with %d vectors for %d columns",
								static_cast<int>(targets.size()), static_cast<int>(columns_.size()));
	}
	// The kernels and the direct writes resolved against Types(): a vector of another type (a plan whose
	// types are not this stager's) would be written at another width -- refused before any write.
	for (size_t i = 0; i < columns_.size(); i++) {
		if (targets[i]->GetType() != types_[i]) {
			throw InternalException("backup: RecordStager::BeginChunk: %s's vector is %s, the stager's type %s",
									columns_[i].name, targets[i]->GetType().ToString(), types_[i].ToString());
		}
	}
	targets_ = targets;
	for (size_t i = 0; i < columns_.size(); i++) {
		uint8_t *direct = columns_[i].ops.direct_write
							  ? reinterpret_cast<uint8_t *>(FlatVector::GetDataMutable(*targets_[i]))
							  : nullptr;
		arena_.Column(i).BeginChunk(direct);
	}
	count_ = 0;
	payload_bytes_ = 0;
	staged_bytes_ = 0;
	direct_values_.clear();
	prepared_valid_ = false;
	large_staged_ = false;
	spoiled_ = false;
	begun_ = true;
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
		staged_bytes_ += v.size + 2;
		return;
	case StageArm::Bytes:
		st.AppendVar(v.data, v.size);
		payload_bytes_ += c.budget_weight * v.size;
		staged_bytes_ += v.size;
		return;
	}
}

void RecordStager::Append(const RecordView &record) {
	if (!begun_ || spoiled_ || !layout_ || count_ == STANDARD_VECTOR_SIZE) {
		throw InternalException("backup: RecordStager::Append on a chunk that is %s", !begun_	 ? "not begun"
																					  : spoiled_ ? "spoiled by an error"
																					  : !layout_ ? "of no layout"
																								 : "full");
	}
	if (!prepared_valid_ || prepared_record_ != record.data) {
		PrepareRow(record);	 // a caller that reserves nothing for it (tests), or another record than prepared
	}
	prepared_valid_ = false;
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
		case ValueState::Inline:
			AppendValue(c, st, v);
			break;
		case ValueState::OffRow:
			if (c.arm != StageArm::Utf16 && c.arm != StageArm::Bytes) {
				throw FormatError("backup: " + c.name +
								  " is a fixed-width column whose value is marked as stored off row");
			}
			if (prepared_[i].direct) {
				AppendDirect(i, c, st, prepared_[i]);
			} else if (prepared_[i].stream) {
				AppendStream(i, c, st, prepared_[i]);
			} else {
				AppendOffRow(c, st, prepared_[i]);
			}
			break;
		case ValueState::Default:
			AppendValue(c, st, v);
			break;
		}
	}
	if (map_off_row_) {
		// The map is staged for the chunk too (the scan keeps it until the chunk is written): it counts
		// toward Full() like a value, so a chunk of deep trees' maps closes on its bytes (review of S3).
		payload_bytes_ += row_lob_map_.size();
		staged_bytes_ += row_lob_map_.size();
	}
	spoiled_ = false;
	count_++;
}

void RecordStager::FinalizeChunk(DataChunk &out) {
	FinalizeChunk();
	out.SetChildCardinality(count_);
}

void RecordStager::FinalizeChunk() {
	if (!begun_ || spoiled_) {
		throw InternalException("backup: RecordStager::FinalizeChunk on a chunk %s",
								begun_ ? "spoiled by an error" : "not begun");
	}
	begun_ = false;
	for (size_t i = 0; i < columns_.size(); i++) {
		const ColumnStaging &st = arena_.Column(i);
		codec::staging::FinalizeStagedColumn(columns_[i].ops.kernel, st, count_, columns_[i].meta, *targets_[i]);
		codec::staging::PublishStagedValidity(st, count_, *targets_[i]);
	}
	// The values written straight into their vectors, over their placeholders.
	for (const auto &d : direct_values_) {
		FlatVector::GetDataMutable<string_t>(*targets_[d.column])[d.row] = d.value;
	}
	direct_values_.clear();
	arena_.EndChunk();
	// A value stored off row may have grown a column's buffer far past what its next chunk needs; what
	// a task keeps between calls stays about one reservation (D17), so the excess goes now.
	for (size_t i = 0; i < columns_.size(); i++) {
		ColumnStaging &st = arena_.Column(i);
		if (!st.payload_bounded && st.buffer.capacity() > RETAINED_STAGING_BYTES) {
			// A first buffer kept, as Configure leaves one: a chunk of empty values still has storage.
			codec::staging::PayloadBuffer fresh;
			fresh.resize(codec::staging::STAGING_INITIAL_PAYLOAD_BYTES);
			st.buffer.swap(fresh);
		}
	}
}

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb
