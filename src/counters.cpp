#include "cachescope/counters.hpp"

#include <cmath>
#include <cstdio>

namespace cachescope {

void MetricTable::set(const std::string& name, double value) {
  auto it = values_.find(name);
  if (it == values_.end()) {
    order_.push_back(name);
    values_.emplace(name, value);
  } else {
    it->second = value;
  }
}

bool MetricTable::has(const std::string& name) const {
  return values_.find(name) != values_.end();
}

double MetricTable::get(const std::string& name, double fallback) const {
  auto it = values_.find(name);
  return it == values_.end() ? fallback : it->second;
}

Json MetricTable::to_json() const {
  Json j = Json::object();
  for (const std::string& n : order_) {
    j.set(n, values_.at(n));
  }
  return j;
}

std::string MetricTable::csv_header() const {
  std::string out;
  for (std::size_t i = 0; i < order_.size(); ++i) {
    if (i) out.push_back(',');
    out += order_[i];
  }
  return out;
}

std::string MetricTable::csv_row() const {
  std::string out;
  char buf[48];
  for (std::size_t i = 0; i < order_.size(); ++i) {
    if (i) out.push_back(',');
    const double v = values_.at(order_[i]);
    if (!std::isfinite(v)) {
      // Leave the cell empty; pandas reads that as NaN.
      continue;
    }
    if (v == std::floor(v) && std::fabs(v) < 9.0e15) {
      std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v));
    } else {
      std::snprintf(buf, sizeof(buf), "%.6f", v);
    }
    out += buf;
  }
  return out;
}

double safe_div(double num, double den) { return den == 0.0 ? 0.0 : num / den; }

double per_kilo_instr(double events, double instructions) {
  return safe_div(events * 1000.0, instructions);
}

}  // namespace cachescope
