//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// backup/backup_opened.hpp
//
// Spec 085 task 9: stage 1 of an opened backup (§ Data structures, OpenedBackup): the files of one
// media set opened through DuckDB's file system (ByteSource), each a plain MTF family or a
// compressed container (located from its tail, DQ-41), walked for its headers and its backup set;
// the families checked to be one complete set: enough for mssql_backup_info. Then the later stages:
// the pages (OpenBackupPages) and the bootstrap tables. Task 22 (D9): an OpenedBackup holds them
// for every statement that opens the same files, in the DuckDB instance's ObjectCache; a statement
// reads it through its own files (backup_files.hpp), and builds its catalog model (OpenBackupCatalog).
// [DuckDB]
//===----------------------------------------------------------------------===//

#pragma once

#include "backup/backup_catalog_model.hpp"
#include "backup/backup_container.hpp"
#include "backup/backup_files.hpp"
#include "backup/backup_mtf.hpp"
#include "backup/backup_open_pass.hpp"
#include "backup/backup_page.hpp"
#include "backup/backup_set.hpp"
#include "backup/backup_source.hpp"
#include "backup/backup_system_tables.hpp"
#include "duckdb/storage/object_cache.hpp"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace duckdb {
class ClientContext;

namespace mssql {
namespace backup {

//! One family of the set: its file, and for a container its chain and data set.
struct OpenedFamily {
	size_t file = 0;  // its number in the backup (OpenedHeaders::files)
	std::unique_ptr<BoundFile> access;
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

//! A file of a backup as it was opened: what the cache key and a statement's binding match.
struct BackupFileKey {
	std::string path;
	uint64_t size = 0;
	std::string version;  // ByteSource::VersionTag
};

//! A media set's families, ordered by family sequence number (a container's from its media label).
struct OpenedHeaders {
	//! This backup among every one opened: what its files' bindings name (FileBinding::backup).
	uint64_t id = 0;
	//! Its files by number, in the order they were opened.
	std::vector<BackupFileKey> files;
	//! A backup opened for one call keeps its statement's files, and reads through them on any thread
	//! (BoundFile's `own`); one held for every statement (OpenedBackup) keeps none. Before the families,
	//! which read through them.
	std::unique_ptr<StatementFiles> own_files;
	std::unique_ptr<FileBinding> own_binding;
	std::vector<std::unique_ptr<OpenedFamily>> families;

	const std::string &Path(size_t file) const {
		return files[file].path;
	}

	const BackupSetHeader &Set() const {
		return families.front()->set;
	}
	bool Compressed() const {
		return families.front()->container;
	}
};

//! Stages 2 and 3 of an opened backup (spec 085 § Data structures, task 11b): where each page's
//! latest copy lies (the page index, D4a, DQ-41) and which pages are live (the allocation map), and
//! the PageStore over both. Keeps `headers` (its streams) by reference.
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
//! families as parallel ranges (D3a) through the statement files read on the thread, and so does a
//! searched pass's fallback later (BoundPassMapper): the pages live as long as `headers`.
std::unique_ptr<OpenedPages> OpenBackupPages(const OpenedHeaders &headers,
											 const OpenPagesOptions &options = OpenPagesOptions());
//! As above, for a backup opened for one call (the tests). `context` is not kept.
std::unique_ptr<OpenedPages> OpenBackupPages(ClientContext &context, const OpenedHeaders &headers,
											 const OpenPagesOptions &options = OpenPagesOptions());

//! The pages each in-row allocation unit's IAM chain lists, in page order, each once: walked at a
//! table's first scan and kept with the opened backup (task 23), so a later scan reads no IAM page
//! again. Its bytes count in the cache entry's estimate.
class UnitPageCache {
public:
	//! The unit's list, `build`'s answer the first time it is asked for.
	std::shared_ptr<const std::vector<PagePointer>> Get(uint64_t auid,
														const std::function<std::vector<PagePointer>()> &build);
	size_t MemoryBytes();

private:
	std::mutex lock_;
	std::unordered_map<uint64_t, std::shared_ptr<const std::vector<PagePointer>>> units_;
};

//! Stages 1-4 but the model, held for every statement that opens the same files (spec 085 D9, task
//! 22): the DuckDB instance's ObjectCache entry, keyed by every file's path, size and version tag,
//! evictable by its memory. Stages 2-4 (the pages, the boot page checked against the trailer, the
//! bootstrap tables) are built by the first statement that needs them, once (EnsureTables). An
//! attached catalog would hold one too. Reads only through a statement's files bound to the thread.
class OpenedBackup : public ObjectCacheEntry {
public:
	explicit OpenedBackup(std::unique_ptr<OpenedHeaders> headers);

