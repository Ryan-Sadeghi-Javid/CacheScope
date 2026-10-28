#include "cachescope/prefetcher.hpp"

#include <cstdlib>

namespace cachescope {

// ---------------------------------------------------------------- next-line

NextLinePrefetcher::NextLinePrefetcher(const PrefetcherConfig& cfg, std::uint32_t block_size)
    : cfg_(cfg), block_size_(block_size) {}

void NextLinePrefetcher::on_access(Addr, Addr block_addr, bool was_miss, std::vector<Addr>& out) {
  if (cfg_.on_miss_only && !was_miss) return;
  for (std::uint32_t i = 0; i < cfg_.degree; ++i) {
    const Addr target = block_addr + static_cast<Addr>(cfg_.distance + i) * block_size_;
    // Skip a wrap past the top of the address space.
    if (target < block_addr) continue;
    out.push_back(target);
  }
}

// ---------------------------------------------------------------- stride

StridePrefetcher::StridePrefetcher(const PrefetcherConfig& cfg, std::uint32_t block_size)
    : cfg_(cfg),
      block_size_(block_size),
      index_mask_(cfg.table_entries - 1),
      table_(cfg.table_entries) {}

void StridePrefetcher::on_access(Addr addr, Addr block_addr, bool was_miss, std::vector<Addr>& out) {
  const Addr region = addr >> kRegionShift;
  Entry& e = table_[region & index_mask_];

  if (!e.valid || e.region != region) {
    // First sighting of this region, or the entry belongs to another region.
    e.valid = true;
    e.region = region;
    e.last_addr = addr;
    e.stride = 0;
    e.confidence = 0;
    return;
  }

  const std::int64_t delta =
      static_cast<std::int64_t>(addr) - static_cast<std::int64_t>(e.last_addr);
  e.last_addr = addr;

  if (delta == 0) {
    // Repeated access to the same address carries no stride information.
    return;
  }

  if (delta == e.stride) {
    if (e.confidence < 1024) ++e.confidence;
  } else {
    e.stride = delta;
    e.confidence = 0;
    return;
  }

  if (e.confidence < cfg_.stride_confidence) return;
  if (cfg_.on_miss_only && !was_miss) return;

  for (std::uint32_t i = 0; i < cfg_.degree; ++i) {
    const std::int64_t ahead = e.stride * static_cast<std::int64_t>(cfg_.distance + i);
    const std::int64_t target_signed = static_cast<std::int64_t>(addr) + ahead;
    if (target_signed < 0) continue;
    const Addr target = static_cast<Addr>(target_signed) & ~static_cast<Addr>(block_size_ - 1);
    if (target == block_addr) continue;  // same line; nothing to fetch
    out.push_back(target);
  }
}

std::size_t StridePrefetcher::state_bytes() const { return table_.size() * sizeof(Entry); }

// ---------------------------------------------------------------- factory

std::unique_ptr<Prefetcher> make_prefetcher(const PrefetcherConfig& cfg,
                                            std::uint32_t block_size) {
  switch (cfg.kind) {
    case PrefetcherKind::None:
      return nullptr;
    case PrefetcherKind::NextLine:
      return std::unique_ptr<Prefetcher>(new NextLinePrefetcher(cfg, block_size));
    case PrefetcherKind::Stride:
      return std::unique_ptr<Prefetcher>(new StridePrefetcher(cfg, block_size));
  }
  return nullptr;
}

}  // namespace cachescope
