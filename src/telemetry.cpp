#include "cachescope/telemetry.hpp"

#include <stdexcept>

namespace cachescope {

Telemetry::Telemetry(const TelemetryConfig& cfg) : cfg_(cfg) {
  if (!cfg_.csv_path.empty()) {
    csv_ = std::fopen(cfg_.csv_path.c_str(), "wb");
    if (csv_ == nullptr) {
      throw std::runtime_error("cannot open telemetry CSV for writing: " + cfg_.csv_path);
    }
  }
  if (!cfg_.jsonl_path.empty()) {
    jsonl_ = std::fopen(cfg_.jsonl_path.c_str(), "wb");
    if (jsonl_ == nullptr) {
      if (csv_ != nullptr) std::fclose(csv_);
      throw std::runtime_error("cannot open telemetry JSONL for writing: " + cfg_.jsonl_path);
    }
  }
}

Telemetry::~Telemetry() {
  if (csv_ != nullptr) std::fclose(csv_);
  if (jsonl_ != nullptr) std::fclose(jsonl_);
}

void Telemetry::write_row(const MetricTable& table) {
  if (csv_ != nullptr) {
    if (!csv_header_written_) {
      const std::string header = table.csv_header();
      std::fwrite(header.data(), 1, header.size(), csv_);
      std::fputc('\n', csv_);
      csv_header_written_ = true;
    }
    const std::string row = table.csv_row();
    std::fwrite(row.data(), 1, row.size(), csv_);
    std::fputc('\n', csv_);
  }
  if (jsonl_ != nullptr) {
    const std::string line = table.to_json().dump();
    std::fwrite(line.data(), 1, line.size(), jsonl_);
    std::fputc('\n', jsonl_);
  }
}

bool Telemetry::write_summary(const Json& summary) const {
  if (cfg_.summary_json_path.empty()) return true;
  std::FILE* f = std::fopen(cfg_.summary_json_path.c_str(), "wb");
  if (f == nullptr) return false;
  const std::string text = summary.dump(2);
  std::fwrite(text.data(), 1, text.size(), f);
  std::fputc('\n', f);
  std::fclose(f);
  return true;
}

}  // namespace cachescope
