#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
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
  std::string out_shm = "/fanout_ring_out";
  uint32_t slots = 1024;
  std::string bind_host = "0.0.0.0";
  std::string port = "9000";
  uint64_t count = 0;
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
    if (a == "--out-shm") c.out_shm = next();
    else if (a == "--slots") c.slots = static_cast<uint32_t>(std::stoul(next()));
    else if (a == "--bind") c.bind_host = next();
    else if (a == "--port") c.port = next();
    else if (a == "--count") c.count = std::stoull(next());
    else if (a == "--idle-ms") c.idle_ms = std::stoull(next());
    else {
      fprintf(stderr, "unknown arg: %s\n", a.c_str());
      std::exit(2);
    }
  }
  return c;
}

int open_bound_udp_socket(const Config& cfg) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_DGRAM;
  hints.ai_flags = AI_PASSIVE;

  addrinfo* result = nullptr;
  const int rc =
      getaddrinfo(cfg.bind_host.c_str(), cfg.port.c_str(), &hints, &result);
  if (rc != 0) {
    fprintf(stderr, "getaddrinfo(%s:%s) failed: %s\n", cfg.bind_host.c_str(),
            cfg.port.c_str(), gai_strerror(rc));
    std::exit(1);
  }

  int fd = -1;
  for (addrinfo* p = result; p; p = p->ai_next) {
    fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
    if (fd < 0) continue;

    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (bind(fd, p->ai_addr, p->ai_addrlen) == 0) break;

    close(fd);
    fd = -1;
  }
  freeaddrinfo(result);

  if (fd < 0) {
    perror("bind");
    std::exit(1);
  }

  int flags = fcntl(fd, F_GETFL, 0);
  if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  return fd;
}

}

int main(int argc, char** argv) {
  Config cfg = parse_args(argc, argv);

  shm::Segment seg =
      shm::Segment::open(cfg.out_shm, shm::region_size(cfg.slots), true);
  shm::Ring ring;
  ring.attach(seg.base(), cfg.slots, true);

  const int sock = open_bound_udp_socket(cfg);

  fprintf(stderr, "receiver: out_shm=%s slots=%u bind=%s:%s count=%llu\n",
          cfg.out_shm.c_str(), cfg.slots, cfg.bind_host.c_str(),
          cfg.port.c_str(), static_cast<unsigned long long>(cfg.count));

  uint64_t received = 0;
  const uint64_t idle_ns = cfg.idle_ms * 1000000ull;
  uint64_t last_progress = util::now_ns();

  uint8_t frame[shm::kFrameCap];
  while (cfg.count == 0 || received < cfg.count) {
    const ssize_t n = recv(sock, frame, sizeof(frame), 0);
    if (n > 0) {
      ring.publish(frame, static_cast<uint32_t>(n));
      ++received;
      last_progress = util::now_ns();
    } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      if (util::now_ns() - last_progress > idle_ns) break;
    } else if (n < 0) {
      perror("recv");
      close(sock);
      return 1;
    }
  }

  fprintf(stderr, "receiver: received=%llu\n",
          static_cast<unsigned long long>(received));
  close(sock);
  seg.unlink();
  return 0;
}
