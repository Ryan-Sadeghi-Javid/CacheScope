// cachescope -- core value types shared across the simulator.
#pragma once

#include <cstdint>
#include <string>

namespace cachescope {

using Addr = std::uint64_t;
using Cycle = std::uint64_t;
using Count = std::uint64_t;

// What kind of request is walking the hierarchy. The distinction matters for
// counters (prefetches must never inflate demand miss rates) and for write
// policy (a writeback never allocates in the level it is heading toward).
enum class AccessType : std::uint8_t {
  Load,
  Store,
  Prefetch,   // speculative fill issued by a prefetcher
  Writeback,  // dirty eviction travelling away from the core
};

const char* to_string(AccessType t);

enum class WritePolicy : std::uint8_t { WriteBack, WriteThrough };
enum class AllocPolicy : std::uint8_t { WriteAllocate, NoWriteAllocate };
enum class ReplacementKind : std::uint8_t { LRU, FIFO, Random };
enum class InclusionPolicy : std::uint8_t { Inclusive, NonInclusive };

bool parse_write_policy(const std::string& s, WritePolicy& out);
bool parse_alloc_policy(const std::string& s, AllocPolicy& out);
bool parse_replacement(const std::string& s, ReplacementKind& out);
bool parse_inclusion(const std::string& s, InclusionPolicy& out);

const char* to_string(WritePolicy p);
const char* to_string(AllocPolicy p);
const char* to_string(ReplacementKind p);
const char* to_string(InclusionPolicy p);

// One record from a memory trace.
struct MemRef {
  AccessType type = AccessType::Load;
  Addr addr = 0;
  std::uint32_t size = 1;
  // Third trace field when interpreted as "instructions since previous access".
  std::uint32_t instr_gap = 0;
};

// One record from a branch trace.
struct BranchRef {
  Addr pc = 0;
  bool taken = false;
};

// Where a memory request was ultimately satisfied. Drives the top-down
// cycle attribution: a request served by DRAM is charged to the DRAM bucket.
enum class ServiceSource : std::uint8_t {
  L1,
  L2,
  LLC,
  Dram,
};

const char* to_string(ServiceSource s);

// Result of pushing one request through the whole hierarchy.
struct AccessResult {
  Cycle latency = 0;          // total cycles this request took to resolve
  ServiceSource source = ServiceSource::L1;
  Cycle dram_queue_delay = 0; // portion of latency spent waiting on DRAM bandwidth
  bool hit_l1 = false;
};

}  // namespace cachescope
