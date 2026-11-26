#include "backup/backup_catalog_model.hpp"

#include "backup/backup_lob.hpp"
#include "backup/backup_sql_variant.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <unordered_map>

namespace duckdb {
namespace mssql {
namespace backup {

namespace {

constexpr int64_t CLASS_SCHEMA = 50;
constexpr uint8_t OWNER_PARTITION = 1;
constexpr uint8_t OWNER_DELTA_STORE = 3;

std::string Utf8OfLatin1(const std::string &bytes) {
	std::string out;
	out.reserve(bytes.size());
	for (unsigned char c : bytes) {
		if (c < 0x80) {
			out.push_back(static_cast<char>(c));
		} else {
			out.push_back(static_cast<char>(0xC0 | (c >> 6)));
			out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
		}
	}
	return out;
}

ModuleDefinition Unreadable(const std::string &reason) {
	ModuleDefinition d;
	d.state = ModuleDefinition::State::Unreadable;
	d.reason = reason;
	return d;
}

// A sysobjvalues row the model keeps: a module's text (valclass 1, subobjid 0) or a computed
// column's (valclass 2, subobjid the column).
struct ModuleKey {
	int64_t valclass;
	int32_t object_id;
	int32_t sub_id;
	bool operator<(const ModuleKey &o) const {
		return valclass != o.valclass ? valclass < o.valclass
									  : (object_id != o.object_id ? object_id < o.object_id : sub_id < o.sub_id);
	}
};

// One row's text: decoded now when in row, its root kept when off row. A row that cannot be read is
// Unreadable with its reason, for its object alone.
ModuleDefinition DefinitionOf(const SystemRow &row, size_t value, size_t imageval, const SysAllocUnit *lob,
							  const std::string &what) {
	try {
		const SystemValue &text = row.values[imageval];
		if (text.is_null || row.values[value].is_null) {
			return ModuleDefinition();
		}
		const int64_t flags =
			SqlVariantInt(ParseSqlVariant(row.values[value].bytes, what + "'s flags"), what + "'s flags");
		ModuleDefinition d;
		if (text.off_row) {
			if (!lob) {
				return Unreadable(what + " is stored off-row, and sysobjvalues has no LOB allocation unit");
			}
			d.state = ModuleDefinition::State::Deferred;
			d.root = text.bytes;
			d.flags = flags;
			d.lob_unit = lob->auid;
			return d;
		}
		d.state = ModuleDefinition::State::Ready;
		d.text = ModuleText(text.bytes, flags, what);
		return d;
	} catch (FormatError &e) {
		return Unreadable(e.what());
	}
}

// The texts the model keeps: the rows of valclass 1 and 2 ObjectValueFilter names (a readable view's or a
// default's definition, a user table's computed column), the ones the model describes.
std::map<ModuleKey, ModuleDefinition> ReadModuleTexts(const PageStore &store, const BootstrapTables &tables) {
	std::map<ModuleKey, ModuleDefinition> out;
	const ObjectValueFilter filter(tables, std::set<int32_t>());
	const SysObject *object = tables.SystemObject("sysobjvalues");
	if (!object) {
		throw FormatError("backup: the database has no system table sysobjvalues");
	}
	RowsetLayout layout = tables.BaseLayout(object->id);
	layout.table = "sysobjvalues";
	const size_t valclass = layout.IndexOf("valclass"), objid = layout.IndexOf("objid"),
				 subobjid = layout.IndexOf("subobjid"), valnum = layout.IndexOf("valnum"),
				 value = layout.IndexOf("value"), imageval = layout.IndexOf("imageval");
	const SysAllocUnit *lob = tables.FindLobUnit(layout.rowset_id);
	ForEachRowsetRow(store, tables.InRowUnit(layout.rowset_id, "sysobjvalues"), layout, [&](const SystemRow &row) {
		const int64_t cls = ValueInt(layout, valclass, row.values[valclass]);
		const int32_t id = static_cast<int32_t>(ValueInt(layout, objid, row.values[objid]));
		const int64_t sub_value = ValueInt(layout, subobjid, row.values[subobjid]);
		if ((cls != 1 && cls != 2) || !filter.Keep(cls, id, sub_value, ValueInt(layout, valnum, row.values[valnum]))) {
			return;
		}
		const int32_t sub = static_cast<int32_t>(sub_value);
		const std::string what =
			"the text of object " + std::to_string(id) + (cls == 2 ? " column " + std::to_string(sub) : "");
		const ModuleKey key{cls, id, sub};
		if (out.count(key)) {
			out[key] = Unreadable(what + " is listed twice in sysobjvalues");
			return;
		}
		out[key] = DefinitionOf(row, value, imageval, lob, what);
	});
	return out;
}

// sysseobjvalues valclass 1: a metadata-only ADD COLUMN's default, by (rowset, hbcolid). A key listed
// twice is kept in `ambiguous`, so that only its table is refused.
std::map<std::pair<uint64_t, int32_t>, std::string> ReadAddedDefaults(
	const PageStore &store, const BootstrapTables &tables, std::set<std::pair<uint64_t, int32_t>> &ambiguous) {
	std::map<std::pair<uint64_t, int32_t>, std::string> out;
	const SysObject *object = tables.SystemObject("sysseobjvalues");
	if (!object) {
		throw FormatError("backup: the database has no system table sysseobjvalues");
	}
	RowsetLayout layout = tables.BaseLayout(object->id);
	layout.table = "sysseobjvalues";
	const size_t valclass = layout.IndexOf("valclass"), id = layout.IndexOf("id"), subid = layout.IndexOf("subid"),
				 valnum = layout.IndexOf("valnum"), value = layout.IndexOf("value");
	ForEachRowsetRow(store, tables.InRowUnit(layout.rowset_id, "sysseobjvalues"), layout, [&](const SystemRow &row) {
		// A row whose value is NULL holds no default (not seen; an ADD ... NULL DEFAULT without WITH
		// VALUES writes none, the column then reads NULL in older rows).
		if (ValueInt(layout, valclass, row.values[valclass]) != 1 ||
			ValueInt(layout, valnum, row.values[valnum]) != 0 || row.values[value].is_null) {
			return;
		}
		const auto key = std::make_pair(static_cast<uint64_t>(ValueInt(layout, id, row.values[id])),
										static_cast<int32_t>(ValueInt(layout, subid, row.values[subid])));
		if (!out.emplace(key, row.values[value].bytes).second) {
			ambiguous.insert(key);
		}
	});
	return out;
}

bool VariableLength(uint8_t xtype) {
	return xtype == 165 || xtype == 167 || xtype == 231;  // varbinary, varchar, nvarchar
}

// The default the record form of `physical` takes for rows written before it was added. False with
// `why` when the catalog's value cannot stand for it: missing, listed twice, malformed, or not of the
// column's own type and size.
bool AddedDefault(const std::map<std::pair<uint64_t, int32_t>, std::string> &defaults,
				  const std::set<std::pair<uint64_t, int32_t>> &ambiguous, const RowsetLayout &layout,
				  const PhysicalColumn &physical, const CatalogColumn &column, std::string &bytes, std::string &why) {
	const auto key = std::make_pair(layout.rowset_id, physical.hbcolid);
	auto it = defaults.find(key);
	if (it == defaults.end()) {
		why = "column " + column.name + " was added with a default the catalog does not hold";
		return false;
	}
	if (ambiguous.count(key)) {
		why = "column " + column.name + " was added with a default sysseobjvalues lists twice";
		return false;
	}
	SqlVariant v;
	try {
		v = ParseSqlVariant(it->second, "the default of column " + column.name);
	} catch (FormatError &e) {
		why = e.what();
		return false;
	}
	const bool scaled = v.xtype == 41 || v.xtype == 42 || v.xtype == 43;
	const bool decimal = v.xtype == 106 || v.xtype == 108;
	const bool text = v.xtype == 167 || v.xtype == 175 || v.xtype == 231 || v.xtype == 239;
	if (v.xtype != column.system_type_id || (scaled && v.scale != column.scale) ||
		(decimal && (v.precision != column.precision || v.scale != column.scale)) ||
		(text && v.collation != column.collation_id)) {
		why = "column " + column.name + " was added with a default stored as xtype " + std::to_string(v.xtype) +
			  ", not in the column's own type";
		return false;
	}
	// A fixed column's record form has its declared size; a variable one at most its maximum.
	const bool fits = VariableLength(v.xtype)
						  ? column.max_length < 0 || v.value.size() <= static_cast<size_t>(column.max_length)
						  : column.max_length >= 0 && v.value.size() == static_cast<size_t>(column.max_length);
	if (!fits) {
		why = "column " + column.name + " was added with a default of " + std::to_string(v.value.size()) +
			  " bytes, which its " + std::to_string(column.max_length) + "-byte type does not take";
		return false;
	}
	bytes = v.value;
	return true;
}

// Adds a file's row count to `sum` (a statistic): a negative count, or a sum past int64, leaves it
// unknown (-1), never UB.
void AddRowCount(int64_t &sum, int64_t rows) {
	if (sum < 0) {
		return;
	}
	sum = rows < 0 || rows > std::numeric_limits<int64_t>::max() - sum ? -1 : sum + rows;
}

// The added columns' defaults, found once per model build.
struct AddedDefaults {
	std::map<std::pair<uint64_t, int32_t>, std::string> values;
	std::set<std::pair<uint64_t, int32_t>> ambiguous;
	std::string pending;  // sysseobjvalues could not be read: why
};

// Two layouts that read records alike (their rowset ids aside).
bool SameLayout(const RowsetLayout &a, const RowsetLayout &b) {
	if (a.columns.size() != b.columns.size() || a.compression != b.compression) {
		return false;
	}
	for (size_t i = 0; i < a.columns.size(); i++) {
		const PhysicalColumn &x = a.columns[i], &y = b.columns[i];
		if (x.name != y.name || x.rscolid != y.rscolid || x.hbcolid != y.hbcolid || x.xtype != y.xtype ||
			x.length != y.length || x.precision != y.precision || x.scale != y.scale || x.collation != y.collation ||
			x.status != y.status || x.offset != y.offset || x.null_bit != y.null_bit ||
			x.bit_position != y.bit_position || x.ordkey != y.ordkey || x.has_default != y.has_default ||
			x.unpadded != y.unpadded || x.default_bytes != y.default_bytes) {
			return false;
		}
	}
	return true;
}

bool IsDecimalXtype(uint8_t xtype) {
	return xtype == XTYPE_DECIMAL || xtype == XTYPE_NUMERIC;
}

// char / nchar / binary: fixed types that may be stored variable, padded when stored fixed.
bool IsPaddedXtype(uint8_t xtype) {
	return xtype == XTYPE_CHAR || xtype == XTYPE_NCHAR || xtype == XTYPE_BINARY;
}

bool IsVariableXtype(uint8_t xtype) {
	return xtype == XTYPE_VARCHAR || xtype == XTYPE_NVARCHAR || xtype == XTYPE_VARBINARY ||
		   xtype == XTYPE_SQL_VARIANT || xtype == XTYPE_XML || xtype == XTYPE_IMAGE || xtype == XTYPE_TEXT ||
		   xtype == XTYPE_NTEXT;
}

// What sysrscols.ti says of `physical`'s type against syscolpars' row of it (`c`): false with `why`.
bool CheckDeclaredType(const PhysicalColumn &physical, const CatalogColumn &c, std::string &why) {
	const bool decimal = IsDecimalXtype(physical.xtype);
	const bool scaled =
		physical.xtype == XTYPE_TIME || physical.xtype == XTYPE_DATETIME2 || physical.xtype == XTYPE_DATETIMEOFFSET;
	// A CLR type's column can be described as varbinary in sysrscols (E22: hierarchyid, geometry of a
	// ROW-compressed rowset, and of an uncompressed one SELECT INTO made), and a user type of Format.Native
	// as binary of its size, stored at a fixed offset (measured on 2022 and 2025: a 9-byte type at offsets 8 and 17;
	// fixture test/data/backup/clr); its bytes are the type's as stored, read as such (ReadingOf).
	const bool clr_as_binary =
		c.system_type_id == XTYPE_CLR && (physical.xtype == XTYPE_VARBINARY || physical.xtype == XTYPE_BINARY);
	if ((physical.xtype != c.system_type_id && !clr_as_binary) || (decimal && physical.precision != c.precision) ||
		((decimal || scaled) && physical.scale != c.scale)) {
		why = "column " + c.name + " is stored as xtype " + std::to_string(physical.xtype) + " (precision " +
			  std::to_string(physical.precision) + ", scale " + std::to_string(physical.scale) +
			  "), which its declaration does not say";
		return false;
	}
	return true;
}

// Where `physical` is stored against what its type allows, and its width: false with `why`.
bool CheckStorageForm(const PhysicalColumn &physical, const CatalogColumn &c, bool cd, const std::string &table,
					  std::string &why) {
	// A fixed type stored as a variable column is the vardecimal storage format (or a layout the
	// reader does not know); a variable type at a fixed offset is not one either. Two are both: char /
	// nchar / binary may be stored variable (and read so: their bytes, unpadded) -- a nullable one
	// declared under ANSI_PADDING OFF with the table is stored fixed and padded, and trimmed on read
	// (`unpadded`, E22) --, and a CLR type of fixed length is stored fixed (its bytes). A type
	// the reader does not know is not checked: it is read as its bytes wherever it is stored (ReadingOf).
	const bool padded = IsPaddedXtype(physical.xtype);
	const bool fixed_type = KnownFixedXtype(physical.xtype) && !padded;
	const bool variable_type = IsVariableXtype(physical.xtype);
	if (!cd && ((fixed_type && physical.offset < 0) || (variable_type && physical.offset > 0))) {
		why = "column " + c.name + " (xtype " + std::to_string(physical.xtype) + ") is stored as a " +
			  (physical.offset < 0 ? "variable" : "fixed") +
			  " column, which its type is not (the vardecimal storage format is not read)";
		return false;
	}
	// A fixed char / nchar / binary is as wide as its declaration: a sysrscols width that disagrees
	// with syscolpars' would read the neighbouring columns as its value (review of #552). A sparse column of a
	// fixed type is checked too: its entry must have that width (LocateValue), so a disagreement found here
	// refuses the table by name rather than its scan at the first value (review of #626).
	const bool fixed_width = physical.offset > 0 || (physical.Sparse() && KnownFixedXtype(physical.xtype));
	if (padded && fixed_width && physical.length != c.max_length) {
		why = "column " + c.name + " is stored as " + std::to_string(physical.length) + " bytes, declared " +
			  std::to_string(c.max_length);
		return false;
	}
	if (fixed_width) {
		try {
			(void)physical.FixedLength(table);
		} catch (FormatError &e) {
			why = e.what();
			return false;
		}
	}
	return true;
}

// What sysrscols.ti says of a live column, checked against syscolpars' row of it (E20 pinned ti's
// fields): a disagreement, or a fixed column whose width is unknown, refuses the table here, by name,
// rather than mid-scan. Marks a column read unpadded.
bool CheckLiveColumn(const std::map<int32_t, const CatalogColumn *> &by_id, const CatalogTable &t, bool cd,
					 PhysicalColumn &physical, std::string &why) {
	if (cd && physical.offset >= 0) {
		why = "column " + physical.name + " is at fixed offset " + std::to_string(physical.offset) +
			  " in a compressed rowset, whose columns are CD indexes";
		return false;
	}
	auto column = by_id.find(physical.rscolid);
	if (column == by_id.end()) {
		if (physical.ordkey > 0) {
			// The scan builds _mssql_rid from the key columns: one unchecked here would fail on every row.
			why = "key column " + std::to_string(physical.rscolid) + " has no column";
			return false;
		}
		return true;  // named with the defaults when it matters (an added column), as before
	}
	const CatalogColumn &c = *column->second;
	if (!CheckDeclaredType(physical, c, why)) {
		return false;
	}
	// A nullable char / binary under ANSI_PADDING OFF is read as the server returns it, its trailing
	// spaces / zero bytes left out (E22, measured: DATALENGTH 1 for 0x0100 in a binary(8)). An
	// uncompressed rowset stores it at a fixed offset, padded, and a CD one trimmed: only the column
	// says which. nchar and NOT NULL columns are padded whatever the setting (is_ansi_padded). By the
	// declared type: a CLR column sysrscols describes as binary (a Format.Native user type) has no
	// ANSI_PADDING status and its trailing zero bytes are its value's (fixture test/data/backup/clr).
	if ((c.system_type_id == XTYPE_CHAR || c.system_type_id == XTYPE_BINARY) && c.is_nullable && !c.is_ansi_padded) {
		physical.unpadded = true;
	}
	// Sparse by sysrscols (status 0x100) and by syscolpars alike, or a value would be looked for where it is not.
	if (physical.Sparse() != c.is_sparse) {
		why = "column " + c.name + " is " + (c.is_sparse ? "" : "not ") + "sparse in syscolpars, and " +
			  (physical.Sparse() ? "" : "not ") + "sparse in sysrscols";
		return false;
	}
	return CheckStorageForm(physical, c, cd, t.name, why);
}

// The default of a column added with one, into `physical`: false with `why`.
bool FillAddedDefault(const AddedDefaults &defaults, const std::map<int32_t, const CatalogColumn *> &by_id,
					  const RowsetLayout &layout, PhysicalColumn &physical, std::string &why) {
	auto column = by_id.find(physical.rscolid);
	if (column == by_id.end()) {
		why = "physical column " + std::to_string(physical.rscolid) + " has no column";
		return false;
	}
	if (!defaults.pending.empty()) {
		why = "column " + column->second->name + " was added with a default, and " + defaults.pending;
		return false;
	}
	if (!AddedDefault(defaults.values, defaults.ambiguous, layout, physical, *column->second, physical.default_bytes,
					  why)) {
		return false;
	}
	physical.has_default = true;
	return true;
}

// One partition's layout, its added columns' defaults filled in; false with `why`.
bool PartitionLayout(const BootstrapTables &tables, const AddedDefaults &defaults,
					 const std::map<int32_t, const CatalogColumn *> &by_id, const CatalogTable &t,
					 const SysRowset &rowset, RowsetLayout &layout, std::string &why) {
	try {
		layout = tables.LayoutOf(t.object_id, rowset);
	} catch (FormatError &e) {
		why = std::string("its physical layout: ") + e.what();
		return false;
	}
	// ROW compression (spec 086 PR 1, E22): every column a CD index, whatever its type (a decimal's precision
	// without its 0x80: LayoutOf). PAGE compression's records are CD records too (spec 086 PR 2, E23).
	const bool cd = rowset.compression == 1 || rowset.compression == 2;
	for (auto &physical : layout.columns) {
		if (physical.Dropped() || physical.Uniquifier()) {
			continue;
		}
		if (!CheckLiveColumn(by_id, t, cd, physical, why)) {
			return false;
		}
	}
	for (auto &physical : layout.columns) {
		if (physical.Dropped() || physical.Uniquifier() || !physical.HasDefaultStatus()) {
			continue;
		}
		if (!FillAddedDefault(defaults, by_id, layout, physical, why)) {
			return false;
		}
	}
	return true;
}

void DecideDecodable(const BootstrapTables &tables, const AddedDefaults &defaults,
					 const std::unordered_map<uint64_t, const SysRowset *> &rowsets, CatalogTable &t) {
	t.decodable = false;
	if (!t.reason.empty()) {
		return;	 // already refused (its schema or a column's type)
	}
	if (t.partitions.empty()) {
		t.reason = "it has no live base rowset";
		return;
	}
	// Each partition has one live base rowset, and a table on no partition scheme has one partition:
	// two rowsets for one partition (a heap and a clustered index both live, an index built or switched
	// while the backup ran) are refused by name, not read with one layout for both (review of #551).
	for (size_t i = 1; i < t.partitions.size(); i++) {
		if (t.partitions[i].partition_number == t.partitions[i - 1].partition_number) {
			t.reason = "partition " + std::to_string(t.partitions[i].partition_number) +
					   " has two live base rowsets (an index built or switched while the backup ran?)";
			return;
		}
	}
	if (!t.partitioned && t.partitions.size() > 1) {
		t.reason = "it has " + std::to_string(t.partitions.size()) +
				   " live base rowsets, and its index is on no partition scheme";
		return;
	}
	if (t.shape == TableShape::Columnstore) {
		t.reason = "a clustered columnstore index is not read (spec 088)";
		return;
	}
	for (const auto &p : t.partitions) {
		if (p.compression > 2) {
			t.reason = "compression level " + std::to_string(p.compression) + " is not read";
			return;
		}
	}
	std::map<int32_t, const CatalogColumn *> by_id;
	// A sparse column has no place of its own (sysrscols offset 0, no null bit): its value is its entry in the
	// row's sparse vector (spec 086 PR 3, E24), so its storage form is not checked here.
	for (const auto &c : t.columns) {
		by_id[c.column_id] = &c;
	}
	for (auto &p : t.partitions) {
		std::string why;
		auto rowset = rowsets.find(p.rowset_id);
		RowsetLayout layout;
		if (rowset == rowsets.end()) {
			why = "its rowset " + std::to_string(p.rowset_id) + " is not in sysrowsets";
		} else if (PartitionLayout(tables, defaults, by_id, t, *rowset->second, layout, why)) {
			// A layout like the previous partition's is shared (neighbours agree, as a rule: one layout
			// for a table of 15000 partitions); compared with that one only, so that a file of distinct
			// layouts costs one comparison a partition, not one per layout kept.
			layout.rowset_id = 0;  // shared: a partition's own is CatalogPartition::rowset_id
			if (t.layouts.empty() || !SameLayout(t.layouts.back(), layout)) {
				t.layouts.push_back(std::move(layout));
			}
			p.layout = t.layouts.size() - 1;
			continue;
		}
		t.reason = t.partitions.size() > 1 ? "partition " + std::to_string(p.partition_number) + ": " + why : why;
		t.layouts.clear();
		for (auto &q : t.partitions) {
			q.layout = 0;
		}
		return;
	}
	t.decodable = true;
}

// A reason as the model keeps it: without the "backup: " prefix FormatError messages carry.
std::string Reason(const std::string &message) {
	return message.compare(0, 8, "backup: ") == 0 ? message.substr(8) : message;
}

// The partition functions (sysclsobjs class 30) and the data spaces (class 31: a partition scheme is
// 'PS'; filegroups 'FG' and the others -- FILESTREAM 'FD', memory-optimized 'FX' -- hold a partition's
// pages).
void ReadDataSpaces(const SystemTable &classes, PartitionCatalog &out) {
	const size_t cls = classes.Column("class"), id = classes.Column("id"), name = classes.Column("name"),
				 status = classes.Column("status"), type = classes.Column("type"), intprop = classes.Column("intprop");
	for (size_t r = 0; r < classes.rows.size(); r++) {
		const int64_t c = classes.Int(r, cls);
		const int32_t i = static_cast<int32_t>(classes.Int(r, id));
		if (c == 30) {
			PartitionCatalog::Function f;
			f.name = classes.Text(r, name);
			f.range_right = (classes.Int(r, status) & 1) != 0;
			f.fanout = static_cast<int32_t>(classes.Int(r, intprop));
			if (!out.functions.emplace(i, f).second) {
				out.functions_twice.insert(i);
			}
		} else if (c == 31) {
			const std::string t = classes.Text(r, type);
			const bool scheme = t == "PS";
			if (out.schemes.count(i) || out.filegroups.count(i)) {
				out.data_spaces_twice.insert(i);
			}
			(scheme ? out.schemes : out.filegroups)[i] = classes.Text(r, name);
		}
	}
}

// Each partition function's parameter (systypedsubobjs class 30, subid 1).
void ReadFunctionParameters(const PageStore &store, const BootstrapTables &tables, PartitionCatalog &out) {
	const SystemTable typed = ReadSystemTable(store, tables, "systypedsubobjs");
	const size_t tc = typed.Column("class"), major = typed.Column("idmajor"), sub = typed.Column("subid"),
				 xtype = typed.Column("xtype"), utype = typed.Column("utype"), length = typed.Column("length"),
				 prec = typed.Column("prec"), scale = typed.Column("scale"), coll = typed.Column("collationid");
	for (size_t r = 0; r < typed.rows.size(); r++) {
		if (typed.Int(r, tc) != 30 || typed.Int(r, sub) != 1) {
			continue;
		}
		PartitionCatalog::Parameter p;
		p.xtype = static_cast<uint8_t>(typed.Int(r, xtype));
		p.user_type = static_cast<int32_t>(typed.Int(r, utype));
		p.length = static_cast<int16_t>(typed.Int(r, length));
		p.precision = static_cast<uint8_t>(typed.Int(r, prec));
		p.scale = static_cast<uint8_t>(typed.Int(r, scale));
		p.collation = static_cast<uint32_t>(typed.Int(r, coll));
		const int32_t f = static_cast<int32_t>(typed.Int(r, major));
		if (!out.parameters.emplace(f, p).second) {
			out.parameters_twice.insert(f);
		}
	}
}

// syssingleobjrefs: a scheme's function (class 31), its destinations (30), an index's data space (7).
void ReadObjectReferences(const PageStore &store, const BootstrapTables &tables, PartitionCatalog &out) {
	const SystemTable refs = ReadSystemTable(store, tables, "syssingleobjrefs");
	const size_t rc = refs.Column("class"), dep = refs.Column("depid"), depsub = refs.Column("depsubid"),
				 indep = refs.Column("indepid");
	for (size_t r = 0; r < refs.rows.size(); r++) {
		const int64_t c = refs.Int(r, rc);
		const int32_t d = static_cast<int32_t>(refs.Int(r, dep)), ds = static_cast<int32_t>(refs.Int(r, depsub)),
					  in = static_cast<int32_t>(refs.Int(r, indep));
		if (c == 31) {
			if (!out.scheme_function.emplace(d, in).second) {
				out.schemes_twice.insert(d);
			}
		} else if (c == 30) {
			if (!out.destinations[d].emplace(ds, in).second) {
				out.destinations_twice.insert(std::make_pair(d, ds));
			}
		} else if (c == 7) {
			if (!out.data_space.emplace(std::make_pair(d, ds), in).second) {
				out.data_space_twice.insert(std::make_pair(d, ds));
			}
		}
	}
}

// Each index's partitioning column (sysiscols, partition ordinal 1).
void ReadPartitionColumns(const PageStore &store, const BootstrapTables &tables, PartitionCatalog &out) {
	const SystemTable iscols = ReadSystemTable(store, tables, "sysiscols");
	const size_t im = iscols.Column("idmajor"), in = iscols.Column("idminor"), ip = iscols.Column("intprop"),
				 t2 = iscols.Column("tinyprop2");
	for (size_t r = 0; r < iscols.rows.size(); r++) {
		if (iscols.Int(r, t2) == 1) {
			const auto key =
				std::make_pair(static_cast<int32_t>(iscols.Int(r, im)), static_cast<int32_t>(iscols.Int(r, in)));
			if (!out.partition_column.emplace(key, static_cast<int32_t>(iscols.Int(r, ip))).second) {
				out.partition_column_twice.insert(key);
			}
		}
	}
}

// The partition functions' boundary values (sysobjvalues valclass 30), by function and boundary id.
void ReadBoundaryValues(const PageStore &store, const BootstrapTables &tables, PartitionCatalog &out) {
	const SysObject *object = tables.SystemObject("sysobjvalues");
	if (!object) {
		throw FormatError("backup: the database has no system table sysobjvalues");
	}
	RowsetLayout layout = tables.BaseLayout(object->id);
	layout.table = "sysobjvalues";
	const size_t vc = layout.IndexOf("valclass"), oid = layout.IndexOf("objid"), sid = layout.IndexOf("subobjid"),
				 vn = layout.IndexOf("valnum"), value = layout.IndexOf("value");
	ForEachRowsetRow(store, tables.InRowUnit(layout.rowset_id, "sysobjvalues"), layout, [&](const SystemRow &row) {
		// valclass 30 of a partition function only, as ObjectValueFilter names them.
		if (ValueInt(layout, vc, row.values[vc]) != 30 || ValueInt(layout, vn, row.values[vn]) != 1) {
			return;
		}
		const int32_t f = static_cast<int32_t>(ValueInt(layout, oid, row.values[oid]));
		if (!out.functions.count(f)) {
			return;
		}
		const int32_t b = static_cast<int32_t>(ValueInt(layout, sid, row.values[sid]));
		if (row.values[value].off_row) {
			out.boundaries_off_row.insert(f);  // a sql_variant is never off-row: refused for its function
			return;
		}
		PartitionBoundary boundary;
		boundary.is_null = row.values[value].is_null;  // a NULL boundary is legal: the lowest value
		boundary.bytes = row.values[value].bytes;
		if (!out.boundaries[f].emplace(b, std::move(boundary)).second) {
			out.boundaries_twice.insert(std::make_pair(f, b));
		}
	});
}

}  // namespace

PartitionCatalog ReadPartitionCatalog(const PageStore &store, const BootstrapTables &tables,
									  const SystemTable &classes) {
	PartitionCatalog out;
	try {
		ReadDataSpaces(classes, out);
		if (out.schemes.empty()) {
			return out;	 // no partition scheme: no table is partitioned, nothing more to read
		}
		ReadFunctionParameters(store, tables, out);
		ReadObjectReferences(store, tables, out);
		out.data_spaces_read = true;
		ReadPartitionColumns(store, tables, out);
		ReadBoundaryValues(store, tables, out);
	} catch (FormatError &e) {
		out.reason = "the partitioning tables: " + Reason(e.what());
	}
	return out;
}

namespace {

// Why boundary `boundary` (not NULL) of `p`'s function cannot be given, or "" when it can: it parses,
// is a value of the parameter's own type, collation and length, and `check` (when given) decodes it.
std::string BoundaryRefusal(const CatalogPartitioning &p, const PartitionBoundary &boundary, const std::string &what,
							const BoundaryCheck &check) {
	SqlVariant variant;
	try {
		variant = ParseSqlVariant(boundary.bytes, what);
	} catch (FormatError &e) {
		return Reason(e.what());
	}
	const uint8_t x = variant.xtype;
	const bool scaled = x == 41 || x == 42 || x == 43;
	const bool decimal = x == 106 || x == 108;
	const bool text = x == 167 || x == 175 || x == 231 || x == 239;
	const bool sized = text || x == 165 || x == 173;
	if (x != p.parameter_xtype || (scaled && variant.scale != p.parameter_scale) ||
		(decimal && (variant.precision != p.parameter_precision || variant.scale != p.parameter_scale))) {
		return what + " is a value of xtype " + std::to_string(x) + ", not of its parameter's type";
	}
	if (text && variant.collation != p.parameter_collation) {
		return what + " is of collation " + std::to_string(variant.collation) + ", its parameter's " +
			   std::to_string(p.parameter_collation);
	}
	if (sized && p.parameter_length > 0 && variant.value.size() > static_cast<size_t>(p.parameter_length)) {
		return what + " holds " + std::to_string(variant.value.size()) + " bytes, its parameter " +
			   std::to_string(p.parameter_length);
	}
	if (check) {
		try {
			check(variant, what);
		} catch (FormatError &e) {
			return Reason(e.what());
		}
	}
	return std::string();
}

// A reason clears every list: never a partial one (D13).
void FailPartitioning(CatalogTable &t, const std::string &why) {
	CatalogPartitioning &p = t.partitioning;
	t.partitioned = true;
	p.reason = why;
	p.boundaries.clear();
	p.destinations.clear();
	p.next_used.clear();
}

// Whether the table's base index is on a partition scheme, its data space into `data_space`. False with
// `why` when the catalog cannot say; false without one when it is on a filegroup or the default data space.
bool OnScheme(const PartitionCatalog &pc, const std::pair<int32_t, int32_t> &space_key, int32_t &data_space,
			  std::string &why) {
	if (pc.data_space_twice.count(space_key)) {
		why = "syssingleobjrefs lists its index's data space twice";
		return false;
	}
	auto space = pc.data_space.find(space_key);
	if (space == pc.data_space.end()) {
		// No class-7 row: the index is on the default data space (measured: tables on a scheme have one,
		// tables on the default filegroup none), unless syssingleobjrefs itself could not be read.
		if (!pc.data_spaces_read && !pc.reason.empty() && !pc.schemes.empty()) {
			why = pc.reason;
		}
		return false;
	}
	data_space = space->second;
	if (pc.data_spaces_twice.count(data_space)) {
		why = "sysclsobjs lists its index's data space " + std::to_string(data_space) + " twice";
		return false;
	}
	return pc.schemes.count(data_space) != 0;  // else on a filegroup
}

// The scheme's partition function and its parameter into `p`; why not, or "".
std::string DescribeFunction(const PartitionCatalog &pc, const std::unordered_map<int32_t, std::string> &type_names,
							 int32_t data_space, CatalogPartitioning &p) {
	if (!pc.reason.empty()) {
		return pc.reason;
	}
	if (pc.schemes_twice.count(data_space)) {
		return "syssingleobjrefs lists scheme " + p.scheme_name + "'s function twice";
	}
	auto fn_id = pc.scheme_function.find(data_space);
	if (fn_id == pc.scheme_function.end()) {
		return "scheme " + p.scheme_name + " names no partition function";
	}
	p.function_id = fn_id->second;
	auto fn = pc.functions.find(p.function_id);
	auto param = pc.parameters.find(p.function_id);
	if (fn == pc.functions.end() || param == pc.parameters.end()) {
		return "scheme " + p.scheme_name + "'s partition function " + std::to_string(p.function_id) +
			   " is not in the catalog";
	}
	if (pc.functions_twice.count(p.function_id) || pc.parameters_twice.count(p.function_id)) {
		return "partition function " + std::to_string(p.function_id) + " is listed twice in the catalog";
	}
	p.function_name = fn->second.name;
	p.range_right = fn->second.range_right;
	p.fanout = fn->second.fanout;
	p.parameter_xtype = param->second.xtype;
	p.parameter_length = param->second.length;
	p.parameter_precision = param->second.precision;
	p.parameter_scale = param->second.scale;
	p.parameter_collation = param->second.collation;
	auto type = type_names.find(param->second.user_type);
	auto system = type_names.find(param->second.xtype);
	if (type == type_names.end() || system == type_names.end()) {
		return "function " + p.function_name + "'s parameter type " + std::to_string(param->second.user_type) +
			   " is not in sysscalartypes";
	}
	p.parameter_type_name = type->second;
	p.parameter_system_type_name = system->second;
	return std::string();
}

// The table's partitioning column into `p`, and the function's fanout checked; why not, or "".
std::string PartitioningColumn(const PartitionCatalog &pc, const std::pair<int32_t, int32_t> &space_key,
							   int32_t base_index, const CatalogTable &t, CatalogPartitioning &p) {
	if (pc.partition_column_twice.count(space_key)) {
		return "sysiscols lists two partitioning columns for its index " + std::to_string(base_index);
	}
	auto column = pc.partition_column.find(space_key);
	if (column == pc.partition_column.end()) {
		return "its index " + std::to_string(base_index) + " has no partitioning column in sysiscols";
	}
	p.column_id = column->second;
	for (const auto &c : t.columns) {
		if (c.column_id == p.column_id) {
			p.column_name = c.name;
		}
	}
	if (p.column_name.empty()) {
		return "its partitioning column " + std::to_string(p.column_id) + " is not one of its columns";
	}
	if (p.fanout < 1 || p.fanout > 15000) {
		return "function " + p.function_name + " has a fanout of " + std::to_string(p.fanout);
	}
	return std::string();
}

// The boundaries: ids 1..fanout-1, each once, each NULL or a value of the parameter's own type, as
// the parameter declares it (a string's collation and length too, as AddedDefault checks a default).
// They are reference only -- the rows are read whatever they hold -- and the destinations do not
// depend on them: a boundary the reader cannot give is the reason returned, the boundaries are not
// given, and the destinations and NEXT USED still are (owner, review of #551).
std::string ReadBoundaries(const PartitionCatalog &pc, const BoundaryCheck &check, const CatalogPartitioning &p,
						   std::vector<PartitionBoundary> &boundaries) {
	auto values = pc.boundaries.find(p.function_id);
	const size_t count = values == pc.boundaries.end() ? 0 : values->second.size();
	if (pc.boundaries_off_row.count(p.function_id)) {
		return "function " + p.function_name + " has a boundary stored off-row";
	}
	if (count != static_cast<size_t>(p.fanout - 1)) {
		return "function " + p.function_name + " has " + std::to_string(count) + " boundaries, its fanout " +
			   std::to_string(p.fanout) + " says " + std::to_string(p.fanout - 1);
	}
	for (int32_t b = 1; b < p.fanout; b++) {
		auto v = values->second.find(b);
		const std::string what = "boundary " + std::to_string(b) + " of " + p.function_name;
		if (v == values->second.end() || pc.boundaries_twice.count(std::make_pair(p.function_id, b))) {
			return "function " + p.function_name + "'s boundary " + std::to_string(b) +
				   (v == values->second.end() ? " is missing" : " is listed twice");
		}
		if (!v->second.is_null) {
			const std::string refusal = BoundaryRefusal(p, v->second, what, check);
			if (!refusal.empty()) {
				return refusal;
			}
		}
		boundaries.push_back(v->second);
	}
	return std::string();
}

// The destinations: 1..fanout each a filegroup, and NEXT USED (into `p`) at fanout + 1 when there is
// one; why not, or "".
std::string ReadDestinations(const PartitionCatalog &pc, int32_t data_space, CatalogPartitioning &p,
							 std::vector<std::string> &destinations) {
	auto dests = pc.destinations.find(data_space);
	if (dests == pc.destinations.end()) {
		return "scheme " + p.scheme_name + " has no destination 1";
	}
	auto filegroup = [&](int32_t fg) {
		auto it = pc.filegroups.find(fg);
		return it == pc.filegroups.end() ? "filegroup " + std::to_string(fg) : it->second;
	};
	for (int32_t d = 1; d <= p.fanout + 1; d++) {
		auto it = dests->second.find(d);
		if (it == dests->second.end()) {
			if (d <= p.fanout) {
				return "scheme " + p.scheme_name + " has no destination " + std::to_string(d);
			}
			break;
		}
		if (pc.destinations_twice.count(std::make_pair(data_space, d)) || pc.data_spaces_twice.count(it->second)) {
			return "scheme " + p.scheme_name + "'s destination " + std::to_string(d) + " is listed twice";
		}
		if (d <= p.fanout) {
			destinations.push_back(filegroup(it->second));
		} else {
			p.next_used = filegroup(it->second);
		}
	}
	return std::string();
}

// Its partitions: 1..fanout, each once (t.partitions is sorted by number); why not, or "".
std::string PartitionsNumbered(const CatalogTable &t, const CatalogPartitioning &p) {
	bool numbered = t.partitions.size() == static_cast<size_t>(p.fanout);
	for (size_t i = 0; numbered && i < t.partitions.size(); i++) {
		numbered = t.partitions[i].partition_number == static_cast<int32_t>(i + 1);
	}
	if (numbered) {
		return std::string();
	}
	return std::to_string(t.partitions.size()) + " partitions where function " + p.function_name + " has " +
		   std::to_string(p.fanout) + " (1.." + std::to_string(p.fanout) + ", each once)";
}

}  // namespace

void DescribePartitioning(const PartitionCatalog &pc, const std::unordered_map<int32_t, std::string> &type_names,
						  int32_t base_index, const BoundaryCheck &check, CatalogTable &t) {
	CatalogPartitioning &p = t.partitioning;
	const auto space_key = std::make_pair(t.object_id, base_index);
	int32_t data_space = 0;
	std::string why;
	if (!OnScheme(pc, space_key, data_space, why)) {
		if (!why.empty()) {
			FailPartitioning(t, why);
		}
		return;
	}
	t.partitioned = true;
	p.scheme_id = data_space;
	p.scheme_name = pc.schemes.find(data_space)->second;
	why = DescribeFunction(pc, type_names, data_space, p);
	if (why.empty()) {
		why = PartitioningColumn(pc, space_key, base_index, t, p);
	}
	if (!why.empty()) {
		return FailPartitioning(t, why);
	}
	std::vector<PartitionBoundary> boundaries;
	const std::string boundary_reason = ReadBoundaries(pc, check, p, boundaries);
	std::vector<std::string> destinations;
	why = ReadDestinations(pc, data_space, p, destinations);
	if (why.empty()) {
		why = PartitionsNumbered(t, p);
	}
	if (!why.empty()) {
		return FailPartitioning(t, why);
	}
	p.destinations = std::move(destinations);
	if (!boundary_reason.empty()) {
		p.reason = boundary_reason;	 // the boundaries alone are not given
		return;
	}
	p.boundaries = std::move(boundaries);
}

namespace {

bool ByName(const std::string &as, const std::string &an, const std::string &bs, const std::string &bn) {
	return as != bs ? as < bs : an < bn;
}

}  // namespace

bool IsTable(const SysObject &o) {
	return o.type == "U " || o.type == "S " || o.type == "IT";
}

bool IsUserView(const SysObject &o) {
	return o.type == "V " && !(o.status & OBJECT_MS_SHIPPED);
}

bool IsReadableView(const SysObject &o) {
	return IsUserView(o) && !(o.status & VIEW_ENCRYPTED);
}

bool IsUserDefault(const SysObject &o) {
	return o.type == "D " && !(o.status & OBJECT_MS_SHIPPED);
}

ObjectValueFilter::ObjectValueFilter(const BootstrapTables &tables, std::set<int32_t> partition_functions)
	: partition_functions_(std::move(partition_functions)) {
	std::set<int32_t> tables_of;
	for (const auto &o : tables.objects) {
		if (IsReadableView(o) || IsUserDefault(o)) {
			definitions_.insert(o.id);
		}
		if (IsTable(o)) {
			tables_of.insert(o.id);
		}
	}
	for (const auto &c : tables.columns) {
		if (c.number == 0 && (c.status & COLUMN_COMPUTED) && tables_of.count(c.object_id)) {
			computed_.insert(std::make_pair(c.object_id, c.column_id));
		}
	}
}

bool ObjectValueFilter::Keep(int64_t valclass, int32_t object_id, int64_t sub_id, int64_t valnum) const {
	switch (valclass) {
	case 1:
		return sub_id == 0 && valnum == 0 && definitions_.count(object_id) > 0;
	case 2:
		return valnum == 0 && computed_.count(std::make_pair(object_id, static_cast<int32_t>(sub_id))) > 0;
	case 30:
		return partition_functions_.count(object_id) > 0;
	default:
		return false;
	}
}

std::string SqlTypeText(const std::string &type_name, const std::string &system_type_name, uint8_t system_type_id,
						int16_t max_length, uint8_t precision, uint8_t scale) {
	if (type_name.empty()) {
		// No sysscalartypes row of its own (the column reads by its system type): named by that.
		return system_type_name.empty()
				   ? std::string()
				   : SqlTypeText(system_type_name, system_type_name, system_type_id, max_length, precision, scale);
	}
	if (system_type_name.empty() || type_name != system_type_name) {
		return type_name;
	}
	switch (system_type_id) {
	case 165:  // varbinary
	case 167:  // varchar
	case 173:  // binary
	case 175:  // char
		return type_name + "(" + (max_length < 0 ? std::string("max") : std::to_string(max_length)) + ")";
	case 231:  // nvarchar
	case 239:  // nchar
		return type_name + "(" + (max_length < 0 ? std::string("max") : std::to_string(max_length / 2)) + ")";
	case 106:  // decimal
	case 108:  // numeric
		return type_name + "(" + std::to_string(precision) + "," + std::to_string(scale) + ")";
	case 41:  // time
	case 42:  // datetime2
	case 43:  // datetimeoffset
		return type_name + "(" + std::to_string(scale) + ")";
	default:
		return type_name;
	}
}

std::string ModuleText(const std::string &bytes, int64_t flags, const std::string &what) {
	if (flags & MODULE_TEXT_LATIN1) {
		return Utf8OfLatin1(bytes);
	}
	if (bytes.size() % 2) {
		throw FormatError("backup: " + what + " is stored as UTF-16 with an odd number of bytes");
	}
	return Utf16LeToUtf8(reinterpret_cast<const uint8_t *>(bytes.data()), bytes.size(), false);
}

std::string ReadModuleDefinition(const PageStore &store, const ModuleDefinition &definition, const std::string &what) {
	switch (definition.state) {
	case ModuleDefinition::State::Ready:
		return definition.text;
	case ModuleDefinition::State::Deferred:
		return ModuleText(ReadLobValue(store, definition.root, definition.lob_unit, what, MAX_MODULE_TEXT_BYTES),
						  definition.flags, what);
	case ModuleDefinition::State::Unreadable:
		throw FormatError(definition.reason);
	default:
		throw FormatError("backup: " + what + " has no text in the catalog");
	}
}

const CatalogTable *CatalogModel::FindTable(const std::string &schema, const std::string &name) const {
	auto it = std::lower_bound(tables.begin(), tables.end(), std::make_pair(schema, name),
							   [](const CatalogTable &t, const std::pair<std::string, std::string> &key) {
								   return ByName(t.schema_name, t.name, key.first, key.second);
							   });
	return it != tables.end() && it->schema_name == schema && it->name == name ? &*it : nullptr;
}

const CatalogTable *CatalogModel::FindIndexedView(const std::string &schema, const std::string &name) const {
	auto it = std::lower_bound(indexed_views.begin(), indexed_views.end(), std::make_pair(schema, name),
							   [](const CatalogTable &t, const std::pair<std::string, std::string> &key) {
								   return ByName(t.schema_name, t.name, key.first, key.second);
							   });
	return it != indexed_views.end() && it->schema_name == schema && it->name == name ? &*it : nullptr;
}

const CatalogView *CatalogModel::FindView(const std::string &schema, const std::string &name) const {
	auto it = std::lower_bound(views.begin(), views.end(), std::make_pair(schema, name),
							   [](const CatalogView &v, const std::pair<std::string, std::string> &key) {
								   return ByName(v.schema_name, v.name, key.first, key.second);
							   });
	return it != views.end() && it->schema_name == schema && it->name == name ? &*it : nullptr;
}

std::string CatalogModel::SchemaName(int32_t id) const {
	auto it = std::lower_bound(schemas.begin(), schemas.end(), id,
							   [](const CatalogSchema &s, int32_t key) { return s.id < key; });
	return it != schemas.end() && it->id == id ? it->name : std::string();
}

namespace {

std::string AsciiLower(const std::string &s) {
	std::string out = s;
	for (auto &ch : out) {
		if (ch >= 'A' && ch <= 'Z') {
			ch = static_cast<char>(ch - 'A' + 'a');
		}
	}
	return out;
}

}  // namespace

void AssignDuckDBNames(std::vector<CatalogColumn> &columns) {
	// DuckDB compares names case-insensitively (ASCII). Every column's own name is reserved first, so a
	// generated name never takes a real column's: (Id, ID, ID_1) reads as Id, ID_2, ID_1.
	std::set<std::string> reserved;
	for (const auto &c : columns) {
		reserved.insert(AsciiLower(c.name));
	}
	std::set<std::string> used;
	for (auto &c : columns) {
		std::string lower = AsciiLower(c.name);
		c.duckdb_name = c.name;
		if (used.insert(lower).second) {
			continue;
		}
		for (size_t k = 1;; k++) {
			std::string candidate = c.name + "_" + std::to_string(k);
			std::string candidate_lower = AsciiLower(candidate);
			if (!reserved.count(candidate_lower) && used.insert(candidate_lower).second) {
				c.duckdb_name = std::move(candidate);
				break;
			}
		}
	}
}

namespace {

//! What a table's rowsets say of it, gathered as they are read.
struct RowsetSummary {
	bool heap = false;
	bool columnstore = false;
	std::map<int32_t, int64_t> delta_rows;	// a columnstore's delta store rows, by partition
	std::unordered_map<uint64_t, const SysRowset *> rowset_of;
};

//! The catalog model built from the base tables: what every object's description reads from (the schemas,
//! the types, the module texts, the partition catalog, the added columns' defaults) read once, then one
//! table or view at a time.
class ModelBuilder {
public:
	ModelBuilder(const PageStore &store, const BootstrapTables &tables, const BoundaryCheck &check,
				 const CatalogScope &scope)
		: store_(store), tables_(tables), check_(check), scope_(scope) {}
	CatalogModel Build();

private:
	void ReadSchemas(const SystemTable &classes);
	void ReadTypes();
	void ReadTexts();
	void ReadDefaults();
	std::string SchemaOf(const SysObject &o, std::string &reason) const;
	CatalogColumn DescribeColumn(const SysObject &o, const SysColumn &c);
	void AddRowset(const SysRowset &r, CatalogTable &t, RowsetSummary &summary) const;
	CatalogTable Describe(const SysObject &o, const std::string &schema_name, const std::string &schema_reason);
	CatalogView DescribeView(const SysObject &o, std::string schema_name, std::string schema_reason) const;

