#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <type_traits>
#include <vector>

#include "message.h"
#include "wire.h"
#include "samples.h"
#include <stdexcept>
#include "shm_ring.h"

namespace fec {

inline constexpr uint32_t kMagic = 0x46454332;
inline constexpr uint16_t kMaxGeneration = 128;

struct Envelope {
  uint32_t magic;
  uint32_t gen_id;
  uint16_t index;
  uint16_t k;
  uint16_t parity_count;
  uint16_t protected_len;
  uint8_t close_reason;
};

inline constexpr size_t kEnvelopeSize=17;
static_assert(kEnvelopeSize + sizeof(uint16_t) + msg::kMaxFrame <= 1500, "fec datagram must fit mtu");

// Cauchy Reed-Solomon coefficients over GF(256), polynomial 0x11d.
// Normalize columns so parity row zero remains XOR.
inline uint8_t mul(uint8_t a, uint8_t b) {
  uint8_t out=0;
  while(b) { if(b&1) out^=a; bool high=a&128; a<<=1; if(high) a^=0x1d; b>>=1; }
  return out;
}
inline uint8_t inverse(uint8_t a) {
  if(!a) throw std::invalid_argument("zero GF inverse");
  uint8_t out=1; for(int n=254;n;n>>=1,a=mul(a,a)) if(n&1) out=mul(out,a);
  return out;
}
inline uint8_t coefficient(uint16_t row,uint16_t col) {
  return mul(static_cast<uint8_t>(col^128), inverse(static_cast<uint8_t>(col^(128+row))));
}
inline void write_envelope(uint8_t* out,const Envelope& h) {
  wire::Writer w;
  auto v=h;
  w(v.magic,v.gen_id,v.index,v.k,v.parity_count,v.protected_len,v.close_reason);
  std::memcpy(out,w.bytes.data(),kEnvelopeSize);
}
inline Envelope read_envelope(const uint8_t* data) {
  Envelope h{}; wire::Reader r{data,kEnvelopeSize};
  r(h.magic,h.gen_id,h.index,h.k,h.parity_count,h.protected_len,h.close_reason);
  return h;
}

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
  std::vector<uint8_t> out(kEnvelopeSize + len);
  Envelope header{kMagic, gen_id, index, k, 0, len, 0};
  write_envelope(out.data(), header);
  std::memcpy(out.data() + kEnvelopeSize, frame, len);
  return out;
}

struct BuiltGeneration {
  std::vector<std::vector<uint8_t>> data;
  std::vector<uint8_t> parity;
  std::vector<std::vector<uint8_t>> extra_parity;
};

class Encoder {
 public:
  explicit Encoder(uint16_t k, uint16_t parity_count=1) : k_(k), parity_count_(parity_count) {
    if(k==0 || k>kMaxGeneration || parity_count==0 || parity_count>16) throw std::invalid_argument("FEC requires k=1..128, parity=1..16");
  }

  bool add(const uint8_t* frame, uint16_t len) {
    if (!frame || !len || len>wire::max_frame || frames_.size() == k_) return false;
    frames_.push_back(std::vector<uint8_t>(frame, frame + len));
    if (len > max_len_) max_len_ = len;
    return true;
  }

  bool empty() const { return frames_.empty(); }
  uint16_t size() const { return static_cast<uint16_t>(frames_.size()); }
  bool full() const { return frames_.size() == k_; }

  BuiltGeneration close(uint32_t gen_id, uint8_t close_reason) {
    if(empty()) throw std::invalid_argument("empty FEC generation");
    const uint16_t actual_k = static_cast<uint16_t>(frames_.size());
    std::vector<uint8_t> parity(static_cast<size_t>(max_len_) + sizeof(uint16_t), 0);
    BuiltGeneration built;
    for (uint16_t i = 0; i < actual_k; ++i) {
      built.data.push_back(data_datagram(gen_id, i, k_, frames_[i].data(), static_cast<uint16_t>(frames_[i].size())));
      std::vector<uint8_t> protected_data = protected_frame(frames_[i].data(), static_cast<uint16_t>(frames_[i].size()), max_len_);
      for (size_t j = 0; j < parity.size(); ++j) parity[j] ^= protected_data[j];
    }
    built.parity.resize(kEnvelopeSize + parity.size());
    Envelope header{kMagic, gen_id, actual_k, actual_k, parity_count_, static_cast<uint16_t>(parity.size()), close_reason};
    write_envelope(built.parity.data(), header);
    std::memcpy(built.parity.data() + kEnvelopeSize, parity.data(), parity.size());
    for(uint16_t row=1;row<parity_count_;++row) {
      std::vector<uint8_t> shard(parity.size(),0);
      for(uint16_t col=0;col<actual_k;++col) {
        auto block=protected_frame(frames_[col].data(),static_cast<uint16_t>(frames_[col].size()),max_len_);
        auto c=coefficient(row,col);
        for(size_t j=0;j<shard.size();++j) shard[j]^=mul(c,block[j]);
      }
      std::vector<uint8_t> packet(kEnvelopeSize+shard.size());
      header.index=actual_k+row; header.parity_count=parity_count_;
      write_envelope(packet.data(),header);
      std::memcpy(packet.data()+kEnvelopeSize,shard.data(),shard.size());
      built.extra_parity.push_back(std::move(packet));
    }
    frames_.clear();
    max_len_ = 0;
    return built;
  }

 private:
  uint16_t k_;
  uint16_t parity_count_;
  uint16_t max_len_ = 0;
  std::vector<std::vector<uint8_t>> frames_;
};

struct Counters {
  uint64_t parity_received = 0;
  uint64_t recovered = 0;
  uint64_t unrecoverable_gens = 0;
  uint64_t recovered_by_k = 0;
  uint64_t recovered_by_timeout = 0;
  uint64_t recovered_by_flush = 0;
};

struct RecoveryStats {
  uint64_t p50 = 0;
  uint64_t p99 = 0;
  uint64_t p999 = 0;
  uint64_t p9999 = 0;
  uint64_t max = 0;
};

struct SplitRecoveryStats {
  RecoveryStats by_k;
  RecoveryStats by_timeout;
  RecoveryStats by_flush;
};

class Decoder {
 public:
  explicit Decoder(uint32_t gens = 64) : gens_(gens ? gens : 1), slots_(gens_) {}

  template <class Publish>
  bool receive(const uint8_t* data, uint32_t len, uint64_t now_ns, Publish publish) {
    if (len < kEnvelopeSize) return call_publish(publish, data, len, false, 0);
    Envelope envelope{};
    envelope=read_envelope(data);
    if (envelope.magic != kMagic) return call_publish(publish, data, len, false, 0);
    if (envelope.k == 0 || envelope.k > kMaxGeneration || envelope.close_reason>2) return false;
    const uint8_t* payload = data + kEnvelopeSize;
    const uint32_t payload_len = len - kEnvelopeSize;
    if (envelope.parity_count == 0) {
      if (!payload_len || payload_len != envelope.protected_len || envelope.protected_len > wire::max_frame ||
          envelope.index >= envelope.k) return false;
      Generation& gen = slot(envelope.gen_id, envelope.k, now_ns);
      if(envelope.k<gen.k || (!gen.parity_received && envelope.k!=gen.k)) return false;
      ensure(gen, envelope.k);
      if (envelope.index >= gen.k) return false;
      gen.seen_any = true;
      gen.first_ns = gen.first_ns == 0 ? now_ns : gen.first_ns;
      if (!call_publish(publish, payload, payload_len, false, gen.close_reason)) return false;
      gen.data[envelope.index] = protected_frame(payload, envelope.protected_len, envelope.protected_len);
      gen.received[envelope.index] = true;
      return try_recover(gen, now_ns, publish);
    }
    if (envelope.parity_count > 16 || envelope.index < envelope.k || envelope.index >= envelope.k+envelope.parity_count ||
        envelope.protected_len != payload_len || payload_len < sizeof(uint16_t) ||
        payload_len > wire::max_frame + sizeof(uint16_t)) return false;
    ++counters_.parity_received;
    Generation& gen = slot(envelope.gen_id, envelope.k, now_ns);
    // Validate closure before changing dimensions of an existing generation.
    if(gen.parity_received && (gen.k!=envelope.k || gen.parity_count!=envelope.parity_count ||
                               gen.close_reason!=envelope.close_reason)) return false;
    if(envelope.k>gen.k) return false;
    for(size_t i=envelope.k;i<gen.received.size();++i) if(gen.received[i]) return false;
    ensure(gen, envelope.k);
    gen.seen_any = true;
    gen.first_ns = gen.first_ns == 0 ? now_ns : gen.first_ns;
    gen.close_reason = envelope.close_reason;
    gen.parity_count = envelope.parity_count;
    const auto row=envelope.index-envelope.k;
    if(gen.parities.empty()) gen.parities.resize(16);
    for(const auto& existing:gen.parities)
      if(!existing.empty() && existing.size()!=payload_len) return false;
    gen.parities[row].assign(payload, payload + payload_len);
    gen.parity_received = true;
    return try_recover(gen, now_ns, publish);
  }

