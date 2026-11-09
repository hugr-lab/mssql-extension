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
	families = std::move(ordered);
	return opened;
}

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb
