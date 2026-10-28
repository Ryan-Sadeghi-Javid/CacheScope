#include "cachescope/hierarchy.hpp"

#include <algorithm>

namespace cachescope {

Hierarchy::Hierarchy(const SimConfig& cfg) : cfg_(cfg), dram_(cfg.dram) {
  levels_.reserve(cfg.levels.size());
  prefetchers_.reserve(cfg.levels.size());
  for (std::size_t i = 0; i < cfg.levels.size(); ++i) {
    // Each level gets a distinct seed so two random-replacement levels do not
    // evict in lockstep.
    levels_.emplace_back(new Cache(cfg.levels[i], cfg.random_seed + 0x9E3779B9ull * (i + 1)));
    prefetchers_.emplace_back(make_prefetcher(cfg.levels[i].prefetcher, cfg.levels[i].block_size));

    LevelFast& f = fast_[i];
    f.hit_latency = cfg.levels[i].hit_latency;
    f.block_mask = ~static_cast<Addr>(cfg.levels[i].block_size - 1);
    f.bucket = cfg.source_for_level(i);
    f.write_policy = cfg.levels[i].write_policy;
    f.alloc_policy = cfg.levels[i].alloc_policy;
    f.has_prefetcher = prefetchers_.back() != nullptr;
    any_prefetcher_ = any_prefetcher_ || f.has_prefetcher;
  }
  num_levels_ = levels_.size();
  inclusive_ = (cfg.inclusion == InclusionPolicy::Inclusive);
  last_block_size_ = cfg.levels.back().block_size;
  missed_.reserve(num_levels_);
  pf_targets_.reserve(8);
}

bool Hierarchy::crosses_line(Addr addr, std::uint32_t size) const {
  if (size <= 1) return false;
  const std::uint32_t bs = levels_.front()->block_size();
  return (addr & ~static_cast<Addr>(bs - 1)) != ((addr + size - 1) & ~static_cast<Addr>(bs - 1));
}

MemOutcome Hierarchy::access(AccessType type, Addr addr, Cycle now) {
  MemOutcome out;
  missed_.clear();
  no_alloc_.fill(false);
  missed_flags_.fill(false);

  const bool is_store = (type == AccessType::Store);
  std::size_t served = num_levels_;  // sentinel: served by DRAM

  for (std::size_t i = 0; i < num_levels_; ++i) {
    Cache& c = *levels_[i];
    const LevelFast& f = fast_[i];
    const Addr blk = addr & f.block_mask;
    const Cycle hit_latency = f.hit_latency;

    // Record the traversed latency in the bucket belonging to this level.
    switch (f.bucket) {
      case ServiceSource::L1: out.l1_latency += hit_latency; break;
      case ServiceSource::L2: out.l2_latency += hit_latency; break;
      case ServiceSource::LLC: out.llc_latency += hit_latency; break;
      case ServiceSource::Dram: break;  // unreachable for a cache level
    }
    out.total_latency += hit_latency;

    LevelStats& st = c.stats();
    if (is_store) ++st.stores; else ++st.loads;

    const int way = c.find(blk);
    if (way >= 0) {
      if (is_store) ++st.store_hits; else ++st.load_hits;
      const std::uint32_t set = c.set_of(blk);
      c.touch(set, static_cast<std::uint32_t>(way));
      if (c.was_prefetched(set, static_cast<std::uint32_t>(way))) {
        ++st.prefetch_useful;
        c.clear_prefetched(set, static_cast<std::uint32_t>(way));
      }
      if (is_store) {
        // A write-back level absorbs the store; a write-through level will
        // forward it below.
        if (f.write_policy == WritePolicy::WriteBack) {
          c.set_dirty(set, static_cast<std::uint32_t>(way), true);
        }
      }
      served = i;
      if (i == 0) out.hit_l1 = true;
      break;
    }

    if (is_store) ++st.store_misses; else ++st.load_misses;
    missed_.push_back(i);
    missed_flags_[i] = true;
    // A no-write-allocate level does not bring a line in on a store miss; the
    // write simply travels outward.
    if (is_store && f.alloc_policy == AllocPolicy::NoWriteAllocate) {
      no_alloc_[i] = true;
    }
  }

  // Will any level actually bring the line in? A store miss against a
  // no-write-allocate level does not, and then there is no line to fetch: the
  // write travels outward on its own and no DRAM *read* is performed.
  bool any_alloc = false;
  for (const std::size_t i : missed_) {
    if (!no_alloc_[i]) { any_alloc = true; break; }
  }

  if (served == num_levels_ && any_alloc) {
    // Every level missed and something will allocate: fetch the line from
    // DRAM. The request arrives after the cache lookups have completed, which
    // is what creates realistic interleaving in the DRAM queue.
    const Dram::Response r =
        dram_.access(now + out.total_latency, /*is_write=*/false, last_block_size_);
    out.total_latency += r.latency;
    out.dram_queue_delay = r.queue_delay;
    out.dram_fixed_latency = r.latency - r.queue_delay;
    out.source = ServiceSource::Dram;
  } else if (served == num_levels_) {
    out.source = ServiceSource::Dram;
  } else {
    out.source = fast_[served].bucket;
  }

  const std::size_t filled_at = fill_missed(addr, is_store, /*prefetched=*/false, now);

  if (is_store) {
    // The store is performed in whichever level holds the line: the level that
    // hit, or the innermost level that allocated on the miss.
    const std::size_t holder = (served < num_levels_) ? served : filled_at;
    if (holder >= num_levels_) {
      // No level accepted it (no-write-allocate everywhere): the write goes
      // straight to memory, consuming bandwidth.
      dram_.access(now + out.total_latency, /*is_write=*/true, last_block_size_);
    } else {
      propagate_write_through(holder, addr, now + out.total_latency);
    }
  }

  // Prefetchers observe demand accesses only. missed_flags_ is read here rather
  // than missed_, which issue_prefetch() reuses as its own fill list.
  // The any_prefetcher_ test skips the loop entirely in the common case where
  // no level has one configured.
  if (any_prefetcher_ && !in_prefetch_ &&
      (type == AccessType::Load || type == AccessType::Store)) {
    for (std::size_t i = 0; i < num_levels_; ++i) {
      if (fast_[i].has_prefetcher) {
        run_prefetcher(i, addr, missed_flags_[i], now + out.total_latency);
      }
    }
  }

  return out;
}

std::size_t Hierarchy::fill_missed(Addr addr, bool store_dirties, bool prefetched, Cycle now) {
  // missed_ is in ascending level order, so the first allocating entry is the
  // innermost one.
  std::size_t innermost_alloc = num_levels_;
  for (const std::size_t i : missed_) {
    if (!no_alloc_[i]) { innermost_alloc = i; break; }
  }

  // Outermost first: an eviction from an inner level then finds its successor
  // already resident in the next level out.
  for (std::size_t k = missed_.size(); k-- > 0;) {
    const std::size_t i = missed_[k];
    if (no_alloc_[i]) continue;
    Cache& c = *levels_[i];
    // Only the level that performs the write holds dirty data, and only if it
    // is write-back; outer copies are clean fills from memory.
    const bool dirty = store_dirties && i == innermost_alloc &&
                       c.config().write_policy == WritePolicy::WriteBack;
    const FillResult fr = c.insert(c.block_addr(addr), dirty, prefetched);
    handle_eviction(i, fr, now);
  }
  return innermost_alloc;
}

void Hierarchy::handle_eviction(std::size_t level, const FillResult& fr, Cycle now) {
  if (!fr.evicted) return;
  if (fr.evicted_dirty) {
    write_back_to(level + 1, fr.evicted_block_addr, now);
  }
  if (inclusive_) {
    back_invalidate_above(level, fr.evicted_block_addr, now);
  }
}

void Hierarchy::write_back_to(std::size_t target, Addr addr, Cycle now) {
  if (target >= num_levels_) {
    // Off the end of the hierarchy: the dirty line is written to memory. This
    // consumes bandwidth but no demand-visible latency.
    dram_.access(now, /*is_write=*/true, last_block_size_);
    return;
  }

  Cache& c = *levels_[target];
  const Addr blk = c.block_addr(addr);
  ++c.stats().writebacks_received;

  const int way = c.find(blk);
  if (way >= 0) {
    const std::uint32_t set = c.set_of(blk);
    if (c.config().write_policy == WritePolicy::WriteBack) {
      c.set_dirty(set, static_cast<std::uint32_t>(way), true);
    } else {
      // A write-through level does not retain dirty data; push it further out.
      write_back_to(target + 1, addr, now);
    }
    return;
  }

  if (c.config().alloc_policy == AllocPolicy::NoWriteAllocate) {
    // Refuses to allocate for a write; the data continues outward.
    write_back_to(target + 1, addr, now);
    return;
  }

  const bool keep_dirty = (c.config().write_policy == WritePolicy::WriteBack);
  const FillResult fr = c.insert(blk, keep_dirty, /*prefetched=*/false);
  handle_eviction(target, fr, now);
  if (!keep_dirty) write_back_to(target + 1, addr, now);
}

void Hierarchy::back_invalidate_above(std::size_t level, Addr addr, Cycle now) {
  for (std::size_t i = 0; i < level; ++i) {
    bool was_dirty = false;
    if (levels_[i]->invalidate(levels_[i]->block_addr(addr), &was_dirty)) {
      ++levels_[i]->stats().back_invalidations;
      if (was_dirty) {
        // The outer copy is going away, so the dirty inner data has to travel
        // past the evicting level rather than into it.
        write_back_to(level + 1, addr, now);
      }
    }
  }
}

void Hierarchy::propagate_write_through(std::size_t from_level, Addr addr, Cycle now) {
  std::size_t j = from_level;
  while (j < num_levels_ && fast_[j].write_policy == WritePolicy::WriteThrough) {
    if (j + 1 >= num_levels_) {
      dram_.access(now, /*is_write=*/true, last_block_size_);
      return;
    }
    Cache& nxt = *levels_[j + 1];
    const Addr blk = nxt.block_addr(addr);
    ++nxt.stats().writebacks_received;
    const int way = nxt.find(blk);
    if (way >= 0) {
      if (nxt.config().write_policy == WritePolicy::WriteBack) {
        nxt.set_dirty(nxt.set_of(blk), static_cast<std::uint32_t>(way), true);
        return;
      }
    } else if (nxt.config().alloc_policy == AllocPolicy::WriteAllocate) {
      const bool keep_dirty = (nxt.config().write_policy == WritePolicy::WriteBack);
      const FillResult fr = nxt.insert(blk, keep_dirty, /*prefetched=*/false);
      handle_eviction(j + 1, fr, now);
      if (keep_dirty) return;
    }
    ++j;
  }
}

void Hierarchy::run_prefetcher(std::size_t level, Addr addr, bool was_miss, Cycle now) {
  Prefetcher* pf = prefetchers_[level].get();
  if (pf == nullptr) return;

  pf_targets_.clear();
  Cache& c = *levels_[level];
  pf->on_access(addr, c.block_addr(addr), was_miss, pf_targets_);
  for (Addr t : pf_targets_) {
    issue_prefetch(level, t, now);
  }
}

void Hierarchy::issue_prefetch(std::size_t level, Addr addr, Cycle now) {
  Cache& c = *levels_[level];
  const Addr blk = c.block_addr(addr);
  LevelStats& st = c.stats();
  ++st.prefetch_issued;

  if (c.find(blk) >= 0) {
    // Already resident: nothing to do, and no bandwidth consumed.
    ++st.prefetch_dropped;
    return;
  }

  // Find the nearest outer level that already holds the line. Anything not
  // found anywhere comes from DRAM and consumes real bandwidth.
  in_prefetch_ = true;
  missed_.clear();
  no_alloc_.fill(false);
  missed_.push_back(level);

  bool found_outer = false;
  for (std::size_t i = level + 1; i < num_levels_; ++i) {
    if (levels_[i]->find(levels_[i]->block_addr(blk)) >= 0) { found_outer = true; break; }
    // Under inclusion the line must also be installed in the outer levels,
    // otherwise an inner-only copy would violate the invariant.
    if (inclusive_) missed_.push_back(i);
  }

  if (!found_outer) {
    dram_.access(now, /*is_write=*/false, last_block_size_);
  }

  fill_missed(blk, /*store_dirties=*/false, /*prefetched=*/true, now);
  in_prefetch_ = false;
}

}  // namespace cachescope
