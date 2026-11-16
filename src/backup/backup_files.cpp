#include "backup/backup_files.hpp"

#include "duckdb/common/exception.hpp"

namespace duckdb {
namespace mssql {
namespace backup {

StatementFiles::StatementFiles(ClientContext &context, std::unique_ptr<ByteSource> source)
	: context_(context), source_(std::move(source)) {
	for (size_t i = 0; i < source_->FileCount(); i++) {
		accesses_.emplace_back(new SourceAccess(*source_, i));
	}
}

size_t StatementFiles::FileOf(const std::string &path) const {
	for (size_t i = 0; i < source_->FileCount(); i++) {
		if (source_->Path(i) == path) {
			return i;
		}
	}
	return source_->FileCount();
}

//! The innermost binding scope of this thread.
static thread_local const BindFiles *innermost = nullptr;

BindFiles::BindFiles(const FileBinding &binding) : binding_(binding), previous_(innermost) {
	innermost = this;
}

BindFiles::~BindFiles() {
	innermost = previous_;
}

BoundFile::BoundFile(uint64_t backup, size_t number, std::string path, uint64_t size, const FileBinding *own)
	: backup_(backup), number_(number), path_(std::move(path)), size_(size), own_(own) {}

const FileBinding &BoundFile::Binding() const {
	for (const BindFiles *scope = innermost; scope; scope = scope->previous_) {
		if (scope->binding_.backup == backup_) {
			return scope->binding_;
		}
	}
	if (own_) {
		return *own_;
	}
	throw InternalException("mssql backup: \"%s\" read outside the statement that opened its files", path_);
}

const StatementFiles &BoundFile::Files(size_t &file) const {
	const auto &binding = Binding();
	file = binding.file_of[number_];
	return *binding.files;
}

void BoundFile::ReadAt(uint64_t offset, uint8_t *dest, size_t count) const {
	size_t file = 0;
	Files(file).Access(file).ReadAt(offset, dest, count);
}

PassMapper BoundPassMapper(const PassScanOptions &options) {
	return [options](const RandomAccess &stream, uint64_t offset, uint64_t length, uint32_t number) {
		auto bound = dynamic_cast<const BoundFile *>(&stream);
		if (!bound) {
			return BuildPassMap(stream, offset, length, number);
		}
		size_t file = 0;
		const auto &files = bound->Files(file);
		return BuildPassMapInRanges(files.Context(), files.Source(), file, offset, length, number, options);
	};
}

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb
