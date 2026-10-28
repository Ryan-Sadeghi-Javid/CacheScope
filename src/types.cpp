#include "cachescope/types.hpp"

namespace cachescope {

const char* to_string(AccessType t) {
  switch (t) {
    case AccessType::Load: return "load";
    case AccessType::Store: return "store";
    case AccessType::Prefetch: return "prefetch";
    case AccessType::Writeback: return "writeback";
  }
  return "unknown";
}

const char* to_string(WritePolicy p) {
  return p == WritePolicy::WriteBack ? "write-back" : "write-through";
}

const char* to_string(AllocPolicy p) {
  return p == AllocPolicy::WriteAllocate ? "write-allocate" : "no-write-allocate";
}

const char* to_string(ReplacementKind p) {
  switch (p) {
    case ReplacementKind::LRU: return "lru";
    case ReplacementKind::FIFO: return "fifo";
    case ReplacementKind::Random: return "random";
  }
  return "unknown";
}

const char* to_string(InclusionPolicy p) {
  return p == InclusionPolicy::Inclusive ? "inclusive" : "non-inclusive";
}

const char* to_string(ServiceSource s) {
  switch (s) {
    case ServiceSource::L1: return "l1";
    case ServiceSource::L2: return "l2";
    case ServiceSource::LLC: return "llc";
    case ServiceSource::Dram: return "dram";
  }
  return "unknown";
}

bool parse_write_policy(const std::string& s, WritePolicy& out) {
  if (s == "write-back" || s == "writeback" || s == "wb") { out = WritePolicy::WriteBack; return true; }
  if (s == "write-through" || s == "writethrough" || s == "wt") { out = WritePolicy::WriteThrough; return true; }
  return false;
}

bool parse_alloc_policy(const std::string& s, AllocPolicy& out) {
  if (s == "write-allocate" || s == "writeallocate" || s == "wa") { out = AllocPolicy::WriteAllocate; return true; }
  if (s == "no-write-allocate" || s == "nowriteallocate" || s == "nwa") { out = AllocPolicy::NoWriteAllocate; return true; }
  return false;
}

bool parse_replacement(const std::string& s, ReplacementKind& out) {
  if (s == "lru") { out = ReplacementKind::LRU; return true; }
  if (s == "fifo") { out = ReplacementKind::FIFO; return true; }
  if (s == "random" || s == "rand") { out = ReplacementKind::Random; return true; }
  return false;
}

bool parse_inclusion(const std::string& s, InclusionPolicy& out) {
  if (s == "inclusive") { out = InclusionPolicy::Inclusive; return true; }
  if (s == "non-inclusive" || s == "noninclusive" || s == "exclusive-ish") {
    out = InclusionPolicy::NonInclusive;
    return true;
  }
  return false;
}

}  // namespace cachescope
