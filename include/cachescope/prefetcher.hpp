// cachescope -- hardware prefetchers.
//
// Note on the stride detector: a real stride prefetcher indexes its table by
// the PC of the accessing instruction. The trace format this simulator consumes
// ("l 0x1fffff50 4") carries no PC, so the table is indexed by 4 KiB memory
// region instead. Within a region, consecutive accesses from the same loop
// still produce a constant delta, which is what the detector locks onto. This
// is weaker than a PC-indexed table when several strided streams interleave
// inside one page, and that limitation is reported in the README.
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "cachescope/config.hpp"
#include "cachescope/types.hpp"

namespace cachescope {

class Prefetcher {
 public:
  virtual ~Prefetcher() = default;

  // Called for every demand access at the owning level. Appends block-aligned
  // target addresses to `out` (which the caller does not clear between calls).
  virtual void on_access(Addr addr, Addr block_addr, bool was_miss, std::vector<Addr>& out) = 0;

  virtual const char* name() const = 0;
};

// Fetches the next `degree` lines after the triggering one.
class NextLinePrefetcher : public Prefetcher {
 public:
  NextLinePrefetcher(const PrefetcherConfig& cfg, std::uint32_t block_size);
  void on_access(Addr addr, Addr block_addr, bool was_miss, std::vector<Addr>& out) override;
  const char* name() const override { return "next-line"; }

 private:
  PrefetcherConfig cfg_;
  std::uint32_t block_size_;
};

// Region-indexed constant-stride detector.
class StridePrefetcher : public Prefetcher {
 public:
  StridePrefetcher(const PrefetcherConfig& cfg, std::uint32_t block_size);
  void on_access(Addr addr, Addr block_addr, bool was_miss, std::vector<Addr>& out) override;
  const char* name() const override { return "stride"; }

  // Bytes of tracking state, reported in the config dump so table cost is
  // visible alongside cache sizes.
  std::size_t state_bytes() const;

 private:
  struct Entry {
    Addr region = 0;
    Addr last_addr = 0;
    std::int64_t stride = 0;
    std::uint32_t confidence = 0;
    bool valid = false;
  };

  static constexpr std::uint32_t kRegionShift = 12;  // 4 KiB

  PrefetcherConfig cfg_;
  std::uint32_t block_size_;
  std::uint32_t index_mask_;
  std::vector<Entry> table_;
};

// Returns nullptr when the level has no prefetcher configured.
std::unique_ptr<Prefetcher> make_prefetcher(const PrefetcherConfig& cfg, std::uint32_t block_size);

}  // namespace cachescope
