#include <algorithm>
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#include <cstdint>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include "fec.h"
#include "message.h"
#include "wire.h"
#include "shm_ring.h"
#include "shm_segment.h"
#include "util.h"
#include <deque>
#include <condition_variable>
#include <mutex>
#include <thread>
#include "stream_transport.h"
#include "multiplex.h"
#include <sys/random.h>
#include <memory>

namespace {

struct Destination {
  std::string host;
  std::string port;
};

struct Config {
  uint64_t stream_id=0,stream_epoch=0;
  bool allow_insecure_udp=false;
  bool durable_acks=false;
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
  uint32_t send_workers = 256;
  size_t queue_bytes=4*1024*1024;
  uint32_t batch_size = 32;
  uint64_t batch_timeout_us = 50;
  uint16_t fec_k = 0;
  uint16_t fec_parity = 1;
  uint64_t fec_timeout_us = 200;
  double test_drop_pct = 0.0;
  double test_reorder_pct = 0.0;
  uint64_t test_reorder_delay_us = 0;
  uint64_t test_seed = 1;
  std::string echo_out_shm;
  int rcvbuf = 4 * 1024 * 1024;
  uint64_t echo_timeout_ms = 2000;
};

struct PendingDatagram {
  std::vector<uint8_t> bytes;
  uint64_t release_ns = 0;
};

struct Batch {
  std::vector<std::vector<uint8_t>> frames;
  std::vector<iovec> iovecs;
  std::vector<mmsghdr> messages;
  std::vector<uint32_t> lengths;
  uint32_t size = 0;
  uint64_t first_ns = 0;
};

struct SendCounters {
  uint64_t packets = 0;
  uint64_t sendmmsg_calls = 0;
  uint64_t send_calls = 0;
  uint32_t max_sendmmsg_batch = 0;
};

struct EchoContext {
  shm::Ring* ring = nullptr;
  uint64_t received = 0;
};

EchoContext* echo_context = nullptr;

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
    if (a == "--stream-id") c.stream_id=std::stoull(next());
    else if (a == "--stream-epoch") c.stream_epoch=std::stoull(next());
    else if (a == "--durable-acks") c.durable_acks=true;
    else if (a == "--allow-insecure-udp") c.allow_insecure_udp=true;
    else if (a == "--in-shm") c.in_shm = next();
    else if (a == "--slots") c.slots = static_cast<uint32_t>(std::stoul(next()));
    else if (a == "--host") c.host = next();
    else if (a == "--port") c.port = next();
    else if (a == "--unix-dst") c.destinations.push_back(Destination{next(), ""});
    else if (a == "--dst") c.destinations.push_back(parse_destination(next()));
    else if (a == "--count") c.count = std::stoull(next());
    else if (a == "--from-edge") c.from_edge = true;
    else if (a == "--idle-ms") c.idle_ms = std::stoull(next());
    else if (a == "--sndbuf") c.sndbuf = std::stoi(next());
    else if (a == "--queue-bytes") {auto n=std::stoull(next());if(n<65536 || n>64*1024*1024) throw std::invalid_argument("queue-bytes must be 65536..67108864");c.queue_bytes=n;}
    else if (a == "--send-workers") { auto n=std::stoul(next()); if(n<1 || n>256) throw std::invalid_argument("send-workers must be 1..256"); c.send_workers=static_cast<uint32_t>(n); }
    else if (a == "--repeat") c.repeat = static_cast<uint32_t>(std::stoul(next()));
    else if (a == "--batch-size") c.batch_size = static_cast<uint32_t>(std::stoul(next()));
    else if (a == "--batch-timeout-us") c.batch_timeout_us = std::stoull(next());
    else if (a == "--fec-k") { auto n=std::stoul(next()); if(n>fec::kMaxGeneration) throw std::invalid_argument("fec-k exceeds 128"); c.fec_k=static_cast<uint16_t>(n); }
    else if (a == "--fec-parity") { auto n=std::stoul(next()); if(n<1 || n>16) throw std::invalid_argument("fec-parity must be 1..16"); c.fec_parity=static_cast<uint16_t>(n); }
    else if (a == "--fec-timeout-us") c.fec_timeout_us = std::stoull(next());
    else if (a == "--test-drop-pct") c.test_drop_pct = std::stod(next());
    else if (a == "--test-reorder-pct") c.test_reorder_pct = std::stod(next());
    else if (a == "--test-reorder-delay-us") c.test_reorder_delay_us = std::stoull(next());
    else if (a == "--test-seed") c.test_seed = std::stoull(next());
    else if (a == "--echo-out-shm") c.echo_out_shm = next();
    else if (a == "--rcvbuf") c.rcvbuf = std::stoi(next());
    else if (a == "--echo-timeout-ms") c.echo_timeout_ms = std::stoull(next());
    else {
      fprintf(stderr, "unknown arg: %s\n", a.c_str());
      std::exit(2);
    }
  }
  if (c.repeat == 0) c.repeat = 1;
  if (c.batch_size == 0) c.batch_size = 1;
  if (c.batch_size > 1024) {
    fprintf(stderr, "--batch-size must be between 1 and 1024\n");
    std::exit(2);
  }
  if (!util::is_power_of_two(c.slots)) {
    fprintf(stderr, "--slots must be a power of two\n");
    std::exit(2);
  }
  if (c.fec_k > fec::kMaxGeneration) {
    fprintf(stderr, "--fec-k must be between 0 and %u\n", fec::kMaxGeneration);
    std::exit(2);
  }
  if (c.test_drop_pct < 0.0 || c.test_drop_pct > 100.0 ||
      c.test_reorder_pct < 0.0 || c.test_reorder_pct > 100.0) {
    fprintf(stderr, "test impairment percentages must be between 0 and 100\n");
    std::exit(2);
  }
  if (c.destinations.size() > 256 || c.sndbuf <= 0 || c.rcvbuf <= 0) {
    fprintf(stderr, "at most 256 destinations are supported and socket buffers must be positive\n");
    std::exit(2);
  }
  if (c.fec_timeout_us == 0) c.fec_timeout_us = 1;
  if (c.destinations.empty()) c.destinations.push_back(Destination{c.host, c.port});
  if (!c.echo_out_shm.empty() && (c.destinations[0].port.empty() || c.destinations.size() != 1 || c.repeat != 1 || c.fec_k != 0 || c.test_drop_pct != 0.0 || c.test_reorder_pct != 0.0)) {
    fprintf(stderr, "echo mode requires one destination, repeat=1, fec-k=0, and no test impairment\n");
    std::exit(2);
  }
  for(const auto& dst:c.destinations) if(!dst.port.empty() && !c.allow_insecure_udp)
    throw std::invalid_argument("UDP requires --allow-insecure-udp; use --unix-dst for the TLS relay");
  if(c.destinations.size()>c.send_workers) throw std::invalid_argument("send-workers must cover every destination for isolated queues");
  if(c.queue_bytes*c.destinations.size()>1024ull*1024*1024) throw std::invalid_argument("total fan-out queue budget exceeds 1 GiB");
  if(c.durable_acks) for(const auto& dst:c.destinations) if(!dst.port.empty()) throw std::invalid_argument("durable-acks requires Unix destinations");
  auto random_id=[] {uint64_t id=0;if(getrandom(&id,sizeof(id),0)!=sizeof(id) || !id) throw std::runtime_error("stream identity generation failed");return id;};
  if(!c.stream_id) c.stream_id=random_id();
  if(!c.stream_epoch) c.stream_epoch=random_id();
  return c;
}

int open_udp_socket(const Destination& dst, int sndbuf, int rcvbuf, uint64_t echo_timeout_ms) {
  if(dst.port.empty()) return transport::unix_socket(dst.host,false);
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_DGRAM;

  addrinfo* result = nullptr;
  const int rc = getaddrinfo(dst.host.c_str(), dst.port.c_str(), &hints, &result);
  if (rc != 0) {
    fprintf(stderr, "getaddrinfo(%s:%s) failed: %s\n", dst.host.c_str(), dst.port.c_str(), gai_strerror(rc));
    throw std::runtime_error("socket setup failed");
  }

  int fd = -1;
  for (addrinfo* p = result; p; p = p->ai_next) {
    fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
    if (fd < 0) continue;
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    timeval send_timeout{2,0};
    setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&send_timeout,sizeof(send_timeout));
    if (echo_timeout_ms != 0) {
      setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
      timeval timeout{};
      timeout.tv_sec = static_cast<time_t>(echo_timeout_ms / 1000);
      timeout.tv_usec = static_cast<suseconds_t>((echo_timeout_ms % 1000) * 1000);
      setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    }
    if (connect(fd, p->ai_addr, p->ai_addrlen) == 0) break;
    close(fd);
    fd = -1;
  }
  freeaddrinfo(result);

  if (fd < 0) {
    perror("connect");
    throw std::runtime_error("socket setup failed");
  }
  if (echo_timeout_ms != 0) {
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  }
  return fd;
}

void stop_fanout();
void close_sockets(const std::vector<int>& sockets) {
  stop_fanout();
  for (int fd : sockets) close(fd);
}

Batch make_batch(uint32_t batch_size) {
  const uint32_t datagram_cap = wire::max_frame + fec::kEnvelopeSize + sizeof(uint16_t) + multiplex::header_size;
  Batch batch;
  batch.frames.resize(batch_size, std::vector<uint8_t>(datagram_cap));
  batch.iovecs.resize(batch_size);
  batch.messages.resize(batch_size);
  batch.lengths.resize(batch_size);
  for (uint32_t i = 0; i < batch_size; ++i) {
    batch.iovecs[i].iov_base = batch.frames[i].data();
    batch.iovecs[i].iov_len = 0;
    batch.messages[i].msg_hdr.msg_iov = &batch.iovecs[i];
    batch.messages[i].msg_hdr.msg_iovlen = 1;
    batch.messages[i].msg_hdr.msg_name = nullptr;
    batch.messages[i].msg_hdr.msg_namelen = 0;
    batch.messages[i].msg_hdr.msg_control = nullptr;
    batch.messages[i].msg_hdr.msg_controllen = 0;
    batch.messages[i].msg_hdr.msg_flags = 0;
    batch.messages[i].msg_len = 0;
  }
  return batch;
}

void queue_datagram(Batch& batch, const uint8_t* data, uint32_t len) {
  std::memcpy(batch.frames[batch.size].data(), data, len);
  batch.lengths[batch.size] = len;
  if (batch.size == 0) batch.first_ns = util::steady_now_ns();
  ++batch.size;
}

bool flush_batch_direct(const Config& cfg, const std::vector<int>& sockets, Batch& batch, SendCounters& counters) {
  if (batch.size == 0) return true;
  for (uint32_t i = 0; i < batch.size; ++i) {
    batch.iovecs[i].iov_len = batch.lengths[i];
    batch.messages[i].msg_len = 0;
  }
  for (int sock : sockets) {
    int kind=0; socklen_t kind_len=sizeof(kind);
    if(getsockopt(sock,SOL_SOCKET,SO_TYPE,&kind,&kind_len)!=0) return false;
    if(kind==SOCK_STREAM) {
      for(uint32_t repeat=0;repeat<cfg.repeat;++repeat) for(uint32_t i=0;i<batch.size;++i) {
        if(!transport::send_frame(sock,batch.frames[i].data(),batch.lengths[i])) return false;
        if(cfg.durable_acks) {
          uint8_t ack=0;
          if(!transport::transfer(sock,&ack,1,false,util::steady_now_ns()+2000000000ull) || ack!=1) return false;
        }
        ++counters.packets; ++counters.send_calls;
      }
      continue;
    }
    for (uint32_t repeat = 0; repeat < cfg.repeat; ++repeat) {
      uint32_t offset = 0;
      while (offset < batch.size) {
        const int n = sendmmsg(sock, batch.messages.data() + offset, batch.size - offset, 0);
        if (n < 0 && errno == EINTR && !util::should_stop()) continue;
        if (n < 0) {
          perror("sendmmsg");
          return false;
        }
        if (n == 0) {
          fprintf(stderr, "sendmmsg sent zero datagrams\n");
          return false;
        }
        ++counters.sendmmsg_calls;
        counters.packets += static_cast<uint64_t>(n);
        if (static_cast<uint32_t>(n) > counters.max_sendmmsg_batch) counters.max_sendmmsg_batch = static_cast<uint32_t>(n);
        for (int i = 0; i < n; ++i) {
          const uint32_t idx = offset + static_cast<uint32_t>(i);
          if (batch.messages[idx].msg_len != batch.lengths[idx]) {
            fprintf(stderr, "short UDP sendmmsg: %u of %u bytes\n", batch.messages[idx].msg_len, batch.lengths[idx]);
            return false;
          }
        }
        offset += static_cast<uint32_t>(n);
      }
    }
  }
  batch.size = 0;
  batch.first_ns = 0;
  return true;
}

class Fanout {
  struct Work {std::vector<std::vector<uint8_t>> frames;size_t bytes=0;};
  struct DestinationState {
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<Work> queue;
    size_t bytes=0;
    bool done=false,failed=false;
    uint64_t rejected=0;
    SendCounters counters;
    std::thread thread;
  };
  const Config cfg_;
  std::vector<std::unique_ptr<DestinationState>> destinations_;
 public:
  Fanout(const Config& cfg,const std::vector<int>& sockets):cfg_(cfg) {
    for(size_t i=0;i<sockets.size();++i) {
      destinations_.push_back(std::make_unique<DestinationState>());
      auto* state=destinations_.back().get();
      state->thread=std::thread([this,state,fd=sockets[i],i] {
        for(;;) {
          Work work;
          {
            std::unique_lock<std::mutex> lock(state->mutex);
            state->cv.wait(lock,[&]{return state->done || state->failed || !state->queue.empty();});
            if(state->failed || (state->done && state->queue.empty())) break;
            work=std::move(state->queue.front());state->queue.pop_front();
            state->bytes-=work.bytes;
          }
          Batch batch=make_batch(cfg_.batch_size);
          for(auto& frame:work.frames) queue_datagram(batch,frame.data(),frame.size());
          if(!flush_batch_direct(cfg_,{fd},batch,state->counters)) {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->failed=true;state->rejected+=work.frames.size();
            for(auto& queued:state->queue) state->rejected+=queued.frames.size();
            state->queue.clear();state->bytes=0;
            fprintf(stderr,"destination %zu failed; healthy destinations continue\n",i);
            break;
          }
        }
      });
    }
  }
  void enqueue(const Batch& batch) {
    for(size_t i=0;i<destinations_.size();++i) {
      auto& state=*destinations_[i];
      std::lock_guard<std::mutex> lock(state.mutex);
      if(state.failed) {state.rejected+=batch.size;continue;}
      size_t bytes=0;for(uint32_t j=0;j<batch.size;++j) bytes+=batch.lengths[j];
      if(state.bytes+bytes>cfg_.queue_bytes) {
        state.failed=true;state.rejected+=batch.size;
        for(auto& queued:state.queue) state.rejected+=queued.frames.size();
        state.queue.clear();state.bytes=0;
        fprintf(stderr,"destination %zu queue full; isolated with explicit failure\n",i);
      } else {
        Work work;work.bytes=bytes;
        for(uint32_t j=0;j<batch.size;++j) work.frames.emplace_back(batch.frames[j].begin(),batch.frames[j].begin()+batch.lengths[j]);
        state.bytes+=bytes;state.queue.push_back(std::move(work));
      }
      state.cv.notify_one();
    }
  }
  bool finish(SendCounters& counters) {
    bool ok=true;
    for(auto& state:destinations_) {
      {std::lock_guard<std::mutex> lock(state->mutex);state->done=true;}
      state->cv.notify_one();
    }
    for(size_t i=0;i<destinations_.size();++i) {
      auto& state=*destinations_[i];
      if(state.thread.joinable()) state.thread.join();
      if(state.failed) ok=false;
      counters.packets+=state.counters.packets;
      counters.sendmmsg_calls+=state.counters.sendmmsg_calls;
      counters.send_calls+=state.counters.send_calls;
      counters.max_sendmmsg_batch=std::max(counters.max_sendmmsg_batch,state.counters.max_sendmmsg_batch);
      fprintf(stderr,"destination %zu packets=%llu rejected=%llu failed=%s\n",i,
        static_cast<unsigned long long>(state.counters.packets),
        static_cast<unsigned long long>(state.rejected),state.failed?"true":"false");
    }
    return ok;
  }
  ~Fanout() {
    for(auto& state:destinations_) {
      {std::lock_guard<std::mutex> lock(state->mutex);state->done=true;state->failed=true;state->queue.clear();}
      state->cv.notify_one();
      if(state->thread.joinable()) state->thread.join();
    }
  }
};
std::unique_ptr<Fanout> fanout;
void stop_fanout() {fanout.reset();}
bool flush_batch(const Config& cfg,const std::vector<int>& sockets,Batch& batch,SendCounters& counters) {
  if(!batch.size) return true;
  for(uint32_t i=0;i<batch.size;++i) {
    auto packet=multiplex::wrap({cfg.stream_id,cfg.stream_epoch},batch.frames[i].data(),batch.lengths[i]);
    batch.lengths[i]=static_cast<uint32_t>(packet.size());
    std::memcpy(batch.frames[i].data(),packet.data(),packet.size());
  }
  if(!fanout) return flush_batch_direct(cfg,sockets,batch,counters);
  fanout->enqueue(batch);batch.size=0;batch.first_ns=0;return true;
}

bool drain_echo(int sock) {
  if (echo_context == nullptr) return true;
  uint8_t frame[wire::max_frame+64];
  for (;;) {
    const ssize_t n = recv(sock, frame, sizeof(frame), MSG_DONTWAIT);
    if (n > 0) {
      if (n < static_cast<ssize_t>(sizeof(msg::Header))) {
        fprintf(stderr, "invalid echoed datagram length: %zd\n", n);
        return false;
      }
      uint8_t native[shm::kFrameCap]; uint32_t native_len=0;
      const uint8_t* payload=frame;uint32_t payload_len=static_cast<uint32_t>(n);multiplex::Key key;
      if(!multiplex::unwrap(payload,payload_len,key) || !wire::decode(payload,payload_len,native,native_len)) return false;
      echo_context->ring->publish(native, native_len);
      ++echo_context->received;
    } else if (n < 0 && errno == EINTR) {
      continue;
    } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      return true;
    } else {
      perror("recv echo");
      return false;
    }
  }
}

bool queue_or_flush(const Config& cfg, const std::vector<int>& sockets, Batch& batch, SendCounters& counters, const uint8_t* data, uint32_t len) {
  queue_datagram(batch, data, len);
  if (batch.size >= cfg.batch_size) return flush_batch(cfg, sockets, batch, counters);
  return true;
}

bool flush_pending(const Config& cfg, const std::vector<int>& sockets, Batch& batch, SendCounters& counters, std::vector<PendingDatagram>& pending, uint64_t now_ns, bool all) {
  size_t write = 0;
  for (size_t i = 0; i < pending.size(); ++i) {
    if (all || pending[i].release_ns <= now_ns) {
      if (!queue_or_flush(cfg, sockets, batch, counters, pending[i].bytes.data(), static_cast<uint32_t>(pending[i].bytes.size()))) return false;
    } else {
      if (write != i) pending[write] = std::move(pending[i]);
      ++write;
    }
  }
  pending.resize(write);
  return true;
}

bool emit_datagram(const Config& cfg, const std::vector<int>& sockets, Batch& batch, SendCounters& counters, std::vector<PendingDatagram>& pending, std::mt19937_64& rng, std::uniform_real_distribution<double>& dist, const uint8_t* data, uint32_t len, uint64_t& test_dropped, uint64_t& test_reordered) {
  const uint64_t now_ns = util::steady_now_ns();
  if (!flush_pending(cfg, sockets, batch, counters, pending, now_ns, false)) return false;
  if (cfg.test_drop_pct > 0.0 && dist(rng) < cfg.test_drop_pct) {
    ++test_dropped;
    return true;
  }
  if (cfg.test_reorder_pct > 0.0 && cfg.test_reorder_delay_us > 0 && dist(rng) < cfg.test_reorder_pct) {
    PendingDatagram pending_datagram;
    pending_datagram.bytes.assign(data, data + len);
    pending_datagram.release_ns = now_ns + cfg.test_reorder_delay_us * 1000ull;
    if(pending.size()>=65536) {fprintf(stderr,"impairment queue limit exceeded\n");return false;}
    pending.push_back(std::move(pending_datagram));
    ++test_reordered;
    return true;
  }
  if (!queue_or_flush(cfg, sockets, batch, counters, data, len)) return false;
  return flush_pending(cfg, sockets, batch, counters, pending, util::steady_now_ns(), false);
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

  shm::Segment seg = shm::Segment::open(cfg.in_shm, shm::region_size(cfg.slots), false);
  shm::Ring ring;
  ring.attach(seg.base(), cfg.slots, false);

  std::optional<shm::Segment> echo_segment;
  shm::Ring echo_ring;
  EchoContext echo_state;
  if (!cfg.echo_out_shm.empty()) {
    echo_segment.emplace(shm::Segment::open(cfg.echo_out_shm, shm::region_size(cfg.slots), true));
    echo_ring.attach(echo_segment->base(), cfg.slots, true);
    echo_segment->ready();
    echo_state.ring = &echo_ring;
    echo_context = &echo_state;
  }

  std::vector<int> sockets;
  sockets.reserve(cfg.destinations.size());
  for (const auto& dst : cfg.destinations) sockets.push_back(open_udp_socket(dst, cfg.sndbuf, cfg.rcvbuf, cfg.echo_out_shm.empty() ? 0 : cfg.echo_timeout_ms));

  if(sockets.size()>1) fanout=std::make_unique<Fanout>(cfg,sockets);

  const bool plain_fast_path = cfg.fec_k == 0 && cfg.test_drop_pct == 0.0 && cfg.test_reorder_pct == 0.0 && cfg.echo_out_shm.empty();
  fprintf(stderr, "sender: in_shm=%s slots=%u targets=%zu count=%llu from_edge=%s sndbuf=%d repeat=%u batch_size=%u batch_timeout_us=%llu fec_k=%u fec_timeout_us=%llu test_drop_pct=%.6f test_reorder_pct=%.6f test_reorder_delay_us=%llu test_seed=%llu orderbook_size=%zu worst_datagram=%zu echo_out_shm=%s rcvbuf=%d echo_timeout_ms=%llu fast_path=%s\n",
          cfg.in_shm.c_str(), cfg.slots, sockets.size(), static_cast<unsigned long long>(cfg.count), cfg.from_edge ? "true" : "false",
          cfg.sndbuf, cfg.repeat, cfg.batch_size, static_cast<unsigned long long>(cfg.batch_timeout_us), cfg.fec_k,
          static_cast<unsigned long long>(cfg.fec_timeout_us), cfg.test_drop_pct, cfg.test_reorder_pct,
          static_cast<unsigned long long>(cfg.test_reorder_delay_us), static_cast<unsigned long long>(cfg.test_seed), sizeof(msg::OrderBook),
          fec::kEnvelopeSize + sizeof(uint16_t) + sizeof(msg::OrderBook), cfg.echo_out_shm.empty() ? "none" : cfg.echo_out_shm.c_str(), cfg.rcvbuf,
          static_cast<unsigned long long>(cfg.echo_timeout_ms), plain_fast_path ? "true" : "false");

  uint64_t read_index = cfg.from_edge ? ring.live_edge() : 0;
  uint64_t sent = 0;
  uint64_t lapped_events = 0;
  uint64_t skipped_messages = 0;
  uint64_t fec_parity_sent = 0;
  uint64_t fec_closed_by_k = 0;
  uint64_t fec_closed_by_timeout = 0;
  uint64_t fec_closed_by_flush = 0;
  uint64_t fec_data_bytes = 0;
  uint64_t fec_parity_bytes = 0;
  uint64_t test_dropped = 0;
  uint64_t test_reordered = 0;
  const uint64_t idle_ns = cfg.idle_ms * 1000000ull;
  const uint64_t batch_timeout_ns = cfg.batch_timeout_us * 1000ull;
  const uint64_t fec_timeout_ns = cfg.fec_timeout_us * 1000ull;
  uint64_t last_progress = util::steady_now_ns();
  bool idle_terminated = false;
  uint64_t fec_first_ns = 0;
  uint64_t fec_last_arrival_ns = 0;
  uint32_t fec_gen_id = 0;
  std::vector<uint64_t> fec_arrival_intervals;
  if (cfg.fec_k != 0 && cfg.count > 1) fec_arrival_intervals.reserve(std::min<uint64_t>(cfg.count - 1,metrics::kSampleLimit));

  fec::Encoder encoder(cfg.fec_k == 0 ? 1 : cfg.fec_k, cfg.fec_parity);
  std::vector<PendingDatagram> pending;
  std::mt19937_64 rng(cfg.test_seed);
  std::uniform_real_distribution<double> dist(0.0, 100.0);
  Batch batch = make_batch(cfg.batch_size);
  SendCounters send_counters;
  const uint64_t sender_start_ns = util::steady_now_ns();

  auto close_generation = [&](uint32_t reason) -> bool {
    if (cfg.fec_k == 0 || encoder.empty()) return true;
    fec::BuiltGeneration built = encoder.close(fec_gen_id, static_cast<uint8_t>(reason));
    if (!emit_datagram(cfg, sockets, batch, send_counters, pending, rng, dist, built.parity.data(), static_cast<uint32_t>(built.parity.size()), test_dropped, test_reordered)) return false;
    for(const auto& parity:built.extra_parity) {
      if(!emit_datagram(cfg,sockets,batch,send_counters,pending,rng,dist,parity.data(),static_cast<uint32_t>(parity.size()),test_dropped,test_reordered)) return false;
      ++fec_parity_sent; fec_parity_bytes+=parity.size();
    }
    ++fec_parity_sent;
    fec_parity_bytes += built.parity.size();
    if (reason == 0) ++fec_closed_by_k;
    else if (reason == 1) ++fec_closed_by_timeout;
    else ++fec_closed_by_flush;
    ++fec_gen_id;
    fec_first_ns = 0;
    return true;
  };

  uint8_t frame[wire::max_frame+64];
  while (!util::should_stop() && (cfg.count == 0 || sent < cfg.count)) {
    if (echo_context != nullptr && !drain_echo(sockets[0])) {
      close_sockets(sockets);
      return 1;
    }
    const uint64_t loop_now = util::steady_now_ns();
    if (batch.size != 0 && loop_now - batch.first_ns >= batch_timeout_ns) {
      if (!flush_batch(cfg, sockets, batch, send_counters)) {
        close_sockets(sockets);
        return 1;
      }
    }
    if (cfg.fec_k != 0 && !encoder.empty() && loop_now - fec_first_ns >= fec_timeout_ns) {
      if (!close_generation(1)) {
        close_sockets(sockets);
        return 1;
      }
    }

    if (plain_fast_path) {
      bool made_progress = false;
      while (batch.size < cfg.batch_size && (cfg.count == 0 || sent < cfg.count)) {
        uint32_t len = 0;
        uint64_t resume = 0;
        auto st = ring.read(read_index, batch.frames[batch.size].data(), &len, &resume);
        if (st == shm::Ring::FrameStatus::kOk) {
          msg::Header h{};std::memcpy(&h,batch.frames[batch.size].data(),sizeof(h));
          h.stream_id=cfg.stream_id;h.stream_epoch=cfg.stream_epoch;
          std::memcpy(batch.frames[batch.size].data(),&h,sizeof(h));
          auto encoded = wire::encode(batch.frames[batch.size].data(), len);
          len = static_cast<uint32_t>(encoded.size());
          std::memcpy(batch.frames[batch.size].data(), encoded.data(), len);
          fec_data_bytes += len;
          batch.lengths[batch.size] = len;
          if (batch.size == 0) batch.first_ns = loop_now;
          ++batch.size;
          ++sent;
          ++read_index;
          made_progress = true;
        } else if (st == shm::Ring::FrameStatus::kLapped) {
          ++lapped_events;
          if (resume > read_index) skipped_messages += resume - read_index;
          read_index = resume;
          break;
        } else if (st == shm::Ring::FrameStatus::kCorrupt) {
          fprintf(stderr, "sender: corrupt shared-memory frame at index %llu\n",
                  static_cast<unsigned long long>(read_index));
          close_sockets(sockets);
          return 1;
        } else {
          break;
        }
      }
      if (batch.size >= cfg.batch_size && !flush_batch(cfg, sockets, batch, send_counters)) {
        close_sockets(sockets);
        return 1;
      }
      if (made_progress) last_progress = loop_now;
      if (batch.size != 0 && !flush_batch(cfg, sockets, batch, send_counters)) {
        close_sockets(sockets);
        return 1;
      }
      if (made_progress) continue;
      util::idle_wait(last_progress);
      const uint64_t now_ns = util::steady_now_ns();
      if (now_ns - last_progress > idle_ns) {
        idle_terminated = true;
        break;
      }
      continue;
    }

    uint32_t len = 0;
    uint64_t resume = 0;
    auto st = ring.read(read_index, frame, &len, &resume);

    if (st == shm::Ring::FrameStatus::kOk) {
      msg::Header h{};std::memcpy(&h,frame,sizeof(h));
      h.stream_id=cfg.stream_id;h.stream_epoch=cfg.stream_epoch;
      std::memcpy(frame,&h,sizeof(h));
      auto encoded = wire::encode(frame, len);
      len = static_cast<uint32_t>(encoded.size());
      std::memcpy(frame, encoded.data(), len);
      if (cfg.fec_k != 0) {
        const uint64_t fec_arrival_ns = loop_now;
        if (fec_last_arrival_ns != 0) metrics::sample(fec_arrival_intervals,fec_arrival_ns-fec_last_arrival_ns,sent);
        fec_last_arrival_ns = fec_arrival_ns;
      }
      if (cfg.fec_k == 0) {
        fec_data_bytes += len;
        if (!emit_datagram(cfg, sockets, batch, send_counters, pending, rng, dist, frame, len, test_dropped, test_reordered)) {
          close_sockets(sockets);
          return 1;
        }
      } else {
        if (encoder.empty()) fec_first_ns = loop_now;
        const uint16_t index = encoder.size();
        if (!encoder.add(frame, static_cast<uint16_t>(len))) {
          close_sockets(sockets);
          return 1;
        }
        std::vector<uint8_t> data = fec::data_datagram(fec_gen_id, index, cfg.fec_k, frame, static_cast<uint16_t>(len));
        fec_data_bytes += data.size();
        if (!emit_datagram(cfg, sockets, batch, send_counters, pending, rng, dist, data.data(), static_cast<uint32_t>(data.size()), test_dropped, test_reordered)) {
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
      last_progress = loop_now;
    } else if (st == shm::Ring::FrameStatus::kLapped) {
      ++lapped_events;
      if (resume > read_index) skipped_messages += resume - read_index;
      read_index = resume;
    } else if (st == shm::Ring::FrameStatus::kCorrupt) {
      fprintf(stderr, "sender: corrupt shared-memory frame at index %llu\n",
              static_cast<unsigned long long>(read_index));
      close_sockets(sockets);
      return 1;
    } else {
      util::idle_wait(last_progress);
      const uint64_t now_ns = util::steady_now_ns();
      if (!flush_pending(cfg, sockets, batch, send_counters, pending, now_ns, false)) {
        close_sockets(sockets);
        return 1;
      }
      if (!flush_batch(cfg, sockets, batch, send_counters)) {
        close_sockets(sockets);
        return 1;
      }
      if (now_ns - last_progress > idle_ns) {
        idle_terminated = true;
        break;
      }
    }
  }

  if (!close_generation(2)) {
    close_sockets(sockets);
    return 1;
  }
  if (!flush_pending(cfg, sockets, batch, send_counters, pending, util::steady_now_ns(), true)) {
    close_sockets(sockets);
    return 1;
  }
  if (!flush_batch(cfg, sockets, batch, send_counters)) {
    close_sockets(sockets);
    return 1;
  }
  if (echo_context != nullptr) {
    const uint64_t deadline = util::steady_now_ns() + cfg.echo_timeout_ms * 1000000ull;
    while (echo_state.received < send_counters.packets) {
      if (!drain_echo(sockets[0])) {
        close_sockets(sockets);
        return 1;
      }
      util::idle_wait(last_progress);
      if (util::steady_now_ns() > deadline) {
        fprintf(stderr, "echo timeout: received=%llu expected=%llu\n", static_cast<unsigned long long>(echo_state.received),
                static_cast<unsigned long long>(send_counters.packets));
        close_sockets(sockets);
        return 1;
      }
    }
  }

  bool destinations_ok=true;
  if(fanout) {destinations_ok=fanout->finish(send_counters);fanout.reset();}

  const uint64_t sender_duration_ns = util::steady_now_ns() - sender_start_ns;
  const uint64_t sender_active_ns = idle_terminated && sender_duration_ns > idle_ns ? sender_duration_ns - idle_ns : sender_duration_ns;
  const double effective_rate = sender_active_ns == 0 ? 0.0 : static_cast<double>(sent) * 1000000000.0 / static_cast<double>(sender_active_ns);
  const Distribution arrival_stats = distribution(fec_arrival_intervals);
  const double fec_overhead_pct = fec_data_bytes == 0 ? 0.0 : static_cast<double>(fec_parity_bytes) * 100.0 / static_cast<double>(fec_data_bytes);
  fprintf(stderr, "sender: sent=%llu packets=%llu echoed=%llu lapped=%llu skipped=%llu fast_path=%s duration_ns=%llu active_ns=%llu idle_terminated=%s effective_rate=%.3f sendmmsg_calls=%llu send_calls=%llu max_sendmmsg_batch=%u fec_parity_sent=%llu fec_closed_by_k=%llu fec_closed_by_timeout=%llu fec_closed_by_flush=%llu fec_data_bytes=%llu fec_parity_bytes=%llu fec_overhead_pct=%.6f fec_arrival_p50_ns=%llu fec_arrival_p99_ns=%llu fec_arrival_max_ns=%llu test_dropped=%llu test_reordered=%llu\n",
          static_cast<unsigned long long>(sent), static_cast<unsigned long long>(send_counters.packets), static_cast<unsigned long long>(echo_state.received),
          static_cast<unsigned long long>(lapped_events),
          static_cast<unsigned long long>(skipped_messages), plain_fast_path ? "true" : "false",
          static_cast<unsigned long long>(sender_duration_ns), static_cast<unsigned long long>(sender_active_ns),
          idle_terminated ? "true" : "false", effective_rate,
          static_cast<unsigned long long>(send_counters.sendmmsg_calls), static_cast<unsigned long long>(send_counters.send_calls), send_counters.max_sendmmsg_batch,
          static_cast<unsigned long long>(fec_parity_sent), static_cast<unsigned long long>(fec_closed_by_k),
          static_cast<unsigned long long>(fec_closed_by_timeout), static_cast<unsigned long long>(fec_closed_by_flush),
          static_cast<unsigned long long>(fec_data_bytes), static_cast<unsigned long long>(fec_parity_bytes), fec_overhead_pct,
          static_cast<unsigned long long>(arrival_stats.p50), static_cast<unsigned long long>(arrival_stats.p99),
          static_cast<unsigned long long>(arrival_stats.max), static_cast<unsigned long long>(test_dropped),
          static_cast<unsigned long long>(test_reordered));
  close_sockets(sockets);
  if(!destinations_ok) return 1;
  if(cfg.count && sent < cfg.count) { fprintf(stderr,"incomplete counted run\n"); return 1; }
  return 0;
}

int main(int argc,char** argv) {
  try {return run(argc,argv);}
  catch(const std::exception& error) {fprintf(stderr,"sender: %s\n",error.what());return 1;}
}
