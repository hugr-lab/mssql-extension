// Fuzz harness: the catalog reader over pages (spec 085 PR 2b, task 18): the boot page, IAM pages,
// FixedVar records through the bootstrap tables' literal layouts and through the layouts their rows
// describe, the bootstrap tables' rows, indexes and self-description check, every value through its
// accessor, sql_variant values, module text, and the LOB tree reader over the same pages.
//
// The input: u8 options, u8 n, n bytes of an in-row LOB root, then up to 16 pages of 8 KB (a short
// last page padded with zeros). Each page is read as what its header says it is: the boot page and an
// IAM page by their readers; a data page by the bootstrap table whose allocation unit its header
// names (sysallocunits, sysrowsets, sysschobjs, syscolpars, sysrscols), its rows appended through
// AddBootstrapRow -- the reader's own conversion. Then the tables' indexes, every object's base layout
// (each page decoded through each), CheckBootstrapLayouts, and the root read over the pages. Every
// malformed input must end in a FormatError; any sanitizer report, signal, hang or other exception is
// a real bug. Seeds (fuzz/gen_backup_seeds.cc): pages of the committed sample, chosen by content so
// that the five bootstrap tables describe themselves (the self-description check passes and their
// layouts decode), and a built LOB tree.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <vector>

#include "backup/backup_boot.hpp"
#include "backup/backup_bytes.hpp"
#include "backup/backup_catalog_model.hpp"
#include "backup/backup_iam.hpp"
#include "backup/backup_lob.hpp"
#include "backup/backup_page.hpp"
#include "backup/backup_sql_variant.hpp"
#include "backup/backup_system_tables.hpp"

using namespace duckdb::mssql::backup;

static volatile size_t g_sink = 0;
static const size_t MAX_PAGES = 16;
// Objects whose described layout decodes every page: bounds a crafted catalog's run.
static const size_t MAX_OBJECTS = 32;

static const char *TableOf(uint64_t au) {
	switch (au) {
	case 7ull << 16:
		return "sysallocunits";
	case 5ull << 16:
		return "sysrowsets";
	case (1ull << 48) | (34ull << 16):
		return "sysschobjs";
	case (1ull << 48) | (41ull << 16):
		return "syscolpars";
	case 3ull << 16:
		return "sysrscols";
	default:
		return nullptr;
	}
}

// Every value of `row` through the accessors its column's type takes.
static void Touch(const RowsetLayout &layout, const SystemRow &row) {
	for (size_t c = 0; c < layout.columns.size() && c < row.values.size(); c++) {
		const SystemValue &v = row.values[c];
		try {
			switch (layout.columns[c].xtype) {
			case XTYPE_TINYINT:
			case XTYPE_SMALLINT:
			case XTYPE_INT:
			case XTYPE_BIGINT:
			case XTYPE_BIT:
				g_sink += static_cast<size_t>(ValueInt(layout, c, v));
				break;
			case XTYPE_NVARCHAR:
			case XTYPE_NCHAR:
			case XTYPE_VARCHAR:
			case XTYPE_CHAR:
				g_sink += ValueText(layout, c, v).size();
				break;
			case XTYPE_BINARY:
				g_sink += ValuePointer(layout, c, v).page;
				break;
			case XTYPE_SQL_VARIANT: {
				// The model reads a module's flags as this sql_variant's integer and decodes its text
				// (imageval) by them: the integer, and the text in both of its forms.
				const SqlVariant variant = ParseSqlVariant(v.bytes, "a value");
				g_sink += variant.value.size();
				try {
					g_sink += static_cast<size_t>(SqlVariantInt(variant, "a value"));
				} catch (FormatError &) {
				}
				g_sink += ModuleText(variant.value, 0, "a value").size();
				g_sink += ModuleText(variant.value, MODULE_TEXT_LATIN1, "a value").size();
				break;
			}
			default:
				g_sink += v.bytes.size();
			}
		} catch (FormatError &) {
		}
	}
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
	if (size < 2) {
		return 0;
	}
	const uint8_t options = data[0];
	const size_t root_size = std::min<size_t>(data[1], size - 2);
	const std::string root(reinterpret_cast<const char *>(data + 2), root_size);
	const uint8_t *body = data + 2 + root_size;
	const size_t body_size = size - 2 - root_size;
	const size_t count = std::min(MAX_PAGES, (body_size + PAGE_SIZE - 1) / PAGE_SIZE);
	std::vector<Page> pages(count);
	for (size_t i = 0; i < count; i++) {
		pages[i].bytes.assign(PAGE_SIZE, 0);
		const size_t take = std::min<size_t>(PAGE_SIZE, body_size - i * PAGE_SIZE);
		std::memcpy(pages[i].bytes.data(), body + i * PAGE_SIZE, take);
		pages[i].header = ReadPageHeader(pages[i].bytes.data());
	}

	BootstrapTables tables;
	for (const Page &page : pages) {
		try {
			const BootPage boot = ReadBootPage(page);
			g_sink += boot.database_name.size() + boot.first_sys_indexes.page;
		} catch (FormatError &) {
		}
		try {
			const IamPage iam = ReadIamPage(page);
			g_sink += iam.sequence + iam.extents.size();
		} catch (FormatError &) {
		}
		const char *table = TableOf(page.header.AllocationUnit());
		if (!table) {
			continue;
		}
		try {
			ForEachPageRow(page, BootstrapLayout(table), [&](const SystemRow &row) {
				Touch(BootstrapLayout(table), row);
				try {
					AddBootstrapRow(tables, table, row);
				} catch (FormatError &) {
				}
			});
		} catch (FormatError &) {
		}
	}
	try {
		tables.BuildIndexes();
		try {
			CheckBootstrapLayouts(tables);
		} catch (FormatError &) {
		}
		size_t described = 0;
		for (const auto &object : tables.objects) {
			if (described == MAX_OBJECTS) {
				break;
			}
			RowsetLayout layout;
			try {
				layout = tables.BaseLayout(object.id);
			} catch (FormatError &) {
				continue;
			}
			described++;
			g_sink += tables.RowsetsOf(object.id).size();
			// A described layout over every page, records and values alike.
			for (const Page &page : pages) {
				try {
					ForEachPageRow(page, layout, [&](const SystemRow &row) { Touch(layout, row); });
				} catch (FormatError &) {
				}
			}
			if (options & 1) {
				break;	// one object: keeps a large synthetic catalog's run short
			}
		}
	} catch (FormatError &) {
	}

	// The root over the pages, as the LOB unit the first page names.
	if (!pages.empty()) {
		std::map<std::pair<uint16_t, uint32_t>, const Page *> by_id;
		for (const Page &page : pages) {
			by_id[std::make_pair(page.header.page_id.file, page.header.page_id.page)] = &page;
		}
		const LobPageFetch fetch = [&](const PagePointer &at, Page &out) {
			auto it = by_id.find(std::make_pair(at.file, at.page));
			if (it == by_id.end()) {
				throw FormatError("backup: no page " + at.ToString());
			}
			out = *it->second;
		};
		try {
			g_sink += ReadLobValue(fetch, root, pages[0].header.AllocationUnit(), "the fuzz value", 1u << 20).size();
		} catch (FormatError &) {
		}
	}
	return 0;
}
