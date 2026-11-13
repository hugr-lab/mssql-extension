//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// backup/backup_opened.hpp
//
// Spec 085 task 9: stage 1 of an opened backup (§ Data structures, OpenedBackup): the files of one
// media set opened through DuckDB's file system (ByteSource), each a plain MTF family or a
// compressed container (located from its tail, DQ-41), walked for its headers and its backup set;
// the families checked to be one complete set: enough for mssql_backup_info. Then the later stages,
// built for one function call: the pages (OpenBackupPages) and the catalog (OpenBackupCatalog). The
// opened-file cache (D9) is task 22. [DuckDB]
//===----------------------------------------------------------------------===//

#pragma once

#include "backup/backup_catalog_model.hpp"
#include "backup/backup_container.hpp"
#include "backup/backup_mtf.hpp"
#include "backup/backup_open_pass.hpp"
#include "backup/backup_page.hpp"
#include "backup/backup_set.hpp"
#include "backup/backup_source.hpp"

#include <memory>
#include <string>
#include <vector>

namespace duckdb {
class ClientContext;

namespace mssql {
namespace backup {

//! One family of the set: its file, and for a container its chain and data set.
struct OpenedFamily {
	size_t file = 0;  // in the ByteSource
	std::unique_ptr<SourceAccess> access;
	bool container = false;
	ContainerHeader container_header;
	BlockChain chain;
	std::vector<SeekEntry> seek;
	BlockDecoder decoder;
	std::unique_ptr<ContainerStream> data_set;
	MtfLayout layout;
	BackupSetHeader set;
	//! Any data or log stream carries media-format attribute 0x100 (BACKUP … WITH CHECKSUM, E2§7).
	bool checksums = false;

	//! The MTF stream the parsers read: the file, or a container's data set.
	const RandomAccess &Stream() const;
};

//! A media set's families, ordered by family sequence number (a container's from its media label).
struct OpenedHeaders {
	std::unique_ptr<ByteSource> source;
	std::vector<std::unique_ptr<OpenedFamily>> families;

	const BackupSetHeader &Set() const {
		return families.front()->set;
	}
	bool Compressed() const {
		return families.front()->container;
	}
};

//! Stages 2 and 3 of an opened backup (spec 085 § Data structures, task 11b): where each page's
//! latest copy lies (the page index, D4a, DQ-41) and which pages are live (the allocation map), and
//! the PageStore over both. Built for one call of a file function; the opened-file cache that keeps
//! them across calls is task 22. Keeps `headers` (its streams) by reference.
struct OpenedPages {
	std::vector<ContainerFamily> containers;  // one per family; a plain family's is empty
	std::vector<FamilyStream> streams;
	PageIndex index;
	std::unique_ptr<AllocationMap> allocation;
	std::unique_ptr<PageStore> store;
};

struct OpenPagesOptions {
	//! DQ-41: a container's pass searched by resync from any offset (trusted row data only).
	bool trust_layout = false;
};

//! The pages of the full backup `headers` holds. A log backup has no data pages and a differential
//! is read over its full (task 24's overlay): both are refused by name. The header scans read the
//! families as parallel ranges (D3a) through `context`, and so does a searched pass's fallback later
//! (the index keeps the mapper): the pages, like `headers`' source, must not outlive `context`.
std::unique_ptr<OpenedPages> OpenBackupPages(ClientContext &context, const OpenedHeaders &headers,
											 const OpenPagesOptions &options = OpenPagesOptions());

//! Stages 1-4 of an opened backup (task 11b): the headers, the pages, the boot page (checked against
//! the trailer), the bootstrap tables and the catalog model. Built for one call of a file function
//! (the cache is task 22). The members depend on one another in this order.
struct OpenedCatalog {
	std::unique_ptr<OpenedHeaders> headers;
	std::unique_ptr<OpenedPages> pages;
	BootstrapTables tables;
	CatalogModel model;
};

//! The catalog of the full backup on `paths`: OpenBackupHeaders, OpenBackupPages, the boot page read
//! and checked (CheckBootAgainstSet), ReadBootstrapTables and BuildCatalogModel. FormatError naming
//! the backup otherwise.
//! `with_model` false stops after the bootstrap tables (the model left empty): what a reader of the
//! base tables needs. `scope`: the tables and views the model is built for (task 16e).
std::unique_ptr<OpenedCatalog> OpenBackupCatalog(ClientContext &context, const std::vector<std::string> &paths,
												 bool with_model = true, const CatalogScope &scope = CatalogScope());

//! The files of `paths` (paths or globs: the families of one media set) opened, walked and checked:
//! one backup set per file (an appended set is refused), the same backup set GUID in every family,
//! and for plain families every FamilySequenceNumber of the media set's FamilyCount present once
//! (E2 R9: a missing one is named); a compressed family's media fields come from its container's
//! header (ReadContainerTape). An encrypted backup is refused (spec 087).
std::unique_ptr<OpenedHeaders> OpenBackupHeaders(ClientContext &context, const std::vector<std::string> &paths);

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb
