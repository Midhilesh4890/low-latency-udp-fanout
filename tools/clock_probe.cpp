#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "harness/include/util.h"

struct Sample {
  int64_t rtt;
  int64_t offset;
};

struct Packet {
  uint64_t seq;
  uint64_t t1;
  uint64_t t2;
  uint64_t t3;
};

uint64_t percentile(std::vector<int64_t> values, double p) {
  if (values.empty()) return 0;
  std::sort(values.begin(), values.end());
  size_t rank = static_cast<size_t>(p * static_cast<double>(values.size()));
  if (static_cast<double>(rank) < p * static_cast<double>(values.size())) ++rank;
  if (rank < 1) rank = 1;
  if (rank > values.size()) rank = values.size();
  return static_cast<uint64_t>(values[rank - 1]);
}

int bind_socket(const char* host, const char* port) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_DGRAM;
  hints.ai_flags = AI_PASSIVE;
  addrinfo* result = nullptr;
  int rc = getaddrinfo(host, port, &hints, &result);
  if (rc != 0) std::exit(2);
  int fd = -1;
  for (addrinfo* p = result; p; p = p->ai_next) {
    fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
    if (fd < 0) continue;
    if (bind(fd, p->ai_addr, p->ai_addrlen) == 0) break;
    close(fd);
    fd = -1;
  }
  freeaddrinfo(result);
  if (fd < 0) std::exit(1);
  return fd;
}

int connect_socket(const char* host, const char* port) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_DGRAM;
  addrinfo* result = nullptr;
  int rc = getaddrinfo(host, port, &hints, &result);
  if (rc != 0) std::exit(2);
  int fd = -1;
  for (addrinfo* p = result; p; p = p->ai_next) {
    fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
    if (fd < 0) continue;
    if (connect(fd, p->ai_addr, p->ai_addrlen) == 0) break;
    close(fd);
    fd = -1;
  }
  freeaddrinfo(result);
  if (fd < 0) std::exit(1);
  return fd;
}

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: clock_probe server PORT | client HOST PORT COUNT\n");
    return 2;
  }
  std::string mode = argv[1];
  if (mode == "server") {
    int fd = bind_socket("0.0.0.0", argv[2]);
    for (;;) {
      Packet packet{};
      sockaddr_storage peer{};
      socklen_t peer_len = sizeof(peer);
      ssize_t n = recvfrom(fd, &packet, sizeof(packet), 0, reinterpret_cast<sockaddr*>(&peer), &peer_len);
      if (n != static_cast<ssize_t>(sizeof(packet))) continue;
      packet.t2 = util::now_ns();
      packet.t3 = util::now_ns();
      sendto(fd, &packet, sizeof(packet), 0, reinterpret_cast<sockaddr*>(&peer), peer_len);
    }
  }
  if (argc < 5) return 2;
  int fd = connect_socket(argv[2], argv[3]);
  uint64_t count = std::strtoull(argv[4], nullptr, 10);
  std::vector<Sample> samples;
  samples.reserve(count);
  for (uint64_t i = 0; i < count; ++i) {
    Packet packet{i + 1, util::now_ns(), 0, 0};
    if (send(fd, &packet, sizeof(packet), 0) != static_cast<ssize_t>(sizeof(packet))) continue;
    Packet reply{};
    ssize_t n = recv(fd, &reply, sizeof(reply), 0);
    uint64_t t4 = util::now_ns();
    if (n != static_cast<ssize_t>(sizeof(reply)) || reply.seq != packet.seq) continue;
    int64_t rtt = static_cast<int64_t>((t4 - reply.t1) - (reply.t3 - reply.t2));
    int64_t offset = (static_cast<int64_t>(reply.t2 - reply.t1) + static_cast<int64_t>(reply.t3 - t4)) / 2;
    samples.push_back(Sample{rtt, offset});
  }
  std::vector<int64_t> rtts;
  rtts.reserve(samples.size());
  for (const auto& sample : samples) rtts.push_back(sample.rtt);
  auto best = std::min_element(samples.begin(), samples.end(), [](const Sample& a, const Sample& b) { return a.rtt < b.rtt; });
  std::printf("samples=%zu rtt_min_ns=%llu rtt_p50_ns=%llu rtt_p99_ns=%llu min_rtt_offset_ns=%lld\n", samples.size(), static_cast<unsigned long long>(percentile(rtts, 0.0)), static_cast<unsigned long long>(percentile(rtts, 0.50)), static_cast<unsigned long long>(percentile(rtts, 0.99)), best == samples.end() ? 0 : static_cast<long long>(best->offset));
  return 0;
}