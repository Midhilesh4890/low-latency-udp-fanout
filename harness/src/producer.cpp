#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>
#include <thread>

#include "message.h"
#include "shm_ring.h"
#include "shm_segment.h"
#include "util.h"

namespace {

enum class Kind { Trade, Bbo, Book, Mixed };

struct Config {
  std::string shm_name = "/fanout_ring";
  uint32_t slots = 1024;
  uint64_t count = 1000000;
  double rate = 0.0;
  uint64_t start_delay_ms = 0;
  uint32_t wait_readers = 0;
  Kind kind = Kind::Mixed;
};

Kind parse_kind(const std::string& s) {
  if (s == "trade") return Kind::Trade;
  if (s == "bbo") return Kind::Bbo;
  if (s == "book") return Kind::Book;
  if (s == "mixed") return Kind::Mixed;
  fprintf(stderr, "--type must be trade|bbo|book|mixed (got %s)\n", s.c_str());
  std::exit(2);
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
    if (a == "--shm") c.shm_name = next();
    else if (a == "--slots") c.slots = static_cast<uint32_t>(std::stoul(next()));
    else if (a == "--count") c.count = std::stoull(next());
    else if (a == "--rate") c.rate = std::stod(next());
    else if (a == "--wait-readers") c.wait_readers = static_cast<uint32_t>(std::stoul(next()));
    else if (a == "--start-delay-ms") c.start_delay_ms = std::stoull(next());
    else if (a == "--type") c.kind = parse_kind(next());
    else {
      fprintf(stderr, "unknown arg: %s\n", a.c_str());
      std::exit(2);
    }
  }
  if (!util::is_power_of_two(c.slots)) {
    fprintf(stderr, "--slots must be a power of two (got %u)\n", c.slots);
    std::exit(2);
  }
  if (c.rate < 0.0) {
    fprintf(stderr, "--rate must be non-negative\n");
    std::exit(2);
  }
  return c;
}

void set_str(char* dst, uint32_t cap, const char* src) {
  std::strncpy(dst, src, cap);
  dst[cap - 1] = '\0';
}

void fill_header(msg::Header& h, uint64_t seq, msg::Type type, uint32_t body_len) {
  h.seq_id = seq;
  h.stream_id=static_cast<uint64_t>(getpid());
  static const auto epoch=util::steady_now_ns();h.stream_epoch=epoch;
  h.type = static_cast<uint16_t>(type);
  h.version = msg::kVersion;
  h.body_len = body_len;
  h.send_ts_ns = util::now_ns();
}

uint32_t build_trade(void* buf, uint64_t seq) {
  auto& m = *new(buf) msg::Trade{};
  set_str(m.symbol, msg::kSymbolLen, "BTCUSDT");
  set_str(m.venue, msg::kVenueLen, "BINANCE");
  set_str(m.base_currency, msg::kCurrencyLen, "BTC");
  set_str(m.quote_currency, msg::kCurrencyLen, "USDT");
  m.trade_id = 100000 + seq;
  m.buyer_order_id = 500000 + seq * 2;
  m.seller_order_id = 500001 + seq * 2;
  m.exchange_ts_ns = util::now_ns();
  m.match_engine_ts_ns = m.exchange_ts_ns;
  m.price = 65000.0 + static_cast<double>(seq % 500) * 0.5;
  m.quantity = 0.001 + static_cast<double>(seq % 100) * 0.01;
  m.notional = m.price * m.quantity;
  m.price_ticks = static_cast<int64_t>(m.price * 100.0);
  m.quantity_lots = static_cast<int64_t>(m.quantity * 1000.0);
  m.tick_direction = seq % 4;
  m.aggressor_side = static_cast<uint8_t>(seq & 1);
  m.is_block_trade = 0;
  m.is_rpi = 0;
  m.is_liquidation = 0;
  m.flags = 0;
  std::memset(m.reserved, 0, sizeof(m.reserved));
  fill_header(m.header, seq, msg::Type::Trade, sizeof(msg::Trade));
  return sizeof(msg::Trade);
}

uint32_t build_bbo(void* buf, uint64_t seq) {
  auto& m = *new(buf) msg::Bbo{};
  set_str(m.symbol, msg::kSymbolLen, "BTCUSDT");
  set_str(m.venue, msg::kVenueLen, "BINANCE");
  m.update_id = 900000 + seq;
  m.exchange_ts_ns = util::now_ns();
  m.match_engine_ts_ns = m.exchange_ts_ns;
  const double mid = 65000.0 + static_cast<double>(seq % 500) * 0.5;
  m.bid_price = mid - 0.5;
  m.ask_price = mid + 0.5;
  m.bid_size = 1.5 + static_cast<double>(seq % 50) * 0.1;
  m.ask_size = 1.5 + static_cast<double>((seq + 7) % 50) * 0.1;
  m.bid_price_ticks = static_cast<int64_t>(m.bid_price * 100.0);
  m.ask_price_ticks = static_cast<int64_t>(m.ask_price * 100.0);
  m.bid_size_lots = static_cast<int64_t>(m.bid_size * 1000.0);
  m.ask_size_lots = static_cast<int64_t>(m.ask_size * 1000.0);
  m.bid_order_count = 3 + static_cast<uint32_t>(seq % 10);
  m.ask_order_count = 3 + static_cast<uint32_t>((seq + 3) % 10);
  m.flags = 0;
  std::memset(m.reserved, 0, sizeof(m.reserved));
  fill_header(m.header, seq, msg::Type::Bbo, sizeof(msg::Bbo));
  return sizeof(msg::Bbo);
}

uint32_t build_book(void* buf, uint64_t seq) {
  auto& m = *new(buf) msg::OrderBook{};
  set_str(m.symbol, msg::kSymbolLen, "BTCUSDT");
  set_str(m.venue, msg::kVenueLen, "BINANCE");
  m.update_id = 900000 + seq;
  m.prev_update_id = m.update_id - 1;
  m.exchange_ts_ns = util::now_ns();
  m.match_engine_ts_ns = m.exchange_ts_ns;
  const double mid = 65000.0 + static_cast<double>(seq % 500) * 0.5;
  for (uint32_t i = 0; i < msg::kBookDepth; ++i) {
    msg::Level& b = m.bids[i];
    b.price = mid - 0.5 - static_cast<double>(i);
    b.size = 1.0 + static_cast<double>((seq + i) % 40) * 0.1;
    b.price_ticks = static_cast<int64_t>(b.price * 100.0);
    b.size_lots = static_cast<int64_t>(b.size * 1000.0);
    b.order_count = 2 + static_cast<uint32_t>((seq + i) % 8);
    b.reserved = 0;

    msg::Level& a = m.asks[i];
    a.price = mid + 0.5 + static_cast<double>(i);
    a.size = 1.0 + static_cast<double>((seq + i + 5) % 40) * 0.1;
    a.price_ticks = static_cast<int64_t>(a.price * 100.0);
    a.size_lots = static_cast<int64_t>(a.size * 1000.0);
    a.order_count = 2 + static_cast<uint32_t>((seq + i + 5) % 8);
    a.reserved = 0;
  }
  m.checksum = static_cast<uint32_t>(seq * 2654435761u);
  m.is_snapshot = 1;
  m.flags = 0;
  std::memset(m.reserved, 0, sizeof(m.reserved));
  fill_header(m.header, seq, msg::Type::OrderBook, sizeof(msg::OrderBook));
  return sizeof(msg::OrderBook);
}

uint32_t build(Kind kind, uint64_t seq, void* buf) {
  Kind k = kind;
  if (k == Kind::Mixed) {
    switch (seq % 3) {
      case 0: k = Kind::Trade; break;
      case 1: k = Kind::Bbo; break;
      default: k = Kind::Book; break;
    }
  }
  switch (k) {
    case Kind::Trade: return build_trade(buf, seq);
    case Kind::Bbo: return build_bbo(buf, seq);
    default: return build_book(buf, seq);
  }
}

const char* kind_name(Kind k) {
  switch (k) {
    case Kind::Trade: return "trade";
    case Kind::Bbo: return "bbo";
    case Kind::Book: return "book";
    default: return "mixed";
  }
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

  shm::Segment seg =
      shm::Segment::open(cfg.shm_name, shm::region_size(cfg.slots), true);
  shm::Ring ring;
  ring.attach(seg.base(), cfg.slots, true);
  seg.ready();

  const auto attach_deadline=util::steady_now_ns()+5000000000ull;
  while(!util::should_stop() && ring.reader_count()<cfg.wait_readers) {
    if(util::steady_now_ns()>attach_deadline) { fprintf(stderr,"reader attachment timed out\n"); return 1; }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  alignas(64) uint8_t frame[shm::kFrameCap];

  if (cfg.start_delay_ms != 0) std::this_thread::sleep_for(std::chrono::milliseconds(cfg.start_delay_ms));

  const uint64_t interval_ns =
      cfg.rate > 0.0 ? static_cast<uint64_t>(1e9 / cfg.rate) : 0;
  uint64_t next_send = util::steady_now_ns();

  fprintf(stderr, "producer: shm=%s slots=%u count=%llu rate=%.0f type=%s start_delay_ms=%llu\n",
          cfg.shm_name.c_str(), cfg.slots,
          static_cast<unsigned long long>(cfg.count), cfg.rate,
          kind_name(cfg.kind), static_cast<unsigned long long>(cfg.start_delay_ms));

  uint64_t seq = 0;
  while (!util::should_stop() && (cfg.count == 0 || seq < cfg.count)) {
    if (interval_ns) {
      while (!util::should_stop() && util::steady_now_ns() < next_send) {
        const auto remaining=next_send-util::steady_now_ns();
        if(remaining>100000 && remaining<interval_ns) std::this_thread::sleep_for(std::chrono::nanoseconds(remaining-50000));
      }
      next_send += interval_ns;
    }
    ++seq;
    const uint32_t len = build(cfg.kind, seq, frame);
    ring.publish(frame, len);
  }

  fprintf(stderr, "producer: sent %llu messages\n",
          static_cast<unsigned long long>(seq));
  seg.unlink();
  return 0;
}

int main(int argc,char** argv) {
  try {return run(argc,argv);}
  catch(const std::exception& error) {fprintf(stderr,"producer: %s\n",error.what());return 1;}
}
