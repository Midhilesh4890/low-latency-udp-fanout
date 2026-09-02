#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <string>
#include <unistd.h>
#include <vector>

#include "message.h"
#include "metrics.h"
#include "shm_ring.h"
#include "shm_segment.h"
#include "util.h"

namespace {

struct Config {
  std::string shm_name = "/fanout_ring";
  uint32_t slots = 1024;
  uint64_t count = 0;
  bool from_edge = false;
  std::string csv;
  uint64_t idle_ms = 2000;
  uint64_t skip = 0;
};

struct CsvRecord {
  uint64_t seq_id;
  uint64_t send_ts_ns;
  uint64_t recv_ts_ns;
};

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
    if (a == "--shm") c.shm_name = next();
    else if (a == "--slots") c.slots = static_cast<uint32_t>(std::stoul(next()));
    else if (a == "--count") c.count = std::stoull(next());
    else if (a == "--from-edge") c.from_edge = true;
    else if (a == "--csv") c.csv = next();
    else if (a == "--idle-ms") c.idle_ms = std::stoull(next());
    else if (a == "--skip") c.skip = std::stoull(next());
    else {
      fprintf(stderr, "unknown arg: %s\n", a.c_str());
      std::exit(2);
    }
  }
  if (!util::is_power_of_two(c.slots)) {
    fprintf(stderr, "--slots must be a power of two\n");
    std::exit(2);
  }
  return c;
}

void print_report(const metrics::Report& r) {
  printf("---- delivery metrics ----\n");
  printf("received     : %llu\n", (unsigned long long)r.received);
  printf("expected     : %llu\n", (unsigned long long)r.expected);
  printf("dropped      : %llu\n", (unsigned long long)r.dropped);
  printf("drop_rate    : %.4f%%\n", r.drop_rate * 100.0);
  printf("latency (ns) : min=%llu mean=%.0f max=%llu\n",
         (unsigned long long)r.lat_min, r.lat_mean,
         (unsigned long long)r.lat_max);
  printf("  p01        : %llu\n", (unsigned long long)r.p01);
  printf("  p50        : %llu\n", (unsigned long long)r.p50);
  printf("  p99        : %llu\n", (unsigned long long)r.p99);
  printf("  p99.9      : %llu\n", (unsigned long long)r.p999);
  printf("  p99.99     : %llu\n", (unsigned long long)r.p9999);
}

}

int main(int argc, char** argv) {
  Config cfg;
  try {
    cfg = parse_args(argc, argv);
  } catch (const std::exception& error) {
    fprintf(stderr, "invalid argument: %s\n", error.what());
    return 2;
  }
  util::install_signal_handlers();

  shm::Segment seg =
      shm::Segment::open(cfg.shm_name, shm::region_size(cfg.slots), false);
  shm::Ring ring;
  ring.attach(seg.base(), cfg.slots, false);

  metrics::Accumulator acc(cfg.count ? cfg.count : 1u << 20);

  std::vector<CsvRecord> records;
  if (!cfg.csv.empty()) {
    records.resize(cfg.count ? cfg.count : 1024);
  }

  uint64_t read_index = cfg.from_edge ? ring.live_edge() : 0;
  uint64_t received = 0;
  uint64_t skipped = 0;
  uint64_t lapped_events = 0;
  const uint64_t idle_ns = cfg.idle_ms * 1000000ull;
  uint64_t last_progress = util::steady_now_ns();

  uint8_t frame[shm::kFrameCap];
  while (!util::should_stop() && (cfg.count == 0 || received < cfg.count)) {
    uint32_t len = 0;
    uint64_t resume = 0;
    auto st = ring.read(read_index, frame, &len, &resume);

    if (st == shm::Ring::FrameStatus::kOk) {
      const uint64_t recv_ts = util::now_ns();
      if (!msg::validate_frame(frame, len)) {
        fprintf(stderr, "consumer: invalid frame at index %llu\n",
                static_cast<unsigned long long>(read_index));
        return 1;
      }
      const auto* hdr = reinterpret_cast<const msg::Header*>(frame);
      if (skipped < cfg.skip) {
        ++skipped;
        ++read_index;
        last_progress = util::steady_now_ns();
        continue;
      }
      const uint64_t latency =
          recv_ts > hdr->send_ts_ns ? recv_ts - hdr->send_ts_ns : 0;
      acc.record(hdr->seq_id, latency);
      if (!cfg.csv.empty()) {
        if (received == records.size()) {
          records.resize(records.empty() ? 1024 : records.size() * 2);
        }
        records[received] = CsvRecord{hdr->seq_id, hdr->send_ts_ns, recv_ts};
      }
      ++received;
      ++read_index;
      last_progress = util::steady_now_ns();
    } else if (st == shm::Ring::FrameStatus::kLapped) {
      ++lapped_events;
      read_index = resume;
    } else if (st == shm::Ring::FrameStatus::kCorrupt) {
      fprintf(stderr, "consumer: corrupt shared-memory frame at index %llu\n",
              static_cast<unsigned long long>(read_index));
      return 1;
    } else {
      if (util::steady_now_ns() - last_progress > idle_ns) break;
    }
  }

  if (!cfg.csv.empty()) {
    const int fd = open(cfg.csv.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd < 0) {
      perror("open");
      return 1;
    }
    const size_t bytes = static_cast<size_t>(received) * sizeof(CsvRecord);
    const ssize_t n = write(fd, records.data(), bytes);
    if (n < 0 || static_cast<size_t>(n) != bytes) {
      perror("write");
      close(fd);
      return 1;
    }
    if (close(fd) != 0) {
      perror("close");
      return 1;
    }
  }

  fprintf(stderr, "consumer: skipped %llu lapped %llu times\n",
          (unsigned long long)skipped, (unsigned long long)lapped_events);
  print_report(acc.report());
  return 0;
}
