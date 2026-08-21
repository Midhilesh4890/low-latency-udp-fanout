#include <algorithm>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "fec.h"
#include "message.h"
#include "shm_ring.h"
#include "shm_segment.h"
#include "util.h"

namespace {

struct Destination {
  std::string host;
  std::string port;
};

struct Config {
  std::string in_shm = "/fanout_ring";
  uint32_t slots = 1024;
  std::string host = "127.0.0.1";
  std::string port = "9000";
  std::vector<Destination> destinations;
  uint64_t count = 0;
  bool from_edge = false;
  uint64_t idle_ms = 2000;
  int sndbuf = 4 * 1024 * 1024;
  uint32_t repeat = 1;
  uint16_t fec_k = 0;
  uint64_t fec_timeout_us = 200;
  double test_drop_pct = 0.0;
  double test_reorder_pct = 0.0;
  uint64_t test_reorder_delay_us = 0;
  uint64_t test_seed = 1;
};

struct PendingDatagram {
  std::vector<uint8_t> bytes;
  uint64_t release_ns = 0;
};


struct Distribution {
  uint64_t p50 = 0;
  uint64_t p99 = 0;
  uint64_t max = 0;
};

Distribution distribution(std::vector<uint64_t> values) {
  Distribution result;
  if (values.empty()) return result;
  std::sort(values.begin(), values.end());
  auto value_at = [&](double p) -> uint64_t {
    size_t rank = static_cast<size_t>(p * static_cast<double>(values.size()));
    if (static_cast<double>(rank) < p * static_cast<double>(values.size())) ++rank;
    if (rank < 1) rank = 1;
    if (rank > values.size()) rank = values.size();
    return values[rank - 1];
  };
  result.p50 = value_at(0.50);
  result.p99 = value_at(0.99);
  result.max = values.back();
  return result;
}

Destination parse_destination(const std::string& value) {
  const size_t pos = value.rfind(":");
  if (pos == std::string::npos || pos == 0 || pos + 1 >= value.size()) {
    fprintf(stderr, "--dst must be host:port (got %s)\n", value.c_str());
    std::exit(2);
  }
  return Destination{value.substr(0, pos), value.substr(pos + 1)};
}

Config parse_args(int argc, char** argv) {
  Config c;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&]() -> std::string {
      if (i + 1 >= argc) {
        fprintf(stderr, "missing value for %s\n", a.c_str());
        std::exit(2);
      }
      return argv[++i];
    };
    if (a == "--in-shm") c.in_shm = next();
    else if (a == "--slots") c.slots = static_cast<uint32_t>(std::stoul(next()));
    else if (a == "--host") c.host = next();
    else if (a == "--port") c.port = next();
    else if (a == "--dst") c.destinations.push_back(parse_destination(next()));
    else if (a == "--count") c.count = std::stoull(next());
    else if (a == "--from-edge") c.from_edge = true;
    else if (a == "--idle-ms") c.idle_ms = std::stoull(next());
    else if (a == "--sndbuf") c.sndbuf = std::stoi(next());
    else if (a == "--repeat") c.repeat = static_cast<uint32_t>(std::stoul(next()));
    else if (a == "--fec-k") c.fec_k = static_cast<uint16_t>(std::stoul(next()));
    else if (a == "--fec-timeout-us") c.fec_timeout_us = std::stoull(next());
    else if (a == "--test-drop-pct") c.test_drop_pct = std::stod(next());
    else if (a == "--test-reorder-pct") c.test_reorder_pct = std::stod(next());
    else if (a == "--test-reorder-delay-us") c.test_reorder_delay_us = std::stoull(next());
    else if (a == "--test-seed") c.test_seed = std::stoull(next());
    else {
      fprintf(stderr, "unknown arg: %s\n", a.c_str());
      std::exit(2);
    }
  }
  if (c.repeat == 0) c.repeat = 1;
  if (c.fec_timeout_us == 0) c.fec_timeout_us = 1;
  if (c.destinations.empty()) c.destinations.push_back(Destination{c.host, c.port});
  return c;
}

int open_udp_socket(const Destination& dst, int sndbuf) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_DGRAM;

  addrinfo* result = nullptr;
  const int rc = getaddrinfo(dst.host.c_str(), dst.port.c_str(), &hints, &result);
  if (rc != 0) {
    fprintf(stderr, "getaddrinfo(%s:%s) failed: %s\n", dst.host.c_str(), dst.port.c_str(), gai_strerror(rc));
    std::exit(1);
  }

  int fd = -1;
  for (addrinfo* p = result; p; p = p->ai_next) {
    fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
    if (fd < 0) continue;
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    if (connect(fd, p->ai_addr, p->ai_addrlen) == 0) break;
    close(fd);
    fd = -1;
  }
  freeaddrinfo(result);

  if (fd < 0) {
    perror("connect");
    std::exit(1);
  }
  return fd;
}

void close_sockets(const std::vector<int>& sockets) {
  for (int fd : sockets) close(fd);
}

