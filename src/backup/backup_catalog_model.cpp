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

// The texts the model keeps: the rows of valclass 1 and 2 the audited ObjectValueFilter lets leave (DQ-11),
// so the model and mssql_backup_catalog keep one rule.
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

// One partition's layout, its added columns' defaults filled in; false with `why`.
bool PartitionLayout(const BootstrapTables &tables, const std::map<std::pair<uint64_t, int32_t>, std::string> &defaults,
					 const std::set<std::pair<uint64_t, int32_t>> &ambiguous, const std::string &defaults_pending,
					 const std::map<int32_t, const CatalogColumn *> &by_id, const CatalogTable &t,
					 const SysRowset &rowset, CatalogPartition &partition, std::string &why) {
	try {
		partition.layout = tables.LayoutOf(t.object_id, rowset);
	} catch (FormatError &e) {
		why = std::string("its physical layout: ") + e.what();
		return false;
	}
	for (auto &physical : partition.layout.columns) {
		if (physical.Dropped() || physical.Uniquifier() || !physical.HasDefaultStatus()) {
			continue;
		}
		auto column = by_id.find(physical.rscolid);
		if (column == by_id.end()) {
			why = "physical column " + std::to_string(physical.rscolid) + " has no column";
			return false;
		}
		if (!defaults_pending.empty()) {
			why = "column " + column->second->name + " was added with a default, and " + defaults_pending;
			return false;
		}
		if (!AddedDefault(defaults, ambiguous, partition.layout, physical, *column->second, physical.default_bytes,
						  why)) {
			return false;
		}
		physical.has_default = true;
	}
	return true;
}

void DecideDecodable(const BootstrapTables &tables, const std::map<std::pair<uint64_t, int32_t>, std::string> &defaults,
					 const std::set<std::pair<uint64_t, int32_t>> &ambiguous, const std::string &defaults_pending,
					 const std::unordered_map<uint64_t, const SysRowset *> &rowsets, CatalogTable &t) {
	t.decodable = false;
	if (!t.reason.empty()) {
		return;	 // already refused (its schema or a column's type)
	}
	if (t.partitions.empty()) {
		t.reason = "it has no live base rowset";
		return;
	}
	if (t.shape == TableShape::Columnstore) {
		t.reason = "a clustered columnstore index is not read (spec 088)";
		return;
	}
	for (const auto &p : t.partitions) {
		if (p.compression == 1 || p.compression == 2) {
			t.reason = std::string(p.compression == 1 ? "ROW" : "PAGE") + " compression is not read (spec 086" +
					   (t.partitions.size() > 1 ? ", partition " + std::to_string(p.partition_number) : "") + ")";
			return;
		}
		if (p.compression != 0) {
			t.reason = "compression level " + std::to_string(p.compression) + " is not read";
			return;
		}
	}
	std::map<int32_t, const CatalogColumn *> by_id;
	for (const auto &c : t.columns) {
		if (c.is_sparse || c.is_column_set) {
			t.reason = "sparse columns are not read (spec 086)";
			return;
		}
		by_id[c.column_id] = &c;
	}
	for (auto &p : t.partitions) {
		std::string why;
		if (!PartitionLayout(tables, defaults, ambiguous, defaults_pending, by_id, t, *rowsets.at(p.rowset_id), p,
							 why)) {
			t.reason = t.partitions.size() > 1 ? "partition " + std::to_string(p.partition_number) + ": " + why : why;
			return;
		}
	}
	t.layout = t.partitions.front().layout;
	t.decodable = true;
}

// The base tables partitioning is read from (E7), each read through its own description.
struct PartitionCatalog {
	struct Function {
		std::string name;
		bool range_right = false;
		int32_t fanout = 0;
	};
	struct Parameter {
		uint8_t xtype = 0;
		int32_t user_type = 0;
		int16_t length = 0;
		uint8_t precision = 0, scale = 0;
		uint32_t collation = 0;
	};
	std::map<int32_t, Function> functions;							  // sysclsobjs class 30
	std::map<int32_t, Parameter> parameters;						  // systypedsubobjs class 30, subid 1
	std::map<int32_t, std::map<int32_t, std::string>> boundaries;	  // sysobjvalues valclass 30
	std::set<std::pair<int32_t, int32_t>> boundaries_twice;			  // (function, boundary id)
	std::map<int32_t, std::string> filegroups, schemes;				  // sysclsobjs class 31
	std::map<int32_t, int32_t> scheme_function;						  // syssingleobjrefs class 31
	std::map<int32_t, std::map<int32_t, int32_t>> destinations;		  // class 30: scheme -> dest -> fg
	std::map<std::pair<int32_t, int32_t>, int32_t> data_space;		  // class 7: (object, index) -> space
	std::map<std::pair<int32_t, int32_t>, int32_t> partition_column;  // sysiscols, partition ordinal 1
	std::set<int32_t> schemes_twice;								  // syssingleobjrefs class 31 listed twice
	std::set<std::pair<int32_t, int32_t>> destinations_twice, data_space_twice;
	std::set<int32_t> boundaries_off_row;  // functions with a boundary stored off-row
	std::string reason;					   // a table of these could not be read: every partitioned table's reason
};

