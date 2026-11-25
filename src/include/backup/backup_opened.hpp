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
#include "backup/backup_fuzzy.hpp"
#include "backup/backup_history.hpp"
#include "backup/backup_mtf.hpp"
#include "backup/backup_open_pass.hpp"
#include "backup/backup_page.hpp"
#include "backup/backup_set.hpp"
#include "backup/backup_source.hpp"
#include "backup/backup_system_tables.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/function/table_function.hpp"
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
class BufferManager;
}

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
	//! Task 24 (D12): a differential's pages over these, its full's (chain checked by the caller):
	//! its own passes win, liveness from the overlaid allocation pages. Must outlive the pages.
	const PageIndex *base = nullptr;
};

//! The pages of the full backup `headers` holds, or of a differential over its full's
//! (`options.base`, task 24). A log backup has no data pages, and a differential with no base is
//! refused by name. The header scans read the
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

//! Memory the reader allocates itself -- the log tail's summary, the chunk its cursor reads (D17) -- held
//! against DuckDB's memory_limit (BufferManager::ReserveMemory) in steps of STEP, and given back by Hold(0)
//! or the destructor (DuckDB resets its attached databases and its ObjectCache before its buffer manager).
//! One holder grows it: not thread-safe. Past what DuckDB can give, an OutOfMemoryException naming `what`.
class ReservedBytes {
public:
	static constexpr uint64_t STEP = 4ull << 20;
	ReservedBytes(BufferManager &buffers, std::string what) : buffers_(buffers), what_(std::move(what)) {}
	~ReservedBytes();
	ReservedBytes(const ReservedBytes &) = delete;
	ReservedBytes &operator=(const ReservedBytes &) = delete;
	//! Holds `bytes`, rounded up to a step: grown or shrunk to it.
	void Hold(uint64_t bytes);
	uint64_t Held() const {
		return reserved_;
	}

private:
	BufferManager &buffers_;
	std::string what_;
	uint64_t reserved_ = 0;
};

//! Stages 1-4 but the model, held for every statement that opens the same files (spec 085 D9, task
//! 22): the DuckDB instance's ObjectCache entry, keyed by every file's path, size and version tag,
//! evictable by its memory. Stages 2-4 (the pages, the boot page checked against the trailer, the
//! bootstrap tables) are built by the first statement that needs them, once (EnsureTables). An
//! attached catalog would hold one too. Reads only through a statement's files bound to the thread.
class OpenedBackup : public ObjectCacheEntry {
public:
	//! `base`: a differential's full (task 24, D12), whose stages this one's pages lie over; its chain is
	//! checked by the caller (CheckDifferentialBase).
	explicit OpenedBackup(std::unique_ptr<OpenedHeaders> headers, shared_ptr<OpenedBackup> base = nullptr);

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
	//! A differential's full, or none.
	const shared_ptr<OpenedBackup> &Base() const {
		return base_;
	}
	//! Stage 5 (D11, task 27): what the backup's log tail says, read now when it is not -- family 1's
	//! MQTL streams (the tail is in that family only, E2§6), through the statement's files bound to the
	//! thread -- and kept. Read outside the entry's locks; a second statement waits for the first's read in
	//! 100 ms slices that ask `context` for an interrupt. A FormatError naming the backup when the tail is
	//! not a log's, or not read whole: kept too and thrown again whenever asked (review of #591: each
	//! statement read the whole tail again only to throw again; the files of an entry do not change). An
	//! interrupt, or memory DuckDB cannot give, keeps nothing. Meaningful for a data backup (a log backup's
	//! records are its whole content, not a tail). `built`: whether this call read it.
	//! The summary's memory, and the cursor's while it reads, is reserved with DuckDB as it grows (D17) and
	//! held by the entry (ReleaseTailReservation).
	const FuzzySummary &Fuzzy(ClientContext &context, bool *built = nullptr) const;
	//! Gives back what the summary holds reserved: once the opened-file cache holds the entry, whose estimate
	//! counts the summary (Reweigh), it is the cache's reservation. An entry not cached keeps it for its life.
	void ReleaseTailReservation() const;
	//! Whether the entry still holds its summary's memory reserved (ReleaseTailReservation not yet called).
	bool HoldsTailReservation() const;
	//! The tables' D11 status over this backup's tail and its own pages, kept per unit (FuzzinessMemo).
	FuzzinessMemo &Fuzziness() const {
		return fuzziness_;
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
	shared_ptr<OpenedBackup> base_;
	std::mutex lock_;
	std::condition_variable built_;
	bool building_ = false;
	std::unique_ptr<OpenedPages> pages_;
	std::unique_ptr<BootstrapTables> tables_;
	mutable UnitPageCache unit_pages_;
	mutable std::mutex fuzzy_lock_;
	mutable std::condition_variable fuzzy_built_;
	mutable bool fuzzy_building_ = false;
	mutable std::unique_ptr<FuzzySummary> fuzzy_;
	//! Why the tail was not read, when it could not be (a FormatError's text): thrown again.
	mutable std::string fuzzy_error_;
	//! The summary's memory reserved with DuckDB, until ReleaseTailReservation (fuzzy_lock_).
	mutable std::unique_ptr<ReservedBytes> fuzzy_memory_;
	mutable FuzzinessMemo fuzziness_;
	//! Held from a Reweigh's measure to its publication (review of #591: two at once could publish the
	//! older, smaller measure last, the entry charged for less than it held).
	std::mutex weigh_lock_;
	std::atomic<size_t> estimate_{0};
};

//! `backup` put in the instance's ObjectCache under `key`, its estimate measured first when it still holds
//! its tail's summary reserved, which it then gives back: the cache's reservation counts it. Every Put of an
//! OpenedBackup goes through here; never under the cache's lock (Reweigh, OpenedBackup::Reweigh).
void PutInCache(ObjectCache &cache, const std::string &key, const shared_ptr<OpenedBackup> &backup);

//! A backup as one statement opened it: the shared state (from the cache, or built for it), the
//! statement's files and their binding to it. Reads of the backup run inside a BindFiles of `binding`.
struct OpenedForStatement {
	shared_ptr<OpenedBackup> backup;  // DuckDB's shared_ptr, the ObjectCache's
	std::unique_ptr<StatementFiles> files;
	//! A differential's: its full opened for the statement too, which `binding.also` binds.
	std::unique_ptr<OpenedForStatement> base;
	FileBinding binding;
	//! The ObjectCache key of the backup, empty when it is not cached (mssql_backup_cache off, or a file
	//! with no version tag).
	std::string cache_key;
};

//! `backup`'s log tail (stage 5, OpenedBackup::Fuzzy), read through the statement's files bound to the
//! thread; when it cannot be read, none, and one WARNING in duckdb_logs naming `function` and why: a
//! listing's status columns are NULL then, its other columns read (D13: refused at the narrowest scope).
const FuzzySummary *TailOrWarn(ClientContext &context, const OpenedForStatement &opened, const char *function);
//! `opened`'s log tail (OpenedBackup::Fuzzy); a summary just read is weighed into its cache entry (put
//! again when its measure changed), as the other stages are.
const FuzzySummary &StatementTail(ClientContext &context, const OpenedForStatement &opened);

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
	//! Its table_filter and table_exclude too (the model was built for them); none: every object.
	std::shared_ptr<const std::function<bool(const std::string &schema, const std::string &object)>> objects;

	bool WantsSchema(const std::string &schema) const {
		return !wants_schema || wants_schema(schema);
	}
	bool WantsObject(const std::string &schema, const std::string &object) const {
		return !objects || (*objects)(schema, object);
	}
};

//! A version of a history (task 24b, DQ-48), as AT names one: `unit` "version" (a number) or "timestamp"
//! (the last version finished by it); none when `unit` is empty -- the default version. `key`, set when a
//! query over an attached backup is bound (PinAttachedVersion), names the version AT named then by its
//! backups and files (the catalog's StateKey), and wins: a plan run or deserialized after a refresh reads
//! that version, whatever its number now (owner, 2026-11-19).
struct BackupAt {
	std::string unit;
	Value value;
	std::string key;
	bool IsSet() const {
		return !unit.empty();
	}
	//! The same version named: a bind data's Equals (two scans of two versions are never one, whatever
	//! DuckDB's plan dedup sees).
	bool Same(const BackupAt &other) const {
		return unit == other.unit && (!IsSet() || value == other.value) && key == other.key;
	}
};

//! The version `at` names in `history` (the latest when none). A BinderException naming `what` when
//! there is none: a number out of range, a time before the first version finished, another unit.
uint64_t ResolveVersion(const BackupHistory &history, optional_ptr<ClientContext> context, const BackupAt &at,
						const std::string &what);

//! `opened`, the backup of `version` opened after the history was read, is the history's: the same
//! files (path, size, version tag) and backup sets. An InvalidInputException naming the file and the
//! catalog `name` otherwise.
//! `attached`: `name` is an attached catalog (refresh or ATTACH again), else a set of files read now.
void CheckVersionFiles(const OpenedBackup &opened, const BackupHistory &history, const HistoryVersion &version,
					   const std::string &name, bool attached);

//! The file functions' `version :=` (ANY: a whole number, checked by ResolveVersion) / `as_of :=` (an attached backup's
//! or a set of files' version).
BackupAt BackupAtOf(const named_argument_map_t &named, const char *function);
//! Those keyword parameters, on a function's signature.
void AddVersionParameters(TableFunction &function);

//! The backup attached as `name` (a catalog of TYPE mssql_backup), or none: its version `at` names (by its
//! key when pinned), its default version when none. [the catalog layer]
std::shared_ptr<const AttachedBackup> FindAttachedBackup(ClientContext &context, const std::string &name,
														 const BackupAt &at = BackupAt());
//! The `fuzzy` ATTACH option of the backup attached as `name` ("" when none, or `name` is no such
//! catalog): what a file function named by it does with a table that is not consistent, after its own
//! fuzzy := and before the setting (review of #591). [the catalog layer]
std::string AttachedFuzzy(ClientContext &context, const std::string &name);
//! The `geometry_as` ATTACH option of the backup attached as `name` (spec 093 T8), "" when `name` is none or
//! it was not given. [the catalog layer]
std::string AttachedGeometryAs(ClientContext &context, const std::string &name);
//! The key a version of an attached backup's history is pinned by (BackupAt::key): its backups and their
//! files. A caller that resolved numbers in one `history` (the catalog's versions it read) opens them by
//! these keys, so a refresh between two opens cannot renumber one of them (review of #580's merge).
//! [the catalog layer]
std::string AttachedStateKey(const BackupHistory &history, uint64_t number);
//! At a file function's bind: when `name` is an attached backup, `at`'s version (its default when none)
//! pinned by its key, so the plan reads it whatever a refresh renumbers before it runs. [the catalog layer]
void PinAttachedVersion(ClientContext &context, const std::string &name, BackupAt &at);

//! The backup on `paths` opened for an ATTACH as `name`: OpenBackup's shared stages (the ObjectCache's
//! entry when there is one, never another attached backup's name) and the model of the tables and views
//! `wants_object` keeps, built now. FormatError naming the backup when it cannot be read.
std::shared_ptr<AttachedBackup> OpenAttachedBackup(
	ClientContext &context, const std::string &name, const std::vector<std::string> &paths,
	std::function<bool(const std::string &schema)> wants_schema,
	std::function<bool(const std::string &schema, const std::string &object)> wants_object,
	const std::vector<std::string> &diff = std::vector<std::string>());

//! The backup on `paths` (paths or globs: the families of one media set): its files opened for this
//! statement and its headers taken from the DuckDB instance's ObjectCache when `mssql_backup_cache`
//! is on and a backup of exactly these files -- path, size, version tag -- is there, else read now
//! (OpenBackupHeaders' checks) and put there. A file that changed is a new key: never trusted. One
//! path that names an attached backup (FindAttachedBackup) is that backup, its files opened again for
//! the statement and refused by name when one changed since the ATTACH.
//! `diff` (task 24, D12): a differential to lay over the full on `paths` -- the families of one
//! differential backup; its chain checked first (CheckDifferentialBase). Its ObjectCache entry holds
//! the full's, keyed by both backups' files.
//! `at` (task 24b): a version of the attached backup's history, or of the files' (ReadBackupHistory over
//! `paths`, the version's full and differential then opened as above); not beside `diff`.
OpenedForStatement OpenBackup(ClientContext &context, const std::vector<std::string> &paths,
							  const std::vector<std::string> &diff = std::vector<std::string>(),
							  const BackupAt &at = BackupAt());

//! D12's chain: `diff` is a differential whose base is `full`'s backup set, of the same database, and
//! `full` a full backup. A FormatError naming both files otherwise.
void CheckDifferentialBase(const OpenedHeaders &full, const OpenedHeaders &diff);

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
												 bool with_model = true, const CatalogScope &scope = CatalogScope(),
												 const std::vector<std::string> &diff = std::vector<std::string>(),
												 const BackupAt &at = BackupAt());
//! The catalog of `attached` for one statement: its files opened again and checked, its model.
std::unique_ptr<OpenedCatalog> OpenAttachedCatalog(ClientContext &context, const AttachedBackup &attached);
//! The model of every object of `catalog` (opened for the statement), whatever an ATTACH's filters keep:
//! what a rowset's owner is found in (review of #580). `what` names the backup in a FormatError.
std::shared_ptr<const CatalogModel> WholeModel(const OpenedCatalog &catalog, const std::string &what);

//! The headers of the backup whose files `files` opened, read through them: one backup set per file
//! (an appended set is refused), the same backup set GUID in every family, and for plain families
//! every FamilySequenceNumber of the media set's FamilyCount present once (E2 R9: a missing one is
//! named); a compressed family's media fields come from its container's header (ReadContainerTape).
//! An encrypted backup is refused (spec 087). Keeps no reference to `files`.
std::unique_ptr<OpenedHeaders> ReadBackupHeaders(const StatementFiles &files);
//! `files`' binding to `headers` (each of the backup's files to the statement file of its path).
FileBinding BindingFor(const OpenedHeaders &headers, const StatementFiles &files);

//! The history of the backups on `paths` (task 24b, DQ-48): each file's header read, and nothing else --
//! no page, no index -- ordered into versions (BuildHistory). FormatError naming the file that is not a
//! backup this reader takes (encrypted, appended sets, …), or files of two databases.
//! `previous` (a refresh): a file it read, at the same path, size and version tag, is taken from it
//! unread.
BackupHistory ReadBackupHistory(ClientContext &context, const std::vector<std::string> &paths,
								const BackupHistory *previous = nullptr);

//! A backup opened for one call (the tests, and nothing cached): ReadBackupHeaders over files opened
//! from `paths`, which the headers keep (own_files) and read through on any thread.
std::unique_ptr<OpenedHeaders> OpenBackupHeaders(ClientContext &context, const std::vector<std::string> &paths);

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb
