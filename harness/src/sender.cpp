#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "shm_ring.h"
#include "shm_segment.h"
#include "util.h"

namespace {

struct Config {
  std::string in_shm = "/fanout_ring";
  uint32_t slots = 1024;
  std::string host = "127.0.0.1";
  std::string port = "9000";
  uint64_t count = 0;
  bool from_edge = false;
  uint64_t idle_ms = 2000;
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
    else if (a == "--slots") c.slots = static_cast<uint32_t>(std::stoul(next()));
    else if (a == "--host") c.host = next();
    else if (a == "--port") c.port = next();
    else if (a == "--count") c.count = std::stoull(next());
    else if (a == "--from-edge") c.from_edge = true;
    else if (a == "--idle-ms") c.idle_ms = std::stoull(next());
    else {
      fprintf(stderr, "unknown arg: %s\n", a.c_str());
      std::exit(2);
    }
  }
  return c;
}

int open_udp_socket(const Config& cfg, sockaddr_storage* dst,
                    socklen_t* dst_len) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_DGRAM;

  addrinfo* result = nullptr;
  const int rc = getaddrinfo(cfg.host.c_str(), cfg.port.c_str(), &hints, &result);
  if (rc != 0) {
    fprintf(stderr, "getaddrinfo(%s:%s) failed: %s\n", cfg.host.c_str(),
            cfg.port.c_str(), gai_strerror(rc));
    std::exit(1);
  }

  int fd = -1;
  for (addrinfo* p = result; p; p = p->ai_next) {
    fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
    if (fd < 0) continue;
    std::memcpy(dst, p->ai_addr, p->ai_addrlen);
    *dst_len = static_cast<socklen_t>(p->ai_addrlen);
    break;
  }
  freeaddrinfo(result);

  if (fd < 0) {
    perror("socket");
    std::exit(1);
  }
  return fd;
}

}

int main(int argc, char** argv) {
  Config cfg = parse_args(argc, argv);

  shm::Segment seg =
      shm::Segment::open(cfg.in_shm, shm::region_size(cfg.slots), false);
  shm::Ring ring;
  ring.attach(seg.base(), cfg.slots, false);

  sockaddr_storage dst{};
  socklen_t dst_len = 0;
  const int sock = open_udp_socket(cfg, &dst, &dst_len);

  fprintf(stderr,
          "sender: in_shm=%s slots=%u dst=%s:%s count=%llu from_edge=%s\n",
          cfg.in_shm.c_str(), cfg.slots, cfg.host.c_str(), cfg.port.c_str(),
          static_cast<unsigned long long>(cfg.count),
          cfg.from_edge ? "true" : "false");

  uint64_t read_index = cfg.from_edge ? ring.live_edge() : 0;
  uint64_t sent = 0;
  uint64_t lapped_events = 0;
  const uint64_t idle_ns = cfg.idle_ms * 1000000ull;
  uint64_t last_progress = util::now_ns();

  uint8_t frame[shm::kFrameCap];
  while (cfg.count == 0 || sent < cfg.count) {
    uint32_t len = 0;
    uint64_t resume = 0;
    auto st = ring.read(read_index, frame, &len, &resume);

    if (st == shm::Ring::FrameStatus::kOk) {
      const ssize_t n =
          sendto(sock, frame, len, 0, reinterpret_cast<sockaddr*>(&dst), dst_len);
      if (n < 0) {
        perror("sendto");
        close(sock);
        return 1;
      }
      if (static_cast<uint32_t>(n) != len) {
        fprintf(stderr, "short UDP send: %zd of %u bytes\n", n, len);
        close(sock);
        return 1;
      }
      ++sent;
      ++read_index;
      last_progress = util::now_ns();
    } else if (st == shm::Ring::FrameStatus::kLapped) {
      ++lapped_events;
      read_index = resume;
    } else {
      if (util::now_ns() - last_progress > idle_ns) break;
    }
  }

  fprintf(stderr, "sender: sent=%llu lapped=%llu\n",
          static_cast<unsigned long long>(sent),
          static_cast<unsigned long long>(lapped_events));
  close(sock);
  return 0;
}
