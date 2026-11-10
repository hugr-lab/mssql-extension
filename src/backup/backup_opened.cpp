#include "backup/backup_opened.hpp"

#include "backup/backup_xpress.hpp"
#include "backup/backup_zstd.hpp"

#include <algorithm>
#include <map>

namespace duckdb {
namespace mssql {
namespace backup {

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

static std::unique_ptr<OpenedFamily> OpenFamily(ByteSource &source, size_t file) {
	std::unique_ptr<OpenedFamily> f(new OpenedFamily());
	f->file = file;
	f->access.reset(new SourceAccess(source, file));
	const RandomAccess &access = *f->access;
	if (IsContainer(access)) {
		f->container = true;
		f->container_header = ReadContainerHeader(access);
		if (f->container_header.encrypted) {
			throw FormatError("backup file \"" + access.Name() +
							  "\" is an encrypted backup: reading one is spec 087's, not supported yet");
		}
		// DQ-41: the chain located from the file's tail, walked only where it is read; a tail of
		// another layout is walked whole.
		if (!OpenChainFromTail(access, f->chain, f->seek)) {
			f->chain = WalkChain(access);
			f->seek = ReadSeekIndex(access, f->chain);
		}
		f->decoder = DecoderFor(f->container_header.algorithm);
		f->data_set.reset(new ContainerStream(access, f->chain, f->decoder));
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
		auto tape = ReadContainerTape(access, f->decoder);
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

std::unique_ptr<OpenedHeaders> OpenBackupHeaders(ClientContext &context, const std::vector<std::string> &paths) {
	std::unique_ptr<OpenedHeaders> opened(new OpenedHeaders());
	opened->source = ByteSource::Open(context, vector<std::string>(paths.begin(), paths.end()));
	auto &source = *opened->source;
	for (size_t i = 0; i < source.FileCount(); i++) {
		try {
			opened->families.push_back(OpenFamily(source, i));
		} catch (FormatError &e) {
			// Name the file where the parser below could not (a ByteReader knows offsets only).
			const std::string &path = source.Path(i);
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
			throw FormatError("backup files \"" + source.Path(first.file) + "\" and \"" + source.Path(f->file) +
							  "\": one is a compressed container and the other is not, not one media set");
		}
		if (f->set.backup_set_guid.bytes != first.set.backup_set_guid.bytes) {
			throw FormatError("backup files \"" + source.Path(first.file) + "\" and \"" + source.Path(f->file) +
							  "\" belong to different backup sets (" + first.set.backup_set_guid.ToString() + ", " +
							  f->set.backup_set_guid.ToString() + ")");
		}
	}
	if (first.set.family_count == 0) {
		throw FormatError("backup file \"" + source.Path(first.file) +
						  "\": its TAPE block carries no RAID stream, so its media set's families are unknown");
	}
	// E2 R9: every family of the media set, once.
	const uint32_t count = first.set.family_count;
	std::map<uint32_t, size_t> by_sequence;
	for (size_t i = 0; i < families.size(); i++) {
		const auto &set = families[i]->set;
		const std::string &path = source.Path(families[i]->file);
		if (set.media_set_guid.bytes != first.set.media_set_guid.bytes || set.family_count != count) {
			throw FormatError("backup file \"" + path + "\" is a family of another media set than \"" +
							  source.Path(first.file) + "\"");
		}
		if (set.family_sequence_number == 0 || set.family_sequence_number > count) {
			throw FormatError("backup file \"" + path + "\" says it is family " +
							  std::to_string(set.family_sequence_number) + " of " + std::to_string(count));
		}
		if (!by_sequence.emplace(set.family_sequence_number, i).second) {
			throw FormatError("backup files \"" + source.Path(families[by_sequence[set.family_sequence_number]]->file) +
							  "\" and \"" + path + "\" are both family " + std::to_string(set.family_sequence_number) +
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
		throw FormatError("backup file \"" + source.Path(first.file) + "\" is one family of a media set of " +
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
			throw FormatError("backup files \"" + source.Path(ordered.front()->file) + "\" and \"" +
							  source.Path(f->file) + "\" record different sizes of their backup set (BackupSize)");
		}
	}
	if (ordered.front()->set.recorded_backup_size != 0 && ordered.front()->set.recorded_backup_size != computed) {
		throw FormatError("backup file \"" + source.Path(ordered.front()->file) +
						  "\": its trailer records a BackupSize of " +
						  std::to_string(ordered.front()->set.recorded_backup_size) + " bytes, its layout gives " +
						  std::to_string(computed));
	}
	families = std::move(ordered);
	return opened;
}

std::unique_ptr<OpenedPages> OpenBackupPages(ClientContext &context, const OpenedHeaders &headers,
											 const OpenPagesOptions &options) {
	const auto &set = headers.Set();
	const std::string &first = headers.source->Path(headers.families.front()->file);
	if (set.kind == BackupKind::Log) {
		throw FormatError("backup file \"" + first + "\" is a log backup: it holds no data pages to read");
	}
	if (set.kind == BackupKind::Differential) {
		throw FormatError("backup file \"" + first +
						  "\" is a differential backup: its pages are read over its full backup, which is not "
						  "supported yet");
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
			s.container = &c;
		}
		pages->streams.push_back(s);
	}
	PageIndexOptions index_options;
	index_options.full_backup = true;
	index_options.trust_layout = options.trust_layout;
	index_options.mapper = RangedPassMapper(context);
	std::vector<uint16_t> data_files;
	for (const auto &file : set.files) {
		if (file.type != 'D') {
			continue;
		}
		// SQL Server's file ids are 1..32767; a page pointer carries one in 16 bits.
		if (file.file_id == 0 || file.file_id > 32767) {
			throw FormatError("backup file \"" + first + "\" lists data file \"" + file.logical_name + "\" with id " +
							  std::to_string(file.file_id) + ", outside 1..32767");
		}
		data_files.push_back(static_cast<uint16_t>(file.file_id));
	}
	if (data_files.empty()) {
		throw FormatError("backup file \"" + first + "\" lists no data file");
	}
	try {
		pages->index = BuildPageIndex(pages->streams, index_options);
		// A removed file (SFIN flag 0x20) has no page in the backup (files fixture, 2022). One that has
		// would mean the flag says something else here, and its pages would silently read as not live.
		for (uint32_t removed : set.removed_file_ids) {
			PageLocation at;
			if (removed >= 1 && removed <= 32767 && pages->index.Locate(static_cast<uint16_t>(removed), 0, at)) {
				throw FormatError("file " + std::to_string(removed) +
								  " is marked removed (SFIN flag 0x20), and the backup carries its pages: a file "
								  "state the reader does not know");
			}
		}
		pages->allocation.reset(new AllocationMap(pages->index, data_files));
	} catch (FormatError &e) {
		// Name the backup, as the headers' stage does (a page or file id alone does not say which).
		throw FormatError("backup file \"" + first + "\": " + e.what());
	}
	pages->store.reset(new PageStore(pages->index, *pages->allocation));
	return pages;
}

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb
