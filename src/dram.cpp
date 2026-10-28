#include "cachescope/dram.hpp"

#include <algorithm>
#include <cmath>

namespace cachescope {

Dram::Dram(const DramConfig& cfg) : cfg_(cfg) {}

Dram::Response Dram::access(Cycle now, bool is_write, std::uint32_t bytes) {
  // Service occupancy of the data bus, at least one cycle per request.
  const double raw = static_cast<double>(bytes) / cfg_.bytes_per_cycle;
  const Cycle service = static_cast<Cycle>(std::max(1.0, std::ceil(raw)));

  const Cycle start = std::max(now, bus_free_at_);
  const Cycle queue_delay = start - now;

  // Outstanding requests still occupying the bus, used only for reporting.
  const Cycle backlog = bus_free_at_ > now ? bus_free_at_ - now : 0;
  const Count depth = static_cast<Count>(backlog / service);
  stats_.max_queue_depth = std::max(stats_.max_queue_depth, depth);
  if (depth >= cfg_.queue_capacity) ++stats_.requests_at_capacity;

  bus_free_at_ = start + service;
  stats_.busy_cycles += service;
  stats_.queue_delay_cycles += queue_delay;

  if (is_write) {
    ++stats_.writes;
    stats_.bytes_written += bytes;
  } else {
    ++stats_.reads;
    stats_.bytes_read += bytes;
  }

  Response r;
  r.queue_delay = queue_delay;
  r.latency = queue_delay + cfg_.latency_cycles;
  return r;
}

double Dram::bw_utilization(Cycle elapsed) const {
  return bw_utilization_window(stats_.bytes_total(), elapsed);
}

double Dram::bw_utilization_window(Count bytes_in_window, Cycle elapsed_in_window) const {
  if (elapsed_in_window == 0) return 0.0;
  const double peak = static_cast<double>(elapsed_in_window) * cfg_.bytes_per_cycle;
  if (peak <= 0.0) return 0.0;
  return std::min(1.0, static_cast<double>(bytes_in_window) / peak);
}

}  // namespace cachescope
