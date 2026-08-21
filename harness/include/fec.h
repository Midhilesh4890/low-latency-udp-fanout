#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "message.h"
#include "shm_ring.h"

namespace fec {

inline constexpr uint32_t kMagic = 0x46454331;

#pragma pack(push, 1)
struct Envelope {
  uint32_t magic;
  uint32_t gen_id;
  uint16_t index;
  uint16_t k;
  uint16_t parity_count;
  uint16_t protected_len;
};
#pragma pack(pop)

static_assert(sizeof(Envelope) == 16, "fec envelope must be 16 bytes");
static_assert(sizeof(Envelope) + sizeof(uint16_t) + msg::kMaxFrame <= 1500, "fec datagram must fit mtu");

inline void put_u16(std::vector<uint8_t>& out, uint16_t value) {
  out[0] = static_cast<uint8_t>(value & 255u);
  out[1] = static_cast<uint8_t>(value >> 8u);
}

inline uint16_t get_u16(const uint8_t* data) {
  return static_cast<uint16_t>(data[0]) | static_cast<uint16_t>(data[1] << 8u);
}

inline std::vector<uint8_t> protected_frame(const uint8_t* frame, uint16_t len, uint16_t padded_len) {
  std::vector<uint8_t> out(static_cast<size_t>(padded_len) + sizeof(uint16_t), 0);
  put_u16(out, len);
  std::memcpy(out.data() + sizeof(uint16_t), frame, len);
  return out;
}

inline std::vector<uint8_t> data_datagram(uint32_t gen_id, uint16_t index, uint16_t k, const uint8_t* frame, uint16_t len) {
  std::vector<uint8_t> out(sizeof(Envelope) + len);
  Envelope header{kMagic, gen_id, index, k, 0, len};
  std::memcpy(out.data(), &header, sizeof(header));
  std::memcpy(out.data() + sizeof(header), frame, len);
  return out;
}

struct BuiltGeneration {
  std::vector<std::vector<uint8_t>> data;
  std::vector<uint8_t> parity;
};

class Encoder {
 public:
  explicit Encoder(uint16_t k) : k_(k) {}

  bool add(const uint8_t* frame, uint16_t len) {
    if (frames_.size() == k_) return false;
    frames_.push_back(std::vector<uint8_t>(frame, frame + len));
    if (len > max_len_) max_len_ = len;
    return true;
  }

  bool empty() const { return frames_.empty(); }
  uint16_t size() const { return static_cast<uint16_t>(frames_.size()); }
  bool full() const { return frames_.size() == k_; }

  BuiltGeneration close(uint32_t gen_id) {
    const uint16_t actual_k = static_cast<uint16_t>(frames_.size());
    std::vector<uint8_t> parity(static_cast<size_t>(max_len_) + sizeof(uint16_t), 0);
    BuiltGeneration built;
    for (uint16_t i = 0; i < actual_k; ++i) {
      built.data.push_back(data_datagram(gen_id, i, k_, frames_[i].data(), static_cast<uint16_t>(frames_[i].size())));
      std::vector<uint8_t> protected_data = protected_frame(frames_[i].data(), static_cast<uint16_t>(frames_[i].size()), max_len_);
      for (size_t j = 0; j < parity.size(); ++j) parity[j] ^= protected_data[j];
    }
    built.parity.resize(sizeof(Envelope) + parity.size());
    Envelope header{kMagic, gen_id, actual_k, actual_k, 1, static_cast<uint16_t>(parity.size())};
    std::memcpy(built.parity.data(), &header, sizeof(header));
    std::memcpy(built.parity.data() + sizeof(header), parity.data(), parity.size());
    frames_.clear();
    max_len_ = 0;
    return built;
  }

 private:
  uint16_t k_;
  uint16_t max_len_ = 0;
  std::vector<std::vector<uint8_t>> frames_;
};

struct Counters {
  uint64_t parity_received = 0;
  uint64_t recovered = 0;
  uint64_t unrecoverable_gens = 0;
};

struct RecoveryStats {
  uint64_t p50 = 0;
  uint64_t p99 = 0;
  uint64_t max = 0;
};

class Decoder {
 public:
  explicit Decoder(uint32_t gens = 64) : gens_(gens ? gens : 1), slots_(gens_) {}

  template <class Publish>
  bool receive(const uint8_t* data, uint32_t len, uint64_t now_ns, Publish publish) {
    if (len < sizeof(Envelope)) return publish(data, len);
    Envelope envelope{};
    std::memcpy(&envelope, data, sizeof(envelope));
    if (envelope.magic != kMagic) return publish(data, len);
    const uint8_t* payload = data + sizeof(Envelope);
    const uint32_t payload_len = len - sizeof(Envelope);
    if (envelope.parity_count == 0) {
      if (payload_len != envelope.protected_len || envelope.protected_len > shm::kFrameCap) return false;
      Generation& gen = slot(envelope.gen_id, envelope.k, now_ns);
      ensure(gen, envelope.k);
      if (envelope.index >= gen.k) return false;
      gen.seen_any = true;
      gen.first_ns = gen.first_ns == 0 ? now_ns : gen.first_ns;
      gen.data[envelope.index] = protected_frame(payload, envelope.protected_len, envelope.protected_len);
      gen.received[envelope.index] = true;
      if (!publish(payload, payload_len)) return false;
      try_recover(gen, now_ns, publish);
      return true;
    }
    ++counters_.parity_received;
    Generation& gen = slot(envelope.gen_id, envelope.k, now_ns);
    ensure(gen, envelope.k);
    gen.seen_any = true;
    gen.first_ns = gen.first_ns == 0 ? now_ns : gen.first_ns;
    gen.parity.assign(payload, payload + payload_len);
    gen.parity_received = true;
    try_recover(gen, now_ns, publish);
    return true;
  }

  void retire_all() {
    for (Generation& gen : slots_) finalize(gen);
  }

  const Counters& counters() const { return counters_; }

  RecoveryStats recovery_stats() const {
    RecoveryStats stats;
    if (recovery_latencies_.empty()) return stats;
    std::vector<uint64_t> values = recovery_latencies_;
    std::sort(values.begin(), values.end());
    stats.p50 = percentile(values, 0.50);
    stats.p99 = percentile(values, 0.99);
    stats.max = values.back();
    return stats;
  }

 private:
  struct Generation {
    bool active = false;
    bool seen_any = false;
    bool parity_received = false;
    bool recovered = false;
    bool unrecoverable_counted = false;
    uint32_t gen_id = 0;
    uint16_t k = 0;
    uint64_t first_ns = 0;
    std::vector<std::vector<uint8_t>> data;
    std::vector<uint8_t> parity;
    std::vector<uint8_t> received;
  };

  Generation& slot(uint32_t gen_id, uint16_t k, uint64_t now_ns) {
    Generation& gen = slots_[gen_id % gens_];
    if (!gen.active || gen.gen_id != gen_id) {
      finalize(gen);
      gen = Generation{};
      gen.active = true;
      gen.gen_id = gen_id;
      gen.k = k;
      gen.first_ns = now_ns;
    }
    return gen;
  }

  void ensure(Generation& gen, uint16_t k) {
    if (gen.k == 0 || k < gen.k) gen.k = k;
    if (gen.data.size() < gen.k) {
      gen.data.resize(gen.k);
      gen.received.resize(gen.k, 0);
    }
  }

  template <class Publish>
  void try_recover(Generation& gen, uint64_t now_ns, Publish publish) {
    if (!gen.parity_received || gen.recovered || gen.k == 0) return;
    uint16_t missing = gen.k;
    uint16_t missing_index = 0;
    for (uint16_t i = 0; i < gen.k; ++i) {
      if (gen.received[i]) --missing;
      else missing_index = i;
    }
    if (missing != 1) return;
    std::vector<uint8_t> recovered = gen.parity;
    for (uint16_t i = 0; i < gen.k; ++i) {
      if (!gen.received[i]) continue;
      for (size_t j = 0; j < gen.data[i].size() && j < recovered.size(); ++j) recovered[j] ^= gen.data[i][j];
    }
    if (recovered.size() < sizeof(uint16_t)) return;
    const uint16_t len = get_u16(recovered.data());
    if (len > shm::kFrameCap || static_cast<size_t>(len) + sizeof(uint16_t) > recovered.size()) return;
    publish(recovered.data() + sizeof(uint16_t), len);
    gen.received[missing_index] = true;
    gen.recovered = true;
    ++counters_.recovered;
    recovery_latencies_.push_back(now_ns >= gen.first_ns ? now_ns - gen.first_ns : 0);
  }

  void finalize(Generation& gen) {
    if (!gen.active || !gen.parity_received || gen.recovered || gen.unrecoverable_counted || gen.k == 0) return;
    uint16_t missing = 0;
    for (uint16_t i = 0; i < gen.k; ++i) {
      if (i >= gen.received.size() || !gen.received[i]) ++missing;
    }
    if (missing > 1) {
      ++counters_.unrecoverable_gens;
      gen.unrecoverable_counted = true;
    }
  }

  static uint64_t percentile(const std::vector<uint64_t>& values, double p) {
    size_t rank = static_cast<size_t>(p * static_cast<double>(values.size()));
    if (static_cast<double>(rank) < p * static_cast<double>(values.size())) ++rank;
    if (rank < 1) rank = 1;
    if (rank > values.size()) rank = values.size();
    return values[rank - 1];
  }

  uint32_t gens_;
  std::vector<Generation> slots_;
  Counters counters_;
  std::vector<uint64_t> recovery_latencies_;
};

}
