#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>
#include "samples.h"
#include <limits>

namespace metrics {

struct Report {
  uint64_t received = 0;
  uint64_t expected = 0;
  uint64_t dropped = 0;
  double drop_rate = 0.0;

  uint64_t lat_min = 0;
  uint64_t lat_max = 0;
  double lat_mean = 0.0;
  uint64_t p01 = 0, p50 = 0, p99 = 0, p999 = 0, p9999 = 0;
};

class Accumulator {
 public:
  explicit Accumulator(size_t reserve = 0) {
    if (reserve) latencies_.reserve(std::min(reserve,kSampleLimit));
  }

  void record(uint64_t seq_id, uint64_t latency_ns) {
    if (received_ == 0) {
      first_seq_ = seq_id;
      last_seq_ = seq_id;
    } else {
      if (seq_id < first_seq_) first_seq_ = seq_id;
      if (seq_id > last_seq_) last_seq_ = seq_id;
    }
    ++received_;
    sample(latencies_,latency_ns,received_);
    min_=std::min(min_,latency_ns);max_=std::max(max_,latency_ns);
    sum_ += static_cast<double>(latency_ns);
  }

  size_t retained_samples() const {return latencies_.size();}

  Report report() const {
    Report r;
    r.received = received_;
    if (received_ == 0) return r;

    r.expected = last_seq_ - first_seq_ + 1;
    r.dropped = r.expected > r.received ? r.expected - r.received : 0;
    r.drop_rate = r.expected ? static_cast<double>(r.dropped) /
                                   static_cast<double>(r.expected)
                             : 0.0;

    std::vector<uint64_t> s = latencies_;
    std::sort(s.begin(), s.end());
    r.lat_min = min_;
    r.lat_max = max_;
    r.lat_mean = sum_ / static_cast<double>(received_);
    r.p01 = percentile(s, 0.01);
    r.p50 = percentile(s, 0.50);
    r.p99 = percentile(s, 0.99);
    r.p999 = percentile(s, 0.999);
    r.p9999 = percentile(s, 0.9999);
    return r;
  }

 private:

  static uint64_t percentile(const std::vector<uint64_t>& sorted, double p) {
    if (sorted.empty()) return 0;
    size_t n = sorted.size();
    size_t rank = static_cast<size_t>(p * static_cast<double>(n));
    if (static_cast<double>(rank) < p * static_cast<double>(n)) ++rank;
    if (rank < 1) rank = 1;
    if (rank > n) rank = n;
    return sorted[rank - 1];
  }

  uint64_t min_=std::numeric_limits<uint64_t>::max(), max_=0;
  uint64_t received_ = 0;
  uint64_t first_seq_ = 0;
  uint64_t last_seq_ = 0;
  double sum_ = 0.0;
  std::vector<uint64_t> latencies_;
};

}
