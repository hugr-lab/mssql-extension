// Standalone corpus-replay driver for any harness in this directory.
//
// WHY THIS EXISTS
//
// The two libduckdb-linked harnesses (fuzz_row_stager, fuzz_bcp_encode) cannot
// run in the ClusterFuzzLite job — a sanitizer build of DuckDB is far too heavy
// for a per-PR lane — and `fuzz/run.sh`'s AFL++ container has no libduckdb
// either. Without something like this they are discovery-only tools that run
// when a human remembers, which for fuzz_bcp_encode's oracles would be a waste:
// a framing invariant that holds today is worth RE-checking on every change to
// the encoders, and that is a regression test, not a fuzzing campaign.
//
// So: link a harness against this instead of a fuzzing engine and it becomes an
// ordinary executable that replays a corpus and fails on the first crash. No
// libFuzzer (which Apple's toolchain does not ship at all), no AFL++, no
// instrumentation — just ASan + UBSan and the harness's own oracles. Seconds,
// not minutes.
//
//   REPLAY=1 MSSQL_DUCKDB_INC=duckdb/src/include MSSQL_DUCKDB_LIB=build/release/src \
//   TARGETS=bcp_encode fuzz/build.sh
//   fuzz/out/fuzz_bcp_encode fuzz/corpus/bcp_encode
//
// Arguments are files or directories; a directory is replayed one level deep,
// in sorted order so a failure is reproducible. With no arguments it reads one
// input from stdin, which is how a crash file from a real campaign is replayed:
//
//   fuzz/out/fuzz_bcp_encode < fuzz/findings/bcp_encode/fuzzer1/crashes/id:000000,...
//
// This is NOT a substitute for a campaign. It explores nothing: it replays what
// a campaign already found. Discovery still belongs on a dedicated machine with
// AFL++ and hours of runtime.

#include <dirent.h>
#include <sys/stat.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

namespace {

bool ReadWholeFile(const std::string &path, std::vector<uint8_t> &out) {
	FILE *f = std::fopen(path.c_str(), "rb");
	if (!f) {
		return false;
	}
	out.clear();
	uint8_t chunk[65536];
	size_t n;
	while ((n = std::fread(chunk, 1, sizeof(chunk), f)) > 0) {
		out.insert(out.end(), chunk, chunk + n);
	}
	std::fclose(f);
	return true;
}

bool IsDirectory(const std::string &path) {
	struct stat st;
	return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

//! Regular files directly inside `dir`, sorted, so a replay is deterministic
//! and a failure names the same input every time.
std::vector<std::string> FilesIn(const std::string &dir) {
	std::vector<std::string> paths;
	DIR *d = ::opendir(dir.c_str());
	if (!d) {
		return paths;
	}
	while (struct dirent *e = ::readdir(d)) {
		if (e->d_name[0] == '.') {
			continue;  // skip ., .. and AFL++'s dotfiles (.state, .cur_input)
		}
		const std::string full = dir + "/" + e->d_name;
		if (!IsDirectory(full)) {
			paths.push_back(full);
		}
	}
	::closedir(d);
	std::sort(paths.begin(), paths.end());
	return paths;
}

//! Announce the input BEFORE running it and flush, so that when the harness
//! aborts — which is how its oracles report — the last line on stderr names the
//! file that did it. Without the flush the crash takes the buffer with it.
int RunOne(const std::string &label, const std::vector<uint8_t> &bytes) {
	std::fprintf(stderr, "  %-56s %6zu bytes\n", label.c_str(), bytes.size());
	std::fflush(stderr);
	return LLVMFuzzerTestOneInput(bytes.data(), bytes.size());
}

}  // namespace

int main(int argc, char **argv) {
	std::vector<uint8_t> bytes;

	if (argc < 2) {
		// stdin: one input, the shape a crash file is replayed in.
		uint8_t chunk[65536];
		size_t n;
		while ((n = std::fread(chunk, 1, sizeof(chunk), stdin)) > 0) {
			bytes.insert(bytes.end(), chunk, chunk + n);
		}
		RunOne("<stdin>", bytes);
		std::fprintf(stderr, "replayed 1 input from stdin, no crash\n");
		return 0;
	}

	std::vector<std::string> inputs;
	for (int i = 1; i < argc; i++) {
		const std::string arg = argv[i];
		if (IsDirectory(arg)) {
			const auto found = FilesIn(arg);
			if (found.empty()) {
				std::fprintf(stderr, "WARNING: %s holds no files\n", arg.c_str());
			}
			inputs.insert(inputs.end(), found.begin(), found.end());
		} else {
			inputs.push_back(arg);
		}
	}

	if (inputs.empty()) {
		std::fprintf(stderr, "ERROR: nothing to replay (no files in the given paths)\n");
		return 2;
	}

	size_t replayed = 0;
	size_t unreadable = 0;
	for (const auto &path : inputs) {
		if (!ReadWholeFile(path, bytes)) {
			std::fprintf(stderr, "WARNING: cannot read %s\n", path.c_str());
			unreadable++;
			continue;
		}
		RunOne(path, bytes);
		replayed++;
	}

	std::fprintf(stderr, "replayed %zu input(s), no crash", replayed);
	if (unreadable > 0) {
		std::fprintf(stderr, " (%zu unreadable)", unreadable);
	}
	std::fprintf(stderr, "\n");
	// An unreadable input is a broken invocation, not a pass.
	return unreadable > 0 ? 2 : 0;
}
