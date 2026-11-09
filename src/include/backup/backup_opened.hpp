//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// backup/backup_opened.hpp
//
// Spec 085 task 9: stage 1 of an opened backup (§ Data structures, OpenedBackup): the files of one
// media set opened through DuckDB's file system (ByteSource), each a plain MTF family or a
// compressed container (located from its tail, DQ-41), walked for its headers and its backup set;
// the families checked to be one complete set. Enough for mssql_backup_info; the page index and
// the catalog are later stages. The opened-file cache (D9) is task 22. [DuckDB]
//===----------------------------------------------------------------------===//

#pragma once

#include "backup/backup_container.hpp"
#include "backup/backup_mtf.hpp"
#include "backup/backup_open_pass.hpp"
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

//! A media set's families, ordered by family sequence number (a container's by the order given).
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

//! The files of `paths` (paths or globs: the families of one media set) opened, walked and checked:
//! one backup set per file (an appended set is refused), the same backup set GUID in every family,
//! and for plain families every FamilySequenceNumber of the media set's FamilyCount present once
//! (E2 R9: a missing one is named); a compressed family's media fields come from its container's
//! header (ReadContainerTape). An encrypted backup is refused (spec 087).
std::unique_ptr<OpenedHeaders> OpenBackupHeaders(ClientContext &context, const std::vector<std::string> &paths);

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb
