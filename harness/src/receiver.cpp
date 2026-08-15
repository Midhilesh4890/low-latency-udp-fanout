#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "message.h"
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
  int rcvbuf = 4 * 1024 * 1024;
  uint32_t batch_size = 32;
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
    else if (a == "--rcvbuf") c.rcvbuf = std::stoi(next());
    else if (a == "--batch-size") c.batch_size = static_cast<uint32_t>(std::stoul(next()));
    else {
      fprintf(stderr, "unknown arg: %s\n", a.c_str());
      std::exit(2);
    }
  }
  if (c.batch_size == 0) c.batch_size = 1;
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
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &cfg.rcvbuf, sizeof(cfg.rcvbuf));
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

  fprintf(stderr, "receiver: out_shm=%s slots=%u bind=%s:%s count=%llu rcvbuf=%d batch_size=%u\n",
          cfg.out_shm.c_str(), cfg.slots, cfg.bind_host.c_str(),
          cfg.port.c_str(), static_cast<unsigned long long>(cfg.count),
          cfg.rcvbuf, cfg.batch_size);

  uint64_t received = 0;
  uint64_t duplicates = 0;
  uint64_t published = 0;
  uint64_t last_seq = 0;
  const uint64_t idle_ns = cfg.idle_ms * 1000000ull;
  uint64_t last_progress = util::now_ns();

  std::vector<std::vector<uint8_t>> frames(cfg.batch_size, std::vector<uint8_t>(shm::kFrameCap));
  std::vector<iovec> iovecs(cfg.batch_size);
  std::vector<mmsghdr> messages(cfg.batch_size);
  for (uint32_t i = 0; i < cfg.batch_size; ++i) {
    iovecs[i].iov_base = frames[i].data();
    iovecs[i].iov_len = frames[i].size();
    std::memset(&messages[i], 0, sizeof(messages[i]));
    messages[i].msg_hdr.msg_iov = &iovecs[i];
    messages[i].msg_hdr.msg_iovlen = 1;
  }

  while (cfg.count == 0 || published < cfg.count) {
    uint32_t batch_count = cfg.batch_size;
    if (cfg.count != 0) {
      const uint64_t remaining = cfg.count - published;
      if (remaining < batch_count) batch_count = static_cast<uint32_t>(remaining);
    }
    for (uint32_t i = 0; i < batch_count; ++i) messages[i].msg_len = 0;
    const int n = recvmmsg(sock, messages.data(), batch_count, 0, nullptr);
    if (n > 0) {
      for (int i = 0; i < n; ++i) {
        ++received;
        const auto* hdr = reinterpret_cast<const msg::Header*>(frames[i].data());
        if (hdr->seq_id > last_seq) {
          ring.publish(frames[i].data(), messages[i].msg_len);
          last_seq = hdr->seq_id;
          ++published;
        } else {
          ++duplicates;
        }
      }
      last_progress = util::now_ns();
    } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      if (util::now_ns() - last_progress > idle_ns) break;
    } else if (n < 0) {
      perror("recvmmsg");
      close(sock);
      return 1;
    }
  }

  fprintf(stderr, "receiver: received=%llu published=%llu duplicates=%llu\n",
          static_cast<unsigned long long>(received),
          static_cast<unsigned long long>(published),
          static_cast<unsigned long long>(duplicates));
  close(sock);
  seg.unlink();
  return 0;
}
