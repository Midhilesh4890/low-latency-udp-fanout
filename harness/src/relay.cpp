#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "shm_ring.h"
#include "shm_segment.h"
#include "util.h"

namespace {

struct Config {
  std::string in_shm;
  std::string out_shm;
  uint32_t slots = 1024;
  uint64_t count = 0;
  uint64_t idle_ms = 2000;
  bool from_edge = false;
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
    if (a == "--in-shm") c.in_shm = next();
    else if (a == "--out-shm") c.out_shm = next();
    else if (a == "--slots") c.slots = static_cast<uint32_t>(std::stoul(next()));
    else if (a == "--count") c.count = std::stoull(next());
    else if (a == "--idle-ms") c.idle_ms = std::stoull(next());
    else if (a == "--from-edge") c.from_edge = true;
    else {
      fprintf(stderr, "unknown arg: %s\n", a.c_str());
      std::exit(2);
    }
  }
  if (c.in_shm.empty() || c.out_shm.empty()) {
    fprintf(stderr, "--in-shm and --out-shm are required\n");
    std::exit(2);
  }
  if (c.in_shm == c.out_shm) {
    fprintf(stderr, "--in-shm and --out-shm must differ\n");
    std::exit(2);
  }
  return c;
}

}

int main(int argc, char** argv) {
  const Config cfg = parse_args(argc, argv);
  shm::Segment in_segment = shm::Segment::open(cfg.in_shm, shm::region_size(cfg.slots), false);
  shm::Ring in_ring;
  in_ring.attach(in_segment.base(), cfg.slots, false);
  shm::Segment out_segment = shm::Segment::open(cfg.out_shm, shm::region_size(cfg.slots), true);
  shm::Ring out_ring;
  out_ring.attach(out_segment.base(), cfg.slots, true);

  uint64_t read_index = cfg.from_edge ? in_ring.live_edge() : 0;
  uint64_t forwarded = 0;
  uint64_t lapped = 0;
  const uint64_t idle_ns = cfg.idle_ms * 1000000ull;
  uint64_t last_progress = util::now_ns();
  uint8_t frame[shm::kFrameCap];

  while (cfg.count == 0 || forwarded < cfg.count) {
    uint32_t len = 0;
    uint64_t resume = 0;
    const auto status = in_ring.read(read_index, frame, &len, &resume);
    if (status == shm::Ring::FrameStatus::kOk) {
      out_ring.publish(frame, len);
      ++read_index;
      ++forwarded;
      last_progress = util::now_ns();
    } else if (status == shm::Ring::FrameStatus::kLapped) {
      ++lapped;
      read_index = resume;
    } else if (util::now_ns() - last_progress > idle_ns) {
      break;
    }
  }

  fprintf(stderr, "relay: in_shm=%s out_shm=%s forwarded=%llu lapped=%llu\n",
          cfg.in_shm.c_str(), cfg.out_shm.c_str(),
          static_cast<unsigned long long>(forwarded),
          static_cast<unsigned long long>(lapped));
  out_segment.unlink();
  return 0;
}