bool direct_send(const Config& cfg, const std::vector<int>& sockets, const uint8_t* data, uint32_t len, uint64_t& packets) {
  for (int sock : sockets) {
    for (uint32_t repeat = 0; repeat < cfg.repeat; ++repeat) {
      const ssize_t n = send(sock, data, len, 0);
      if (n < 0) {
        perror("send");
        return false;
      }
      if (static_cast<uint32_t>(n) != len) {
        fprintf(stderr, "short UDP send: %zd of %u bytes\n", n, len);
        return false;
      }
      ++packets;
    }
  }
  return true;
}

bool flush_pending(const Config& cfg, const std::vector<int>& sockets, std::vector<PendingDatagram>& pending, uint64_t now_ns, bool all, uint64_t& packets) {
  size_t write = 0;
  for (size_t i = 0; i < pending.size(); ++i) {
    if (all || pending[i].release_ns <= now_ns) {
      if (!direct_send(cfg, sockets, pending[i].bytes.data(), static_cast<uint32_t>(pending[i].bytes.size()), packets)) return false;
    } else {
      if (write != i) pending[write] = std::move(pending[i]);
      ++write;
    }
  }
  pending.resize(write);
  return true;
}

bool emit_datagram(const Config& cfg, const std::vector<int>& sockets, std::vector<PendingDatagram>& pending, std::mt19937_64& rng, std::uniform_real_distribution<double>& dist, const uint8_t* data, uint32_t len, uint64_t& packets, uint64_t& test_dropped, uint64_t& test_reordered) {
  const uint64_t now_ns = util::now_ns();
  if (!flush_pending(cfg, sockets, pending, now_ns, false, packets)) return false;
  if (cfg.test_drop_pct > 0.0 && dist(rng) < cfg.test_drop_pct) {
    ++test_dropped;
    return true;
  }
  if (cfg.test_reorder_pct > 0.0 && cfg.test_reorder_delay_us > 0 && dist(rng) < cfg.test_reorder_pct) {
    PendingDatagram pending_datagram;
    pending_datagram.bytes.assign(data, data + len);
    pending_datagram.release_ns = now_ns + cfg.test_reorder_delay_us * 1000ull;
    pending.push_back(std::move(pending_datagram));
    ++test_reordered;
    return true;
  }
  if (!direct_send(cfg, sockets, data, len, packets)) return false;
  return flush_pending(cfg, sockets, pending, util::now_ns(), false, packets);
}

}

int main(int argc, char** argv) {
  Config cfg = parse_args(argc, argv);

  shm::Segment seg = shm::Segment::open(cfg.in_shm, shm::region_size(cfg.slots), false);
  shm::Ring ring;
  ring.attach(seg.base(), cfg.slots, false);

  std::vector<int> sockets;
  sockets.reserve(cfg.destinations.size());
  for (const auto& dst : cfg.destinations) sockets.push_back(open_udp_socket(dst, cfg.sndbuf));

  fprintf(stderr, "sender: in_shm=%s slots=%u targets=%zu count=%llu from_edge=%s sndbuf=%d repeat=%u fec_k=%u fec_timeout_us=%llu test_drop_pct=%.6f test_reorder_pct=%.6f test_reorder_delay_us=%llu test_seed=%llu orderbook_size=%zu worst_datagram=%zu\n",
          cfg.in_shm.c_str(), cfg.slots, sockets.size(), static_cast<unsigned long long>(cfg.count), cfg.from_edge ? "true" : "false",
          cfg.sndbuf, cfg.repeat, cfg.fec_k, static_cast<unsigned long long>(cfg.fec_timeout_us), cfg.test_drop_pct, cfg.test_reorder_pct,
          static_cast<unsigned long long>(cfg.test_reorder_delay_us), static_cast<unsigned long long>(cfg.test_seed), sizeof(msg::OrderBook),
          sizeof(fec::Envelope) + sizeof(uint16_t) + sizeof(msg::OrderBook));

  uint64_t read_index = cfg.from_edge ? ring.live_edge() : 0;
  uint64_t sent = 0;
  uint64_t packets = 0;
  uint64_t lapped_events = 0;
  uint64_t fec_parity_sent = 0;
  uint64_t fec_closed_by_k = 0;
  uint64_t fec_closed_by_timeout = 0;
  uint64_t fec_closed_by_flush = 0;
  uint64_t fec_data_bytes = 0;
  uint64_t fec_parity_bytes = 0;
  uint64_t test_dropped = 0;
  uint64_t test_reordered = 0;
  const uint64_t idle_ns = cfg.idle_ms * 1000000ull;
  const uint64_t fec_timeout_ns = cfg.fec_timeout_us * 1000ull;
  uint64_t last_progress = util::now_ns();
  uint64_t fec_first_ns = 0;
  uint64_t fec_last_arrival_ns = 0;
  uint32_t fec_gen_id = 0;
  std::vector<uint64_t> fec_arrival_intervals;

  fec::Encoder encoder(cfg.fec_k == 0 ? 1 : cfg.fec_k);
  std::vector<PendingDatagram> pending;
  std::mt19937_64 rng(cfg.test_seed);
  std::uniform_real_distribution<double> dist(0.0, 100.0);

  auto close_generation = [&](uint32_t reason) -> bool {
    if (cfg.fec_k == 0 || encoder.empty()) return true;
    fec::BuiltGeneration built = encoder.close(fec_gen_id);
    if (!emit_datagram(cfg, sockets, pending, rng, dist, built.parity.data(), static_cast<uint32_t>(built.parity.size()), packets, test_dropped, test_reordered)) return false;
    ++fec_parity_sent;
    fec_parity_bytes += built.parity.size();
    if (reason == 0) ++fec_closed_by_k;
    else if (reason == 1) ++fec_closed_by_timeout;
    else ++fec_closed_by_flush;
    ++fec_gen_id;
    fec_first_ns = 0;
    return true;
  };

  uint8_t frame[shm::kFrameCap];
  while (cfg.count == 0 || sent < cfg.count) {
    if (cfg.fec_k != 0 && !encoder.empty() && util::now_ns() - fec_first_ns >= fec_timeout_ns) {
      if (!close_generation(1)) {
        close_sockets(sockets);
        return 1;
      }
    }

    uint32_t len = 0;
    uint64_t resume = 0;
    auto st = ring.read(read_index, frame, &len, &resume);

    if (st == shm::Ring::FrameStatus::kOk) {
      const uint64_t fec_arrival_ns = util::now_ns();
      if (fec_last_arrival_ns != 0) fec_arrival_intervals.push_back(fec_arrival_ns - fec_last_arrival_ns);
      fec_last_arrival_ns = fec_arrival_ns;
      if (cfg.fec_k == 0) {
        fec_data_bytes += len;
        if (!emit_datagram(cfg, sockets, pending, rng, dist, frame, len, packets, test_dropped, test_reordered)) {
          close_sockets(sockets);
          return 1;
        }
      } else {
        if (encoder.empty()) fec_first_ns = util::now_ns();
        const uint16_t index = encoder.size();
        if (!encoder.add(frame, static_cast<uint16_t>(len))) {
          close_sockets(sockets);
          return 1;
        }
        std::vector<uint8_t> data = fec::data_datagram(fec_gen_id, index, cfg.fec_k, frame, static_cast<uint16_t>(len));
        fec_data_bytes += data.size();
        if (!emit_datagram(cfg, sockets, pending, rng, dist, data.data(), static_cast<uint32_t>(data.size()), packets, test_dropped, test_reordered)) {
          close_sockets(sockets);
          return 1;
        }
        if (encoder.full()) {
          if (!close_generation(0)) {
            close_sockets(sockets);
            return 1;
          }
        }
      }
      ++sent;
      ++read_index;
      last_progress = util::now_ns();
    } else if (st == shm::Ring::FrameStatus::kLapped) {
      ++lapped_events;
      read_index = resume;
    } else {
      if (!flush_pending(cfg, sockets, pending, util::now_ns(), false, packets)) {
        close_sockets(sockets);
        return 1;
      }
      if (util::now_ns() - last_progress > idle_ns) break;
    }
  }

  if (!close_generation(2)) {
    close_sockets(sockets);
    return 1;
  }
  if (!flush_pending(cfg, sockets, pending, util::now_ns(), true, packets)) {
    close_sockets(sockets);
    return 1;
  }

  const Distribution arrival_stats = distribution(fec_arrival_intervals);
  const double fec_overhead_pct = fec_data_bytes == 0 ? 0.0 : static_cast<double>(fec_parity_bytes) * 100.0 / static_cast<double>(fec_data_bytes);
  fprintf(stderr, "sender: sent=%llu packets=%llu lapped=%llu fec_parity_sent=%llu fec_closed_by_k=%llu fec_closed_by_timeout=%llu fec_closed_by_flush=%llu fec_data_bytes=%llu fec_parity_bytes=%llu fec_overhead_pct=%.6f fec_arrival_p50_ns=%llu fec_arrival_p99_ns=%llu fec_arrival_max_ns=%llu test_dropped=%llu test_reordered=%llu\n",
          static_cast<unsigned long long>(sent), static_cast<unsigned long long>(packets), static_cast<unsigned long long>(lapped_events),
          static_cast<unsigned long long>(fec_parity_sent), static_cast<unsigned long long>(fec_closed_by_k),
          static_cast<unsigned long long>(fec_closed_by_timeout), static_cast<unsigned long long>(fec_closed_by_flush),
          static_cast<unsigned long long>(fec_data_bytes), static_cast<unsigned long long>(fec_parity_bytes), fec_overhead_pct,
          static_cast<unsigned long long>(arrival_stats.p50), static_cast<unsigned long long>(arrival_stats.p99),
          static_cast<unsigned long long>(arrival_stats.max), static_cast<unsigned long long>(test_dropped),
          static_cast<unsigned long long>(test_reordered));
  close_sockets(sockets);
  return 0;
}