	const PageStore &store_;
	const BootstrapTables &tables_;
	const BoundaryCheck &check_;
	const CatalogScope &scope_;
	CatalogModel model_;
	std::unordered_map<int32_t, std::string> type_names_;
	std::unordered_map<int32_t, int32_t> type_schemas_;	 // a type's schema id (sysscalartypes.schid)
	std::map<int32_t, std::vector<const SysColumn *>> columns_of_;
	// A Pending page of sysobjvalues / sysseobjvalues may hold any of their rows: what they define is named
	// as unreadable, on the objects that need it, instead of failing the whole model (D13). The base
	// tables the model is built from (bootstrap, sysclsobjs, ...) still fail it: they are read whole.
	std::map<ModuleKey, ModuleDefinition> texts_;
	std::string texts_pending_;
	PartitionCatalog partition_catalog_;
	AddedDefaults defaults_;
};

void ModelBuilder::ReadSchemas(const SystemTable &classes) {
	const size_t cls = classes.Column("class"), id = classes.Column("id"), name = classes.Column("name");
	for (size_t r = 0; r < classes.rows.size(); r++) {
		if (classes.Int(r, cls) == CLASS_SCHEMA) {
			CatalogSchema s;
			s.id = static_cast<int32_t>(classes.Int(r, id));
			s.name = classes.Text(r, name);
			model_.schemas.push_back(std::move(s));
		}
	}
	std::sort(model_.schemas.begin(), model_.schemas.end(),
			  [](const CatalogSchema &a, const CatalogSchema &b) { return a.id < b.id; });
}

void ModelBuilder::ReadTypes() {
	const SystemTable types = ReadSystemTable(store_, tables_, "sysscalartypes");
	const size_t id = types.Column("id"), name = types.Column("name"), schid = types.Column("schid");
	for (size_t r = 0; r < types.rows.size(); r++) {
		type_names_[static_cast<int32_t>(types.Int(r, id))] = types.Text(r, name);
		type_schemas_[static_cast<int32_t>(types.Int(r, id))] = static_cast<int32_t>(types.Int(r, schid));
	}
}

void ModelBuilder::ReadTexts() {
	try {
		texts_ = ReadModuleTexts(store_, tables_);
	} catch (PendingPageError &e) {
		texts_.clear();
		texts_pending_ = std::string("sysobjvalues cannot be read: ") + e.what();
	}
}

void ModelBuilder::ReadDefaults() {
	try {
		defaults_.values = ReadAddedDefaults(store_, tables_, defaults_.ambiguous);
	} catch (PendingPageError &e) {
		defaults_.values.clear();
		defaults_.ambiguous.clear();
		defaults_.pending = std::string("sysseobjvalues cannot be read: ") + e.what();
	}
}

//! The object's schema; one sysclsobjs does not list is named on the object, not the model.
std::string ModelBuilder::SchemaOf(const SysObject &o, std::string &reason) const {
	const std::string name = model_.SchemaName(o.schema_id);
	if (!name.empty()) {
		return name;
	}
	reason = "its schema " + std::to_string(o.schema_id) + " is not in sysclsobjs";
	return "schema " + std::to_string(o.schema_id);
}

CatalogColumn ModelBuilder::DescribeColumn(const SysObject &o, const SysColumn &c) {
	CatalogColumn col;
	col.column_id = c.column_id;
	col.name = c.name;
	col.system_type_id = c.xtype;
	col.user_type_id = c.user_type;
	auto type = type_names_.find(c.user_type);
	// A type with no sysscalartypes row keeps an empty name: the column is read by its xtype, or as
	// its stored bytes (ReadingOf), never refused by its type (owner, 2026-11-11).
	if (type != type_names_.end()) {
		col.type_name = type->second;
		col.type_schema = model_.SchemaName(type_schemas_[c.user_type]);
	}
	// A CLR type (xtype 240) has no row of its xtype: the system ones are known by their user type.
	const int32_t system_id = c.xtype == 240 ? (c.user_type < 256 ? c.user_type : -1) : c.xtype;
	auto system = type_names_.find(system_id);
	col.system_type_name = system == type_names_.end() ? std::string() : system->second;
	col.max_length = c.length;
	col.precision = c.precision;
	col.scale = c.scale;
	col.collation_id = c.collation_id;
	col.is_nullable = !(c.status & COLUMN_NOT_NULL);
	col.is_ansi_padded = (c.status & COLUMN_ANSI_PADDED) != 0;
	col.is_identity = (c.status & COLUMN_IDENTITY) != 0;
	col.is_rowguidcol = (c.status & COLUMN_ROWGUIDCOL) != 0;
	col.is_computed = (c.status & COLUMN_COMPUTED) != 0;
	col.is_persisted = (c.status & COLUMN_PERSISTED) != 0;
	col.is_sparse = (c.status & COLUMN_SPARSE) != 0;
	col.is_column_set = (c.status & COLUMN_SET) != 0;
	if (col.is_computed && !texts_pending_.empty()) {
		col.definition = Unreadable(texts_pending_);
	} else if (col.is_computed) {
		auto text = texts_.find(ModuleKey{2, o.id, c.column_id});
		if (text != texts_.end()) {
			col.definition = text->second;
		}
	}
	return col;
}

//! Rowset `r` of the table: its allocation units, and for its base rowset (index 0 or 1 with an in-row
//! unit) its partition, its delta store's rows and its share of the row count.
void ModelBuilder::AddRowset(const SysRowset &r, CatalogTable &t, RowsetSummary &summary) const {
	for (const SysAllocUnit *a : tables_.UnitsOf(r.rowset_id)) {
		CatalogUnit u;
		u.unit = *a;
		u.index_id = r.index_id;
		u.partition_number = r.partition_number;
		u.compression = r.compression;
		t.units.push_back(u);
	}
	const SysAllocUnit *in_row = r.index_id == 0 || r.index_id == 1 ? tables_.FindInRowUnit(r.rowset_id) : nullptr;
	if (!in_row) {
		return;	 // another index, or a deferred drop
	}
	summary.heap = summary.heap || r.index_id == 0;
	summary.columnstore = summary.columnstore || r.compression >= 3;
	if (r.owner_type == OWNER_PARTITION) {
		summary.rowset_of[r.rowset_id] = &r;
		CatalogPartition p;
		p.rowset_id = r.rowset_id;
		p.partition_number = r.partition_number;
		AddRowCount(p.rows, r.rows);
		p.compression = r.compression;
		p.filegroup_id = in_row->filegroup_id;
		auto fg = partition_catalog_.filegroups.find(in_row->filegroup_id);
		p.filegroup = fg == partition_catalog_.filegroups.end() ? std::string() : fg->second;
		t.partitions.push_back(std::move(p));
	}
	if (r.owner_type == OWNER_DELTA_STORE) {
		AddRowCount(summary.delta_rows[r.partition_number], r.rows);
	}
	if (r.owner_type == OWNER_PARTITION || r.owner_type == OWNER_DELTA_STORE) {
		// The file's counts, a statistic: a negative one (an upgraded database can hold one until
		// DBCC UPDATEUSAGE), or a sum past int64, leaves the count unknown -- never UB, and the
		// table's rows are read all the same.
		if (t.row_count_known && (r.rows < 0 || r.rows > std::numeric_limits<int64_t>::max() - t.row_count)) {
			t.row_count_known = false;
			t.row_count = 0;
		} else if (t.row_count_known) {
			t.row_count += r.rows;
		}
	}
}

//! A table, or an indexed view's stored rows (task 16f: its clustered index is a rowset of the view's
//! object, laid out as a clustered table's): its columns, allocation units, partitions, shape,
//! partitioning, and whether the scan reads it.
CatalogTable ModelBuilder::Describe(const SysObject &o, const std::string &schema_name,
									const std::string &schema_reason) {
	CatalogTable t;
	t.object_id = o.id;
	t.schema_id = o.schema_id;
	t.schema_name = schema_name;
	t.reason = schema_reason;
	t.name = o.name;
	t.type = o.type == "IT" ? "IT" : o.type.substr(0, 1);
	t.is_ms_shipped = (o.status & OBJECT_MS_SHIPPED) != 0;
	t.parent_object_id = o.type == "IT" ? o.parent_id : 0;
	t.created = o.created;
	t.modified = o.modified;
	std::vector<const SysColumn *> &cols = columns_of_[o.id];
	std::sort(cols.begin(), cols.end(),
			  [](const SysColumn *a, const SysColumn *b) { return a->column_id < b->column_id; });
	for (const SysColumn *c : cols) {
		t.columns.push_back(DescribeColumn(o, *c));
	}
	AssignDuckDBNames(t.columns);
	RowsetSummary summary;
	for (const SysRowset *r : tables_.RowsetsOf(o.id)) {
		AddRowset(*r, t, summary);
	}
	for (auto &p : t.partitions) {
		auto delta = summary.delta_rows.find(p.partition_number);
		if (delta == summary.delta_rows.end()) {
			continue;
		}
		if (delta->second < 0) {
			p.rows = -1;
		} else {
			AddRowCount(p.rows, delta->second);
		}
	}
	std::sort(t.partitions.begin(), t.partitions.end(), [](const CatalogPartition &a, const CatalogPartition &b) {
		return a.partition_number < b.partition_number;
	});
	t.shape = summary.columnstore ? TableShape::Columnstore : summary.heap ? TableShape::Heap : TableShape::Clustered;
	DescribePartitioning(partition_catalog_, type_names_, summary.heap ? 0 : 1, check_, t);
	DecideDecodable(tables_, defaults_, summary.rowset_of, t);
	return t;
}

CatalogView ModelBuilder::DescribeView(const SysObject &o, std::string schema_name, std::string schema_reason) const {
	CatalogView v;
	v.object_id = o.id;
	v.schema_id = o.schema_id;
	v.schema_name = std::move(schema_name);
	v.reason = std::move(schema_reason);
	v.name = o.name;
	v.created = o.created;
	v.modified = o.modified;
	v.is_encrypted = (o.status & VIEW_ENCRYPTED) != 0;
	v.is_ms_shipped = (o.status & OBJECT_MS_SHIPPED) != 0;
	auto text = texts_.find(ModuleKey{1, o.id, 0});
	if (!v.is_encrypted && !texts_pending_.empty()) {
		v.definition = Unreadable(texts_pending_);
	} else if (!v.is_encrypted && text != texts_.end()) {
		v.definition = text->second;
	}
	// An indexed view (task 16f): a unique clustered index on the view stores its rows, a rowset
	// of index 1 of the view's object with its in-row unit (none: a deferred drop, as for a table).
	for (const SysRowset *r : tables_.RowsetsOf(o.id)) {
		v.indexed = v.indexed || (r->index_id == 1 && tables_.FindInRowUnit(r->rowset_id));
	}
	return v;
}

CatalogModel ModelBuilder::Build() {
	const SystemTable classes = ReadSystemTable(store_, tables_, "sysclsobjs");
	ReadSchemas(classes);
	ReadTypes();
	for (const auto &c : tables_.columns) {
		if (c.number == 0) {
			columns_of_[c.object_id].push_back(&c);
		}
	}
	std::vector<const SysObject *> table_objects, views;
	for (const auto &o : tables_.objects) {
		// A negative id is the resource database's (sys.trace_xe_event_map, ...): listed in sysschobjs, its rows
		// are in mssqlsystemresource, which a database's backup does not hold (no rowset of it in sysrowsets).
		if (IsTable(o) && o.id >= 0) {
			table_objects.push_back(&o);
		} else if (IsUserView(o)) {
			views.push_back(&o);
		}
	}
	ReadTexts();
	partition_catalog_ = ReadPartitionCatalog(store_, tables_, classes);
	ReadDefaults();
	for (const SysObject *o : table_objects) {
		std::string schema_reason;
		std::string schema_name = SchemaOf(*o, schema_reason);
		if (!scope_.WantsTable(schema_name, o->name)) {
			continue;
		}
		model_.tables.push_back(Describe(*o, schema_name, schema_reason));
	}
	for (const SysObject *o : views) {
		std::string schema_reason;
		std::string schema_name = SchemaOf(*o, schema_reason);
		if (!scope_.WantsView(schema_name, o->name)) {
			continue;
		}
		CatalogView v = DescribeView(*o, std::move(schema_name), std::move(schema_reason));
		if (v.indexed) {
			model_.indexed_views.push_back(Describe(*o, v.schema_name, v.reason));
		}
		model_.views.push_back(std::move(v));
	}
	std::sort(model_.tables.begin(), model_.tables.end(), [](const CatalogTable &a, const CatalogTable &b) {
		return ByName(a.schema_name, a.name, b.schema_name, b.name);
	});
	std::sort(model_.views.begin(), model_.views.end(), [](const CatalogView &a, const CatalogView &b) {
		return ByName(a.schema_name, a.name, b.schema_name, b.name);
	});
	std::sort(model_.indexed_views.begin(), model_.indexed_views.end(),
			  [](const CatalogTable &a, const CatalogTable &b) {
				  return ByName(a.schema_name, a.name, b.schema_name, b.name);
			  });
	return std::move(model_);
}

}  // namespace

CatalogModel BuildCatalogModel(const PageStore &store, const BootstrapTables &tables, const BoundaryCheck &check,
							   const CatalogScope &scope) {
	ModelBuilder builder(store, tables, check, scope);
	return builder.Build();
}

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb
