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

#include "dedupe_window.h"
#include "fec.h"
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
  uint64_t dedupe_window = 65536;
  bool dedupe_enabled = true;
  uint32_t fec_gens = 64;
  std::string fec_recovery_csv;
  bool echo = false;
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
    else if (a == "--dedupe-window") c.dedupe_window = std::stoull(next());
    else if (a == "--dedupe-disable") c.dedupe_enabled = false;
    else if (a == "--fec-gens") c.fec_gens = static_cast<uint32_t>(std::stoul(next()));
    else if (a == "--fec-recovery-csv") c.fec_recovery_csv = next();
    else if (a == "--echo") c.echo = true;
    else {
      fprintf(stderr, "unknown arg: %s\n", a.c_str());
      std::exit(2);
    }
  }
  if (c.batch_size == 0) c.batch_size = 1;
  if (c.fec_gens == 0) c.fec_gens = 1;
  return c;
}

int open_bound_udp_socket(const Config& cfg) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_DGRAM;
  hints.ai_flags = AI_PASSIVE;

  addrinfo* result = nullptr;
  const int rc = getaddrinfo(cfg.bind_host.c_str(), cfg.port.c_str(), &hints, &result);
  if (rc != 0) {
    fprintf(stderr, "getaddrinfo(%s:%s) failed: %s\n", cfg.bind_host.c_str(), cfg.port.c_str(), gai_strerror(rc));
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

  shm::Segment seg = shm::Segment::open(cfg.out_shm, shm::region_size(cfg.slots), true);
  shm::Ring ring;
  ring.attach(seg.base(), cfg.slots, true);

  const int sock = open_bound_udp_socket(cfg);

  fprintf(stderr, "receiver: out_shm=%s slots=%u bind=%s:%s count=%llu rcvbuf=%d batch_size=%u dedupe=%s dedupe_window=%llu fec_gens=%u echo=%s\n",
          cfg.out_shm.c_str(), cfg.slots, cfg.bind_host.c_str(), cfg.port.c_str(), static_cast<unsigned long long>(cfg.count),
          cfg.rcvbuf, cfg.batch_size, cfg.dedupe_enabled ? "true" : "false", static_cast<unsigned long long>(cfg.dedupe_window), cfg.fec_gens,
          cfg.echo ? "true" : "false");

  uint64_t received = 0;
  uint64_t published = 0;
  uint64_t echoed = 0;
  uint64_t old_duplicates = 0;
  uint64_t old_last_seq = 0;
  uint64_t rejected = 0;
  uint64_t fec_late_recovered_too_old = 0;
  const uint64_t idle_ns = cfg.idle_ms * 1000000ull;
  uint64_t last_progress = util::now_ns();

  dedupe::Window dedupe_window(cfg.dedupe_window);
  fec::Decoder decoder(cfg.fec_gens);
  const uint32_t datagram_cap = shm::kFrameCap + sizeof(fec::Envelope) + sizeof(uint16_t);
  std::vector<std::vector<uint8_t>> frames(cfg.batch_size, std::vector<uint8_t>(datagram_cap));
  std::vector<iovec> iovecs(cfg.batch_size);
  std::vector<mmsghdr> messages(cfg.batch_size);
  std::vector<sockaddr_storage> peers(cfg.batch_size);
  for (uint32_t i = 0; i < cfg.batch_size; ++i) {
    iovecs[i].iov_base = frames[i].data();
    iovecs[i].iov_len = frames[i].size();
    std::memset(&messages[i], 0, sizeof(messages[i]));
    messages[i].msg_hdr.msg_iov = &iovecs[i];
    messages[i].msg_hdr.msg_iovlen = 1;
    messages[i].msg_hdr.msg_name = &peers[i];
    messages[i].msg_hdr.msg_namelen = sizeof(peers[i]);
  }

  auto publish_frame = [&](const uint8_t* frame, uint32_t len, bool recovered, uint8_t) -> bool {
    if (len < sizeof(msg::Header) || len > shm::kFrameCap) {
      ++rejected;
      return false;
    }
    const auto* hdr = reinterpret_cast<const msg::Header*>(frame);
    if (!cfg.dedupe_enabled) {
      if (hdr->seq_id > old_last_seq) {
        ring.publish(frame, len);
        old_last_seq = hdr->seq_id;
        ++published;
      } else {
        ++old_duplicates;
      }
      return true;
    }
    const dedupe::ObserveResult result = dedupe_window.observe(hdr->seq_id);
    if (recovered && result == dedupe::ObserveResult::kTooOld) ++fec_late_recovered_too_old;
    if (result == dedupe::ObserveResult::kAccept || result == dedupe::ObserveResult::kAdvanced) {
      ring.publish(frame, len);
      ++published;
    }
    return true;
  };

  while (cfg.count == 0 || (cfg.echo ? echoed : published) < cfg.count) {
    uint32_t batch_count = cfg.batch_size;
    for (uint32_t i = 0; i < batch_count; ++i) {
      iovecs[i].iov_len = frames[i].size();
      messages[i].msg_hdr.msg_namelen = sizeof(peers[i]);
      messages[i].msg_len = 0;
    }
    const int n = recvmmsg(sock, messages.data(), batch_count, 0, nullptr);
    if (n > 0) {
      if (cfg.echo) {
        for (int i = 0; i < n; ++i) iovecs[i].iov_len = messages[i].msg_len;
        int offset = 0;
        while (offset < n) {
          const int sent_now = sendmmsg(sock, messages.data() + offset, static_cast<unsigned int>(n - offset), 0);
          if (sent_now <= 0) {
            perror("sendmmsg");
            close(sock);
            return 1;
          }
          offset += sent_now;
        }
        received += static_cast<uint64_t>(n);
        echoed += static_cast<uint64_t>(n);
      } else {
        for (int i = 0; i < n; ++i) {
          ++received;
          if (!decoder.receive(frames[i].data(), messages[i].msg_len, util::now_ns(), publish_frame)) ++rejected;
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

  decoder.retire_all();
  const dedupe::Counters& dc = dedupe_window.counters();
  const fec::Counters& fc = decoder.counters();
  const fec::RecoveryStats rs = decoder.recovery_stats();
  const fec::SplitRecoveryStats split_rs = decoder.split_recovery_stats();
  if (!cfg.fec_recovery_csv.empty()) {
    FILE* recovery_file = fopen(cfg.fec_recovery_csv.c_str(), "w");
    if (recovery_file == nullptr) {
      perror("fopen");
      return 1;
    }
    fprintf(recovery_file, "recovery_latency_ns\n");
    for (uint64_t value : decoder.recovery_latencies()) fprintf(recovery_file, "%llu\n", static_cast<unsigned long long>(value));
    fclose(recovery_file);
  }
  fprintf(stderr, "receiver: received=%llu published=%llu echoed=%llu rejected=%llu accepted=%llu duplicates=%llu too_old=%llu lost_confirmed=%llu window_slides=%llu max_reorder_depth=%llu receiver_seq_jump_raw=%llu old_duplicates=%llu fec_parity_received=%llu fec_recovered=%llu fec_late_recovered_too_old=%llu fec_unrecoverable_gens=%llu fec_recovered_by_k=%llu fec_recovered_by_timeout=%llu fec_recovered_by_flush=%llu fec_recovery_p50_ns=%llu fec_recovery_p99_ns=%llu fec_recovery_p999_ns=%llu fec_recovery_p9999_ns=%llu fec_recovery_max_ns=%llu fec_recovery_by_k_p50_ns=%llu fec_recovery_by_k_p99_ns=%llu fec_recovery_by_k_p999_ns=%llu fec_recovery_by_k_p9999_ns=%llu fec_recovery_by_k_max_ns=%llu fec_recovery_by_timeout_p50_ns=%llu fec_recovery_by_timeout_p99_ns=%llu fec_recovery_by_timeout_p999_ns=%llu fec_recovery_by_timeout_p9999_ns=%llu fec_recovery_by_timeout_max_ns=%llu fec_recovery_by_flush_p50_ns=%llu fec_recovery_by_flush_p99_ns=%llu fec_recovery_by_flush_p999_ns=%llu fec_recovery_by_flush_p9999_ns=%llu fec_recovery_by_flush_max_ns=%llu\n",
          static_cast<unsigned long long>(received), static_cast<unsigned long long>(published), static_cast<unsigned long long>(echoed), static_cast<unsigned long long>(rejected),
          static_cast<unsigned long long>(cfg.dedupe_enabled ? dc.accepted : published),
          static_cast<unsigned long long>(cfg.dedupe_enabled ? dc.duplicates : old_duplicates),
          static_cast<unsigned long long>(cfg.dedupe_enabled ? dc.too_old : 0),
          static_cast<unsigned long long>(cfg.dedupe_enabled ? dc.lost_confirmed : 0),
          static_cast<unsigned long long>(cfg.dedupe_enabled ? dc.window_slides : 0),
          static_cast<unsigned long long>(cfg.dedupe_enabled ? dc.max_reorder_depth : 0),
          static_cast<unsigned long long>(cfg.dedupe_enabled ? dc.receiver_seq_jump_raw : 0),
          static_cast<unsigned long long>(old_duplicates),
          static_cast<unsigned long long>(fc.parity_received), static_cast<unsigned long long>(fc.recovered),
          static_cast<unsigned long long>(fec_late_recovered_too_old), static_cast<unsigned long long>(fc.unrecoverable_gens),
          static_cast<unsigned long long>(fc.recovered_by_k), static_cast<unsigned long long>(fc.recovered_by_timeout),
          static_cast<unsigned long long>(fc.recovered_by_flush), static_cast<unsigned long long>(rs.p50),
          static_cast<unsigned long long>(rs.p99), static_cast<unsigned long long>(rs.p999),
          static_cast<unsigned long long>(rs.p9999), static_cast<unsigned long long>(rs.max),
          static_cast<unsigned long long>(split_rs.by_k.p50), static_cast<unsigned long long>(split_rs.by_k.p99),
          static_cast<unsigned long long>(split_rs.by_k.p999), static_cast<unsigned long long>(split_rs.by_k.p9999),
          static_cast<unsigned long long>(split_rs.by_k.max), static_cast<unsigned long long>(split_rs.by_timeout.p50),
          static_cast<unsigned long long>(split_rs.by_timeout.p99), static_cast<unsigned long long>(split_rs.by_timeout.p999),
          static_cast<unsigned long long>(split_rs.by_timeout.p9999), static_cast<unsigned long long>(split_rs.by_timeout.max),
          static_cast<unsigned long long>(split_rs.by_flush.p50), static_cast<unsigned long long>(split_rs.by_flush.p99),
          static_cast<unsigned long long>(split_rs.by_flush.p999), static_cast<unsigned long long>(split_rs.by_flush.p9999),
          static_cast<unsigned long long>(split_rs.by_flush.max));
  close(sock);
  seg.unlink();
  return 0;
}
