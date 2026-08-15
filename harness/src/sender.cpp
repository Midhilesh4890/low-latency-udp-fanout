#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

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
  uint32_t batch_size = 32;
  uint64_t batch_timeout_us = 50;
};

struct Batch {
  explicit Batch(uint32_t capacity)
      : frames(capacity, std::vector<uint8_t>(shm::kFrameCap)),
        iovecs(capacity),
        messages(capacity),
        lengths(capacity) {}

  std::vector<std::vector<uint8_t>> frames;
  std::vector<iovec> iovecs;
  std::vector<mmsghdr> messages;
  std::vector<uint32_t> lengths;
  uint32_t size = 0;
  uint64_t first_ns = 0;
};

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
    else if (a == "--batch-size") c.batch_size = static_cast<uint32_t>(std::stoul(next()));
    else if (a == "--batch-timeout-us") c.batch_timeout_us = std::stoull(next());
    else {
      fprintf(stderr, "unknown arg: %s\n", a.c_str());
      std::exit(2);
    }
  }
  if (c.repeat == 0) c.repeat = 1;
  if (c.batch_size == 0) c.batch_size = 1;
  if (c.batch_timeout_us == 0) c.batch_timeout_us = 1;
  if (c.destinations.empty()) {
    c.destinations.push_back(Destination{c.host, c.port});
  }
  return c;
}

int open_udp_socket(const Destination& dst, int sndbuf) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_DGRAM;

  addrinfo* result = nullptr;
  const int rc = getaddrinfo(dst.host.c_str(), dst.port.c_str(), &hints, &result);
  if (rc != 0) {
    fprintf(stderr, "getaddrinfo(%s:%s) failed: %s\n", dst.host.c_str(),
            dst.port.c_str(), gai_strerror(rc));
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

void queue_frame(Batch& batch, const uint8_t* frame, uint32_t len) {
  if (batch.size == 0) batch.first_ns = util::now_ns();
  std::memcpy(batch.frames[batch.size].data(), frame, len);
  batch.lengths[batch.size] = len;
  ++batch.size;
}

bool flush_batch(const Config& cfg, const std::vector<int>& sockets, Batch& batch, uint64_t& packets) {
  if (batch.size == 0) return true;
  for (uint32_t i = 0; i < batch.size; ++i) {
    batch.iovecs[i].iov_base = batch.frames[i].data();
    batch.iovecs[i].iov_len = batch.lengths[i];
    std::memset(&batch.messages[i], 0, sizeof(batch.messages[i]));
    batch.messages[i].msg_hdr.msg_iov = &batch.iovecs[i];
    batch.messages[i].msg_hdr.msg_iovlen = 1;
  }
  for (int sock : sockets) {
    for (uint32_t repeat = 0; repeat < cfg.repeat; ++repeat) {
      uint32_t offset = 0;
      while (offset < batch.size) {
        for (uint32_t i = offset; i < batch.size; ++i) batch.messages[i].msg_len = 0;
        const int n = sendmmsg(sock, batch.messages.data() + offset, batch.size - offset, 0);
        if (n < 0) {
          perror("sendmmsg");
          return false;
        }
        if (n == 0) {
          fprintf(stderr, "sendmmsg returned zero\n");
          return false;
        }
        for (int i = 0; i < n; ++i) {
          if (batch.messages[offset + i].msg_len != batch.lengths[offset + i]) {
            fprintf(stderr, "short UDP send: %u of %u bytes\n",
                    batch.messages[offset + i].msg_len, batch.lengths[offset + i]);
            return false;
          }
        }
        packets += static_cast<uint64_t>(n);
        offset += static_cast<uint32_t>(n);
      }
    }
  }
  batch.size = 0;
  batch.first_ns = 0;
  return true;
}

}

int main(int argc, char** argv) {
  Config cfg = parse_args(argc, argv);

  shm::Segment seg =
      shm::Segment::open(cfg.in_shm, shm::region_size(cfg.slots), false);
  shm::Ring ring;
  ring.attach(seg.base(), cfg.slots, false);

  std::vector<int> sockets;
  sockets.reserve(cfg.destinations.size());
  for (const auto& dst : cfg.destinations) {
    sockets.push_back(open_udp_socket(dst, cfg.sndbuf));
  }

  fprintf(stderr,
          "sender: in_shm=%s slots=%u targets=%zu count=%llu from_edge=%s sndbuf=%d repeat=%u batch_size=%u batch_timeout_us=%llu\n",
          cfg.in_shm.c_str(), cfg.slots, sockets.size(),
          static_cast<unsigned long long>(cfg.count),
          cfg.from_edge ? "true" : "false", cfg.sndbuf, cfg.repeat,
          cfg.batch_size, static_cast<unsigned long long>(cfg.batch_timeout_us));

  uint64_t read_index = cfg.from_edge ? ring.live_edge() : 0;
  uint64_t sent = 0;
  uint64_t packets = 0;
  uint64_t lapped_events = 0;
  const uint64_t idle_ns = cfg.idle_ms * 1000000ull;
  const uint64_t batch_timeout_ns = cfg.batch_timeout_us * 1000ull;
  uint64_t last_progress = util::now_ns();

  Batch batch(cfg.batch_size);
  uint8_t frame[shm::kFrameCap];
  while (cfg.count == 0 || sent < cfg.count) {
    if (batch.size > 0 && util::now_ns() - batch.first_ns >= batch_timeout_ns) {
      if (!flush_batch(cfg, sockets, batch, packets)) {
        close_sockets(sockets);
        return 1;
      }
    }

    uint32_t len = 0;
    uint64_t resume = 0;
    auto st = ring.read(read_index, frame, &len, &resume);

    if (st == shm::Ring::FrameStatus::kOk) {
      queue_frame(batch, frame, len);
      ++sent;
      ++read_index;
      last_progress = util::now_ns();
      if (batch.size == cfg.batch_size) {
        if (!flush_batch(cfg, sockets, batch, packets)) {
          close_sockets(sockets);
          return 1;
        }
      }
    } else if (st == shm::Ring::FrameStatus::kLapped) {
      ++lapped_events;
      read_index = resume;
    } else {
      if (batch.size > 0) {
        if (!flush_batch(cfg, sockets, batch, packets)) {
          close_sockets(sockets);
          return 1;
        }
      }
      if (util::now_ns() - last_progress > idle_ns) break;
    }
  }

  if (!flush_batch(cfg, sockets, batch, packets)) {
    close_sockets(sockets);
    return 1;
  }

  fprintf(stderr, "sender: sent=%llu packets=%llu lapped=%llu\n",
          static_cast<unsigned long long>(sent),
          static_cast<unsigned long long>(packets),
          static_cast<unsigned long long>(lapped_events));
  close_sockets(sockets);
  return 0;
}
