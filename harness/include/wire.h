#pragma once
#include <limits>
#include <type_traits>
#include <vector>
#include <stdexcept>
#include "message.h"

// Protocol 3: "PUL3", followed by explicit fields in declaration order.
// Integers are big endian, signed integers use two's complement, doubles
// use IEEE754 binary64. No alignment bytes are transmitted.
namespace wire {
inline constexpr uint32_t magic = 0x50554c33;
inline constexpr uint32_t max_frame = 1024;
template<class IO> void fields(IO& io, msg::Header& m) { io(m.seq_id, m.send_ts_ns, m.type, m.version, m.body_len, m.stream_id, m.stream_epoch); }
template<class IO> void fields(IO& io, msg::Trade& m) { io(m.header, m.symbol, m.venue, m.base_currency, m.quote_currency, m.trade_id, m.buyer_order_id, m.seller_order_id, m.exchange_ts_ns, m.match_engine_ts_ns, m.price, m.quantity, m.notional, m.price_ticks, m.quantity_lots, m.tick_direction, m.aggressor_side, m.is_block_trade, m.is_rpi, m.is_liquidation, m.flags, m.reserved); }
template<class IO> void fields(IO& io, msg::Bbo& m) { io(m.header, m.symbol, m.venue, m.update_id, m.exchange_ts_ns, m.match_engine_ts_ns, m.bid_price, m.bid_size, m.ask_price, m.ask_size, m.bid_price_ticks, m.ask_price_ticks, m.bid_size_lots, m.ask_size_lots, m.bid_order_count, m.ask_order_count, m.flags, m.reserved); }
template<class IO> void fields(IO& io, msg::Level& m) { io(m.price, m.size, m.price_ticks, m.size_lots, m.order_count, m.reserved); }
template<class IO> void fields(IO& io, msg::OrderBook& m) { io(m.header, m.symbol, m.venue, m.update_id, m.prev_update_id, m.exchange_ts_ns, m.match_engine_ts_ns, m.bids, m.asks, m.checksum, m.is_snapshot, m.flags, m.reserved); }
struct Writer {
  std::vector<uint8_t> bytes;
  template<class... T> void operator()(T&... v) { (value(v), ...); }
  template<class T, size_t N> void value(T (&v)[N]) { for (auto& x : v) value(x); }
  template<class T> void value(T& v) {
    if constexpr (std::is_integral_v<T>) {
      using U = std::make_unsigned_t<T>;
      U bits = static_cast<U>(v);
      for (size_t i=sizeof(T); i>0; --i) bytes.push_back(static_cast<uint8_t>(bits >> ((i-1)*8)));
    } else if constexpr (std::is_floating_point_v<T>) {
      static_assert(sizeof(T)==8 && std::numeric_limits<T>::is_iec559);
      uint64_t bits; std::memcpy(&bits,&v,8); value(bits);
    } else fields(*this,v);
  }
};
struct Reader {
  const uint8_t* data;
  size_t size, pos=0;
  template<class... T> void operator()(T&... v) { (value(v), ...); }
  template<class T, size_t N> void value(T (&v)[N]) { for(auto& x:v) value(x); }
  template<class T> void value(T& v) {
    if constexpr (std::is_integral_v<T>) {
      if (pos+sizeof(T)>size) throw std::length_error("truncated wire frame");
      using U = std::make_unsigned_t<T>;
      U bits=0;
      for(size_t i=0;i<sizeof(T);++i) bits=static_cast<U>((bits<<8)|data[pos++]);
      if constexpr (std::is_signed_v<T>) {
        v = bits <= static_cast<U>(std::numeric_limits<T>::max()) ?
          static_cast<T>(bits) : static_cast<T>(-1 - static_cast<T>(static_cast<U>(~bits)));
      } else v=bits;
    } else if constexpr (std::is_floating_point_v<T>) {
      uint64_t bits; value(bits); std::memcpy(&v,&bits,8);
    } else fields(*this,v);
  }
};
template<class T> std::vector<uint8_t> encode_as(const void* data) {
  T m{}; std::memcpy(&m,data,sizeof(m));
  Writer w; auto tag=magic; w(tag);
  // This is the serialized frame length, independent of sizeof(T).
  m.header.body_len=0; w(m);
  m.header.body_len=static_cast<uint32_t>(w.bytes.size());
  w.bytes.clear(); w(tag,m); return w.bytes;
}
inline std::vector<uint8_t> encode(const void* data,uint32_t len) {
  if(!msg::validate_frame(data,len)) throw std::invalid_argument("invalid native message");
  msg::Header h{}; std::memcpy(&h,data,sizeof(h));
  switch(static_cast<msg::Type>(h.type)) {
    case msg::Type::Trade: return encode_as<msg::Trade>(data);
    case msg::Type::Bbo: return encode_as<msg::Bbo>(data);
    case msg::Type::OrderBook: return encode_as<msg::OrderBook>(data);
  }
  throw std::invalid_argument("unknown message type");
}
template<class T> bool decode_as(const uint8_t* data,uint32_t len,void* out,uint32_t& native_len) {
  T m{}; Reader r{data,len,4}; r(m);
  if(r.pos!=len || m.header.body_len!=len || m.header.version!=msg::kVersion) return false;
  m.header.body_len=sizeof(T); native_len=sizeof(T); std::memcpy(out,&m,sizeof(T)); return true;
}
inline bool decode(const uint8_t* data,uint32_t len,void* out,uint32_t& native_len) {
  if(!data || !out || len<44 || len>max_frame) return false;
  try {
    Reader r{data,len}; uint32_t tag; msg::Header h{}; r(tag,h);
    if(tag!=magic || h.version!=msg::kVersion || h.body_len!=len) return false;
    switch(static_cast<msg::Type>(h.type)) {
      case msg::Type::Trade: return decode_as<msg::Trade>(data,len,out,native_len);
      case msg::Type::Bbo: return decode_as<msg::Bbo>(data,len,out,native_len);
      case msg::Type::OrderBook: return decode_as<msg::OrderBook>(data,len,out,native_len);
    }
  } catch(const std::length_error&) {}
  return false;
}
}