  void expire(uint64_t now_ns, uint64_t ttl_ns=1000000000ull) {
    for(auto& gen:slots_) if(gen.active && now_ns-gen.first_ns>=ttl_ns) { finalize(gen); gen=Generation{}; }
  }

  void retire_all() {
    for (Generation& gen : slots_) finalize(gen);
  }

  static RecoveryStats summarize(const std::vector<uint64_t>& source) {return recovery_stats_for(source);}

  const Counters& counters() const { return counters_; }

  RecoveryStats recovery_stats() const {
    return recovery_stats_for(recovery_latencies_);
  }

  SplitRecoveryStats split_recovery_stats() const {
    return SplitRecoveryStats{recovery_stats_for(recovery_by_k_), recovery_stats_for(recovery_by_timeout_), recovery_stats_for(recovery_by_flush_)};
  }

  const std::vector<uint64_t>& samples_for(uint8_t reason) const {
    return reason==0?recovery_by_k_:(reason==1?recovery_by_timeout_:recovery_by_flush_);
  }

  const std::vector<uint64_t>& recovery_latencies() const {
    return recovery_latencies_;
  }

 private:
  static RecoveryStats recovery_stats_for(const std::vector<uint64_t>& source) {
    RecoveryStats stats;
    if (source.empty()) return stats;
    std::vector<uint64_t> values = source;
    std::sort(values.begin(), values.end());
    stats.p50 = percentile(values, 0.50);
    stats.p99 = percentile(values, 0.99);
    stats.p999 = percentile(values, 0.999);
    stats.p9999 = percentile(values, 0.9999);
    stats.max = values.back();
    return stats;
  }

  struct Generation {
    bool active = false;
    bool seen_any = false;
    bool parity_received = false;
    bool recovered = false;
    bool unrecoverable_counted = false;
    uint32_t gen_id = 0;
    uint16_t k = 0;
    uint16_t parity_count = 0;
    uint8_t close_reason = 0;
    uint64_t first_ns = 0;
    std::vector<std::vector<uint8_t>> data;
    std::vector<std::vector<uint8_t>> parities;
    std::vector<uint8_t> received;
  };

  template <class Publish>
  static bool call_publish(Publish& publish, const uint8_t* data, uint32_t len, bool recovered, uint8_t close_reason) {
    if constexpr (std::is_invocable_r_v<bool, Publish, const uint8_t*, uint32_t, bool, uint8_t>) {
      return publish(data, len, recovered, close_reason);
    } else if constexpr (std::is_invocable_r_v<bool, Publish, const uint8_t*, uint32_t, bool>) {
      return publish(data, len, recovered);
    } else {
      return publish(data, len);
    }
  }

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
  bool try_recover(Generation& gen, uint64_t now_ns, Publish publish) {
    if (!gen.parity_received || gen.recovered || gen.k == 0) return true;
    uint16_t missing = gen.k;
    uint16_t missing_index = 0;
    for (uint16_t i = 0; i < gen.k; ++i) {
      if (gen.received[i]) --missing;
      else missing_index = i;
    }
    (void)missing_index;
    if(!missing) return true;
    std::vector<uint16_t> absent, rows;
    for(uint16_t i=0;i<gen.k;++i) if(!gen.received[i]) absent.push_back(i);
    for(uint16_t i=0;i<gen.parities.size();++i) if(!gen.parities[i].empty()) rows.push_back(i);
    if(rows.size()<missing) return true;
    rows.resize(missing);
    const size_t width=gen.parities[rows[0]].size();
    std::vector<std::vector<uint8_t>> matrix(missing,std::vector<uint8_t>(missing));
    std::vector<std::vector<uint8_t>> rhs;
    for(uint16_t r=0;r<missing;++r) {
      rhs.push_back(gen.parities[rows[r]]);
      for(uint16_t c=0;c<missing;++c) matrix[r][c]=coefficient(rows[r],absent[c]);
      for(uint16_t c=0;c<gen.k;++c) if(gen.received[c]) {
        if(gen.data[c].size()>width) return false;
        auto factor=coefficient(rows[r],c);
        for(size_t j=0;j<gen.data[c].size();++j) rhs[r][j]^=mul(factor,gen.data[c][j]);
      }
    }
    for(uint16_t c=0;c<missing;++c) {
      uint16_t pivot=c;
      while(pivot<missing && !matrix[pivot][c]) ++pivot;
      if(pivot==missing) return false;
      std::swap(matrix[pivot],matrix[c]); std::swap(rhs[pivot],rhs[c]);
      auto scale=inverse(matrix[c][c]);
      for(auto& x:matrix[c]) x=mul(x,scale);
      for(auto& x:rhs[c]) x=mul(x,scale);
      for(uint16_t r=0;r<missing;++r) if(r!=c) {
        auto factor=matrix[r][c];
        for(uint16_t j=0;j<missing;++j) matrix[r][j]^=mul(factor,matrix[c][j]);
        for(size_t j=0;j<width;++j) rhs[r][j]^=mul(factor,rhs[c][j]);
      }
    }
    for(uint16_t r=0;r<missing;++r) {
      auto& recovered=rhs[r];
      const uint16_t len=get_u16(recovered.data());
      if(!len || len>wire::max_frame || static_cast<size_t>(len)+2>width) return false;
    }
    for(uint16_t r=0;r<missing;++r) {
      auto& recovered=rhs[r]; auto len=get_u16(recovered.data());
      if(!call_publish(publish,recovered.data()+2,len,true,gen.close_reason)) return false;
      gen.received[absent[r]]=true; gen.data[absent[r]]=recovered;
      ++counters_.recovered;
      if(gen.close_reason==0) ++counters_.recovered_by_k;
      else if(gen.close_reason==1) ++counters_.recovered_by_timeout;
      else ++counters_.recovered_by_flush;
    }
    gen.recovered=true;
    const uint64_t latency = now_ns >= gen.first_ns ? now_ns - gen.first_ns : 0;
    metrics::sample(recovery_latencies_,latency,++recovery_events_);
    if (gen.close_reason == 0) metrics::sample(recovery_by_k_,latency,++recovery_k_events_);
    else if (gen.close_reason == 1) metrics::sample(recovery_by_timeout_,latency,++recovery_timeout_events_);
    else metrics::sample(recovery_by_flush_,latency,++recovery_flush_events_);
    return true;
  }

  void finalize(Generation& gen) {
    if (!gen.active || !gen.parity_received || gen.recovered || gen.unrecoverable_counted || gen.k == 0) return;
    uint16_t missing = 0;
    for (uint16_t i = 0; i < gen.k; ++i) {
      if (i >= gen.received.size() || !gen.received[i]) ++missing;
    }
    if (missing > 0) {
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

  uint64_t recovery_events_=0,recovery_k_events_=0,recovery_timeout_events_=0,recovery_flush_events_=0;
  uint32_t gens_;
  std::vector<Generation> slots_;
  Counters counters_;
  std::vector<uint64_t> recovery_latencies_;
  std::vector<uint64_t> recovery_by_k_;
  std::vector<uint64_t> recovery_by_timeout_;
  std::vector<uint64_t> recovery_by_flush_;
};

}
