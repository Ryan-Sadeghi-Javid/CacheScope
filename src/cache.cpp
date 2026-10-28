#include "cachescope/cache.hpp"

#include <stdexcept>

namespace cachescope {

std::uint32_t log2_exact(std::uint64_t v) {
  if (v == 0 || (v & (v - 1)) != 0) {
    throw std::invalid_argument("log2_exact requires a power of two");
  }
  std::uint32_t n = 0;
  while ((v >> n) != 1) ++n;
  return n;
}

Cache::Cache(const LevelConfig& cfg, std::uint64_t seed)
    : cfg_(cfg),
      num_sets_(cfg.num_sets()),
      block_bits_(log2_exact(cfg.block_size)),
      set_bits_(log2_exact(cfg.num_sets())),
      set_mask_(static_cast<Addr>(cfg.num_sets()) - 1),
      lines_(static_cast<std::size_t>(cfg.num_sets()) * cfg.assoc),
      repl_(make_replacement(cfg.replacement, cfg.num_sets(), cfg.assoc, seed)) {}

int Cache::find(Addr a) const {
  const std::uint32_t set = set_of(a);
  const Addr tag = tag_of(a);
  const CacheLine* row = &lines_[static_cast<std::size_t>(set) * cfg_.assoc];
  for (std::uint32_t w = 0; w < cfg_.assoc; ++w) {
    if (row[w].valid && row[w].tag == tag) return static_cast<int>(w);
  }
  return -1;
}

FillResult Cache::insert(Addr a, bool dirty, bool prefetched) {
  const std::uint32_t set = set_of(a);
  const Addr tag = tag_of(a);
  FillResult res;

  // Prefer an invalid way so a cold cache never reports an eviction.
  std::uint32_t way = cfg_.assoc;
  for (std::uint32_t w = 0; w < cfg_.assoc; ++w) {
    if (!line(set, w).valid) { way = w; break; }
  }

  if (way == cfg_.assoc) {
    way = repl_->pick_victim(set);
    CacheLine& victim = line(set, way);
    res.evicted = true;
    res.evicted_dirty = victim.dirty;
    res.evicted_block_addr = line_block_addr(set, way);
    ++stats_.evictions;
    if (victim.dirty) ++stats_.dirty_evictions;
  }

  CacheLine& l = line(set, way);
  l.valid = true;
  l.tag = tag;
  l.dirty = dirty;
  l.prefetched = prefetched;
  ++stats_.fills;
  if (prefetched) ++stats_.prefetch_fills;
  repl_->on_fill(set, way);
  return res;
}

bool Cache::invalidate(Addr a, bool* was_dirty) {
  const int way = find(a);
  if (way < 0) {
    if (was_dirty) *was_dirty = false;
    return false;
  }
  const std::uint32_t set = set_of(a);
  CacheLine& l = line(set, static_cast<std::uint32_t>(way));
  if (was_dirty) *was_dirty = l.dirty;
  l.valid = false;
  l.dirty = false;
  l.prefetched = false;
  repl_->on_invalidate(set, static_cast<std::uint32_t>(way));
  return true;
}

Addr Cache::line_block_addr(std::uint32_t set, std::uint32_t way) const {
  const CacheLine& l = line(set, way);
  return (l.tag << (block_bits_ + set_bits_)) | (static_cast<Addr>(set) << block_bits_);
}

Count Cache::valid_lines() const {
  Count n = 0;
  for (const CacheLine& l : lines_) {
    if (l.valid) ++n;
  }
  return n;
}

}  // namespace cachescope