PartitionCatalog ReadPartitionCatalog(const PageStore &store, const BootstrapTables &tables) {
	PartitionCatalog out;
	try {
		const SystemTable classes = ReadSystemTable(store, tables, "sysclsobjs");
		const size_t cls = classes.Column("class"), id = classes.Column("id"), name = classes.Column("name"),
					 status = classes.Column("status"), type = classes.Column("type"),
					 intprop = classes.Column("intprop");
		for (size_t r = 0; r < classes.rows.size(); r++) {
			const int64_t c = classes.Int(r, cls);
			const int32_t i = static_cast<int32_t>(classes.Int(r, id));
			if (c == 30) {
				PartitionCatalog::Function f;
				f.name = classes.Text(r, name);
				f.range_right = (classes.Int(r, status) & 1) != 0;
				f.fanout = static_cast<int32_t>(classes.Int(r, intprop));
				out.functions[i] = f;
			} else if (c == 31) {
				// A partition scheme is 'PS'; filegroups ('FG') and the other data spaces (FILESTREAM 'FD',
				// memory-optimized 'FX') hold a partition's pages.
				const std::string t = classes.Text(r, type);
				(t == "PS" ? out.schemes : out.filegroups)[i] = classes.Text(r, name);
			}
		}
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
			out.parameters[static_cast<int32_t>(typed.Int(r, major))] = p;
		}
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
		const SystemTable iscols = ReadSystemTable(store, tables, "sysiscols");
		const size_t im = iscols.Column("idmajor"), in = iscols.Column("idminor"), ip = iscols.Column("intprop"),
					 t2 = iscols.Column("tinyprop2");
		for (size_t r = 0; r < iscols.rows.size(); r++) {
			if (iscols.Int(r, t2) == 1) {
				out.partition_column[std::make_pair(static_cast<int32_t>(iscols.Int(r, im)),
													static_cast<int32_t>(iscols.Int(r, in)))] =
					static_cast<int32_t>(iscols.Int(r, ip));
			}
		}
		const SysObject *object = tables.SystemObject("sysobjvalues");
		if (!object) {
			throw FormatError("backup: the database has no system table sysobjvalues");
		}
		RowsetLayout layout = tables.BaseLayout(object->id);
		layout.table = "sysobjvalues";
		const size_t vc = layout.IndexOf("valclass"), oid = layout.IndexOf("objid"), sid = layout.IndexOf("subobjid"),
					 vn = layout.IndexOf("valnum"), value = layout.IndexOf("value");
		ForEachRowsetRow(store, tables.InRowUnit(layout.rowset_id, "sysobjvalues"), layout, [&](const SystemRow &row) {
			if (ValueInt(layout, vc, row.values[vc]) != 30 || ValueInt(layout, vn, row.values[vn]) != 1 ||
				row.values[value].is_null) {
				return;
			}
			const int32_t f = static_cast<int32_t>(ValueInt(layout, oid, row.values[oid]));
			const int32_t b = static_cast<int32_t>(ValueInt(layout, sid, row.values[sid]));
			if (row.values[value].off_row) {
				out.boundaries_off_row.insert(f);  // a sql_variant is never off-row: refused for its function
				return;
			}
			if (!out.boundaries[f].emplace(b, row.values[value].bytes).second) {
				out.boundaries_twice.insert(std::make_pair(f, b));
			}
		});
	} catch (FormatError &e) {
		out.reason = std::string("the partitioning tables: ") + e.what();
	}
	return out;
}

// Table `t`'s partitioning from `pc`, or its reason (D13). `base_index` is its heap's 0 or clustered 1.
void DescribePartitioning(const PartitionCatalog &pc, const std::unordered_map<int32_t, std::string> &type_names,
						  int32_t base_index, CatalogTable &t) {
	const auto space_key = std::make_pair(t.object_id, base_index);
	auto space = pc.data_space.find(space_key);
	const int32_t data_space = space == pc.data_space.end() ? 1 : space->second;  // no row: PRIMARY
	auto scheme = pc.schemes.find(data_space);
	if (scheme == pc.schemes.end()) {
		if (!pc.reason.empty() && t.partitions.size() > 1) {
			t.partitioned = true;
			t.partitioning.reason = pc.reason;
		}
		return;	 // on a filegroup
	}
	t.partitioned = true;
	CatalogPartitioning &p = t.partitioning;
	p.scheme_id = data_space;
	p.scheme_name = scheme->second;
	auto fail = [&](const std::string &why) { p.reason = why; };
	if (!pc.reason.empty()) {
		return fail(pc.reason);
	}
	if (pc.data_space_twice.count(space_key) || pc.schemes_twice.count(data_space)) {
		return fail("syssingleobjrefs lists its index's data space or its scheme's function twice");
	}
	auto fn_id = pc.scheme_function.find(data_space);
	if (fn_id == pc.scheme_function.end()) {
		return fail("scheme " + p.scheme_name + " names no partition function");
	}
	p.function_id = fn_id->second;
	auto fn = pc.functions.find(p.function_id);
	auto param = pc.parameters.find(p.function_id);
	if (fn == pc.functions.end() || param == pc.parameters.end()) {
		return fail("scheme " + p.scheme_name + "'s partition function " + std::to_string(p.function_id) +
					" is not in the catalog");
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
	p.parameter_type_name = type == type_names.end() ? std::string() : type->second;
	auto system = type_names.find(param->second.xtype);
	p.parameter_system_type_name = system == type_names.end() ? std::string() : system->second;
	auto column = pc.partition_column.find(std::make_pair(t.object_id, base_index));
	if (column == pc.partition_column.end()) {
		return fail("its index " + std::to_string(base_index) + " has no partitioning column in sysiscols");
	}
	p.column_id = column->second;
	for (const auto &c : t.columns) {
		if (c.column_id == p.column_id) {
			p.column_name = c.name;
		}
	}
	if (p.column_name.empty()) {
		return fail("its partitioning column " + std::to_string(p.column_id) + " is not one of its columns");
	}
	if (pc.boundaries_off_row.count(p.function_id)) {
		return fail("function " + p.function_name + " has a boundary stored off-row");
	}
	if (p.fanout < 1 || p.fanout > 15000) {
		return fail("function " + p.function_name + " has a fanout of " + std::to_string(p.fanout));
	}
	// The boundaries: ids 1..fanout-1, each once, each a value of the parameter's own type.
	auto values = pc.boundaries.find(p.function_id);
	const size_t count = values == pc.boundaries.end() ? 0 : values->second.size();
	if (count != static_cast<size_t>(p.fanout - 1)) {
		return fail("function " + p.function_name + " has " + std::to_string(count) + " boundaries, its fanout " +
					std::to_string(p.fanout) + " says " + std::to_string(p.fanout - 1));
	}
	for (int32_t b = 1; b < p.fanout; b++) {
		auto v = values->second.find(b);
		if (v == values->second.end() || pc.boundaries_twice.count(std::make_pair(p.function_id, b))) {
			return fail("function " + p.function_name + "'s boundary " + std::to_string(b) +
						(v == values->second.end() ? " is missing" : " is listed twice"));
		}
		SqlVariant variant;
		try {
			variant = ParseSqlVariant(v->second, "boundary " + std::to_string(b) + " of " + p.function_name);
		} catch (FormatError &e) {
			return fail(e.what());
		}
		const bool scaled = variant.xtype == 41 || variant.xtype == 42 || variant.xtype == 43;
		const bool decimal = variant.xtype == 106 || variant.xtype == 108;
		if (variant.xtype != p.parameter_xtype || (scaled && variant.scale != p.parameter_scale) ||
			(decimal && (variant.precision != p.parameter_precision || variant.scale != p.parameter_scale))) {
			return fail("function " + p.function_name + "'s boundary " + std::to_string(b) + " is a value of xtype " +
						std::to_string(variant.xtype) + ", not of its parameter's type");
		}
		p.boundaries.push_back(v->second);
	}
	// The destinations: 1..fanout each a filegroup, and NEXT USED at fanout + 1 when there is one.
	auto dests = pc.destinations.find(data_space);
	for (int32_t d = 1; d <= p.fanout + 1; d++) {
		auto it = dests == pc.destinations.end() ? std::map<int32_t, int32_t>::const_iterator() : dests->second.find(d);
		const bool found = dests != pc.destinations.end() && it != dests->second.end();
		if (!found) {
			if (d <= p.fanout) {
				p.boundaries.clear();
				p.destinations.clear();
				return fail("scheme " + p.scheme_name + " has no destination " + std::to_string(d));
			}
			break;
		}
		if (pc.destinations_twice.count(std::make_pair(data_space, d))) {
			p.boundaries.clear();
			p.destinations.clear();
			return fail("scheme " + p.scheme_name + " lists destination " + std::to_string(d) + " twice");
		}
		auto fg = pc.filegroups.find(it->second);
		const std::string name = fg == pc.filegroups.end() ? "filegroup " + std::to_string(it->second) : fg->second;
		(d <= p.fanout ? p.destinations.push_back(name) : (void)(p.next_used = name));
	}
	// Its partitions: 1..fanout, each once (t.partitions is sorted by number).
	bool numbered = t.partitions.size() == static_cast<size_t>(p.fanout);
	for (size_t i = 0; numbered && i < t.partitions.size(); i++) {
		numbered = t.partitions[i].partition_number == static_cast<int32_t>(i + 1);
	}
	if (!numbered) {
		return fail(std::to_string(t.partitions.size()) + " partitions where function " + p.function_name + " has " +
					std::to_string(p.fanout) + " (1.." + std::to_string(p.fanout) + ", each once)");
	}
}

bool ByName(const std::string &as, const std::string &an, const std::string &bs, const std::string &bn) {
	return as != bs ? as < bs : an < bn;
}

}  // namespace

bool IsUserTable(const SysObject &o) {
	return o.type == "U " && !(o.status & OBJECT_MS_SHIPPED);
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
	std::set<int32_t> user_tables;
	for (const auto &o : tables.objects) {
		if (IsReadableView(o) || IsUserDefault(o)) {
			definitions_.insert(o.id);
		}
		if (IsUserTable(o)) {
			user_tables.insert(o.id);
		}
	}
	for (const auto &c : tables.columns) {
		if (c.number == 0 && (c.status & COLUMN_COMPUTED) && user_tables.count(c.object_id)) {
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

std::set<int32_t> ReadPartitionFunctions(const PageStore &store, const BootstrapTables &tables) {
	std::set<int32_t> out;
	const SystemTable classes = ReadSystemTable(store, tables, "sysclsobjs");
	const size_t cls = classes.Column("class"), id = classes.Column("id");
	for (size_t r = 0; r < classes.rows.size(); r++) {
		if (classes.Int(r, cls) == 30) {
			out.insert(static_cast<int32_t>(classes.Int(r, id)));
		}
	}
	return out;
}

std::string SqlTypeText(const std::string &type_name, const std::string &system_type_name, uint8_t system_type_id,
						int16_t max_length, uint8_t precision, uint8_t scale) {
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

std::string CatalogModel::SchemaName(int32_t id) const {
	auto it = std::lower_bound(schemas.begin(), schemas.end(), id,
							   [](const CatalogSchema &s, int32_t key) { return s.id < key; });
	return it != schemas.end() && it->id == id ? it->name : std::string();
}

CatalogModel BuildCatalogModel(const PageStore &store, const BootstrapTables &tables) {
	CatalogModel model;
	{
		const SystemTable classes = ReadSystemTable(store, tables, "sysclsobjs");
		const size_t cls = classes.Column("class"), id = classes.Column("id"), name = classes.Column("name");
		for (size_t r = 0; r < classes.rows.size(); r++) {
			if (classes.Int(r, cls) == CLASS_SCHEMA) {
				CatalogSchema s;
				s.id = static_cast<int32_t>(classes.Int(r, id));
				s.name = classes.Text(r, name);
				model.schemas.push_back(std::move(s));
			}
		}
		std::sort(model.schemas.begin(), model.schemas.end(),
				  [](const CatalogSchema &a, const CatalogSchema &b) { return a.id < b.id; });
	}
	std::unordered_map<int32_t, std::string> type_names;
	{
		const SystemTable types = ReadSystemTable(store, tables, "sysscalartypes");
		const size_t id = types.Column("id"), name = types.Column("name");
		for (size_t r = 0; r < types.rows.size(); r++) {
			type_names[static_cast<int32_t>(types.Int(r, id))] = types.Text(r, name);
		}
	}
	std::map<int32_t, std::vector<const SysColumn *>> columns_of;
	for (const auto &c : tables.columns) {
		if (c.number == 0) {
			columns_of[c.object_id].push_back(&c);
		}
	}
	std::vector<const SysObject *> user_tables, views;
	for (const auto &o : tables.objects) {
		if (IsUserTable(o)) {
			user_tables.push_back(&o);
		} else if (IsUserView(o)) {
			views.push_back(&o);
		}
	}
	// A Pending page of sysobjvalues / sysseobjvalues may hold any of their rows: what they define is named
	// as unreadable, on the objects that need it, instead of failing the whole model (D13). The base
	// tables the model is built from (bootstrap, sysclsobjs, ...) still fail it: they are read whole.
	std::map<ModuleKey, ModuleDefinition> texts;
	std::string texts_pending;
	try {
		texts = ReadModuleTexts(store, tables);
	} catch (PendingPageError &e) {
		texts.clear();
		texts_pending = std::string("sysobjvalues cannot be read: ") + e.what();
	}
	const PartitionCatalog partition_catalog = ReadPartitionCatalog(store, tables);
	std::set<std::pair<uint64_t, int32_t>> ambiguous;
	std::map<std::pair<uint64_t, int32_t>, std::string> defaults;
	std::string defaults_pending;
	try {
		defaults = ReadAddedDefaults(store, tables, ambiguous);
	} catch (PendingPageError &e) {
		defaults.clear();
		ambiguous.clear();
		defaults_pending = std::string("sysseobjvalues cannot be read: ") + e.what();
	}
	// The object's schema; one sysclsobjs does not list is named on the object, not the model.
	auto schema_of = [&](const SysObject &o, std::string &reason) {
		const std::string name = model.SchemaName(o.schema_id);
		if (!name.empty()) {
			return name;
		}
		reason = "its schema " + std::to_string(o.schema_id) + " is not in sysclsobjs";
		return "schema " + std::to_string(o.schema_id);
	};
	for (const SysObject *o : user_tables) {
		CatalogTable t;
		t.object_id = o->id;
		t.schema_id = o->schema_id;
		t.schema_name = schema_of(*o, t.reason);
		t.name = o->name;
		t.created = o->created;
		t.modified = o->modified;
		std::vector<const SysColumn *> &cols = columns_of[o->id];
		std::sort(cols.begin(), cols.end(),
				  [](const SysColumn *a, const SysColumn *b) { return a->column_id < b->column_id; });
		for (const SysColumn *c : cols) {
			CatalogColumn col;
			col.column_id = c->column_id;
			col.name = c->name;
			col.system_type_id = c->xtype;
			col.user_type_id = c->user_type;
			auto type = type_names.find(c->user_type);
			if (type != type_names.end()) {
				col.type_name = type->second;
			} else if (t.reason.empty()) {
				t.reason =
					"column " + c->name + "'s type " + std::to_string(c->user_type) + " is not in sysscalartypes";
			}
			// A CLR type (xtype 240) has no row of its xtype: the system ones are known by their user type.
			const int32_t system_id = c->xtype == 240 ? (c->user_type < 256 ? c->user_type : -1) : c->xtype;
			auto system = type_names.find(system_id);
			col.system_type_name = system == type_names.end() ? std::string() : system->second;
			col.max_length = c->length;
			col.precision = c->precision;
			col.scale = c->scale;
			col.collation_id = c->collation_id;
			col.is_nullable = !(c->status & COLUMN_NOT_NULL);
			col.is_identity = (c->status & COLUMN_IDENTITY) != 0;
			col.is_rowguidcol = (c->status & COLUMN_ROWGUIDCOL) != 0;
			col.is_computed = (c->status & COLUMN_COMPUTED) != 0;
			col.is_persisted = (c->status & COLUMN_PERSISTED) != 0;
			col.is_sparse = (c->status & COLUMN_SPARSE) != 0;
			col.is_column_set = (c->status & COLUMN_SET) != 0;
			if (col.is_computed && !texts_pending.empty()) {
				col.definition = Unreadable(texts_pending);
			} else if (col.is_computed) {
				auto text = texts.find(ModuleKey{2, o->id, c->column_id});
				if (text != texts.end()) {
					col.definition = text->second;
				}
			}
			t.columns.push_back(std::move(col));
		}
		bool heap = false, columnstore = false;
		std::map<int32_t, int64_t> delta_rows;	// a columnstore's delta store rows, by partition
		std::unordered_map<uint64_t, const SysRowset *> rowset_of;
		for (const SysRowset *r : tables.RowsetsOf(o->id)) {
			for (const SysAllocUnit *a : tables.UnitsOf(r->rowset_id)) {
				CatalogUnit u;
				u.unit = *a;
				u.index_id = r->index_id;
				u.partition_number = r->partition_number;
				u.compression = r->compression;
				t.units.push_back(u);
			}
			if ((r->index_id != 0 && r->index_id != 1) || !tables.FindInRowUnit(r->rowset_id)) {
				continue;  // another index, or a deferred drop
			}
			heap = heap || r->index_id == 0;
			columnstore = columnstore || r->compression >= 3;
			if (r->owner_type == OWNER_PARTITION) {
				rowset_of[r->rowset_id] = r;
				CatalogPartition p;
				p.rowset_id = r->rowset_id;
				p.partition_number = r->partition_number;
				AddRowCount(p.rows, r->rows);
				p.compression = r->compression;
				const SysAllocUnit *in_row = tables.FindInRowUnit(r->rowset_id);
				p.filegroup_id = in_row->filegroup_id;
				auto fg = partition_catalog.filegroups.find(in_row->filegroup_id);
				p.filegroup = fg == partition_catalog.filegroups.end() ? std::string() : fg->second;
				t.partitions.push_back(p);
			}
			if (r->owner_type == OWNER_DELTA_STORE) {
				AddRowCount(delta_rows[r->partition_number], r->rows);
			}
			if (r->owner_type == OWNER_PARTITION || r->owner_type == OWNER_DELTA_STORE) {
				// The file's counts, a statistic: a negative one (an upgraded database can hold one until
				// DBCC UPDATEUSAGE), or a sum past int64, leaves the count unknown -- never UB, and the
				// table's rows are read all the same.
				if (t.row_count_known && (r->rows < 0 || r->rows > std::numeric_limits<int64_t>::max() - t.row_count)) {
					t.row_count_known = false;
					t.row_count = 0;
				} else if (t.row_count_known) {
					t.row_count += r->rows;
				}
			}
		}
		for (auto &p : t.partitions) {
			const int64_t delta = delta_rows[p.partition_number];
			if (delta < 0) {
				p.rows = -1;
			} else {
				AddRowCount(p.rows, delta);
			}
		}
		std::sort(t.partitions.begin(), t.partitions.end(), [](const CatalogPartition &a, const CatalogPartition &b) {
			return a.partition_number < b.partition_number;
		});
		t.shape = columnstore ? TableShape::Columnstore : heap ? TableShape::Heap : TableShape::Clustered;
		DescribePartitioning(partition_catalog, type_names, heap ? 0 : 1, t);
		DecideDecodable(tables, defaults, ambiguous, defaults_pending, rowset_of, t);
		model.tables.push_back(std::move(t));
	}
	for (const SysObject *o : views) {
		CatalogView v;
		v.object_id = o->id;
		v.schema_id = o->schema_id;
		v.schema_name = schema_of(*o, v.reason);
		v.name = o->name;
		v.created = o->created;
		v.modified = o->modified;
		v.is_encrypted = (o->status & VIEW_ENCRYPTED) != 0;
		auto text = texts.find(ModuleKey{1, o->id, 0});
		if (!v.is_encrypted && !texts_pending.empty()) {
			v.definition = Unreadable(texts_pending);
		} else if (!v.is_encrypted && text != texts.end()) {
			v.definition = text->second;
		}
		model.views.push_back(std::move(v));
	}
	std::sort(model.tables.begin(), model.tables.end(), [](const CatalogTable &a, const CatalogTable &b) {
		return ByName(a.schema_name, a.name, b.schema_name, b.name);
	});
	std::sort(model.views.begin(), model.views.end(), [](const CatalogView &a, const CatalogView &b) {
		return ByName(a.schema_name, a.name, b.schema_name, b.name);
	});
	return model;
}

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb
