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

void DecideDecodable(const BootstrapTables &tables, const std::map<std::pair<uint64_t, int32_t>, std::string> &defaults,
					 const std::set<std::pair<uint64_t, int32_t>> &ambiguous, const std::string &defaults_pending,
					 const SysRowset *base, CatalogTable &t) {
	if (!t.reason.empty()) {
		t.decodable = false;  // already refused (its schema or a column's type)
		return;
	}
	t.decodable = false;
	if (!base) {
		t.reason = "it has no live base rowset";
		return;
	}
	if (t.shape == TableShape::Columnstore) {
		t.reason = "a clustered columnstore index is not read (spec 088)";
		return;
	}
	if (t.partitions.size() > 1) {
		t.reason = "a partitioned table is not read yet";
		return;
	}
	for (const auto &p : t.partitions) {
		if (p.compression == 1 || p.compression == 2) {
			t.reason = std::string(p.compression == 1 ? "ROW" : "PAGE") + " compression is not read (spec 086)";
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
	try {
		t.layout = tables.LayoutOf(t.object_id, *base);
	} catch (FormatError &e) {
		t.reason = std::string("its physical layout: ") + e.what();
		return;
	}
	for (auto &physical : t.layout.columns) {
		if (physical.Dropped() || physical.Uniquifier() || !physical.HasDefaultStatus()) {
			continue;
		}
		auto column = by_id.find(physical.rscolid);
		if (column == by_id.end()) {
			t.reason = "physical column " + std::to_string(physical.rscolid) + " has no column";
			return;
		}
		if (!defaults_pending.empty()) {
			t.reason = "column " + column->second->name + " was added with a default, and " + defaults_pending;
			return;
		}
		std::string why;
		if (!AddedDefault(defaults, ambiguous, t.layout, physical, *column->second, physical.default_bytes, why)) {
			t.reason = why;
			return;
		}
		physical.has_default = true;
	}
	t.decodable = true;
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
		const SysRowset *base = nullptr;
		bool heap = false, columnstore = false;
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
				CatalogPartition p;
				p.rowset_id = r->rowset_id;
				p.partition_number = r->partition_number;
				p.rows = r->rows;
				p.compression = r->compression;
				t.partitions.push_back(p);
				base = base ? base : r;
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
		std::sort(t.partitions.begin(), t.partitions.end(), [](const CatalogPartition &a, const CatalogPartition &b) {
			return a.partition_number < b.partition_number;
		});
		t.shape = columnstore ? TableShape::Columnstore : heap ? TableShape::Heap : TableShape::Clustered;
		DecideDecodable(tables, defaults, ambiguous, defaults_pending, base, t);
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
