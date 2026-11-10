#include "backup/backup_system_tables.hpp"

#include "backup/backup_iam.hpp"

#include <algorithm>
#include <set>

namespace duckdb {
namespace mssql {
namespace backup {

namespace {

// The bootstrap tables' allocation units: m_indexId << 48 | m_objId << 16 of their pages' headers
// (sysallocunits' is its first page's; measured 2017-2025, and E1's BOOT_TABLES).
constexpr uint64_t AU_SYSROWSETS = 5ull << 16;
constexpr uint64_t AU_SYSSCHOBJS = (1ull << 48) | (34ull << 16);
constexpr uint64_t AU_SYSCOLPARS = (1ull << 48) | (41ull << 16);
constexpr uint64_t AU_SYSRSCOLS = 3ull << 16;

// A forwarded record's back pointer: u16 0x0400, then the original RID (u32 page, u16 file, u16 slot).
constexpr uint16_t BACK_POINTER_TAG = 0x0400;
constexpr uint16_t BACK_POINTER_BYTES = 10;

// A physical column is numbered by a 16-bit null bit.
constexpr size_t MAX_PHYSICAL_COLUMNS = 65535;

PhysicalColumn Literal(const char *name, uint8_t xtype, int16_t offset, uint16_t null_bit, uint16_t length = 0) {
	PhysicalColumn c;
	c.name = name;
	c.xtype = xtype;
	c.offset = offset;
	c.null_bit = null_bit;
	c.length = length;
	c.status = 0x80;
	return c;
}

RowsetLayout LiteralLayout(const char *table, std::vector<PhysicalColumn> columns) {
	RowsetLayout l;
	l.table = table;
	l.columns = std::move(columns);
	return l;
}

// spec 085 § Rules, catalog: the fields the reader takes, at their record offsets.
const std::map<std::string, RowsetLayout> &Literals() {
	static const std::map<std::string, RowsetLayout> literals = {
		{"sysallocunits",
		 LiteralLayout("sysallocunits",
					   {Literal("auid", XTYPE_BIGINT, 4, 1), Literal("type", XTYPE_TINYINT, 12, 2),
						Literal("ownerid", XTYPE_BIGINT, 13, 3), Literal("status", XTYPE_INT, 21, 4),
						Literal("fgid", XTYPE_SMALLINT, 25, 5), Literal("pgfirst", XTYPE_BINARY, 27, 6, 6),
						Literal("pgroot", XTYPE_BINARY, 33, 7, 6), Literal("pgfirstiam", XTYPE_BINARY, 39, 8, 6)})},
		{"sysrowsets",
		 LiteralLayout("sysrowsets",
					   {Literal("rowsetid", XTYPE_BIGINT, 4, 1), Literal("ownertype", XTYPE_TINYINT, 12, 2),
						Literal("idmajor", XTYPE_INT, 13, 3), Literal("idminor", XTYPE_INT, 17, 4),
						Literal("numpart", XTYPE_INT, 21, 5), Literal("status", XTYPE_INT, 25, 6),
						Literal("rcrows", XTYPE_BIGINT, 31, 8), Literal("cmprlevel", XTYPE_TINYINT, 39, 9)})},
		{"sysschobjs",
		 LiteralLayout("sysschobjs",
					   {Literal("id", XTYPE_INT, 4, 1), Literal("name", XTYPE_NVARCHAR, -1, 2, 256),
						Literal("nsid", XTYPE_INT, 8, 3), Literal("status", XTYPE_INT, 13, 5),
						Literal("type", XTYPE_CHAR, 17, 6, 2), Literal("pid", XTYPE_INT, 19, 7),
						Literal("created", XTYPE_DATETIME, 28, 10), Literal("modified", XTYPE_DATETIME, 36, 11)})},
		{"syscolpars",
		 LiteralLayout("syscolpars", {Literal("id", XTYPE_INT, 4, 1), Literal("number", XTYPE_SMALLINT, 8, 2),
									  Literal("colid", XTYPE_INT, 10, 3), Literal("name", XTYPE_NVARCHAR, -1, 4, 256),
									  Literal("xtype", XTYPE_TINYINT, 14, 5), Literal("utype", XTYPE_INT, 15, 6),
									  Literal("length", XTYPE_SMALLINT, 19, 7), Literal("prec", XTYPE_TINYINT, 21, 8),
									  Literal("scale", XTYPE_TINYINT, 22, 9), Literal("collationid", XTYPE_INT, 23, 10),
									  Literal("status", XTYPE_INT, 27, 11)})},
		{"sysrscols",
		 LiteralLayout("sysrscols",
					   {Literal("rsid", XTYPE_BIGINT, 4, 1), Literal("rscolid", XTYPE_INT, 12, 2),
						Literal("hbcolid", XTYPE_INT, 16, 3), Literal("ti", XTYPE_INT, 28, 5),
						Literal("cid", XTYPE_INT, 32, 6), Literal("ordkey", XTYPE_SMALLINT, 36, 7),
						Literal("status", XTYPE_INT, 40, 9), Literal("offset", XTYPE_INT, 44, 10),
						Literal("nullbit", XTYPE_INT, 48, 11), Literal("bitpos", XTYPE_SMALLINT, 52, 12)})},
	};
	return literals;
}

const char *const BOOTSTRAP_ORDER[] = {"sysallocunits", "sysrowsets", "sysschobjs", "syscolpars", "sysrscols"};

std::string XtypeName(uint8_t xtype) {
	return "xtype " + std::to_string(xtype);
}

std::string ColumnName(const RowsetLayout &layout, size_t column) {
	return layout.table + "." + layout.columns[column].name;
}

uint16_t SlotOffset(const Page &page, uint16_t slot) {
	const size_t at = PAGE_SIZE - 2 - 2 * static_cast<size_t>(slot);
	return static_cast<uint16_t>(page.bytes[at] | (page.bytes[at + 1] << 8));
}

// The data records of one page through `layout`, each handed to `row`.
void PageRows(const Page &page, const RowsetLayout &layout, const std::function<void(const SystemRow &)> &row) {
	const std::string what = layout.table + " page " + page.header.page_id.ToString();
	if (page.bytes.size() != PAGE_SIZE) {
		throw FormatError("backup: " + what + " holds " + std::to_string(page.bytes.size()) + " bytes, not a page's " +
						  std::to_string(PAGE_SIZE));
	}
	if (static_cast<size_t>(page.header.slot_count) * 2 + PAGE_HEADER_SIZE > PAGE_SIZE) {
		throw FormatError("backup: " + what + " has " + std::to_string(page.header.slot_count) +
						  " slots, more than an 8 KB page holds");
	}
	std::vector<uint16_t> offsets;
	offsets.reserve(page.header.slot_count);
	for (uint16_t s = 0; s < page.header.slot_count; s++) {
		const uint16_t offset = SlotOffset(page, s);
		if (offset >= PAGE_HEADER_SIZE) {
			offsets.push_back(offset);
		}
	}
	// Two slots at one record would hand it out twice (and a crafted page could alias every slot).
	std::sort(offsets.begin(), offsets.end());
	if (std::adjacent_find(offsets.begin(), offsets.end()) != offsets.end()) {
		throw FormatError("backup: " + what + " has two slots pointing to one record");
	}
	// Records lie apart: a FixedVar record ends at or before the next record's start (its variable
	// columns are bounded by the slot array alone otherwise, so one could take the next record's bytes
	// as its own value; review of #549).
	for (size_t i = 0; i + 1 < offsets.size(); i++) {
		const RecordView record =
			ReadRecord(page.bytes.data(), PAGE_SIZE, PAGE_SIZE - 2u * page.header.slot_count, offsets[i], what.c_str());
		const uint32_t end = static_cast<uint32_t>(offsets[i]) + record.End();
		if (record.End() != 0 && end > offsets[i + 1]) {
			throw FormatError("backup: " + what + " has a record at offset " + std::to_string(offsets[i]) +
							  " that runs past the next record's start (" + std::to_string(offsets[i + 1]) + ")");
		}
	}
	SystemRow out;
	for (uint16_t s = 0; s < page.header.slot_count; s++) {
		if (SlotOffset(page, s) < PAGE_HEADER_SIZE) {
			continue;  // a freed slot
		}
		const RecordView record = ReadSlot(page.bytes.data(), PAGE_SIZE, page.header.slot_count, s, what.c_str());
		if (record.Type() != 0 && record.Type() != 1) {
			continue;  // ghosts, stubs, index and LOB records
		}
		out.values = DecodeRecord(record, layout);
		out.page = page.header.page_id;
		out.slot = s;
		row(out);
	}
}

// The live level-0 data pages of `unit`'s IAM chain, each handed to `page_fn`; `table` names it.
void ForEachDataPage(const PageStore &store, const SysAllocUnit &unit, const std::string &table,
					 const std::function<void(const Page &)> &page_fn) {
	Page page;
	for (const PagePointer &at : UnitPages(store, unit)) {
		if (ReadUnitDataPage(store, unit, at, table, page)) {
			page_fn(page);
		}
	}
}

// The value of a NOT NULL bootstrap field.
const SystemValue &Required(const RowsetLayout &layout, const SystemRow &row, size_t column) {
	const SystemValue &v = row.values[column];
	if (v.is_null) {
		throw FormatError("backup: " + ColumnName(layout, column) + " is NULL in the row at " + row.page.ToString() +
						  " slot " + std::to_string(row.slot) + ", a column the server never leaves NULL");
	}
	return v;
}

int64_t RequiredInt(const RowsetLayout &layout, const SystemRow &row, size_t column) {
	return ValueInt(layout, column, Required(layout, row, column));
}

void RequireSize(const RowsetLayout &layout, size_t column, const SystemValue &value, size_t size) {
	if (value.bytes.size() != size) {
		throw FormatError("backup: " + ColumnName(layout, column) + " holds " + std::to_string(value.bytes.size()) +
						  " bytes, where its type takes " + std::to_string(size));
	}
}

}  // namespace

uint16_t PhysicalColumn::FixedLength(const std::string &table) const {
	// A time part's bytes by its scale (fractional digits 0-7), the server's storage sizes.
	auto time_bytes = [&]() -> uint16_t {
		if (scale > 7) {
			throw FormatError("backup: " + table + "." + name + " is a " + XtypeName(xtype) + " of scale " +
							  std::to_string(scale) + ", past 7");
		}
		return scale <= 2 ? 3 : scale <= 4 ? 4 : 5;
	};
	switch (xtype) {
	case XTYPE_TINYINT:
	case XTYPE_BIT:
		return 1;
	case XTYPE_SMALLINT:
		return 2;
	case XTYPE_DATE:
		return 3;
	case XTYPE_INT:
	case XTYPE_SMALLDATETIME:
	case XTYPE_REAL:
	case XTYPE_SMALLMONEY:
		return 4;
	case XTYPE_BIGINT:
	case XTYPE_DATETIME:
	case XTYPE_FLOAT:
	case XTYPE_MONEY:
	case XTYPE_TIMESTAMP:
		return 8;
	case XTYPE_UNIQUEIDENTIFIER:
		return 16;
	case XTYPE_TIME:
		return time_bytes();
	case XTYPE_DATETIME2:
		return static_cast<uint16_t>(time_bytes() + 3);
	case XTYPE_DATETIMEOFFSET:
		return static_cast<uint16_t>(time_bytes() + 5);
	case XTYPE_DECIMAL:
	case XTYPE_NUMERIC:
		// A sign byte, then the mantissa in 4, 8, 12 or 16 bytes by precision.
		if (precision < 1 || precision > 38 || scale > precision) {
			throw FormatError("backup: " + table + "." + name + " is a " + XtypeName(xtype) + " of precision " +
							  std::to_string(precision) + " and scale " + std::to_string(scale));
		}
		return precision <= 9 ? 5 : precision <= 19 ? 9 : precision <= 28 ? 13 : 17;
	case XTYPE_BINARY:
	case XTYPE_CHAR:
	case XTYPE_NCHAR:
		return length;
	default:
		throw FormatError("backup: " + table + "." + name + " is a fixed column of " + XtypeName(xtype) +
						  ", a type the record decoder does not read");
	}
}

void SetTypeInfo(PhysicalColumn &column, uint32_t ti) {
	column.xtype = static_cast<uint8_t>(ti & 0xFF);
	column.length = 0;
	column.precision = 0;
	column.scale = 0;
	switch (column.xtype) {
	case XTYPE_DECIMAL:
	case XTYPE_NUMERIC:
		column.precision = static_cast<uint8_t>((ti >> 8) & 0xFF);
		column.scale = static_cast<uint8_t>((ti >> 16) & 0xFF);
		break;
	case XTYPE_TIME:
	case XTYPE_DATETIME2:
	case XTYPE_DATETIMEOFFSET:
		column.scale = static_cast<uint8_t>((ti >> 8) & 0xFF);
		break;
	default:
		column.length = static_cast<uint16_t>((ti >> 8) & 0xFFFF);
	}
}

const PhysicalColumn *RowsetLayout::Find(const std::string &name) const {
	for (const auto &c : columns) {
		if (c.name == name) {
			return &c;
		}
	}
	return nullptr;
}

size_t RowsetLayout::IndexOf(const std::string &name) const {
	for (size_t i = 0; i < columns.size(); i++) {
		if (columns[i].name == name) {
			return i;
		}
	}
	throw FormatError("backup: " + table + " has no column " + name);
}

size_t RowVarCount(const RecordView &record, const std::string &table) {
	const size_t var_count = record.HasVariableColumns() ? record.var_count : 0;
	if (record.Type() != RECORD_FORWARDED) {
		return var_count;
	}
	// A forwarded record: its last variable column is the back pointer (spec 085 § Rules, records).
	const size_t last = var_count - 1;
	if (var_count == 0 || !record.VarComplex(last) ||
		record.VarEnd(last) - record.VarStart(last) != BACK_POINTER_BYTES ||
		record.Var(last, "a back pointer").U16(0) != BACK_POINTER_TAG) {
		throw FormatError("backup: a forwarded record of " + table + " at offset " + std::to_string(record.offset) +
						  " has no back pointer as its last variable column");
	}
	return last;
}

RowLocation ForwardedFrom(const RecordView &record, const std::string &table) {
	const size_t last = RowVarCount(record, table);	 // checks the back pointer
	if (record.Type() != RECORD_FORWARDED) {
		throw FormatError("backup: the record of " + table + " at offset " + std::to_string(record.offset) +
						  " is of type " + std::to_string(record.Type()) + ", not a forwarded record");
	}
	const ByteReader b = record.Var(last, "a back pointer");
	RowLocation at;
	at.page.page = b.U32(2);
	at.page.file = b.U16(6);
	at.slot = b.U16(8);
	return at;
}

ValueRef LocateValue(const RecordView &record, size_t var_count, const RowsetLayout &layout, size_t k) {
	const PhysicalColumn &c = layout.columns[k];
	ValueRef v;
	if (c.Dropped()) {
		return v;
	}
	if (record.HasNullBitmap() && c.null_bit > record.column_count) {
		// Added after the record was written (a metadata-only ADD COLUMN): its default, else NULL.
		if (c.has_default) {
			v.state = ValueState::Default;
			v.data = reinterpret_cast<const uint8_t *>(c.default_bytes.data());
			v.size = static_cast<uint32_t>(c.default_bytes.size());
		} else if (c.HasDefaultStatus()) {
			throw FormatError("backup: " + layout.table + "." + c.name +
							  " was added after a record was written, with a default the reader was not given");
		}
		return v;
	}
	if (record.HasNullBitmap() && c.null_bit >= 1 && record.IsNull(c.null_bit - 1)) {
		return v;
	}
	if (c.offset > 0) {
		const uint16_t length = c.FixedLength(layout.table);
		if (c.offset < 4) {
			throw FormatError("backup: " + layout.table + "." + c.name + " is described at offset " +
							  std::to_string(c.offset) + ", inside the record's 4-byte header");
		}
		if (c.offset + length > record.fixed_end) {
			throw FormatError("backup: " + layout.table + "." + c.name + " (" + std::to_string(length) +
							  " bytes at offset " + std::to_string(c.offset) +
							  ") lies past the fixed part of the record at offset " + std::to_string(record.offset) +
							  ", which ends at " + std::to_string(record.fixed_end));
		}
		if (c.xtype == XTYPE_BIT && c.bit_position > 7) {
			throw FormatError("backup: " + layout.table + "." + c.name + " has bit position " +
							  std::to_string(c.bit_position));
		}
		v.state = ValueState::Inline;
		v.data = record.data + c.offset;
		v.size = length;
		return v;
	}
	if (c.offset < 0) {
		const size_t i = static_cast<size_t>(-static_cast<int32_t>(c.offset)) - 1;
		v.state = ValueState::Inline;
		if (i >= var_count) {
			v.data = record.data;  // past the record's variable count: empty (spec 085 § Rules, records)
			return v;
		}
		const ByteReader bytes = record.Var(i, c.name.c_str());
		v.data = bytes.Data();
		v.size = static_cast<uint32_t>(bytes.Size());
		if (record.VarComplex(i)) {
			v.state = ValueState::OffRow;
		}
		return v;
	}
	throw FormatError("backup: " + layout.table + "." + c.name + " has neither a fixed offset nor a variable index");
}

std::vector<SystemValue> DecodeRecord(const RecordView &record, const RowsetLayout &layout) {
	std::vector<SystemValue> values(layout.columns.size());
	const size_t var_count = RowVarCount(record, layout.table);
	for (size_t k = 0; k < layout.columns.size(); k++) {
		const PhysicalColumn &c = layout.columns[k];
		if (c.Uniquifier()) {
			continue;
		}
		const ValueRef ref = LocateValue(record, var_count, layout, k);
		SystemValue &v = values[k];
		if (ref.state == ValueState::Null) {
			continue;
		}
		v.is_null = false;
		v.off_row = ref.state == ValueState::OffRow;
		if (c.xtype == XTYPE_BIT && c.offset > 0 && ref.state == ValueState::Inline) {
			v.bytes.assign(1, static_cast<char>((ref.data[0] >> c.bit_position) & 1));
		} else {
			v.bytes.assign(reinterpret_cast<const char *>(ref.data), ref.size);
		}
	}
	return values;
}

int64_t ValueInt(const RowsetLayout &layout, size_t column, const SystemValue &v, int64_t null_value) {
	if (v.is_null) {
		return null_value;
	}
	if (v.off_row) {
		throw FormatError("backup: " + ColumnName(layout, column) + " is stored off-row");
	}
	const PhysicalColumn &c = layout.columns[column];
	const uint8_t *b = reinterpret_cast<const uint8_t *>(v.bytes.data());
	ByteReader r(b, v.bytes.size(), c.name.c_str());
	switch (c.xtype) {
	case XTYPE_TINYINT:
	case XTYPE_BIT:
		RequireSize(layout, column, v, 1);
		return r.U8(0);
	case XTYPE_SMALLINT:
		RequireSize(layout, column, v, 2);
		return r.I16(0);
	case XTYPE_INT:
		RequireSize(layout, column, v, 4);
		return r.I32(0);
	case XTYPE_BIGINT:
		RequireSize(layout, column, v, 8);
		return r.I64(0);
	default:
		throw FormatError("backup: " + ColumnName(layout, column) + " is " + XtypeName(c.xtype) + ", not an integer");
	}
}

std::string ValueText(const RowsetLayout &layout, size_t column, const SystemValue &v) {
	const PhysicalColumn &c = layout.columns[column];
	if (v.is_null) {
		return std::string();
	}
	if (v.off_row) {
		throw FormatError("backup: " + ColumnName(layout, column) + " is stored off-row");
	}
	switch (c.xtype) {
	case XTYPE_NVARCHAR:
	case XTYPE_NCHAR:
		if (v.bytes.size() % 2) {
			throw FormatError("backup: " + ColumnName(layout, column) + " holds an odd number of bytes of UTF-16");
		}
		return Utf16LeToUtf8(reinterpret_cast<const uint8_t *>(v.bytes.data()), v.bytes.size(), false);
	case XTYPE_VARCHAR:
	case XTYPE_CHAR:
		return v.bytes;
	default:
		throw FormatError("backup: " + ColumnName(layout, column) + " is " + XtypeName(c.xtype) + ", not a string");
	}
}

PagePointer ValuePointer(const RowsetLayout &layout, size_t column, const SystemValue &v) {
	const PhysicalColumn &c = layout.columns[column];
	if (v.is_null) {
		return PagePointer();
	}
	if (v.off_row) {
		throw FormatError("backup: " + ColumnName(layout, column) + " is stored off-row");
	}
	if (c.xtype != XTYPE_BINARY) {
		throw FormatError("backup: " + ColumnName(layout, column) + " is " + XtypeName(c.xtype) +
						  ", not a page pointer");
	}
	RequireSize(layout, column, v, 6);
	ByteReader r(reinterpret_cast<const uint8_t *>(v.bytes.data()), 6, c.name.c_str());
	return ReadPagePointer(r, 0);
}

void BootstrapTables::BuildIndexes() {
	au_index_.clear();
	object_index_.clear();
	system_object_index_.clear();
	rowsets_by_object_.clear();
	column_index_.clear();
	rscol_order_.clear();
	in_row_index_.clear();
	units_by_owner_.clear();
	for (size_t i = 0; i < allocation_units.size(); i++) {
		const SysAllocUnit &a = allocation_units[i];
		if (!au_index_.emplace(a.auid, i).second) {
			throw FormatError("backup: sysallocunits lists allocation unit " + std::to_string(a.auid) + " twice");
		}
		units_by_owner_[a.owner_id].push_back(i);
		if (a.type == 1 && !in_row_index_.emplace(a.owner_id, i).second) {
			throw FormatError("backup: sysallocunits gives rowset " + std::to_string(a.owner_id) +
							  " two in-row allocation units");
		}
	}
	for (size_t i = 0; i < objects.size(); i++) {
		if (!object_index_.emplace(objects[i].id, i).second) {
			throw FormatError("backup: sysschobjs lists object " + std::to_string(objects[i].id) + " twice");
		}
		if (objects[i].type == "S " && !system_object_index_.emplace(objects[i].name, i).second) {
			throw FormatError("backup: sysschobjs lists system table " + objects[i].name + " twice");
		}
	}
	std::set<uint64_t> rowset_ids;
	for (size_t i = 0; i < rowsets.size(); i++) {
		if (!rowset_ids.insert(rowsets[i].rowset_id).second) {
			throw FormatError("backup: sysrowsets lists rowset " + std::to_string(rowsets[i].rowset_id) + " twice");
		}
		rowsets_by_object_[rowsets[i].object_id].push_back(i);
	}
	for (size_t i = 0; i < columns.size(); i++) {
		const SysColumn &c = columns[i];
		if (c.number == 0 && !column_index_.emplace(std::make_pair(c.object_id, c.column_id), i).second) {
			throw FormatError("backup: syscolpars lists column " + std::to_string(c.column_id) + " of object " +
							  std::to_string(c.object_id) + " twice");
		}
	}
	rscol_order_.resize(rscols.size());
	for (size_t i = 0; i < rscols.size(); i++) {
		rscol_order_[i] = i;
	}
	const std::vector<SysRsCol> &rc = rscols;
	std::sort(rscol_order_.begin(), rscol_order_.end(), [&rc](size_t a, size_t b) {
		return rc[a].rowset_id != rc[b].rowset_id ? rc[a].rowset_id < rc[b].rowset_id : rc[a].rscolid < rc[b].rscolid;
	});
	for (size_t i = 1; i < rscol_order_.size(); i++) {
		const SysRsCol &a = rc[rscol_order_[i - 1]], &b = rc[rscol_order_[i]];
		if (a.rowset_id == b.rowset_id && a.rscolid == b.rscolid) {
			throw FormatError("backup: sysrscols lists physical column " + std::to_string(b.rscolid) + " of rowset " +
							  std::to_string(b.rowset_id) + " twice");
		}
	}
}

const SysAllocUnit *BootstrapTables::AllocationUnit(uint64_t auid) const {
	auto it = au_index_.find(auid);
	return it == au_index_.end() ? nullptr : &allocation_units[it->second];
}

const SysObject *BootstrapTables::Object(int32_t id) const {
	auto it = object_index_.find(id);
	return it == object_index_.end() ? nullptr : &objects[it->second];
}

const SysObject *BootstrapTables::SystemObject(const std::string &name) const {
	auto it = system_object_index_.find(name);
	return it == system_object_index_.end() ? nullptr : &objects[it->second];
}

const SysAllocUnit *BootstrapTables::FindInRowUnit(uint64_t rowset_id) const {
	auto it = in_row_index_.find(rowset_id);
	return it == in_row_index_.end() ? nullptr : &allocation_units[it->second];
}

const SysAllocUnit *BootstrapTables::FindLobUnit(uint64_t rowset_id) const {
	auto it = units_by_owner_.find(rowset_id);
	if (it == units_by_owner_.end()) {
		return nullptr;
	}
	for (size_t i : it->second) {
		if (allocation_units[i].type == 2) {
			return &allocation_units[i];
		}
	}
	return nullptr;
}

std::vector<const SysRowset *> BootstrapTables::RowsetsOf(int32_t object_id) const {
	std::vector<const SysRowset *> out;
	auto it = rowsets_by_object_.find(object_id);
	if (it != rowsets_by_object_.end()) {
		for (size_t i : it->second) {
			out.push_back(&rowsets[i]);
		}
	}
	return out;
}

std::vector<const SysAllocUnit *> BootstrapTables::UnitsOf(uint64_t rowset_id) const {
	std::vector<const SysAllocUnit *> out;
	auto it = units_by_owner_.find(rowset_id);
	if (it != units_by_owner_.end()) {
		for (size_t i : it->second) {
			out.push_back(&allocation_units[i]);
		}
	}
	return out;
}

const SysAllocUnit &BootstrapTables::InRowUnit(uint64_t rowset_id, const std::string &table) const {
	const SysAllocUnit *unit = FindInRowUnit(rowset_id);
	if (!unit) {
		throw FormatError("backup: " + table + "'s rowset " + std::to_string(rowset_id) +
						  " has no in-row allocation unit in sysallocunits");
	}
	return *unit;
}

RowsetLayout BootstrapTables::BaseLayout(int32_t object_id) const {
	const SysObject *object = Object(object_id);
	const std::string name = object ? object->name : "object " + std::to_string(object_id);
	size_t base_rowsets = 0;
	std::vector<const SysRowset *> live;
	auto found = rowsets_by_object_.find(object_id);
	if (found != rowsets_by_object_.end()) {
		for (size_t i : found->second) {
			const SysRowset &r = rowsets[i];
			if (r.index_id != 0 && r.index_id != 1) {
				continue;
			}
			base_rowsets++;
			if (FindInRowUnit(r.rowset_id)) {
				live.push_back(&r);	 // one with no in-row unit is a deferred drop
			}
		}
	}
	if (live.empty()) {
		throw FormatError(
			"backup: " + name + " has no live base rowset (index 0 or 1) in sysrowsets" +
			(base_rowsets ? " (" + std::to_string(base_rowsets) + " with no in-row allocation unit)" : std::string()));
	}
	if (live.size() > 1) {
		throw FormatError(
			"backup: " + name + " has " + std::to_string(live.size()) +
			" live base rowsets: a system table has one (a partitioned or columnstore one is not read here)");
	}
	return LayoutOf(object_id, *live[0]);
}

RowsetLayout BootstrapTables::LayoutOf(int32_t object_id, const SysRowset &base) const {
	const SysObject *object = Object(object_id);
	const std::string name = object ? object->name : "object " + std::to_string(object_id);
	RowsetLayout layout;
	layout.table = name;
	layout.rowset_id = base.rowset_id;
	// rscol_order_ is sorted by (rowset, rscolid): the rowset's columns are one range of it.
	const std::vector<SysRsCol> &rc = rscols;
	auto first = std::lower_bound(rscol_order_.begin(), rscol_order_.end(), base.rowset_id,
								  [&rc](size_t i, uint64_t rowset) { return rc[i].rowset_id < rowset; });
	for (auto it = first; it != rscol_order_.end() && rc[*it].rowset_id == base.rowset_id; ++it) {
		const SysRsCol &r = rc[*it];
		if (layout.columns.size() == MAX_PHYSICAL_COLUMNS) {
			throw FormatError("backup: " + name + "'s base rowset lists more than " +
							  std::to_string(MAX_PHYSICAL_COLUMNS) + " physical columns, more than a null bit numbers");
		}
		PhysicalColumn c;
		c.rscolid = r.rscolid;
		c.hbcolid = r.hbcolid;
		SetTypeInfo(c, static_cast<uint32_t>(r.ti));
		c.collation = r.collation;
		c.status = r.status;
		c.offset = r.offset;
		c.null_bit = r.null_bit;
		c.bit_position = r.bit_position;
		c.ordkey = r.ordkey;
		if (!c.Dropped() && !c.Uniquifier()) {
			auto named = column_index_.find(std::make_pair(object_id, r.rscolid));
			if (named == column_index_.end()) {
				throw FormatError("backup: " + name + "'s physical column " + std::to_string(r.rscolid) +
								  " has no column in syscolpars");
			}
			c.name = columns[named->second].name;
		}
		layout.columns.push_back(std::move(c));
	}
	if (layout.columns.empty()) {
		throw FormatError("backup: " + name + "'s base rowset has no column in sysrscols");
	}
	return layout;
}

std::vector<PagePointer> UnitPages(const PageStore &store, const SysAllocUnit &unit) {
	const AuPages pages = ReadIamChain(store, unit.first_iam, unit.auid);
	// An IAM page whose bits are all set lists 511,232 pages: no unit lists more pages than the backup
	// carries (review of #549), and the list is built once at its size.
	if (pages.PageCount() > store.PageCapacity()) {
		throw FormatError("backup: allocation unit " + std::to_string(unit.auid) + "'s IAM chain lists " +
						  std::to_string(pages.PageCount()) + " pages, more than the backup's " +
						  std::to_string(store.PageCapacity()));
	}
	std::vector<PagePointer> out(pages.single_pages.begin(), pages.single_pages.end());
	out.reserve(static_cast<size_t>(pages.PageCount()));
	for (const auto &run : pages.extents) {
		for (uint64_t e = run.first; e < static_cast<uint64_t>(run.first) + run.count; e++) {
			for (uint32_t k = 0; k < 8; k++) {
				PagePointer p;
				p.file = run.file;
				p.page = static_cast<uint32_t>(e * 8 + k);
				out.push_back(p);
			}
		}
	}
	return out;
}

bool ReadUnitDataPage(const PageStore &store, const SysAllocUnit &unit, const PagePointer &at, const std::string &table,
					  Page &out) {
	try {
		if (!store.Read(at.file, at.page, out)) {
			return false;  // in one of the unit's extents, and not in use
		}
	} catch (PendingPageError &e) {
		// Kept a PendingPageError: the catalog model names it on the objects that need the table (D13).
		throw PendingPageError(std::string(e.what()) + " (reading " + table + ")");
	} catch (FormatError &e) {
		throw FormatError(std::string(e.what()) + " (reading " + table + ")");
	}
	if (out.header.type != static_cast<uint8_t>(PageType::Data) || out.header.level != 0) {
		return false;  // its IAM pages, a clustered index's upper levels
	}
	if (out.header.AllocationUnit() != unit.auid) {
		throw FormatError("backup: data page " + out.header.page_id.ToString() + ", listed by the IAM chain of " +
						  table + "'s allocation unit " + std::to_string(unit.auid) + ", names allocation unit " +
						  std::to_string(out.header.AllocationUnit()));
	}
	return true;
}

void ForEachPageRow(const Page &page, const RowsetLayout &layout, const std::function<void(const SystemRow &)> &row) {
	if (layout.columns.size() > MAX_SYSTEM_TABLE_COLUMNS) {
		throw FormatError("backup: " + layout.table + " is described with " + std::to_string(layout.columns.size()) +
						  " physical columns, more than the system-table reader takes (" +
						  std::to_string(MAX_SYSTEM_TABLE_COLUMNS) + ")");
	}
	PageRows(page, layout, row);
}

const RowsetLayout &BootstrapLayout(const std::string &name) {
	auto it = Literals().find(name);
	if (it == Literals().end()) {
		throw FormatError("backup: " + name + " is not a bootstrap table");
	}
	return it->second;
}

void ForEachRowsetRow(const PageStore &store, const SysAllocUnit &unit, const RowsetLayout &layout,
					  const std::function<void(const SystemRow &)> &row) {
	if (layout.columns.size() > MAX_SYSTEM_TABLE_COLUMNS) {
		throw FormatError("backup: " + layout.table + " is described with " + std::to_string(layout.columns.size()) +
						  " physical columns, more than the system-table reader takes (" +
						  std::to_string(MAX_SYSTEM_TABLE_COLUMNS) + ")");
	}
	ForEachDataPage(store, unit, layout.table, [&](const Page &page) { PageRows(page, layout, row); });
}

std::vector<SystemRow> ReadRowsetRows(const PageStore &store, const SysAllocUnit &unit, const RowsetLayout &layout) {
	std::vector<SystemRow> rows;
	ForEachRowsetRow(store, unit, layout, [&](const SystemRow &r) { rows.push_back(r); });
	return rows;
}

void AddBootstrapRow(BootstrapTables &out, const std::string &table, const SystemRow &r) {
	if (r.values.size() != BootstrapLayout(table).columns.size()) {
		throw FormatError("backup: a row of " + std::to_string(r.values.size()) + " values given as one of " + table +
						  ", whose layout has " + std::to_string(BootstrapLayout(table).columns.size()));
	}
	if (table == "sysallocunits") {
		static const RowsetLayout &l = BootstrapLayout("sysallocunits");
		static const size_t auid = l.IndexOf("auid"), type = l.IndexOf("type"), owner = l.IndexOf("ownerid"),
							status = l.IndexOf("status"), fgid = l.IndexOf("fgid"), first = l.IndexOf("pgfirst"),
							root = l.IndexOf("pgroot"), iam = l.IndexOf("pgfirstiam");
		SysAllocUnit a;
		a.auid = static_cast<uint64_t>(RequiredInt(l, r, auid));
		a.type = static_cast<uint8_t>(RequiredInt(l, r, type));
		a.owner_id = static_cast<uint64_t>(RequiredInt(l, r, owner));
		a.status = static_cast<int32_t>(RequiredInt(l, r, status));
		a.filegroup_id = static_cast<int16_t>(RequiredInt(l, r, fgid));
		a.first_page = ValuePointer(l, first, r.values[first]);
		a.root_page = ValuePointer(l, root, r.values[root]);
		a.first_iam = ValuePointer(l, iam, r.values[iam]);
		out.allocation_units.push_back(a);
	} else if (table == "sysrowsets") {
		static const RowsetLayout &l = BootstrapLayout("sysrowsets");
		static const size_t id = l.IndexOf("rowsetid"), owner = l.IndexOf("ownertype"), major = l.IndexOf("idmajor"),
							minor = l.IndexOf("idminor"), part = l.IndexOf("numpart"), status = l.IndexOf("status"),
							rows = l.IndexOf("rcrows"), cmpr = l.IndexOf("cmprlevel");
		SysRowset s;
		s.rowset_id = static_cast<uint64_t>(RequiredInt(l, r, id));
		s.owner_type = static_cast<uint8_t>(RequiredInt(l, r, owner));
		s.object_id = static_cast<int32_t>(RequiredInt(l, r, major));
		s.index_id = static_cast<int32_t>(RequiredInt(l, r, minor));
		s.partition_number = static_cast<int32_t>(RequiredInt(l, r, part));
		s.status = static_cast<int32_t>(RequiredInt(l, r, status));
		s.rows = RequiredInt(l, r, rows);
		s.compression = static_cast<uint8_t>(RequiredInt(l, r, cmpr));
		out.rowsets.push_back(s);
	} else if (table == "sysschobjs") {
		static const RowsetLayout &l = BootstrapLayout("sysschobjs");
		static const size_t id = l.IndexOf("id"), name = l.IndexOf("name"), nsid = l.IndexOf("nsid"),
							status = l.IndexOf("status"), type = l.IndexOf("type"), pid = l.IndexOf("pid"),
							created = l.IndexOf("created"), modified = l.IndexOf("modified");
		SysObject o;
		o.id = static_cast<int32_t>(RequiredInt(l, r, id));
		o.name = ValueText(l, name, Required(l, r, name));
		o.schema_id = static_cast<int32_t>(RequiredInt(l, r, nsid));
		o.status = static_cast<int32_t>(RequiredInt(l, r, status));
		o.type = Required(l, r, type).bytes;
		o.parent_id = static_cast<int32_t>(RequiredInt(l, r, pid));
		o.created = Required(l, r, created).bytes;
		o.modified = Required(l, r, modified).bytes;
		out.objects.push_back(std::move(o));
	} else if (table == "syscolpars") {
		static const RowsetLayout &l = BootstrapLayout("syscolpars");
		static const size_t id = l.IndexOf("id"), number = l.IndexOf("number"), colid = l.IndexOf("colid"),
							name = l.IndexOf("name"), xtype = l.IndexOf("xtype"), utype = l.IndexOf("utype"),
							length = l.IndexOf("length"), prec = l.IndexOf("prec"), scale = l.IndexOf("scale"),
							coll = l.IndexOf("collationid"), status = l.IndexOf("status");
		SysColumn c;
		c.object_id = static_cast<int32_t>(RequiredInt(l, r, id));
		c.number = static_cast<int16_t>(RequiredInt(l, r, number));
		c.column_id = static_cast<int32_t>(RequiredInt(l, r, colid));
		c.name = ValueText(l, name, r.values[name]);  // nullable: a parameter's return value has none
		c.xtype = static_cast<uint8_t>(RequiredInt(l, r, xtype));
		c.user_type = static_cast<int32_t>(RequiredInt(l, r, utype));
		c.length = static_cast<int16_t>(RequiredInt(l, r, length));
		c.precision = static_cast<uint8_t>(RequiredInt(l, r, prec));
		c.scale = static_cast<uint8_t>(RequiredInt(l, r, scale));
		c.collation_id = static_cast<uint32_t>(RequiredInt(l, r, coll));
		c.status = static_cast<int32_t>(RequiredInt(l, r, status));
		out.columns.push_back(std::move(c));
	} else if (table == "sysrscols") {
		static const RowsetLayout &l = BootstrapLayout("sysrscols");
		static const size_t rsid = l.IndexOf("rsid"), rscolid = l.IndexOf("rscolid"), hbcolid = l.IndexOf("hbcolid"),
							ti = l.IndexOf("ti"), cid = l.IndexOf("cid"), ordkey = l.IndexOf("ordkey"),
							status = l.IndexOf("status"), offset = l.IndexOf("offset"), nullbit = l.IndexOf("nullbit"),
							bitpos = l.IndexOf("bitpos");
		SysRsCol c;
		c.rowset_id = static_cast<uint64_t>(RequiredInt(l, r, rsid));
		c.rscolid = static_cast<int32_t>(RequiredInt(l, r, rscolid));
		c.hbcolid = static_cast<int32_t>(RequiredInt(l, r, hbcolid));
		c.ti = static_cast<uint32_t>(RequiredInt(l, r, ti));
		c.collation = static_cast<uint32_t>(RequiredInt(l, r, cid));
		c.ordkey = static_cast<int16_t>(RequiredInt(l, r, ordkey));
		c.status = static_cast<uint32_t>(RequiredInt(l, r, status));
		// offset, nullbit and bitpos are the low 16 / 16 / 8 bits of their int columns.
		c.offset = static_cast<int16_t>(static_cast<uint32_t>(RequiredInt(l, r, offset)) & 0xFFFF);
		c.null_bit = static_cast<uint16_t>(static_cast<uint32_t>(RequiredInt(l, r, nullbit)) & 0xFFFF);
		c.bit_position = static_cast<uint8_t>(RequiredInt(l, r, bitpos) & 0xFF);
		out.rscols.push_back(c);
	} else {
		throw FormatError("backup: " + table + " is not a bootstrap table");
	}
}

namespace {

BootstrapTables ReadBootstrapTablesThroughLiterals(const PageStore &store, const BootPage &boot, uint64_t &self_au);

}  // namespace

static void CheckBootstrapUnits(const BootstrapTables &out, uint64_t self_au);

BootstrapTables ReadBootstrapTables(const PageStore &store, const BootPage &boot) {
	// The five are decoded through literal layouts before their own description can be read, and the
	// literals are checked against it only once all five are in (the description IS those tables). A
	// version that moved a field fails on the way, and says so: the failure is named with the
	// database's version and whether the reader was measured on it (review of #549).
	uint64_t self_au = 0;
	BootstrapTables out;
	try {
		out = ReadBootstrapTablesThroughLiterals(store, boot, self_au);
		CheckBootstrapLayouts(out);
		CheckBootstrapUnits(out, self_au);
	} catch (FormatError &e) {
		const bool measured = boot.version == 869 || boot.version == 904 || boot.version == 957 || boot.version == 998;
		throw FormatError(std::string(e.what()) +
						  " (reading the bootstrap system tables through the layouts the reader "
						  "knows, measured on database versions 869, 904, 957 and 998: this "
						  "database is version " +
						  std::to_string(boot.version) +
						  (measured ? ")" : ", which the reader has not been checked on: a layout it may not know)"));
	}
	return out;
}

namespace {

BootstrapTables ReadBootstrapTablesThroughLiterals(const PageStore &store, const BootPage &boot, uint64_t &self_au) {
	BootstrapTables out;
	// sysallocunits along its leaf chain from dbi_firstSysIndexes.
	std::set<std::pair<uint16_t, uint32_t>> chain;
	{
		const RowsetLayout &l = BootstrapLayout("sysallocunits");
		auto add = [&](const SystemRow &r) { AddBootstrapRow(out, "sysallocunits", r); };

		PagePointer at = boot.first_sys_indexes;
		Page page;
		while (!at.IsNull()) {
			if (!chain.insert(std::make_pair(at.file, at.page)).second) {
				throw FormatError("backup: the leaf chain of sysallocunits loops at page " + at.ToString());
			}
			store.ReadLive(at.file, at.page, page, "the leaf chain of sysallocunits");
			if (page.header.type != static_cast<uint8_t>(PageType::Data) || page.header.level != 0) {
				throw FormatError("backup: page " + at.ToString() +
								  " of the leaf chain of sysallocunits is not a data page of level 0 (type " +
								  std::to_string(page.header.type) + ", level " + std::to_string(page.header.level) +
								  ")");
			}
			if (chain.size() == 1) {
				self_au = page.header.AllocationUnit();
			} else if (page.header.AllocationUnit() != self_au) {
				throw FormatError(
					"backup: page " + at.ToString() + " of the leaf chain of sysallocunits names allocation unit " +
					std::to_string(page.header.AllocationUnit()) + ", its first page " + std::to_string(self_au));
			}
			PageRows(page, l, add);
			at = page.header.next;
		}
		if (chain.empty()) {
			throw FormatError("backup: the boot page points to no sysallocunits page");
		}
	}
	out.BuildIndexes();
	// Its own row: the first page the boot page names, and an IAM chain listing the leaf chain's pages.
	const SysAllocUnit *self = out.AllocationUnit(self_au);
	if (!self) {
		throw FormatError("backup: sysallocunits has no row for its own allocation unit " + std::to_string(self_au));
	}
	if (!(self->first_page == boot.first_sys_indexes)) {
		throw FormatError("backup: sysallocunits' own row names " + self->first_page.ToString() +
						  " as its first page, the boot page " + boot.first_sys_indexes.ToString());
	}
	{
		std::set<std::pair<uint16_t, uint32_t>> by_iam;
		ForEachDataPage(store, *self, "sysallocunits", [&](const Page &page) {
			by_iam.insert(std::make_pair(page.header.page_id.file, page.header.page_id.page));
		});
		if (by_iam != chain) {
			throw FormatError("backup: the IAM chain of sysallocunits lists " + std::to_string(by_iam.size()) +
							  " level-0 data pages, its leaf chain " + std::to_string(chain.size()) +
							  ", and the two sets differ");
		}
	}
	// The other four, decoded page by page straight into their structs.
	auto read = [&](const char *name, uint64_t au) {
		const SysAllocUnit *unit = out.AllocationUnit(au);
		if (!unit) {
			throw FormatError(std::string("backup: sysallocunits has no row for ") + name + "'s allocation unit " +
							  std::to_string(au));
		}
		ForEachRowsetRow(store, *unit, BootstrapLayout(name),
						 [&](const SystemRow &r) { AddBootstrapRow(out, name, r); });
	};
	read("sysrowsets", AU_SYSROWSETS);
	read("sysschobjs", AU_SYSSCHOBJS);
	read("syscolpars", AU_SYSCOLPARS);
	read("sysrscols", AU_SYSRSCOLS);
	out.BuildIndexes();
	return out;
}

}  // namespace

static void CheckBootstrapUnits(const BootstrapTables &out, uint64_t self_au) {
	// The units the reader found the four by are their base rowsets' in-row units.
	const std::pair<const char *, uint64_t> units_of[] = {{"sysrowsets", AU_SYSROWSETS},
														  {"sysschobjs", AU_SYSSCHOBJS},
														  {"syscolpars", AU_SYSCOLPARS},
														  {"sysrscols", AU_SYSRSCOLS},
														  {"sysallocunits", self_au}};
	for (const auto &u : units_of) {
		const RowsetLayout layout = out.BaseLayout(out.SystemObject(u.first)->id);
		const uint64_t in_row = out.InRowUnit(layout.rowset_id, u.first).auid;
		if (in_row != u.second) {
			throw FormatError(std::string("backup: ") + u.first + " was read from allocation unit " +
							  std::to_string(u.second) + ", and sysallocunits gives its rowset the unit " +
							  std::to_string(in_row));
		}
	}
}

void CheckBootstrapLayouts(const BootstrapTables &tables) {
	for (const char *name : BOOTSTRAP_ORDER) {
		const SysObject *object = tables.SystemObject(name);
		if (!object) {
			throw FormatError(std::string("backup: sysschobjs has no system table ") + name);
		}
		const RowsetLayout described = tables.BaseLayout(object->id);
		for (const auto &literal : BootstrapLayout(name).columns) {
			const PhysicalColumn *d = described.Find(literal.name);
			auto describe = [](const PhysicalColumn &c) {
				return XtypeName(c.xtype) + (c.length ? " of " + std::to_string(c.length) + " bytes" : std::string()) +
					   (c.offset > 0 ? " at offset " + std::to_string(c.offset)
									 : " as variable column " + std::to_string(-c.offset)) +
					   ", null bit " + std::to_string(c.null_bit);
			};
			const bool sized = literal.xtype == XTYPE_BINARY || literal.xtype == XTYPE_CHAR ||
							   literal.xtype == XTYPE_NCHAR || literal.offset < 0;
			if (!d || d->Dropped() || d->xtype != literal.xtype || d->offset != literal.offset ||
				d->null_bit != literal.null_bit || (sized && d->length != literal.length)) {
				throw FormatError(std::string("backup: the database describes ") + name + "." + literal.name + " as " +
								  (d ? describe(*d) : std::string("absent")) + ", where the reader reads " +
								  describe(literal) + ": a layout of the system tables this reader does not know");
			}
		}
	}
}

SystemTable ReadSystemTable(const PageStore &store, const BootstrapTables &tables, const std::string &name) {
	const SysObject *object = tables.SystemObject(name);
	if (!object) {
		throw FormatError("backup: the database has no system table " + name);
	}
	SystemTable t;
	t.layout = tables.BaseLayout(object->id);
	t.layout.table = name;
	t.rows = ReadRowsetRows(store, tables.InRowUnit(t.layout.rowset_id, name), t.layout);
	return t;
}

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb
