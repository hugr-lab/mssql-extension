// Spec 085 task 2, D3 / D3a: ByteSource over local files in an in-process DuckDB.
//
// Reads through CachingFileSystem return the file's bytes; a read past the end is a FormatError
// naming the file; a glob expands sorted and an empty one is refused; fetch tasks fill a batch
// (run in place and through ExecuteTasksSynchronously); with local caching on and a tiny cache
// block, a range that crosses blocks comes back contiguous (the copy path). A fetch that fails
// inside its task cannot be induced on a local file; that path is reached through the remote
// lane (task 25).

#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "backup/backup_source.hpp"
#include "duckdb.hpp"

using namespace duckdb;
using namespace duckdb::mssql::backup;

namespace {

int failures = 0;

void Check(bool ok, const std::string &what) {
	std::cout << (ok ? "  ok   " : "  FAIL ") << what << std::endl;
	if (!ok) {
		failures++;
	}
}

std::vector<uint8_t> Pattern(size_t n, uint8_t seed) {
	std::vector<uint8_t> bytes(n);
	for (size_t i = 0; i < n; i++) {
		bytes[i] = static_cast<uint8_t>((i * 31 + seed) & 0xFF);
	}
	return bytes;
}

void WriteFile(const std::string &path, const std::vector<uint8_t> &bytes) {
	std::ofstream out(path, std::ios::binary);
	out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

bool Same(const FetchedRange &fetched, const std::vector<uint8_t> &bytes) {
	const auto &r = fetched.Range();
	if (fetched.Size() != r.length) {
		return false;
	}
	for (size_t i = 0; i < r.length; i++) {
		if (fetched.Data()[i] != bytes[r.offset + i]) {
			return false;
		}
	}
	return true;
}

ByteRange R(uint32_t family, uint64_t offset, uint64_t length) {
	ByteRange r;
	r.family = family;
	r.offset = offset;
	r.length = length;
	return r;
}

}  // namespace

int main() {
	std::cout << "ByteSource (spec 085 task 2)" << std::endl;
	const char *tmp = std::getenv("TMPDIR");
	std::string base = tmp ? tmp : "/tmp";
	while (base.size() > 1 && base.back() == '/') {
		base.pop_back();
	}
	std::string dir = base + "/mssql_backup_source_test";
	std::string mkdir = "mkdir -p '" + dir + "'";
	if (std::system(mkdir.c_str()) != 0) {
		std::cout << "cannot create " << dir << std::endl;
		return 1;
	}
	auto a = Pattern(300000, 1);
	auto b = Pattern(70000, 7);
	WriteFile(dir + "/set_1.bak", a);
	WriteFile(dir + "/set_2.bak", b);

	DuckDB db(nullptr);
	Connection con(db);
	auto &context = *con.context;

	auto source = ByteSource::Open(context, {dir + "/set_*.bak"});
	Check(source->FileCount() == 2 && source->Path(0) == dir + "/set_1.bak" && source->FileSize(0) == a.size() &&
			  source->FileSize(1) == b.size(),
		  "a glob expands to both files, sorted, with their sizes");
	Check(!source->VersionTag(0).empty() && !source->IsRemote(0), "a local file has a version tag (mtime)");

	auto head = source->Read(R(0, 1000, 8192), "a page");
	Check(Same(head, a), "a synchronous read returns the file's bytes");
	Check(head.Reader("a page").U8(0) == a[1000] && head.Reader("a page").Base() == 1000,
		  "its span carries the file offset");

	std::string past;
	try {
		source->Read(R(1, 69990, 20), "a block header");
	} catch (FormatError &e) {
		past = e.what();
	}
	Check(past.find("set_2.bak") != std::string::npos && past.find("a block header") != std::string::npos &&
			  past.find("70000-byte file") != std::string::npos,
		  "a read past the end names the file, the structure and the size");

	{
		// A wrapping member reaches the source as its own request, and the bounds check names it.
		std::vector<ByteRange> wanted = {R(0, 100, 50), R(0, 120, UINT64_MAX - 60)};
		auto batch = make_shared_ptr<FetchBatch>(Coalesce(wanted, CoalescePolicy()));
		std::string wrapped;
		try {
			source->FetchTasks(batch);
		} catch (FormatError &e) {
			wrapped = e.what();
		}
		Check(wrapped.find("runs past the end") != std::string::npos, "a wrapping range is refused by name");
	}

	std::string none;
	try {
		ByteSource::Open(context, {dir + "/nothing_*.bak"});
	} catch (std::exception &e) {
		none = ErrorData(e).RawMessage();
	}
	Check(none.find("no file matches") != std::string::npos, "a glob that matches nothing is refused by name");
	std::string twice;
	try {
		ByteSource::Open(context, {dir + "/set_1.bak", dir + "/set_*.bak"});
	} catch (std::exception &e) {
		twice = ErrorData(e).RawMessage();
	}
	Check(twice.find("given twice") != std::string::npos, "a file named twice is refused");

	{
		std::vector<ByteRange> wanted = {R(0, 0, 8192), R(0, 8192, 8192), R(1, 100, 50), R(0, 200000, 4096)};
		CoalescePolicy policy;
		auto batch = make_shared_ptr<FetchBatch>(Coalesce(wanted, policy));
		auto tasks = source->FetchTasks(batch);
		Check(tasks.size() == 2, "two requests for four ranges (file 0's three within the gap merge, file 1 alone)");
		for (auto &task : tasks) {
			task->Execute();
		}
		bool all = true;
		for (size_t m = 0; m < wanted.size(); m++) {
			const auto &fetched = batch->Request(batch->RequestOf(m));
			auto view = fetched.Reader(wanted[m], "a wanted range");
			const auto &bytes = wanted[m].family == 0 ? a : b;
			for (size_t i = 0; i < wanted[m].length; i++) {
				all = all && view.U8(i) == bytes[wanted[m].offset + i];
			}
		}
		Check(all, "tasks run in place fill every wanted range");
	}
	{
		std::vector<ByteRange> wanted = {R(0, 0, 100000), R(1, 0, 70000)};
		auto batch = make_shared_ptr<FetchBatch>(Coalesce(wanted, CoalescePolicy()));
		source->FetchNow(context, batch);
		Check(Same(batch->Request(0), a) && Same(batch->Request(1), b), "FetchNow fills the batch");
		std::string rerun;
		try {
			source->FetchTasks(batch);
		} catch (std::exception &e) {
			rerun = ErrorData(e).RawMessage();
		}
		Check(rerun.find("scheduled twice") != std::string::npos, "a batch is not scheduled twice");
	}

	// The copy path: local files cached, cache blocks of 4 KiB, so a 20000-byte range crosses blocks.
	con.Query("SET cache_local_files = true");
	con.Query("SET external_file_cache_local_max_block_size = 4096");
	{
		auto cached = ByteSource::Open(context, {dir + "/set_1.bak"});
		auto crossing = cached->Read(R(0, 5000, 20000), "a range across cache blocks");
		Check(Same(crossing, a) && crossing.Copied(), "a range the cache returns in pieces comes back contiguous");
		auto inside = cached->Read(R(0, 8192, 4096), "a range inside one cache block");
		Check(Same(inside, a), "a range inside one block is read too");
		auto again = cached->Read(R(0, 5000, 20000), "the same range");
		Check(Same(again, a), "and the same range read again matches");
	}

	std::remove((dir + "/set_1.bak").c_str());
	std::remove((dir + "/set_2.bak").c_str());
	std::cout << (failures ? "FAILED: " + std::to_string(failures) + " check(s)" : "all checks passed") << std::endl;
	return failures ? 1 : 0;
}
