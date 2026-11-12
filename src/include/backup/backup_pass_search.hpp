//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// backup/backup_pass_search.hpp
//
// Spec 085 task 7a part 4, DQ-41 / E13: an ordered data pass searched, not read. Every pass of the
// samples is strictly ascending by (file, extent) (210 of 210, each family of a striped set
// included), so the group of an extent is found by bisection over the pass: over its groups in a
// family file (group g at a computed offset), over its blocks in a container (a block, which is one
// extent group there, found from any offset by its next four headers). The groups probed are kept
// as samples, so later searches start between the nearest ones; the last stretch (at most
// `WINDOW_BYTES`) is read once. The order is checked whenever a sample is kept: a pass whose order
// breaks throws OrderBroken, and its caller maps it by its headers instead (D4a). DuckDB-free.
//===----------------------------------------------------------------------===//

#pragma once

#include "backup/backup_bytes.hpp"
#include "backup/backup_container.hpp"
#include "backup/backup_mtf.hpp"
#include "backup/backup_page_index.hpp"

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace duckdb {
namespace mssql {
namespace backup {

//! A pass that is not ascending where a search looked: the caller maps it by its headers.
class OrderBroken : public FormatError {
public:
	explicit OrderBroken(const std::string &message) : FormatError(message) {}
};

//! A group a search knows: its position in the pass's medium and the extent its pages name.
struct PassSample {
	uint64_t position = 0;
	uint64_t next = 0;	// the position just after its group
	ExtentId extent;
};

//! Where a pass's groups lie, in positions that ascend with the stream: a family file's group
//! index, a container's block offsets. The search asks it for groups; it never holds page bytes.
class PassMedium {
public:
	virtual ~PassMedium() {}
	//! The positions the pass spans: [Begin(), End()).
	virtual uint64_t Begin() const = 0;
	virtual uint64_t End() const = 0;
	//! The first group at a position >= `at` and < `limit` whose pages name an extent; false when
	//! none does. `resumed` is the position the probe settled on (a container's resynced block) even
	//! when no group named an extent, so the search can narrow to it.
	virtual bool Probe(uint64_t at, uint64_t limit, PassSample &sample, uint64_t &resumed) const = 0;
	//! Every group from `from` (a group's position) to before `limit`, read at once: each that names
	//! an extent goes to `out`, in order.
	virtual void Window(uint64_t from, uint64_t limit, std::vector<PassSample> &out) const = 0;
	//! The span of positions read as one window rather than bisected.
	virtual uint64_t WindowSpan() const = 0;
	//! Positions are group indexes, one group a position: the search may then guess a group from
	//! the extents between it and a known sample (PassSearch::Find).
	virtual bool GroupPositions() const {
		return false;
	}
	virtual const std::string &Name() const = 0;
	//! The file bytes its reads have asked for, for the counters and the tests.
	uint64_t BytesRead() const {
		return bytes_read_.load();
	}

protected:
	mutable std::atomic<uint64_t> bytes_read_{0};
};

//! A family file's pass: positions are group indexes (group g at units_offset + g × 64 KB).
class FilePassMedium : public PassMedium {
public:
	FilePassMedium(const RandomAccess &file, uint64_t units_offset, uint64_t groups);
	uint64_t Begin() const override {
		return 0;
	}
	uint64_t End() const override {
		return groups_;
	}
	bool Probe(uint64_t at, uint64_t limit, PassSample &sample, uint64_t &resumed) const override;
	void Window(uint64_t from, uint64_t limit, std::vector<PassSample> &out) const override;
	uint64_t WindowSpan() const override;
	bool GroupPositions() const override {
		return true;
	}
	const std::string &Name() const override {
		return file_.Name();
	}
	uint64_t GroupOffset(uint64_t group) const;

private:
	//! Groups [from, from + count) read at once, those that name an extent appended to `out`.
	void ReadGroups(uint64_t from, uint64_t count, std::vector<PassSample> &out) const;

private:
	const RandomAccess &file_;
	uint64_t units_offset_;
	uint64_t groups_;
};

//! A container's pass: positions are the physical offsets of its blocks, from the block whose
//! output starts the pass's units to the block after its last (a seek index entry). Each block of
//! a pass is one extent group (E13); a probe decodes the block it settles on.
class ContainerPassMedium : public PassMedium {
public:
	//! The pass whose blocks start at physical `begin` (a block's header) and end before `end`;
	//! `chain_end` bounds the headers a resync may read.
	ContainerPassMedium(const RandomAccess &file, uint64_t begin, uint64_t end, uint64_t chain_end,
						BlockDecoder decoder);
	uint64_t Begin() const override {
		return begin_;
	}
	uint64_t End() const override {
		return end_;
	}
	bool Probe(uint64_t at, uint64_t limit, PassSample &sample, uint64_t &resumed) const override;
	void Window(uint64_t from, uint64_t limit, std::vector<PassSample> &out) const override;
	uint64_t WindowSpan() const override;
	const std::string &Name() const override {
		return file_.Name();
	}
	//! The 64 KB group of the block whose header is at `position`, decoded into `out`.
	void DecodeGroup(uint64_t position, std::vector<uint8_t> &out) const;

private:
	bool ProbeChecked(uint64_t at, uint64_t limit, PassSample &sample, uint64_t &resumed) const;
	void WindowChecked(uint64_t from, uint64_t limit, std::vector<PassSample> &out) const;
	struct Block {
		uint8_t kind = 0;
		uint32_t payload = 0;
	};
	//! The header at `at` of the bytes `bytes` (file offset `base`) if it is a plausible block
	//! header whose payload ends inside the chain.
	bool Plausible(const uint8_t *bytes, uint64_t base, uint64_t at, Block &block) const;
	//! The block whose header is at `position` (within `bytes` from `base` when it holds it): a
	//! 64 KB block of the pass, else OrderBroken (E13: every block of a pass outputs 64 KB).
	Block BlockAt(const std::vector<uint8_t> &bytes, uint64_t base, uint64_t position) const;
	//! The group of the block at `position`, decoded: true with its extent, false for one of zero
	//! units and pads.
	bool GroupOf(const std::vector<uint8_t> &bytes, uint64_t base, uint64_t position, PassSample &sample) const;
	//! `count` bytes at `at`, clipped at the chain's end.
	std::vector<uint8_t> ReadSpan(uint64_t at, uint64_t count) const;

	const RandomAccess &file_;
	uint64_t begin_;
	uint64_t end_;
	uint64_t chain_end_;
	BlockDecoder decoder_;
};

//! The search over one pass, its samples kept between calls; safe from several threads (the
//! medium's reads run outside the lock).
class PassSearch {
public:
	static constexpr uint64_t WINDOW_BYTES = 1 << 20;

	//! Bisect: probe the middle of what is left (a medium where any position costs the same).
	//! Gallop: probe 1, 2, 4 … positions past the lower bound, then bisect the last step (a medium
	//! that walks to what it reads, as a container's chain from its known blocks, DQ-41: it walks at
	//! most about twice as far as the group it finds). Meant for a medium whose positions are group
	//! indexes (a FilePassMedium, over a ContainerStream for a container); over a ContainerPassMedium
	//! its first steps would be bytes, so that medium (`trust_layout`) bisects.
	enum class Strategy { Bisect, Gallop };

	explicit PassSearch(std::unique_ptr<PassMedium> medium, Strategy strategy = Strategy::Bisect);

	//! The position of `extent`'s group, or false when the pass does not carry it. Throws
	//! OrderBroken when what it reads is not ascending.
	bool Find(const ExtentId &extent, uint64_t &position);

	const PassMedium &Medium() const {
		return *medium_;
	}
	size_t SampleCount() const;
	//! The samples' and empty spans' memory, roughly (map nodes).
	size_t MemoryBytes() const;
	//! The medium calls made (probes and windows), for the counters and the tests.
	uint64_t Reads() const;
	uint64_t BytesRead() const {
		return medium_->BytesRead();
	}

private:
	//! The narrowest [from, to) the samples leave for `extent`, or true with its position when a
	//! sample is it. `below`, when given, is the nearest sample below `extent` (position UINT64_MAX
	//! when none).
	bool Bounds(const ExtentId &extent, uint64_t &from, uint64_t &to, uint64_t &position,
				PassSample *below = nullptr) const;
	//! Keeps `sample` (up to MAX_SAMPLES), checking it against its neighbours: OrderBroken when the
	//! extents do not ascend with the positions.
	void Keep(const PassSample &sample);
	//! Records that no group in [from, to) names an extent (merged with the spans it touches), so a
	//! later search narrows past it without reading it again.
	void KeepEmpty(uint64_t from, uint64_t to);
	//! [from, to) narrowed by the known-empty spans at its ends; the lock is held.
	void SkipEmptyLocked(uint64_t &from, uint64_t &to) const;

	static constexpr size_t MAX_SAMPLES = 1 << 18;	// 256 K samples, ~24 MB at most

	std::unique_ptr<PassMedium> medium_;
	Strategy strategy_;
	mutable std::mutex lock_;
	std::map<uint64_t, PassSample> by_position_;
	std::map<ExtentId, uint64_t> by_extent_;
	std::map<uint64_t, uint64_t> empty_;  // start -> end of spans known to name no extent
	uint64_t reads_ = 0;
};

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb
