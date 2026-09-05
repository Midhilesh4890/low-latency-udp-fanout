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
#include <exception>
#include <string>
#include <vector>

#include "dedupe_window.h"
#include "fec.h"
#include "message.h"
#include "wire.h"
#include "shm_ring.h"
#include "shm_segment.h"
#include "util.h"
#include "stream_transport.h"
#include "multiplex.h"
#include <sys/stat.h>

namespace {

struct Config {
  bool allow_insecure_udp=false;
  bool ack_publish=false,stream_reconnect=false;
  uint32_t max_streams=16;
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
  uint32_t wait_readers = 0;
  bool echo = false;
  std::string unix_listen;
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
    if (a == "--allow-insecure-udp") c.allow_insecure_udp=true;
    else if (a == "--ack-publish") c.ack_publish=true;
    else if (a == "--stream-reconnect") c.stream_reconnect=true;
    else if (a == "--max-streams") {auto n=std::stoul(next());if(!n || n>256) throw std::invalid_argument("max-streams must be 1..256");c.max_streams=n;}
    else if (a == "--out-shm") c.out_shm = next();
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
    else if (a == "--wait-readers") c.wait_readers=static_cast<uint32_t>(std::stoul(next()));
    else if (a == "--unix-listen") c.unix_listen=next();
    else if (a == "--echo") c.echo = true;
    else {
      fprintf(stderr, "unknown arg: %s\n", a.c_str());
      std::exit(2);
    }
  }
  if (c.batch_size == 0) c.batch_size = 1;
  if (c.batch_size > 1024) {
    fprintf(stderr, "--batch-size must be between 1 and 1024\n");
    std::exit(2);
  }
  if (!util::is_power_of_two(c.slots) || !util::is_power_of_two(c.dedupe_window) ||
      c.dedupe_window < 64 || c.dedupe_window > (1ull << 26)) {
    fprintf(stderr, "--slots and --dedupe-window must be powers of two; dedupe window must be between 64 and 67108864\n");
    std::exit(2);
  }
  if (c.fec_gens == 0) c.fec_gens = 1;
  if (c.fec_gens > 65536 || c.rcvbuf <= 0) {
    fprintf(stderr, "--fec-gens must be at most 65536 and --rcvbuf must be positive\n");
    std::exit(2);
  }
  if(c.echo && !c.unix_listen.empty()) throw std::invalid_argument("echo requires UDP");
  if(static_cast<uint64_t>(c.max_streams)*(static_cast<uint64_t>(c.fec_gens)*fec::kMaxGeneration*(wire::max_frame+32)+c.dedupe_window/8+4*metrics::kSampleLimit*sizeof(uint64_t))>512ull*1024*1024)
    throw std::invalid_argument("configured stream/FEC memory budget exceeds 512 MiB");
  if(c.unix_listen.empty() && (c.ack_publish || c.stream_reconnect)) throw std::invalid_argument("stream controls require --unix-listen");
  if(c.unix_listen.empty() && !c.allow_insecure_udp) throw std::invalid_argument("UDP requires --allow-insecure-udp");
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
    throw std::runtime_error("socket setup failed");
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
    throw std::runtime_error("socket setup failed");
  }

  int flags = fcntl(fd, F_GETFL, 0);
  if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  return fd;
}

}

int run(int argc, char** argv) {
  Config cfg;
  try {
    cfg = parse_args(argc, argv);
  } catch (const std::exception& error) {
    fprintf(stderr, "invalid argument: %s\n", error.what());
    return 2;
  }
  util::install_signal_handlers();

  shm::Segment seg = shm::Segment::open(cfg.out_shm, shm::region_size(cfg.slots), true);
  shm::Ring ring;
  ring.attach(seg.base(), cfg.slots, true);

  int sock = cfg.unix_listen.empty() ? open_bound_udp_socket(cfg) : transport::unix_socket(cfg.unix_listen,true);
  struct Listener {
    int fd=-1;std::string path;
    ~Listener() {if(fd>=0) {close(fd);::unlink(path.c_str());}}
  } listener;
  if(!cfg.unix_listen.empty()) {
    listener.fd=sock;listener.path=cfg.unix_listen;
    chmod(cfg.unix_listen.c_str(),0600);
  }
  auto accept_stream=[&] {
    pollfd event{listener.fd,POLLIN,0};
    const auto deadline=util::steady_now_ns()+cfg.idle_ms*1000000ull;
    while(!util::should_stop() && util::steady_now_ns()<deadline) {
      if(poll(&event,1,10)>0) return accept(listener.fd,nullptr,nullptr);
    }
    return -1;
  };
  seg.ready();
  const auto attach_deadline=util::steady_now_ns()+5000000000ull;
  while(!util::should_stop() && ring.reader_count()<cfg.wait_readers) {
    if(util::steady_now_ns()>attach_deadline) {fprintf(stderr,"reader attachment timed out\n");return 1;}
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  if(listener.fd>=0) {
    sock=accept_stream();
    if(sock<0) {fprintf(stderr,"stream accept timed out\n");return 1;}
  }

  fprintf(stderr, "receiver: out_shm=%s slots=%u bind=%s:%s count=%llu rcvbuf=%d batch_size=%u dedupe=%s dedupe_window=%llu fec_gens=%u echo=%s\n",
          cfg.out_shm.c_str(), cfg.slots, cfg.bind_host.c_str(), cfg.port.c_str(), static_cast<unsigned long long>(cfg.count),
          cfg.rcvbuf, cfg.batch_size, cfg.dedupe_enabled ? "true" : "false", static_cast<unsigned long long>(cfg.dedupe_window), cfg.fec_gens,
          cfg.echo ? "true" : "false");

  uint64_t received = 0;
  uint64_t published = 0;
  uint64_t echoed = 0;
  uint64_t old_duplicates = 0;
  uint64_t rejected = 0;
  uint64_t fec_late_recovered_too_old = 0;
  const uint64_t idle_ns = cfg.idle_ms * 1000000ull;
  uint64_t last_progress = util::steady_now_ns();

  multiplex::Streams streams(cfg.max_streams,cfg.fec_gens,cfg.dedupe_window);
  multiplex::Stream* current_stream=nullptr;
  multiplex::Key current_key;
  const uint32_t datagram_cap = wire::max_frame + fec::kEnvelopeSize + sizeof(uint16_t) + multiplex::header_size;
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
    alignas(64) uint8_t native[shm::kFrameCap]; uint32_t native_len=0;
    if (!wire::decode(frame, len, native, native_len)) {
      return false;
    }
    frame=native; len=native_len;
    msg::Header header{}; std::memcpy(&header, frame, sizeof(header));
    const auto* hdr = &header;
    if(header.stream_id!=current_key.stream || header.stream_epoch!=current_key.epoch) return false;
    if (!cfg.dedupe_enabled) {
      if (hdr->seq_id > current_stream->last_seq) {
        ring.publish(frame, len);
        current_stream->last_seq = hdr->seq_id;
        ++published;
      } else {
        ++old_duplicates;
      }
      return true;
    }
    const dedupe::ObserveResult result = current_stream->dedupe.observe(hdr->seq_id);
    if (recovered && result == dedupe::ObserveResult::kTooOld) ++fec_late_recovered_too_old;
    if (result == dedupe::ObserveResult::kAccept || result == dedupe::ObserveResult::kAdvanced) {
      ring.publish(frame, len);
      ++published;
    }
    return true;
  };

  while (!util::should_stop() && (cfg.count == 0 || (cfg.echo ? echoed : published) < cfg.count)) {
    for(auto& item:streams.values()) item.second->decoder.expire(util::steady_now_ns());
    uint32_t batch_count = cfg.batch_size;
    for (uint32_t i = 0; i < batch_count; ++i) {
      iovecs[i].iov_len = frames[i].size();
      messages[i].msg_hdr.msg_namelen = sizeof(peers[i]);
      messages[i].msg_len = 0;
    }
    int n;
    if(cfg.unix_listen.empty()) n=recvmmsg(sock,messages.data(),batch_count,0,nullptr);
    else {
      int len=transport::receive_frame(sock,frames[0].data(),frames[0].size(),idle_ns);
      if(len<0) {
        if(cfg.stream_reconnect && !util::should_stop()) {
          close(sock);sock=accept_stream();if(sock>=0) continue;
        }
        if(cfg.count && published<cfg.count) {fprintf(stderr,"stream closed before expected count\n");close(sock);return 1;}
        break;
      }
      messages[0].msg_len=static_cast<uint32_t>(len);
      messages[0].msg_hdr.msg_flags=0; n=1;
    }
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
          if (messages[i].msg_hdr.msg_flags & MSG_TRUNC) { ++rejected; continue; }
          const uint8_t* payload=frames[i].data();uint32_t length=messages[i].msg_len;
          if(!multiplex::unwrap(payload,length,current_key) || !(current_stream=streams.get(current_key))) {++rejected;continue;}
          const bool accepted=current_stream->decoder.receive(payload,length,util::steady_now_ns(),publish_frame);
          if(!accepted) ++rejected;
          if(cfg.ack_publish) {
            uint8_t ack=accepted?1:0;
            if(!transport::transfer(sock,&ack,1,true,util::steady_now_ns()+2000000000ull)) {
              if(!cfg.stream_reconnect) {close(sock);return 1;}
            }
          }
        }
      }
      last_progress = util::steady_now_ns();
    } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      if (util::steady_now_ns() - last_progress > idle_ns) break;
      util::idle_wait(last_progress);
    } else if (n < 0) {
      perror("recvmmsg");
      close(sock);
      return 1;
    }
  }

  dedupe::Counters dc{};
  fec::Counters fc{};
  std::vector<uint64_t> samples;
  uint64_t sample_count=0;
  std::vector<uint64_t> split_samples[3];uint64_t split_counts[3]{};
  for(auto& item:streams.values()) {
    auto& stream=*item.second;stream.decoder.retire_all();
    const auto& d=stream.dedupe.counters();const auto& f=stream.decoder.counters();
    dc.accepted+=d.accepted;dc.duplicates+=d.duplicates;dc.too_old+=d.too_old;
    dc.lost_confirmed+=d.lost_confirmed;dc.window_slides+=d.window_slides;
    dc.max_reorder_depth=std::max(dc.max_reorder_depth,d.max_reorder_depth);
    dc.receiver_seq_jump_raw+=d.receiver_seq_jump_raw;
    fc.parity_received+=f.parity_received;fc.recovered+=f.recovered;
    fc.unrecoverable_gens+=f.unrecoverable_gens;
    fc.recovered_by_k+=f.recovered_by_k;fc.recovered_by_timeout+=f.recovered_by_timeout;
    fc.recovered_by_flush+=f.recovered_by_flush;
    for(auto sample:stream.decoder.recovery_latencies()) metrics::sample(samples,sample,++sample_count);
    for(uint8_t reason=0;reason<3;++reason) for(auto sample:stream.decoder.samples_for(reason))
      metrics::sample(split_samples[reason],sample,++split_counts[reason]);
  }
  const fec::RecoveryStats rs = fec::Decoder::summarize(samples);
  const fec::SplitRecoveryStats split_rs{fec::Decoder::summarize(split_samples[0]),fec::Decoder::summarize(split_samples[1]),fec::Decoder::summarize(split_samples[2])};
  if(!cfg.fec_recovery_csv.empty()) {
    FILE* file=fopen(cfg.fec_recovery_csv.c_str(),"w");
    if(!file) {perror("recovery CSV");return 1;}
    fprintf(file,"sampled_recovery_latency_ns\n");
    for(auto sample:samples) fprintf(file,"%llu\n",static_cast<unsigned long long>(sample));
    if(fclose(file)) return 1;
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
  if(cfg.count && (cfg.echo ? echoed : published) < cfg.count) { fprintf(stderr,"incomplete counted run\n"); return 1; }
  return 0;
}

int main(int argc,char** argv) {
  try {return run(argc,argv);}
  catch(const std::exception& error) {fprintf(stderr,"receiver: %s\n",error.what());return 1;}
}
