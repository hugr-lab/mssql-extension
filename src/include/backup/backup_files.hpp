//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// backup/backup_files.hpp
//
// Spec 085 D9, task 22: what an opened backup reads its files through. An opened backup's parsed
// state -- the chains, the page index and its searches, the allocation bits, the catalog -- can
// outlive a statement (the opened-file cache, an ATTACH); its files cannot: a ByteSource's handles
// keep the opening ClientContext (backup_source.hpp). So the long-lived objects read a family file
// through a BoundFile, which holds the file's name and size only and reads through the statement's
// files bound to the thread (BindFiles), or the backup's own when it was opened for one call.
// [DuckDB]
//===----------------------------------------------------------------------===//

#pragma once

#include "backup/backup_open_pass.hpp"
#include "backup/backup_source.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace duckdb {
class ClientContext;

namespace mssql {
namespace backup {

//! A backup's files as one statement opened them: its ByteSource and an access per file.
class StatementFiles {
public:
	StatementFiles(ClientContext &context, std::unique_ptr<ByteSource> source);

	ClientContext &Context() const {
		return context_;
	}
	ByteSource &Source() const {
		return *source_;
	}
	const SourceAccess &Access(size_t file) const {
		return *accesses_[file];
	}
	//! The source's file whose path is `path`, or FileCount() when none is.
	size_t FileOf(const std::string &path) const;

private:
	ClientContext &context_;
	std::unique_ptr<ByteSource> source_;
	std::vector<std::unique_ptr<SourceAccess>> accesses_;
};

//! Which statement files one opened backup reads: for each of the backup's files (its number in the
//! backup, BoundFile::Number) the file of `files` that is it.
struct FileBinding {
	uint64_t backup = 0;
	const StatementFiles *files = nullptr;
	std::vector<size_t> file_of;
	//! Task 24: a differential's binding names its full's too, so one BindFiles binds both.
	const FileBinding *also = nullptr;
};

//! Binds `binding` to this thread for the scope's life: a BoundFile of its backup read on the thread
//! reads through it. Scopes nest (a scan of one backup inside a call of another); the innermost
//! binding of a backup wins.
class BindFiles {
public:
	explicit BindFiles(const FileBinding &binding);
	~BindFiles();
	BindFiles(const BindFiles &) = delete;
	BindFiles &operator=(const BindFiles &) = delete;

private:
	const FileBinding &binding_;
	const BindFiles *previous_;
	friend class BoundFile;
};

//! One file of an opened backup, as its long-lived objects read it: through the binding of its backup
//! on the reading thread, else through `own` (a backup opened for one call keeps its statement's
//! files, and its objects never outlive them). A read with neither is an internal error: the reader
//! was called outside the statement that bound the files.
class BoundFile final : public RandomAccess {
public:
	BoundFile(uint64_t backup, size_t number, std::string path, uint64_t size, const FileBinding *own);

	uint64_t Size() const override {
		return size_;
	}
	void ReadAt(uint64_t offset, uint8_t *dest, size_t count) const override;
	const std::string &Name() const override {
		return path_;
	}
	size_t Number() const {
		return number_;
	}
	//! The statement files this file reads through on this thread, and its file there.
	const StatementFiles &Files(size_t &file) const;

private:
	const FileBinding &Binding() const;

	const uint64_t backup_;
	const size_t number_;
	const std::string path_;
	const uint64_t size_;
	const FileBinding *own_;
};

//! A PassMapper for BuildPageIndex that keeps no context: a family whose stream is a BoundFile is
//! scanned in ranges through the statement files it reads on the thread (BuildPassMapInRanges), any
//! other (a container's decoded data set) through BuildPassMap. Kept by the page index for a fallback
//! (task 16c), which then runs in whatever statement reads the index.
PassMapper BoundPassMapper(const PassScanOptions &options = PassScanOptions());

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb
