// Shared fixtures for the cachescope unit tests.
#pragma once

#include <string>
#include <vector>

#include "cachescope/config.hpp"
#include "cachescope/types.hpp"

namespace cachescope {
namespace testing {

// A deliberately tiny level so that hand-computed expectations stay tractable.
// 4 sets x `assoc` ways x 16-byte blocks.
//
// Address decomposition with 16-byte blocks and 4 sets:
//   bits [3:0]  block offset
//   bits [5:4]  set index
//   bits [63:6] tag
// So 0x00 -> set 0 tag 0, 0x10 -> set 1 tag 0, 0x40 -> set 0 tag 1.
inline LevelConfig tiny_level(const std::string& name, std::uint32_t assoc = 1,
                              std::uint32_t num_sets = 4,
                              ReplacementKind repl = ReplacementKind::LRU) {
  LevelConfig l;
  l.name = name;
  l.block_size = 16;
  l.assoc = assoc;
  l.size_bytes = static_cast<std::uint64_t>(num_sets) * assoc * l.block_size;
  l.hit_latency = 1;
  l.write_policy = WritePolicy::WriteBack;
  l.alloc_policy = AllocPolicy::WriteAllocate;
  l.replacement = repl;
  return l;
}

// Single-level configuration around tiny_level, with DRAM latency 100 and
// effectively unlimited bandwidth so that latency maths stays predictable.
inline SimConfig tiny_config(std::uint32_t assoc = 1, std::uint32_t num_sets = 4,
                             ReplacementKind repl = ReplacementKind::LRU) {
  SimConfig c;
  c.name = "tiny";
  c.levels = {tiny_level("l1d", assoc, num_sets, repl)};
  c.dram.latency_cycles = 100;
  c.dram.bytes_per_cycle = 1024.0;  // one cycle of occupancy per request
  c.timing.base_cpi = 1.0;
  c.timing.instructions_per_access = 1.0;
  c.third_field = ThirdField::Ignore;
  c.inclusion = InclusionPolicy::NonInclusive;
  c.validate();
  return c;
}

inline MemRef load(Addr a) {
  MemRef r;
  r.type = AccessType::Load;
  r.addr = a;
  r.size = 1;
  return r;
}

inline MemRef store(Addr a) {
  MemRef r;
  r.type = AccessType::Store;
  r.addr = a;
  r.size = 1;
  return r;
}

}  // namespace testing
}  // namespace cachescope
