#pragma once

#include <cstdint>
#include <cstring>

namespace msg {

inline constexpr uint32_t kSymbolLen = 16;
inline constexpr uint32_t kVenueLen = 16;
inline constexpr uint32_t kCurrencyLen = 8;
inline constexpr uint32_t kBookDepth = 5;
inline constexpr uint16_t kVersion = 1;

enum class Type : uint16_t {
  Trade = 1,
  Bbo = 2,
  OrderBook = 3,
};


struct Header {
  uint64_t seq_id;
  uint64_t send_ts_ns;
  uint16_t type;
  uint16_t version;
  uint32_t body_len;
};

struct alignas(64) Trade {
  Header header;
  char symbol[kSymbolLen];
  char venue[kVenueLen];
  char base_currency[kCurrencyLen];
  char quote_currency[kCurrencyLen];
  uint64_t trade_id;
  uint64_t buyer_order_id;
  uint64_t seller_order_id;
  uint64_t exchange_ts_ns;
  uint64_t match_engine_ts_ns;
  double price;
  double quantity;
  double notional;
  int64_t price_ticks;
  int64_t quantity_lots;
  uint32_t tick_direction;
  uint8_t aggressor_side;
  uint8_t is_block_trade;
  uint8_t is_rpi;
  uint8_t is_liquidation;
  uint8_t flags;
  uint8_t reserved[19];
};

struct alignas(64) Bbo {
  Header header;
  char symbol[kSymbolLen];
  char venue[kVenueLen];
  uint64_t update_id;
  uint64_t exchange_ts_ns;
  uint64_t match_engine_ts_ns;
  double bid_price;
  double bid_size;
  double ask_price;
  double ask_size;
  int64_t bid_price_ticks;
  int64_t ask_price_ticks;
  int64_t bid_size_lots;
  int64_t ask_size_lots;
  uint32_t bid_order_count;
  uint32_t ask_order_count;
  uint8_t flags;
  uint8_t reserved[23];
};

struct Level {
  double price;
  double size;
  int64_t price_ticks;
  int64_t size_lots;
  uint32_t order_count;
  uint32_t reserved;
};

struct alignas(64) OrderBook {
  Header header;
  char symbol[kSymbolLen];
  char venue[kVenueLen];
  uint64_t update_id;
  uint64_t prev_update_id;
  uint64_t exchange_ts_ns;
  uint64_t match_engine_ts_ns;
  Level bids[kBookDepth];
  Level asks[kBookDepth];
  uint32_t checksum;
  uint8_t is_snapshot;
  uint8_t flags;
  uint8_t reserved[26];
};

inline constexpr uint32_t kMaxFrame = sizeof(OrderBook);
static_assert(sizeof(Trade) <= kMaxFrame, "kMaxFrame must fit every message");
static_assert(sizeof(Bbo) <= kMaxFrame, "kMaxFrame must fit every message");

inline uint32_t expected_size(Type type) {
  switch (type) {
    case Type::Trade: return sizeof(Trade);
    case Type::Bbo: return sizeof(Bbo);
    case Type::OrderBook: return sizeof(OrderBook);
  }
  return 0;
}

// Validate network-controlled bytes before interpreting them as a message.
// memcpy keeps the header read safe even when the datagram is not aligned.
inline bool validate_frame(const void* data, uint32_t len) {
  if (data == nullptr || len < sizeof(Header) || len > kMaxFrame) return false;
  Header header{};
  std::memcpy(&header, data, sizeof(header));
  if (header.version != kVersion || header.body_len != len) return false;
  return expected_size(static_cast<Type>(header.type)) == len;
}

}