	const OpenedHeaders &Headers() const {
		return *headers_;
	}
	//! Stages 2-4, built now when they are not; true when this call built them. A second statement
	//! waits for the first's build, asking `context` for an interrupt meanwhile. FormatError naming the
	//! backup when they cannot be (a log backup, a differential); asked again, they are tried again.
	bool EnsureTables(ClientContext &context);
	//! Measures what the entry holds now (the searches' samples, the GAM intervals and PFS pages grow
	//! as statements read) and keeps it for GetEstimatedCacheMemory; true when it changed. Never called
	//! under the ObjectCache's lock: the measures take the index's and the searches' locks, and a
	//! fallback holding them reads through the buffer pool, whose eviction takes the cache's.
	bool Reweigh();
	//! After EnsureTables.
	const OpenedPages &Pages() const {
		return *pages_;
	}
	const BootstrapTables &Tables() const {
		return *tables_;
	}
	//! The in-row units' page lists the scans have walked (task 23).
	UnitPageCache &UnitPages() const {
		return unit_pages_;
	}

	static std::string ObjectType() {
		return "mssql_backup_opened";
	}
	std::string GetObjectType() override {
		return ObjectType();
	}
	//! The last Reweigh's measure: read under the ObjectCache's lock, so it takes no lock of its own.
	optional_idx GetEstimatedCacheMemory() const override {
		return optional_idx(estimate_.load(std::memory_order_relaxed));
	}

private:
	std::unique_ptr<OpenedHeaders> headers_;
	std::mutex lock_;
	std::condition_variable built_;
	bool building_ = false;
	std::unique_ptr<OpenedPages> pages_;
	std::unique_ptr<BootstrapTables> tables_;
	mutable UnitPageCache unit_pages_;
	std::atomic<size_t> estimate_{0};
};

//! A backup as one statement opened it: the shared state (from the cache, or built for it), the
//! statement's files and their binding to it. Reads of the backup run inside a BindFiles of `binding`.
struct OpenedForStatement {
	shared_ptr<OpenedBackup> backup;  // DuckDB's shared_ptr, the ObjectCache's
	std::unique_ptr<StatementFiles> files;
	FileBinding binding;
	//! The ObjectCache key of the backup, empty when it is not cached (mssql_backup_cache off, or a file
	//! with no version tag).
	std::string cache_key;
};

//! A backup attached as a catalog (ATTACH … (TYPE mssql_backup), task 23): what its ATTACH opened, held
//! for its life -- the opened backup (the ObjectCache's entry, which stays alive while attached even if
//! the cache evicts it) and its model, built once for the ATTACH's filters and exclusions, its
//! schemas filtered too. The file functions take its name in place of a path and read through it.
struct AttachedBackup {
	std::string name;
	shared_ptr<OpenedBackup> backup;
	std::string cache_key;	// the ObjectCache's, empty when not cached
	std::shared_ptr<const CatalogModel> model;
	//! The ATTACH's schema_filter and schema_exclude; none: every schema.
	std::function<bool(const std::string &schema)> wants_schema;

	bool WantsSchema(const std::string &schema) const {
		return !wants_schema || wants_schema(schema);
	}
};

//! The backup attached as `name` (a catalog of TYPE mssql_backup), or none. [the catalog layer]
std::shared_ptr<const AttachedBackup> FindAttachedBackup(ClientContext &context, const std::string &name);

//! The backup on `paths` opened for an ATTACH as `name`: OpenBackup's shared stages (the ObjectCache's
//! entry when there is one, never another attached backup's name) and the model of the tables and views
//! `wants_object` keeps, built now. FormatError naming the backup when it cannot be read.
std::shared_ptr<AttachedBackup> OpenAttachedBackup(
	ClientContext &context, const std::string &name, const std::vector<std::string> &paths,
	std::function<bool(const std::string &schema)> wants_schema,
	std::function<bool(const std::string &schema, const std::string &object)> wants_object);

//! The backup on `paths` (paths or globs: the families of one media set): its files opened for this
//! statement and its headers taken from the DuckDB instance's ObjectCache when `mssql_backup_cache`
//! is on and a backup of exactly these files -- path, size, version tag -- is there, else read now
//! (OpenBackupHeaders' checks) and put there. A file that changed is a new key: never trusted. One
//! path that names an attached backup (FindAttachedBackup) is that backup, its files opened again for
//! the statement and refused by name when one changed since the ATTACH.
OpenedForStatement OpenBackup(ClientContext &context, const std::vector<std::string> &paths);

//! Stages 1-4 of an opened backup for one statement: the shared stages (OpenBackup, EnsureTables)
//! and the catalog model, the statement's own (its scope is the statement's), or an attached backup's.
struct OpenedCatalog {
	OpenedForStatement opened;
	//! The shared stages' (owned by `opened.backup`).
	const OpenedHeaders *headers = nullptr;
	const OpenedPages *pages = nullptr;
	const BootstrapTables *tables = nullptr;
	std::shared_ptr<const CatalogModel> model;
	//! The opened backup's (owned by `opened.backup`).
	UnitPageCache *unit_pages = nullptr;
	const CatalogModel &Model() const {
		return *model;
	}
	const FileBinding &Binding() const {
		return opened.binding;
	}
};

//! The catalog of the full backup on `paths`: OpenBackup, EnsureTables and BuildCatalogModel, read
//! through the statement's files. FormatError naming the backup otherwise.
//! `with_model` false stops after the bootstrap tables (the model left empty): what a reader of the
//! base tables needs. `scope`: the tables and views the model is built for (task 16e). An attached
//! backup's is its own model, built at its ATTACH (`with_model` and `scope` do not apply).
std::unique_ptr<OpenedCatalog> OpenBackupCatalog(ClientContext &context, const std::vector<std::string> &paths,
												 bool with_model = true, const CatalogScope &scope = CatalogScope());
//! The catalog of `attached` for one statement: its files opened again and checked, its model.
std::unique_ptr<OpenedCatalog> OpenAttachedCatalog(ClientContext &context, const AttachedBackup &attached);

//! The headers of the backup whose files `files` opened, read through them: one backup set per file
//! (an appended set is refused), the same backup set GUID in every family, and for plain families
//! every FamilySequenceNumber of the media set's FamilyCount present once (E2 R9: a missing one is
//! named); a compressed family's media fields come from its container's header (ReadContainerTape).
//! An encrypted backup is refused (spec 087). Keeps no reference to `files`.
std::unique_ptr<OpenedHeaders> ReadBackupHeaders(const StatementFiles &files);
//! `files`' binding to `headers` (each of the backup's files to the statement file of its path).
FileBinding BindingFor(const OpenedHeaders &headers, const StatementFiles &files);

//! A backup opened for one call (the tests, and nothing cached): ReadBackupHeaders over files opened
//! from `paths`, which the headers keep (own_files) and read through on any thread.
std::unique_ptr<OpenedHeaders> OpenBackupHeaders(ClientContext &context, const std::vector<std::string> &paths);

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb
