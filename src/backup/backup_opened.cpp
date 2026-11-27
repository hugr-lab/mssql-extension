#include "backup/backup_opened.hpp"

#include "backup/backup_key_secret.hpp"

#include "backup/backup_boot.hpp"
#include "backup/backup_function_common.hpp"
#include "backup/backup_xpress.hpp"
#include "backup/backup_zstd.hpp"
#include "duckdb/common/error_data.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/logging/logger.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/storage/buffer_manager.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <map>

namespace duckdb {
namespace mssql {
namespace backup {

// Spec 087 (TDE): what a page TDE leaves in clear is told by its header -- version 1, then the type: the
// boot page (13) and the file header (15) are not encrypted, nor the 96-byte header of any page.
static constexpr uint8_t TDE_PAGE_VERSION = 1;
static constexpr uint8_t TDE_PAGE_BOOT = 13;
static constexpr uint8_t TDE_PAGE_FILE_HEADER = 15;
static constexpr size_t TDE_PAGE_HEADER = 96;

static constexpr uint16_t MEDIA_ATTRIBUTE_CHECKSUM = 0x100;

const RandomAccess &OpenedFamily::Stream() const {
	if (data_set) {
		return *data_set;
	}
	return *access;
}

static BlockDecoder DecoderFor(CompressionAlgorithm algorithm) {
	switch (algorithm) {
	case CompressionAlgorithm::Zstd:
		return [](const uint8_t *in, size_t in_size, uint8_t *out, size_t out_size) {
			ZstdDecode(in, in_size, out, out_size);
		};
	case CompressionAlgorithm::MsXpress:
		return [](const uint8_t *in, size_t in_size, uint8_t *out, size_t out_size) {
			XpressHuffmanDecode(in, in_size, out, out_size);
		};
	default:
		return BlockDecoder();
	}
}

static uint64_t NextBackupId() {
	static std::atomic<uint64_t> next{1};
	return next.fetch_add(1);
}

static std::unique_ptr<OpenedFamily> OpenFamily(ClientContext &context, const OpenedHeaders &headers, size_t file) {
	std::unique_ptr<OpenedFamily> f(new OpenedFamily());
	f->file = file;
	const auto &key = headers.files[file];
	f->access.reset(new BoundFile(headers.id, file, key.path, key.size, headers.own_binding.get()));
	const RandomAccess &access = *f->access;
	if (IsContainer(access)) {
		f->container = true;
		f->container_header = ReadContainerHeader(access);
		if (f->container_header.encrypted) {
			// Spec 087 D3: the secret whose scope and thumbprint select it; its private key unwraps the
			// backup's key (a named error when none does).
			f->keys.backup = SelectContentKey(context, key.path, f->container_header.encryptor_thumbprint,
											  f->container_header.wrapped_key, "a backup");
		}
		if (f->container_header.tde) {
			// E4c: a TDE database's compressed backup -- its blocks under the DEK the header keeps wrapped.
			f->keys.dek = SelectContentKey(context, key.path, f->container_header.tde_thumbprint,
										   f->container_header.wrapped_dek, "a backup of a TDE database, its DEK");
		}
		// DQ-41: the chain located from the file's tail, walked only where it is read; a tail of
		// another layout is walked whole.
		const uint64_t start = ChainStart(f->container_header);
		if (!OpenChainFromTail(access, f->chain, f->seek, start)) {
			f->chain = WalkChain(access, start);
			f->seek = ReadSeekIndex(access, f->chain);
		}
		f->decoder = DecoderFor(f->container_header.algorithm);
		f->data_set.reset(new ContainerStream(access, f->chain, f->decoder, f->keys));
		f->layout = WalkMtfDataSet(*f->data_set);
	} else {
		f->layout = WalkMtf(access);
	}
	const RandomAccess &stream = f->Stream();
	if (f->layout.FindAll("SSET").size() > 1) {
		throw FormatError("backup file \"" + access.Name() +
						  "\" holds more than one backup set (appended with NOINIT): reading a set by position is "
						  "not supported yet");
	}
	f->set = ReadBackupSet(stream, f->layout);
	if (f->container) {
		// The container keeps its media label in its header, outside its data set.
		auto tape = ReadContainerTape(access, f->decoder, f->keys, f->container_header.tde);
		MemoryAccess label(access.Name() + " (media label)", tape.data(), tape.size());
		ReadMediaFields(label, WalkMtfTape(label), f->set);
	}
	for (const auto &block : f->layout.blocks) {
		for (const auto &s : block.streams) {
			if ((s.id == "MQDA" || s.id == "MQTL") && (s.media_attributes & MEDIA_ATTRIBUTE_CHECKSUM)) {
				f->checksums = true;
			}
		}
	}
	return f;
}

static FileBinding IdentityBinding(const OpenedHeaders &headers, const StatementFiles &files) {
	FileBinding binding;
	binding.backup = headers.id;
	binding.files = &files;
	for (size_t i = 0; i < headers.files.size(); i++) {
		binding.file_of.push_back(i);
	}
	return binding;
}

//! `opened`'s files and families, read through `files` (bound to the thread meanwhile).
static void FillHeaders(OpenedHeaders *opened, const StatementFiles &files) {
	const auto &source = files.Source();
	opened->id = NextBackupId();
	for (size_t i = 0; i < source.FileCount(); i++) {
		BackupFileKey key;
		key.path = source.Path(i);
		key.size = source.FileSize(i);
		key.version = source.VersionTag(i);
		opened->files.push_back(std::move(key));
	}
	if (opened->own_binding) {
		*opened->own_binding = IdentityBinding(*opened, files);
	}
	const FileBinding binding = IdentityBinding(*opened, files);
	BindFiles bound(binding);
	for (size_t i = 0; i < opened->files.size(); i++) {
		try {
			opened->families.push_back(OpenFamily(files.Context(), *opened, i));
		} catch (FormatError &e) {
			// Name the file where the parser below could not (a ByteReader knows offsets only).
			const std::string &path = opened->Path(i);
			if (std::string(e.what()).find(path) != std::string::npos) {
				throw;
			}
			throw FormatError("backup file \"" + path + "\": " + e.what());
		}
	}
	auto &families = opened->families;
	const auto &first = *families.front();
	for (const auto &f : families) {
		if (f->container != first.container) {
			throw FormatError("backup files \"" + opened->Path(first.file) + "\" and \"" + opened->Path(f->file) +
							  "\": one is a compressed container and the other is not, not one media set");
		}
		if (f->set.backup_set_guid.bytes != first.set.backup_set_guid.bytes) {
			throw FormatError("backup files \"" + opened->Path(first.file) + "\" and \"" + opened->Path(f->file) +
							  "\" belong to different backup sets (" + first.set.backup_set_guid.ToString() + ", " +
							  f->set.backup_set_guid.ToString() + ")");
		}
	}
	if (first.set.family_count == 0) {
		throw FormatError("backup file \"" + opened->Path(first.file) +
						  "\": its TAPE block carries no RAID stream, so its media set's families are unknown");
	}
	// E2 R9: every family of the media set, once.
	const uint32_t count = first.set.family_count;
	std::map<uint32_t, size_t> by_sequence;
	for (size_t i = 0; i < families.size(); i++) {
		const auto &set = families[i]->set;
		const std::string &path = opened->Path(families[i]->file);
		if (set.media_set_guid.bytes != first.set.media_set_guid.bytes || set.family_count != count) {
			throw FormatError("backup file \"" + path + "\" is a family of another media set than \"" +
							  opened->Path(first.file) + "\"");
		}
		if (set.family_sequence_number == 0 || set.family_sequence_number > count) {
			throw FormatError("backup file \"" + path + "\" says it is family " +
							  std::to_string(set.family_sequence_number) + " of " + std::to_string(count));
		}
		if (!by_sequence.emplace(set.family_sequence_number, i).second) {
			throw FormatError("backup files \"" +
							  opened->Path(families[by_sequence[set.family_sequence_number]]->file) + "\" and \"" +
							  path + "\" are both family " + std::to_string(set.family_sequence_number) +
							  " of the media set");
		}
	}
	if (by_sequence.size() != count) {
		std::string missing;
		for (uint32_t n = 1; n <= count; n++) {
			if (!by_sequence.count(n)) {
				missing += (missing.empty() ? "" : ", ") + std::to_string(n);
			}
		}
		throw FormatError("backup file \"" + opened->Path(first.file) + "\" is one family of a media set of " +
						  std::to_string(count) + ": the families with FamilySequenceNumber " + missing +
						  " are missing (give every file of the set)");
	}
	std::vector<std::unique_ptr<OpenedFamily>> ordered;
	for (auto &entry : by_sequence) {
		ordered.push_back(std::move(families[entry.second]));
	}
	// The BackupSize the trailer records (the set's, alike in every family) against the one the files'
	// layout gives (the families' MSLS offsets, task 3): two readings of one number.
	uint64_t computed = 0;
	for (const auto &f : ordered) {
		computed += f->set.backup_size;
		if (f->set.recorded_backup_size != ordered.front()->set.recorded_backup_size) {
			throw FormatError("backup files \"" + opened->Path(ordered.front()->file) + "\" and \"" +
							  opened->Path(f->file) + "\" record different sizes of their backup set (BackupSize)");
		}
	}
	if (ordered.front()->set.recorded_backup_size != 0 && ordered.front()->set.recorded_backup_size != computed) {
		throw FormatError("backup file \"" + opened->Path(ordered.front()->file) +
						  "\": its trailer records a BackupSize of " +
						  std::to_string(ordered.front()->set.recorded_backup_size) + " bytes, its layout gives " +
						  std::to_string(computed));
	}
	families = std::move(ordered);
}

BackupHistory ReadBackupHistory(ClientContext &context, const std::vector<std::string> &paths,
								const BackupHistory *previous) {
	std::map<std::string, const HistoryFamily *> known;
	if (previous) {
		for (const auto &f : previous->families) {
			known[f.path] = &f;
		}
	}
	// The files a batch at a time (review of #579): a history of thousands of files must not hold a
	// descriptor for each at once.
	constexpr size_t BATCH = 64;
	const auto expanded = ByteSource::Expand(context, vector<std::string>(paths.begin(), paths.end()));
	std::vector<HistoryFamily> families;
	for (size_t from = 0; from < expanded.size(); from += BATCH) {
		const vector<std::string> batch(expanded.begin() + from,
										expanded.begin() + std::min(expanded.size(), from + BATCH));
		OpenedHeaders opened;
		opened.own_files.reset(new StatementFiles(context, ByteSource::Open(context, batch)));
		opened.own_binding.reset(new FileBinding());
		const auto &source = opened.own_files->Source();
		opened.id = NextBackupId();
		for (size_t i = 0; i < source.FileCount(); i++) {
			BackupFileKey key;
			key.path = source.Path(i);
			key.size = source.FileSize(i);
			key.version = source.VersionTag(i);
			opened.files.push_back(std::move(key));
		}
		*opened.own_binding = IdentityBinding(opened, *opened.own_files);
		for (size_t i = 0; i < opened.files.size(); i++) {
			auto was = known.find(opened.Path(i));
			if (was != known.end() && was->second->size == opened.files[i].size &&
				was->second->version_tag == opened.files[i].version && !opened.files[i].version.empty()) {
				families.push_back(*was->second);
				continue;
			}
			try {
				HistoryFamily family;
				family.path = opened.Path(i);
				family.size = opened.files[i].size;
				family.version_tag = opened.files[i].version;
				family.set = OpenFamily(context, opened, i)->set;
				families.push_back(std::move(family));
			} catch (FormatError &e) {
				const std::string &path = opened.Path(i);
				if (std::string(e.what()).find(path) != std::string::npos) {
					throw;
				}
				throw FormatError("backup file \"" + path + "\": " + e.what());
			}
		}
	}
	return BuildHistory(std::move(families));
}

bool OpenedHeaders::Tde(Thumbprint *thumbprint) const {
	for (const auto &f : families) {
		if (f->container && f->container_header.tde) {
			if (thumbprint) {
				*thumbprint = f->container_header.tde_thumbprint;
			}
			return true;
		}
	}
	for (const auto &file : Set().files) {
		if (file.tde) {
			if (thumbprint) {
				*thumbprint = file.tde_thumbprint;
			}
			return true;
		}
	}
	return false;
}

std::string OpenedHeaders::TdeLogRefusal() const {
	Thumbprint certificate;
	if (!Tde(&certificate)) {
		return std::string();
	}
	// E4c: the log of every TDE fixture is ciphertext in its backup, the compressed form's too (its stream
	// decrypted under the DEK, the log blocks still fail their checks); how it is encrypted is E10's.
	return "backup file \"" + Path(families.front()->file) +
		   "\": its log is a TDE database's (the DEK wrapped by the certificate " + ThumbprintHex(certificate) +
		   "), encrypted, and not decrypted yet (spec 087 E10)";
}

std::vector<ClearFileHeader> ReadClearHeaders(ClientContext &context, const std::vector<std::string> &paths) {
	OpenedHeaders opened;
	opened.own_files.reset(
		new StatementFiles(context, ByteSource::Open(context, vector<std::string>(paths.begin(), paths.end()))));
	opened.own_binding.reset(new FileBinding());
	const auto &source = opened.own_files->Source();
	opened.id = NextBackupId();
	for (size_t i = 0; i < source.FileCount(); i++) {
		BackupFileKey key;
		key.path = source.Path(i);
		key.size = source.FileSize(i);
		key.version = source.VersionTag(i);
		opened.files.push_back(std::move(key));
	}
	*opened.own_binding = IdentityBinding(opened, *opened.own_files);
	std::vector<ClearFileHeader> out;
	for (size_t i = 0; i < opened.files.size(); i++) {
		ClearFileHeader h;
		h.path = opened.Path(i);
		BoundFile access(opened.id, i, h.path, opened.files[i].size, opened.own_binding.get());
		try {
			if (IsContainer(access)) {
				h.container = true;
				h.header = ReadContainerHeader(access);
			}
		} catch (FormatError &e) {
			if (std::string(e.what()).find(h.path) != std::string::npos) {
				throw;
			}
			throw FormatError("backup file \"" + h.path + "\": " + e.what());
		}
		out.push_back(std::move(h));
	}
	return out;
}

std::unique_ptr<OpenedHeaders> ReadBackupHeaders(const StatementFiles &files) {
	std::unique_ptr<OpenedHeaders> opened(new OpenedHeaders());
	FillHeaders(opened.get(), files);
	return opened;
}

std::unique_ptr<OpenedHeaders> OpenBackupHeaders(ClientContext &context, const std::vector<std::string> &paths) {
	std::unique_ptr<OpenedHeaders> opened(new OpenedHeaders());
	opened->own_files.reset(
		new StatementFiles(context, ByteSource::Open(context, vector<std::string>(paths.begin(), paths.end()))));
	opened->own_binding.reset(new FileBinding());
	FillHeaders(opened.get(), *opened->own_files);
	return opened;
}

FileBinding BindingFor(const OpenedHeaders &headers, const StatementFiles &files) {
	FileBinding binding;
	binding.backup = headers.id;
	binding.files = &files;
	for (const auto &key : headers.files) {
		const size_t file = files.FileOf(key.path);
		if (file == files.Source().FileCount()) {
			throw InternalException("mssql backup: \"%s\" is not among the statement's files", key.path);
		}
		binding.file_of.push_back(file);
	}
	return binding;
}

std::unique_ptr<OpenedPages> OpenBackupPages(ClientContext &, const OpenedHeaders &headers,
											 const OpenPagesOptions &options) {
	return OpenBackupPages(headers, options);
}

//! Spec 087 (TDE): a database under TDE is told by its boot page (9, 1), which TDE leaves in clear: at 912
//! (2025, E4b) u32 20, the certificate's thumbprint, u32 the wrapped DEK's length (the modulus: 256 to 512),
//! the wrapped DEK. Its DEK, from `options.tde_key`, then decrypts the index's own pages as they are read --
//! before the allocation map reads its GAM / PFS / IAM pages, encrypted too (E4§3).
static void ApplyTde(PageIndex &index, const OpenPagesOptions &options, const std::string &path) {
	static constexpr size_t FCB_FILE_DEK = 912;
	std::vector<uint8_t> boot;
	if (!index.ReadPage(1, 9, boot, true) || boot.size() != PAGE_SIZE || boot[0] != TDE_PAGE_VERSION ||
		boot[1] != TDE_PAGE_BOOT) {
		return;
	}
	ByteReader r(boot.data(), boot.size(), "the boot page's FCBFileDEK");
	const uint32_t wrapped = r.U32(FCB_FILE_DEK + 24);
	if (r.U32(FCB_FILE_DEK) != 20 || wrapped < 256 || wrapped > 512 || FCB_FILE_DEK + 28 + wrapped > PAGE_SIZE) {
		return;
	}
	Thumbprint thumbprint{};
	std::memcpy(thumbprint.data(), boot.data() + FCB_FILE_DEK + 4, 20);
	if (std::all_of(thumbprint.begin(), thumbprint.end(), [](uint8_t b) { return b == 0; })) {
		return;
	}
	if (!options.tde_key) {
		throw FormatError("backup file \"" + path + "\" is of a TDE database (its DEK wrapped by the certificate " +
						  ThumbprintHex(thumbprint) +
						  "): its pages need the certificate's key, a mssql_backup_key secret");
	}
	const std::vector<uint8_t> blob(boot.begin() + FCB_FILE_DEK + 28, boot.begin() + FCB_FILE_DEK + 28 + wrapped);
	index.SetPageKey(options.tde_key(thumbprint, blob));
}

std::unique_ptr<OpenedPages> OpenBackupPages(const OpenedHeaders &headers, const OpenPagesOptions &options) {
	const auto &set = headers.Set();
	const std::string &first = headers.Path(headers.families.front()->file);
	if (set.kind == BackupKind::Log) {
		throw FormatError("backup file \"" + first + "\" is a log backup: it holds no data pages to read");
	}
	const std::string partial = set.NotWholeDatabase();
	if (!partial.empty()) {
		// Its other files' pages are not in it, and the base's are of another time (a read/write
		// filegroup is consistent with the rest of the database only through the log).
		throw FormatError("backup file \"" + first + "\" is " + partial);
	}
	if (set.kind == BackupKind::Differential && !options.base) {
		// It lacks every page that did not change since its base, the catalog's among them (089 D5).
		throw FormatError("backup file \"" + first +
						  "\" is a differential backup: its pages are read over its full backup -- give the full "
						  "as the path and this one as diff := '...'");
	}
	if (set.kind != BackupKind::Differential && options.base) {
		throw FormatError("backup file \"" + first + "\" is not a differential backup, so it is not read over another");
	}
	std::unique_ptr<OpenedPages> pages(new OpenedPages());
	const size_t n = headers.families.size();
	pages->containers.resize(n);
	for (size_t i = 0; i < n; i++) {
		const auto &f = *headers.families[i];
		FamilyStream s;
		s.stream = &f.Stream();
		s.layout = &f.layout;
		if (f.container) {
			auto &c = pages->containers[i];
			c.file = f.access.get();
			c.stream = f.data_set.get();
			c.seek = &f.seek;
			c.chain_end = f.chain.end;
			c.decoder = f.decoder;
			c.keys = f.keys;
			s.container = &c;
		}
		pages->streams.push_back(s);
	}
	PageIndexOptions index_options;
	// A differential's pass 1 is ranked from its own DCM (DQ-41 revisited) -- a striped one's searched, its
	// extents the DCM set of its data files --, its later passes mapped by their headers (D4a).
	index_options.full_backup = !options.base;
	index_options.trust_layout = options.trust_layout;
	index_options.mapper = BoundPassMapper();
	std::vector<uint16_t> data_files;
	std::vector<uint16_t> marked_removed;
	for (const auto &file : set.files) {
		if (file.type != 'D') {
			continue;
		}
		// SQL Server's file ids are 1..32767; a page pointer carries one in 16 bits.
		if (file.file_id == 0 || file.file_id > 32767) {
			throw FormatError("backup file \"" + first + "\" lists data file \"" + file.logical_name + "\" with id " +
							  std::to_string(file.file_id) + ", outside 1..32767");
		}
		(file.marked_removed ? marked_removed : data_files).push_back(static_cast<uint16_t>(file.file_id));
	}
	// By id, as a pass orders its extents.
	index_options.data_files = data_files;
	std::sort(index_options.data_files.begin(), index_options.data_files.end());
	try {
		pages->index = BuildPageIndex(pages->streams, index_options);
		// D12: a page the differential does not carry is its full's; liveness is then the overlaid
		// allocation pages' (the map below reads them through the index), never the full's alone -- a
		// naive overlay doubled every row after a shrink (E2 apply_diff).
		pages->index.SetBase(options.base);
		ApplyTde(pages->index, options, first);
		// A file marked removed (SFIN flag 0x20, a hint: DatabaseFile::marked_removed) is one only when
		// the backup carries no page 0 of it -- asked as an expected page, so a searched pass is mapped
		// rather than trusted --; else it is a live file, read as any other (its id reused by a later ADD
		// FILE, or the bit meaning something else there; review of #549). The cost: for a file that is
		// removed, the answer "absent" maps every searched pass (a striped set, a container) by its
		// headers -- one read of such a backup's passes per open, paid only by a backup that lists a
		// file marked removed.
		for (uint16_t id : marked_removed) {
			PageLocation at;
			// This backup's own passes: under an overlay, the full's page 0 of a file removed since says nothing.
			if (pages->index.LocateOwn(id, 0, at, true)) {
				data_files.push_back(id);
			}
		}
		if (data_files.empty()) {
			throw FormatError("the set lists no data file");
		}
		pages->allocation.reset(new AllocationMap(pages->index, data_files));
	} catch (FormatError &e) {
		// Name the backup, as the headers' stage does (a page or file id alone does not say which).
		throw FormatError("backup file \"" + first + "\": " + e.what());
	}
	pages->store.reset(new PageStore(pages->index, *pages->allocation));
	return pages;
}

static std::string WithoutPrefix(const FormatError &e) {
	// The readers' messages start "backup: "; under the file's name that is said already.
	std::string why = e.what();
	if (why.compare(0, 8, "backup: ") == 0) {
		why = why.substr(8);
	}
	return why;
}

OpenedBackup::OpenedBackup(std::unique_ptr<OpenedHeaders> headers, shared_ptr<OpenedBackup> base)
	: headers_(std::move(headers)), base_(std::move(base)) {}

bool OpenedBackup::EnsureTables(ClientContext &context) {
	std::unique_lock<std::mutex> guard(lock_);
	while (building_) {
		// Another statement builds them: wait for it in slices, so an interrupt still ends this one.
		built_.wait_for(guard, std::chrono::milliseconds(100));
		if (context.IsInterrupted()) {
			throw InterruptException();
		}
	}
	if (tables_) {
		return false;
	}
	building_ = true;
	guard.unlock();
	std::unique_ptr<OpenedPages> pages;
	std::unique_ptr<BootstrapTables> tables(new BootstrapTables());
	try {
		OpenPagesOptions options;
		if (base_) {
			// A differential: its full's stages first (the statement's binding names both backups).
			base_->EnsureTables(context);
			options.base = &base_->Pages().index;
		}
		const std::string path = headers_->Path(headers_->families.front()->file);
		options.tde_key = [&context, path](const Thumbprint &thumbprint, const std::vector<uint8_t> &wrapped) {
			return SelectContentKey(context, path, thumbprint, wrapped, "a backup of a TDE database, its DEK");
		};
		pages = OpenBackupPages(*headers_, options);
		try {
			Page boot_page;
			pages->store->ReadLive(1, 9, boot_page, "the boot page");
			const BootPage boot = ReadBootPage(boot_page);
			CheckBootAgainstSet(boot, headers_->Set());
			*tables = ReadBootstrapTables(*pages->store, boot);
		} catch (FormatError &e) {
			throw FormatError("backup file \"" + headers_->Path(headers_->families.front()->file) +
							  "\": " + WithoutPrefix(e));
		}
	} catch (...) {
		guard.lock();
		building_ = false;
		built_.notify_all();
		throw;
	}
	guard.lock();
	pages_ = std::move(pages);
	tables_ = std::move(tables);
	building_ = false;
	built_.notify_all();
	return true;
}

const FuzzySummary &OpenedBackup::Fuzzy(ClientContext &context, bool *built) const {
	if (built) {
		*built = false;
	}
	std::unique_lock<std::mutex> guard(fuzzy_lock_);
	while (fuzzy_building_) {
		// Another statement reads it: wait for it in slices, so an interrupt still ends this one.
		fuzzy_built_.wait_for(guard, std::chrono::milliseconds(100));
		if (context.IsInterrupted()) {
			throw InterruptException();
		}
	}
	if (fuzzy_) {
		return *fuzzy_;
	}
	if (!fuzzy_error_.empty()) {
		throw FormatError(fuzzy_error_);
	}
	const std::string tde = headers_->TdeLogRefusal();
	if (!tde.empty()) {
		// Spec 087 D9: not parsed, so its fuzzy status is unknown -- said by name, kept like any tail's error.
		fuzzy_error_ = tde + ": its fuzzy status is unknown";
		throw FormatError(fuzzy_error_);
	}
	// Made before the build is marked: nothing may throw between the mark and the try that clears it.
	const auto &family = *headers_->families.front();
	std::unique_ptr<ReservedBytes> memory(
		new ReservedBytes(BufferManager::GetBufferManager(context),
						  "backup file \"" + headers_->Path(family.file) + "\": its log tail's summary"));
	std::unique_ptr<FuzzySummary> summary;
	fuzzy_building_ = true;
	guard.unlock();
	try {
		const auto &set = headers_->Set();
		try {
			LogTailReader tail(family.Stream(), family.layout, LogBlockHeaderBytes(set.software_major));
			// The cursor's chunk first, then the summary as it grows (D17; review of #591: built on the heap
			// before anything charged for it). A log backup's whole log is read here: Ctrl-C ends it.
			memory->Hold(LOG_CURSOR_BYTES);
			summary.reset(new FuzzySummary(SummarizeLogTail(tail, set.first_lsn, set.last_lsn, set.checkpoint_lsn,
															[&context, &memory](uint64_t bytes) {
																if (context.IsInterrupted()) {
																	throw InterruptException();
																}
																memory->Hold(LOG_CURSOR_BYTES + bytes);
															})));
		} catch (FileChangedError &e) {
			// A short read: the file changed under the statement, which may not hold for the next one. Not kept.
			throw FileChangedError("backup file \"" + headers_->Path(family.file) +
								   "\": its log tail: " + WithoutPrefix(e));
		} catch (FormatError &e) {
			const std::string why =
				"backup file \"" + headers_->Path(family.file) + "\": its log tail: " + WithoutPrefix(e);
			// The tail does not change under the entry: the next statement is told the same, not read again.
			guard.lock();
			fuzzy_error_ = why;
			guard.unlock();
			throw FormatError(why);
		}
		memory->Hold(summary->MemoryBytes());  // the cursor gone
	} catch (...) {
		guard.lock();
		fuzzy_building_ = false;
		fuzzy_built_.notify_all();
		throw;
	}
	guard.lock();
	fuzzy_ = std::move(summary);
	fuzzy_memory_ = std::move(memory);
	fuzzy_building_ = false;
	fuzzy_built_.notify_all();
	if (built) {
		*built = true;
	}
	return *fuzzy_;
}

bool OpenedBackup::HoldsTailReservation() const {
	std::lock_guard<std::mutex> guard(fuzzy_lock_);
	return fuzzy_memory_ != nullptr;
}

void PutInCache(ObjectCache &cache, const std::string &key, const shared_ptr<OpenedBackup> &backup) {
	// A summary the entry still holds reserved is weighed in first, so the cache's reservation counts it, and
	// given back after (review: an entry put by a later statement, an attached one with the setting turned
	// back on, was charged twice).
	const bool holds = backup->HoldsTailReservation();
	if (holds) {
		backup->Reweigh();
	}
	cache.Put(key, backup);
	if (holds) {
		backup->ReleaseTailReservation();
	}
}

void OpenedBackup::ReleaseTailReservation() const {
	std::unique_ptr<ReservedBytes> memory;
	{
		std::lock_guard<std::mutex> guard(fuzzy_lock_);
		memory = std::move(fuzzy_memory_);
	}
	// Given back outside the lock (FreeReservedMemory only adjusts a counter).
}

ReservedBytes::~ReservedBytes() {
	if (reserved_) {
		buffers_.FreeReservedMemory(reserved_);
	}
}

void ReservedBytes::Hold(uint64_t bytes) {
	const uint64_t target = (bytes + STEP - 1) / STEP * STEP;
	if (target < reserved_) {
		buffers_.FreeReservedMemory(reserved_ - target);
		reserved_ = target;
	}
	while (reserved_ < target) {
		try {
			buffers_.ReserveMemory(STEP);
		} catch (OutOfMemoryException &e) {
			throw OutOfMemoryException("%s (%s) passes what DuckDB's memory_limit leaves: %s", what_,
									   StringUtil::BytesToHumanReadableString(bytes), ErrorData(e).RawMessage());
		}
		reserved_ += STEP;
	}
}

const FuzzySummary &StatementTail(ClientContext &context, const OpenedForStatement &opened) {
	bool built = false;
	const FuzzySummary &tail = opened.backup->Fuzzy(context, &built);
	if (built && !opened.cache_key.empty()) {
		// The cache's reservation counts the summary from here (PutInCache): not held twice. Not cached, the
		// entry keeps its own for its life (review of #591: it was charged to nothing).
		PutInCache(ObjectCache::GetObjectCache(context), opened.cache_key, opened.backup);
	}
	return tail;
}

const FuzzySummary *TailOrWarn(ClientContext &context, const OpenedForStatement &opened, const char *function) {
	try {
		return &StatementTail(context, opened);
	} catch (FormatError &e) {
		DUCKDB_LOG_WARNING(context, "%s: the backup's log tail is not read, its fuzzy status unknown: %s", function,
						   e.what());
		return nullptr;
	}
}

//! What an entry holds, roughly, for the buffer pool's eviction (D9): the page index and the
//! allocation map as they measure themselves, a container family's recent blocks (up to 4 MiB) and
//! its chain, and the bootstrap tables at a few hundred bytes a row.
bool OpenedBackup::Reweigh() {
	std::lock_guard<std::mutex> weighing(weigh_lock_);
	size_t bytes = 64 * 1024 * headers_->families.size();
	for (const auto &f : headers_->families) {
		if (f->container) {
			bytes += 4 * 1024 * 1024 + f->chain.checkpoints.size() * 64 + f->seek.size() * 32;
		}
	}
	const OpenedPages *pages = nullptr;
	const BootstrapTables *tables = nullptr;
	{
		std::lock_guard<std::mutex> guard(lock_);
		pages = pages_.get();
		tables = tables_.get();
	}
	if (pages) {
		bytes += pages->index.MemoryBytes() + pages->allocation->MemoryBytes();
	}
	if (tables) {
		const size_t rows = tables->allocation_units.size() + tables->rowsets.size() + tables->objects.size() +
							tables->columns.size() + tables->rscols.size();
		bytes += rows * 256;
	}
	bytes += unit_pages_.MemoryBytes();
	{
		// Never held for a tail's read (Fuzzy reads outside it): a weigh does not wait for one.
		std::lock_guard<std::mutex> guard(fuzzy_lock_);
		if (fuzzy_) {
			bytes += fuzzy_->MemoryBytes();
		}
	}
	bytes += fuzziness_.MemoryBytes();
	return estimate_.exchange(bytes, std::memory_order_relaxed) != bytes;
}

//! Every file's path, size and version tag, in path order: the same files, unchanged, whatever order
//! or glob named them. Empty when a file has no version tag: an overwrite of the same size could not
//! be told from it, so such a backup is not cached.
static std::string CacheKey(const StatementFiles &files) {
	std::vector<std::string> parts;
	const auto &source = files.Source();
	for (size_t i = 0; i < source.FileCount(); i++) {
		const std::string version = source.VersionTag(i);
		if (version.empty()) {
			return std::string();
		}
		parts.push_back(source.Path(i) + '\x1f' + std::to_string(source.FileSize(i)) + '\x1f' + version);
	}
	std::sort(parts.begin(), parts.end());
	std::string key = "mssql_backup:";
	for (const auto &part : parts) {
		key += part;
		key += '\x1e';
	}
	return key;
}

static bool BackupCacheOn(ClientContext &context) {
	Value value;
	if (context.TryGetCurrentSetting("mssql_backup_cache", value) && !value.IsNull()) {
		return BooleanValue::Get(value.DefaultCastAs(LogicalType::BOOLEAN));
	}
	return true;
}

std::shared_ptr<const std::vector<PagePointer>> UnitPageCache::Get(
	uint64_t auid, const std::function<std::vector<PagePointer>()> &build) {
	{
		std::lock_guard<std::mutex> guard(lock_);
		auto found = units_.find(auid);
		if (found != units_.end()) {
			return found->second;
		}
	}
	// Built outside the lock (an IAM walk reads pages); two scans of one unit at once may both build it,
	// and the first kept is the one every later scan takes.
	auto built = std::make_shared<const std::vector<PagePointer>>(build());
	std::lock_guard<std::mutex> guard(lock_);
	return units_.emplace(auid, std::move(built)).first->second;
}

size_t UnitPageCache::MemoryBytes() {
	std::lock_guard<std::mutex> guard(lock_);
	size_t bytes = 0;
	for (const auto &unit : units_) {
		bytes += sizeof(unit) + unit.second->capacity() * sizeof(PagePointer);
	}
	return bytes;
}

void CheckDifferentialBase(const OpenedHeaders &full, const OpenedHeaders &diff) {
	const auto &f = full.Set();
	const auto &d = diff.Set();
	const std::string full_name = "\"" + full.Path(full.families.front()->file) + "\"";
	const std::string diff_name = "\"" + diff.Path(diff.families.front()->file) + "\"";
	auto kind = [](BackupKind k) {
		return k == BackupKind::Full ? "a full" : k == BackupKind::Differential ? "a differential" : "a log";
	};
	if (d.kind != BackupKind::Differential) {
		throw FormatError("diff := " + diff_name + " is " + kind(d.kind) + " backup, not a differential");
	}
	if (f.kind != BackupKind::Full) {
		throw FormatError("backup file " + full_name + " is " + kind(f.kind) +
						  " backup: diff := goes beside the full backup the differential is based on");
	}
	if (!f.family_guid.IsZero() && !d.family_guid.IsZero() && f.family_guid.bytes != d.family_guid.bytes) {
		throw FormatError("the differential " + diff_name + " is of another database than " + full_name +
						  " (family GUID " + d.family_guid.ToString() + ", not " + f.family_guid.ToString() + ")");
	}
	if (d.differential_base_guid.bytes != f.backup_set_guid.bytes) {
		// E1 rule 1: a differential over another full than its base corrupted the tables it changed.
		throw FormatError("the differential " + diff_name + " is based on backup set " +
						  d.differential_base_guid.ToString() + ", not on " + full_name + " (backup set " +
						  f.backup_set_guid.ToString() + "): give it its own full backup");
	}
}

//! `backup`'s files opened for this statement, each checked to be as when `backup` was opened (an
//! attached backup's, named `name`), its base's too for a differential.
static OpenedForStatement OpenHeld(ClientContext &context, const shared_ptr<OpenedBackup> &backup,
								   const std::string &cache_key, const std::string &name) {
	const OpenedHeaders &headers = backup->Headers();
	vector<std::string> paths;
	for (const auto &key : headers.files) {
		paths.push_back(key.path);
	}
	OpenedForStatement opened;
	try {
		opened.files.reset(new StatementFiles(context, ByteSource::Open(context, paths)));
	} catch (IOException &e) {
		throw InvalidInputException(
			"mssql backup: a file of %s is gone or unreadable since it was attached (%s): "
			"refresh a history (mssql_backup_refresh) or DETACH %s and ATTACH it again",
			name, e.what(), name);
	}
	const auto &source = opened.files->Source();
	for (const auto &key : headers.files) {
		const size_t file = opened.files->FileOf(key.path);
		if (file == source.FileCount() || source.FileSize(file) != key.size || source.VersionTag(file) != key.version) {
			throw InvalidInputException(
				"mssql backup: the file \"%s\" of %s changed since it was attached (its "
				"size or modification time): DETACH %s and ATTACH it again",
				key.path, name, name);
		}
	}
	opened.backup = backup;
	// Re-measured and put again by the statement (OpenBackupCatalog) only while the cache is on.
	opened.cache_key = BackupCacheOn(context) ? cache_key : std::string();
	opened.binding = BindingFor(headers, *opened.files);
	if (backup->Base()) {
		opened.base.reset(new OpenedForStatement(OpenHeld(context, backup->Base(), std::string(), name)));
		opened.binding.also = &opened.base->binding;
	}
	return opened;
}

//! The attached backup's files opened for this statement, each checked to be as its ATTACH opened it.
static OpenedForStatement OpenAttached(ClientContext &context, const AttachedBackup &attached) {
	return OpenHeld(context, attached.backup, attached.cache_key, attached.name);
}

static OpenedForStatement OpenBackupFiles(ClientContext &context, const std::vector<std::string> &paths,
										  const std::vector<std::string> &diff);

void CheckVersionFiles(const OpenedBackup &opened, const BackupHistory &history, const HistoryVersion &version,
					   const std::string &name, bool attached) {
	const OpenedBackup *full = opened.Base() ? opened.Base().get() : &opened;
	auto check = [&](const OpenedBackup &backup, const HistoryBackup &expected) {
		const auto &headers = backup.Headers();
		for (size_t i = 0; i < expected.paths.size(); i++) {
			bool same = headers.Set().backup_set_guid.bytes == expected.set.backup_set_guid.bytes;
			bool found = false;
			for (const auto &key : headers.files) {
				if (key.path == expected.paths[i]) {
					found = true;
					same = same && key.size == expected.sizes[i] && key.version == expected.version_tags[i];
				}
			}
			if (!found || !same) {
				if (attached) {
					throw InvalidInputException(
						"mssql backup: the file \"%s\" of %s changed since it was attached (its size, modification "
						"time or backup set): refresh a history (mssql_backup_refresh), or DETACH %s and ATTACH it "
						"again",
						expected.paths[i], name, name);
				}
				throw InvalidInputException(
					"mssql backup: the file \"%s\" of %s changed while it was read (its size, "
					"modification time or backup set): run the statement again",
					expected.paths[i], name);
			}
		}
	};
	check(*full, history.backups[version.full]);
	if (version.IsDifferential()) {
		check(opened, history.backups[version.diff]);
	}
}

uint64_t ResolveVersion(const BackupHistory &history, optional_ptr<ClientContext> context, const BackupAt &at,
						const std::string &what) {
	const uint64_t latest = history.versions.back().number;
	if (!at.IsSet()) {
		return latest;
	}
	const std::string unit = StringUtil::Lower(at.unit);
	if (unit != "version" && unit != "timestamp") {
		throw BinderException("%s: AT (%s => …) names no version of a backup history (VERSION or TIMESTAMP)", what,
							  at.unit);
	}
	if (at.value.IsNull()) {
		throw BinderException("%s: %s NULL names no version", what, unit);
	}
	if (unit == "version") {
		// A whole number: 1.6 or '2.5' names no version (a string, as DuckLake's METADATA_PARAMETERS sends
		// one, when it reads as one).
		// Exactly a whole number (review of #579: 1.6 is no version, nor is an exact DECIMAL a hair past
		// 1): the number cast back to the value's own type must be the value.
		int64_t number = 0;
		if (at.value.type().IsIntegral()) {
			number = at.value.DefaultCastAs(LogicalType::BIGINT).GetValue<int64_t>();
		} else {
			const auto as_number = at.value.DefaultTryCastAs(LogicalType::BIGINT, nullptr, true);
			const auto back = as_number ? as_number->DefaultTryCastAs(at.value.type()) : optional<Value>();
			if (!as_number || !back || *back != at.value) {
				throw BinderException("%s: VERSION %s is not a version number (a whole number, 1 the first)", what,
									  at.value.ToString());
			}
			number = as_number->GetValue<int64_t>();
		}
		if (number < 1 || static_cast<uint64_t>(number) > latest) {
			throw BinderException("%s has versions 1 to %llu, not %lld (mssql_backup_snapshots lists them)", what,
								  (unsigned long long)latest, (long long)number);
		}
		return static_cast<uint64_t>(number);
	}
	// The backups' times are the server's local time, as RESTORE HEADERONLY gives them: compared as a
	// TIMESTAMP (a TIMESTAMPTZ cast to one in the session's time zone).
	const Value cast =
		context ? at.value.CastAs(*context, LogicalType::TIMESTAMP) : at.value.DefaultCastAs(LogicalType::TIMESTAMP);
	const timestamp_t when = cast.GetValue<timestamp_t>();
	if (when == timestamp_t::infinity()) {
		return latest;
	}
	const auto &first = history.Last(history.versions.front()).set.backup_finish_date;
	if (!when.IsFinite()) {
		throw BinderException("%s: no version finished by %s (the first finished at %s)", what, cast.ToString(),
							  first.ToString());
	}
	date_t day;
	dtime_t time;
	Timestamp::Convert(when, day, time);
	int32_t year, month, mday, hour, minute, second, micros;
	Date::Convert(day, year, month, mday);
	Time::Convert(time, hour, minute, second, micros);
	MtfDate by;
	by.year = year;
	by.month = month;
	by.day = mday;
	by.hour = hour;
	by.minute = minute;
	by.second = second;
	const HistoryVersion *found = history.FinishedBy(by);
	if (!found) {
		throw BinderException("%s: no version finished by %s (the first finished at %s)", what, cast.ToString(),
							  first.ToString());
	}
	return found->number;
}

BackupAt BackupAtOf(const named_argument_map_t &named, const char *function) {
	BackupAt at;
	for (const auto &kv : named) {
		if ((kv.first != "version" && kv.first != "as_of") || kv.second.IsNull()) {
			continue;
		}
		if (at.IsSet()) {
			throw BinderException("%s: version := and as_of := name a version twice: give one", function);
		}
		at.unit = kv.first == "version" ? "version" : "timestamp";
		at.value = kv.second;
	}
	return at;
}

void AddVersionParameters(TableFunction &function) {
	// ANY, so the value reaches ResolveVersion as given (a BIGINT parameter would round 1.6 to 2 first).
	function.GetSignature().AddKeywordOnly("version", LogicalType::ANY, Value());
	function.GetSignature().AddKeywordOnly("as_of", LogicalType::TIMESTAMP, Value(LogicalType::TIMESTAMP));
}

//! The version `at` names in the history of the files on `paths`: its full and differential paths into
//! `paths` / `diff`, the history and the version kept to check the files opened next against them.
struct ChosenVersion {
	BackupHistory history;
	HistoryVersion version;
	std::string name;
};

static ChosenVersion PathsOfVersion(ClientContext &context, std::vector<std::string> &paths,
									std::vector<std::string> &diff, const BackupAt &at) {
	if (!diff.empty()) {
		throw BinderException(
			"mssql backup: version := / as_of := pick a version of the files' history: diff := "
			"does not go beside them");
	}
	ChosenVersion chosen;
	chosen.name = "the backups \"" + paths.front() + "\"";
	chosen.history = ReadBackupHistory(context, paths);
	const auto number = ResolveVersion(chosen.history, context, at, chosen.name);
	chosen.version = chosen.history.versions[number - 1];
	paths = chosen.history.backups[chosen.version.full].paths;
	if (chosen.version.IsDifferential()) {
		diff = chosen.history.backups[chosen.version.diff].paths;
	}
	return chosen;
}

OpenedForStatement OpenBackup(ClientContext &context, const std::vector<std::string> &paths_p,
							  const std::vector<std::string> &diff_p, const BackupAt &at) {
	std::vector<std::string> paths = paths_p;
	std::vector<std::string> diff = diff_p;
	if (paths.size() == 1) {
		auto attached = FindAttachedBackup(context, paths.front(), at);
		if (attached) {
			if (!diff.empty()) {
				throw InvalidInputException("mssql backup: %s is an attached backup: give diff := to its ATTACH",
											attached->name);
			}
			return OpenAttached(context, *attached);
		}
	}
	if (at.IsSet()) {
		// The files opened must be the ones the history chose (review of #579: one replaced meanwhile).
		const auto chosen = PathsOfVersion(context, paths, diff, at);
		auto opened = OpenBackupFiles(context, paths, diff);
		CheckVersionFiles(*opened.backup, chosen.history, chosen.version, chosen.name, false);
		return opened;
	}
	return OpenBackupFiles(context, paths, diff);
}

//! Spec 087 D4: a cached entry's encrypted families selected again, every call: a `DROP SECRET` (or a
//! changed scope) takes effect at once, the cached key used only while a secret still holds it.
static void SelectKeysAgain(ClientContext &context, const OpenedHeaders &headers) {
	for (const auto &f : headers.families) {
		if (f->keys.backup) {
			SelectContentKey(context, headers.Path(f->file), f->container_header.encryptor_thumbprint,
							 f->container_header.wrapped_key, "a backup");
		}
		if (f->keys.dek) {
			SelectContentKey(context, headers.Path(f->file), f->container_header.tde_thumbprint,
							 f->container_header.wrapped_dek, "a backup of a TDE database, its DEK");
		}
	}
}

//! The differential on `diff` over `full` (opened for the statement): its own files, its entry in the
//! ObjectCache keyed by both backups' files (the full's stages its base), its chain checked.
static OpenedForStatement OpenOverlay(ClientContext &context, OpenedForStatement full,
									  const std::vector<std::string> &diff) {
	OpenedForStatement opened;
	opened.files.reset(
		new StatementFiles(context, ByteSource::Open(context, vector<std::string>(diff.begin(), diff.end()))));
	auto &cache = ObjectCache::GetObjectCache(context);
	const std::string diff_key = CacheKey(*opened.files);
	if (!full.cache_key.empty() && !diff_key.empty()) {
		opened.cache_key = "mssql_backup_overlay:" + full.cache_key + '\x1d' + diff_key;
		opened.backup = cache.Get<OpenedBackup>(opened.cache_key);
		if (opened.backup) {
			SelectKeysAgain(context, opened.backup->Headers());
		}
	}
	if (opened.backup && opened.backup->Base() != full.backup) {
		// The overlay's entry outlived the full's (each is evicted on its own) or was put by another
		// statement that opened the full meanwhile: its base is the full the overlay was built on, so the
		// statement's files of the full -- the same files, by the key -- are bound to that one, and it is
		// put back as the full's entry (review of #575): the overlay holds it anyway, so one copy of the
		// full is kept and counted, not that one unaccounted beside a second built anew.
		full.backup = opened.backup->Base();
		full.binding = BindingFor(full.backup->Headers(), *full.files);
		if (!full.cache_key.empty()) {
			PutInCache(cache, full.cache_key, full.backup);
		}
	}
	if (!opened.backup) {
		auto headers = ReadBackupHeaders(*opened.files);
		CheckDifferentialBase(full.backup->Headers(), *headers);
		opened.backup = make_shared_ptr<OpenedBackup>(std::move(headers), full.backup);
		if (!opened.cache_key.empty()) {
			opened.backup->Reweigh();
			PutInCache(cache, opened.cache_key, opened.backup);
		}
	}
	opened.binding = BindingFor(opened.backup->Headers(), *opened.files);
	opened.base.reset(new OpenedForStatement(std::move(full)));
	opened.binding.also = &opened.base->binding;
	return opened;
}

static OpenedForStatement OpenBackupFiles(ClientContext &context, const std::vector<std::string> &paths,
										  const std::vector<std::string> &diff) {
	if (!diff.empty()) {
		return OpenOverlay(context, OpenBackupFiles(context, paths, std::vector<std::string>()), diff);
	}
	OpenedForStatement opened;
	opened.files.reset(
		new StatementFiles(context, ByteSource::Open(context, vector<std::string>(paths.begin(), paths.end()))));
	auto &cache = ObjectCache::GetObjectCache(context);
	// D9: the key (each file's path, size and version tag) is trusted alone, no header read again on a
	// hit: a file rewritten in place at the same size whose modification time reads as before is taken for
	// the cached one (owner, review of #567; documented, `mssql_backup_cache = false` the way out).
	if (BackupCacheOn(context)) {
		opened.cache_key = CacheKey(*opened.files);
		opened.backup = cache.Get<OpenedBackup>(opened.cache_key);
		if (opened.backup) {
			SelectKeysAgain(context, opened.backup->Headers());
		}
	}
	if (!opened.backup) {
		opened.backup = make_shared_ptr<OpenedBackup>(ReadBackupHeaders(*opened.files));
		if (!opened.cache_key.empty()) {
			opened.backup->Reweigh();
			PutInCache(cache, opened.cache_key, opened.backup);
		}
	}
	opened.binding = BindingFor(opened.backup->Headers(), *opened.files);
	return opened;
}

//! The shared stages of the backup `catalog->opened` opened, built now when they are not, into `catalog`.
static void TakeStages(ClientContext &context, OpenedCatalog &catalog) {
	auto &opened = catalog.opened;
	BindFiles bound(opened.binding);
	opened.backup->EnsureTables(context);
	// What it holds grows as statements read it (the stages just built, the searches' samples, GAM
	// intervals, the units' page lists): measured at every open, outside the cache's lock, and put
	// again when it changed, the reservation being taken at a Put.
	if (!opened.cache_key.empty() && opened.backup->Reweigh()) {
		PutInCache(ObjectCache::GetObjectCache(context), opened.cache_key, opened.backup);
	}
	// A differential's full: its stages were built through the overlay, and grow as its reads search it.
	if (opened.base && !opened.base->cache_key.empty() && opened.base->backup->Reweigh()) {
		PutInCache(ObjectCache::GetObjectCache(context), opened.base->cache_key, opened.base->backup);
	}
	catalog.headers = &opened.backup->Headers();
	catalog.pages = &opened.backup->Pages();
	catalog.tables = &opened.backup->Tables();
	catalog.unit_pages = &opened.backup->UnitPages();
}

std::unique_ptr<OpenedCatalog> OpenAttachedCatalog(ClientContext &context, const AttachedBackup &attached) {
	std::unique_ptr<OpenedCatalog> catalog(new OpenedCatalog());
	catalog->opened = OpenAttached(context, attached);
	TakeStages(context, *catalog);
	catalog->model = attached.model;
	return catalog;
}

std::shared_ptr<const CatalogModel> WholeModel(const OpenedCatalog &catalog, const std::string &what) {
	BindFiles bound(catalog.Binding());	 // the model reads the catalog's pages
	auto model = std::make_shared<CatalogModel>();
	try {
		*model = BuildCatalogModel(
			*catalog.pages->store, *catalog.tables,
			[](const SqlVariant &v, const std::string &what) { (void)SqlVariantValue(v, what); }, CatalogScope());
	} catch (FormatError &e) {
		throw FormatError(what + ": " + WithoutPrefix(e));
	}
	return model;
}

std::unique_ptr<OpenedCatalog> OpenBackupCatalog(ClientContext &context, const std::vector<std::string> &paths_p,
												 bool with_model, const CatalogScope &scope,
												 const std::vector<std::string> &diff_p, const BackupAt &at) {
	std::vector<std::string> paths = paths_p;
	std::vector<std::string> diff = diff_p;
	if (paths.size() == 1) {
		auto attached = FindAttachedBackup(context, paths.front(), at);
		if (attached) {
			if (!diff.empty()) {
				throw InvalidInputException("mssql backup: %s is an attached backup: give diff := to its ATTACH",
											attached->name);
			}
			return OpenAttachedCatalog(context, *attached);
		}
	}
	std::unique_ptr<OpenedCatalog> catalog(new OpenedCatalog());
	if (at.IsSet()) {
		const auto chosen = PathsOfVersion(context, paths, diff, at);
		catalog->opened = OpenBackupFiles(context, paths, diff);
		CheckVersionFiles(*catalog->opened.backup, chosen.history, chosen.version, chosen.name, false);
	} else {
		catalog->opened = OpenBackupFiles(context, paths, diff);
	}
	TakeStages(context, *catalog);
	auto model = std::make_shared<CatalogModel>();
	catalog->model = model;
	if (with_model) {
		BindFiles bound(catalog->Binding());  // the model reads the catalog's pages
		try {
			// A boundary decodes when the value mssql_backup_tables would give it does: one verdict, the
			// model's (review of #551).
			*model = BuildCatalogModel(
				*catalog->pages->store, *catalog->tables,
				[](const SqlVariant &v, const std::string &what) { (void)SqlVariantValue(v, what); }, scope);
		} catch (FormatError &e) {
			throw FormatError("backup file \"" + paths.front() + "\"" +
							  (diff.empty() ? std::string() : " with the differential \"" + diff.front() + "\"") +
							  ": " + WithoutPrefix(e));
		}
	}
	return catalog;
}

std::shared_ptr<AttachedBackup> OpenAttachedBackup(
	ClientContext &context, const std::string &name, const std::vector<std::string> &paths,
	std::function<bool(const std::string &schema)> wants_schema,
	std::function<bool(const std::string &schema, const std::string &object)> wants_object,
	const std::vector<std::string> &diff) {
	auto attached = std::make_shared<AttachedBackup>();
	attached->name = name;
	attached->wants_schema = std::move(wants_schema);
	OpenedForStatement opened = OpenBackupFiles(context, paths, diff);
	BindFiles bound(opened.binding);
	opened.backup->EnsureTables(context);
	if (!opened.cache_key.empty() && opened.backup->Reweigh()) {
		PutInCache(ObjectCache::GetObjectCache(context), opened.cache_key, opened.backup);
	}
	CatalogScope scope;
	if (wants_object) {
		scope.objects = std::make_shared<const std::function<bool(const std::string &, const std::string &)>>(
			std::move(wants_object));
	}
	auto model = std::make_shared<CatalogModel>();
	try {
		*model = BuildCatalogModel(
			*opened.backup->Pages().store, opened.backup->Tables(),
			[](const SqlVariant &v, const std::string &what) { (void)SqlVariantValue(v, what); }, scope);
	} catch (FormatError &e) {
		throw FormatError("backup file \"" + paths.front() + "\"" +
						  (diff.empty() ? std::string() : " with the differential \"" + diff.front() + "\"") + ": " +
						  WithoutPrefix(e));
	}
	// The schemas a listing of the attached backup shows are the filter's too.
	model->schemas.erase(std::remove_if(model->schemas.begin(), model->schemas.end(),
										[&](const CatalogSchema &s) { return !attached->WantsSchema(s.name); }),
						 model->schemas.end());
	attached->backup = opened.backup;
	attached->cache_key = opened.cache_key;
	attached->objects = scope.objects;
	attached->model = std::move(model);
	return attached;
}

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb
