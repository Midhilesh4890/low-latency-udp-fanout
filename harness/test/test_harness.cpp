#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "dedupe_window.h"
#include "fec.h"
#include "message.h"
#include "wire.h"
#include "multiplex.h"
#include <thread>
#include <sys/mman.h>
#include <sys/wait.h>
#include <array>
#include <memory>
#include "metrics.h"
#include "shm_ring.h"

static void test_metrics_basic() {
  metrics::Accumulator acc;
  for (uint64_t i = 1; i <= 100; ++i) acc.record(i, i);
  metrics::Report r = acc.report();

  assert(r.received == 100);
  assert(r.expected == 100);
  assert(r.dropped == 0);
  assert(r.drop_rate == 0.0);
  assert(r.lat_min == 1);
  assert(r.lat_max == 100);
  assert(r.p50 == 50);
  assert(r.p99 == 99);
  assert(r.lat_mean > 50.0 && r.lat_mean < 51.0);
  printf("test_metrics_basic OK\n");
}

static void test_metrics_drops() {
  metrics::Accumulator acc;
  for (uint64_t i = 2; i <= 100; i += 2) acc.record(i, 10);
  metrics::Report r = acc.report();

  assert(r.received == 50);
  assert(r.expected == 99);
  assert(r.dropped == 49);
  assert(r.drop_rate > 0.49 && r.drop_rate < 0.50);
  printf("test_metrics_drops OK\n");
}

static void test_ring_roundtrip() {
  const uint32_t slots = 8;
  std::unique_ptr<uint8_t[], void(*)(uint8_t*)> storage(
      static_cast<uint8_t*>(::operator new[](shm::region_size(slots), std::align_val_t(64))),
      [](uint8_t* p){::operator delete[](p,std::align_val_t(64));});
  struct { uint8_t* ptr; uint8_t* data() {return ptr;} } mem{storage.get()};
  shm::Ring prod;
  prod.attach(mem.data(), slots, true);
  shm::Ring cons;
  cons.attach(mem.data(), slots, false);

  for (uint32_t i = 0; i < 5; ++i) {
    uint8_t frame[16];
    std::memset(frame, static_cast<int>(i), sizeof(frame));
    prod.publish(frame, sizeof(frame));
  }

  uint64_t read_index = 0;
  for (uint32_t i = 0; i < 5; ++i) {
    uint8_t out[64];
    uint32_t len = 0;
    uint64_t resume = 0;
    auto st = cons.read(read_index, out, &len, &resume);
    assert(st == shm::Ring::FrameStatus::kOk);
    assert(len == 16);
    assert(out[0] == static_cast<uint8_t>(i));
    ++read_index;
  }
  uint8_t out[64];
  uint32_t len = 0;
  uint64_t resume = 0;
  assert(cons.read(read_index, out, &len, &resume) == shm::Ring::FrameStatus::kEmpty);
  printf("test_ring_roundtrip OK\n");
}

static void test_ring_lapping() {
  const uint32_t slots = 4;
  std::unique_ptr<uint8_t[], void(*)(uint8_t*)> storage(
      static_cast<uint8_t*>(::operator new[](shm::region_size(slots), std::align_val_t(64))),
      [](uint8_t* p){::operator delete[](p,std::align_val_t(64));});
  struct { uint8_t* ptr; uint8_t* data() {return ptr;} } mem{storage.get()};
  shm::Ring prod;
  prod.attach(mem.data(), slots, true);
  prod.set_backpressure(false);
  shm::Ring cons;
  cons.attach(mem.data(), slots, false);

  for (uint32_t i = 0; i < 10; ++i) {
    uint8_t frame[8];
    std::memset(frame, static_cast<int>(i), sizeof(frame));
    prod.publish(frame, sizeof(frame));
  }

  uint8_t out[64];
  uint32_t len = 0;
  uint64_t resume = 0;
  auto st = cons.read(0, out, &len, &resume);
  assert(st == shm::Ring::FrameStatus::kLapped);
  assert(resume == 6);

  st = cons.read(resume, out, &len, &resume);
  assert(st == shm::Ring::FrameStatus::kOk);
  assert(out[0] == 6);
  printf("test_ring_lapping OK\n");
}

static void test_frame_roundtrip_preserves_header() {
  const uint32_t slots = 8;
  std::unique_ptr<uint8_t[], void(*)(uint8_t*)> storage(
      static_cast<uint8_t*>(::operator new[](shm::region_size(slots), std::align_val_t(64))),
      [](uint8_t* p){::operator delete[](p,std::align_val_t(64));});
  struct { uint8_t* ptr; uint8_t* data() {return ptr;} } mem{storage.get()};
  shm::Ring prod;
  prod.attach(mem.data(), slots, true);
  shm::Ring cons;
  cons.attach(mem.data(), slots, false);

  msg::Trade frame{};
  frame.header.seq_id = 42;
  frame.header.send_ts_ns = 123456789;
  frame.header.type = static_cast<uint16_t>(msg::Type::Trade);
  frame.header.version = msg::kVersion;
  frame.header.body_len = sizeof(frame);
  frame.trade_id = 777;
  frame.price = 101.25;
  frame.quantity = 3.5;

  prod.publish(&frame, sizeof(frame));

  msg::Trade out{};
  uint32_t len = 0;
  uint64_t resume = 0;
  auto st = cons.read(0, &out, &len, &resume);

  assert(st == shm::Ring::FrameStatus::kOk);
  assert(len == sizeof(frame));
  assert(out.header.seq_id == frame.header.seq_id);
  assert(out.header.send_ts_ns == frame.header.send_ts_ns);
  assert(out.header.type == frame.header.type);
  assert(out.header.version == frame.header.version);
  assert(out.header.body_len == frame.header.body_len);
  assert(out.trade_id == frame.trade_id);
  assert(out.price == frame.price);
  assert(out.quantity == frame.quantity);
  printf("test_frame_roundtrip_preserves_header OK\n");
}

static void test_frame_validation() {
  msg::Trade frame{};
  frame.header.seq_id = 1;
  frame.header.send_ts_ns = 2;
  frame.header.type = static_cast<uint16_t>(msg::Type::Trade);
  frame.header.version = msg::kVersion;
  frame.header.body_len = sizeof(frame);
  assert(msg::validate_frame(&frame, sizeof(frame)));

  frame.header.version = 99;
  assert(!msg::validate_frame(&frame, sizeof(frame)));
  frame.header.version = msg::kVersion;
  frame.header.body_len = sizeof(frame) - 1;
  assert(!msg::validate_frame(&frame, sizeof(frame)));
  frame.header.body_len = sizeof(frame);
  frame.header.type = 999;
  assert(!msg::validate_frame(&frame, sizeof(frame)));
  assert(!msg::validate_frame(&frame, sizeof(msg::Header) - 1));
  printf("test_frame_validation OK\n");
}

static void test_ring_rejects_invalid_metadata_and_lengths() {
  const uint32_t slots = 8;
  std::unique_ptr<uint8_t[], void(*)(uint8_t*)> storage(
      static_cast<uint8_t*>(::operator new[](shm::region_size(slots), std::align_val_t(64))),
      [](uint8_t* p){::operator delete[](p,std::align_val_t(64));});
  struct { uint8_t* ptr; uint8_t* data() {return ptr;} } mem{storage.get()};
  shm::Ring producer;
  producer.attach(mem.data(), slots, true);
  bool threw = false;
  try {
    uint8_t oversized[shm::kFrameCap + 1]{};
    producer.publish(oversized, sizeof(oversized));
  } catch (const std::length_error&) {
    threw = true;
  }
  assert(threw);

  auto* header = reinterpret_cast<shm::Header*>(mem.data());
  header->magic = 0;
  threw = false;
  try {
    shm::Ring consumer;
    consumer.attach(mem.data(), slots, false);
  } catch (const std::runtime_error&) {
    threw = true;
  }
  assert(threw);
  printf("test_ring_rejects_invalid_metadata_and_lengths OK\n");
}

static void test_dedupe_in_order() {
  dedupe::Window window(64);
  for (uint64_t i = 1; i <= 100; ++i) assert(window.observe(i) == dedupe::ObserveResult::kAccept);
  assert(window.counters().accepted == 100);
  assert(window.counters().duplicates == 0);
  assert(window.counters().too_old == 0);
  printf("test_dedupe_in_order OK\n");
}

static void test_dedupe_swapped_adjacent() {
  dedupe::Window window(64);
  assert(window.observe(1) == dedupe::ObserveResult::kAccept);
  assert(window.observe(3) == dedupe::ObserveResult::kAccept);
  assert(window.observe(2) == dedupe::ObserveResult::kAccept);
  assert(window.base_seq() == 4);
  assert(window.counters().accepted == 3);
  printf("test_dedupe_swapped_adjacent OK\n");
}

static void test_dedupe_delayed_w_minus_one() {
  dedupe::Window window(64);
  assert(window.observe(1) == dedupe::ObserveResult::kAccept);
  assert(window.observe(64) == dedupe::ObserveResult::kAccept);
  assert(window.counters().max_reorder_depth == 62);
  printf("test_dedupe_delayed_w_minus_one OK\n");
}

static void test_dedupe_delayed_w_plus_one_too_old() {
  dedupe::Window window(64);
  assert(window.observe(1) == dedupe::ObserveResult::kAccept);
  assert(window.observe(66) == dedupe::ObserveResult::kAdvanced);
  assert(window.observe(2) == dedupe::ObserveResult::kTooOld);
  assert(window.counters().too_old == 1);
  printf("test_dedupe_delayed_w_plus_one_too_old OK\n");
}

static void test_dedupe_duplicate() {
  dedupe::Window window(64);
  assert(window.observe(1) == dedupe::ObserveResult::kAccept);
  assert(window.observe(3) == dedupe::ObserveResult::kAccept);
  assert(window.observe(3) == dedupe::ObserveResult::kDuplicate);
  assert(window.counters().duplicates == 1);
  printf("test_dedupe_duplicate OK\n");
}

static void test_dedupe_gap_lost_once() {
  dedupe::Window window(64);
  assert(window.observe(1) == dedupe::ObserveResult::kAccept);
  assert(window.observe(66) == dedupe::ObserveResult::kAdvanced);
  assert(window.counters().lost_confirmed == 1);
  assert(window.observe(2) == dedupe::ObserveResult::kTooOld);
  assert(window.counters().lost_confirmed == 1);
  printf("test_dedupe_gap_lost_once OK\n");
}

static void test_dedupe_base_advances_full_window() {
  dedupe::Window window(64);
  assert(window.observe(1) == dedupe::ObserveResult::kAccept);
  for (uint64_t i = 3; i <= 64; ++i) assert(window.observe(i) == dedupe::ObserveResult::kAccept);
  assert(window.base_seq() == 2);
  assert(window.observe(2) == dedupe::ObserveResult::kAccept);
  assert(window.base_seq() == 65);
  printf("test_dedupe_base_advances_full_window OK\n");
}

static std::vector<uint8_t> make_trade(uint64_t seq, uint32_t extra) {
  msg::Trade frame{};
  frame.header.seq_id = seq;
  frame.header.send_ts_ns = 1000 + seq;
  frame.header.type = static_cast<uint16_t>(msg::Type::Trade);
  frame.header.version = msg::kVersion;
  frame.header.body_len = sizeof(frame);
  frame.trade_id = 7000 + seq;
  frame.price = 10.0 + static_cast<double>(seq);
  frame.quantity = 1.0 + static_cast<double>(extra);
  std::vector<uint8_t> out(sizeof(frame) - extra);
  std::memcpy(out.data(), &frame, out.size());
  return out;
}

static fec::BuiltGeneration build_generation(uint16_t k, uint32_t gen_id, std::vector<std::vector<uint8_t>>& frames, uint16_t count) {
  fec::Encoder encoder(k);
  frames.clear();
  for (uint16_t i = 0; i < count; ++i) {
    frames.push_back(make_trade(i + 1, i % 7));
    assert(encoder.add(frames.back().data(), static_cast<uint16_t>(frames.back().size())));
  }
  return encoder.close(gen_id, 0);
}

static std::vector<std::vector<uint8_t>> decode_datagrams(const std::vector<std::vector<uint8_t>>& datagrams, fec::Decoder& decoder) {
  std::vector<std::vector<uint8_t>> published;
  uint64_t now = 100;
  for (const auto& datagram : datagrams) {
    assert(decoder.receive(datagram.data(), static_cast<uint32_t>(datagram.size()), now, [&](const uint8_t* data, uint32_t len) {
      published.push_back(std::vector<uint8_t>(data, data + len));
      return true;
    }));
    now += 10;
  }
  return published;
}

static bool contains_frame(const std::vector<std::vector<uint8_t>>& published, const std::vector<uint8_t>& frame) {
  return std::find(published.begin(), published.end(), frame) != published.end();
}


static void test_fec_disabled_raw_stream() {
  std::vector<std::vector<uint8_t>> frames;
  frames.push_back(make_trade(1, 0));
  frames.push_back(make_trade(2, 1));
  fec::Decoder decoder(64);
  std::vector<std::vector<uint8_t>> published;
  uint64_t parity_frames = 0;
  for (const auto& frame : frames) {
    assert(decoder.receive(frame.data(), static_cast<uint32_t>(frame.size()), 100, [&](const uint8_t* data, uint32_t len) {
      published.push_back(std::vector<uint8_t>(data, data + len));
      return true;
    }));
  }
  assert(parity_frames == 0);
  assert(published == frames);
  assert(decoder.counters().parity_received == 0);
  assert(decoder.counters().recovered == 0);
  printf("test_fec_disabled_raw_stream OK\n");
}

static void test_fec_single_drop_each_index() {
  const uint16_t k = 8;
  for (uint16_t drop = 0; drop < k; ++drop) {
    std::vector<std::vector<uint8_t>> frames;
    fec::BuiltGeneration built = build_generation(k, 10 + drop, frames, k);
    std::vector<std::vector<uint8_t>> datagrams;
    for (uint16_t i = 0; i < k; ++i) {
      if (i != drop) datagrams.push_back(built.data[i]);
    }
    datagrams.push_back(built.parity);
    fec::Decoder decoder(64);
    auto published = decode_datagrams(datagrams, decoder);
    assert(published.size() == k);
    assert(contains_frame(published, frames[drop]));
    assert(decoder.counters().recovered == 1);
  }
  printf("test_fec_single_drop_each_index OK\n");
}

static void test_fec_two_drops_unrecoverable() {
  const uint16_t k = 8;
  std::vector<std::vector<uint8_t>> frames;
  fec::BuiltGeneration built = build_generation(k, 30, frames, k);
  std::vector<std::vector<uint8_t>> datagrams;
  for (uint16_t i = 2; i < k; ++i) datagrams.push_back(built.data[i]);
  datagrams.push_back(built.parity);
  fec::Decoder decoder(1);
  auto published = decode_datagrams(datagrams, decoder);
  decoder.retire_all();
  assert(published.size() == k - 2);
  assert(decoder.counters().recovered == 0);
  assert(decoder.counters().unrecoverable_gens == 1);
  printf("test_fec_two_drops_unrecoverable OK\n");
}

static void test_fec_drop_parity() {
  const uint16_t k = 8;
  std::vector<std::vector<uint8_t>> frames;
  fec::BuiltGeneration built = build_generation(k, 40, frames, k);
  fec::Decoder decoder(64);
  auto published = decode_datagrams(built.data, decoder);
  assert(published.size() == k);
  assert(decoder.counters().recovered == 0);
  printf("test_fec_drop_parity OK\n");
}

static void test_fec_partial_generation_recovers() {
  std::vector<std::vector<uint8_t>> frames;
  fec::BuiltGeneration built = build_generation(8, 50, frames, 3);
  std::vector<std::vector<uint8_t>> datagrams{built.data[0], built.data[2], built.parity};
  fec::Decoder decoder(64);
  auto published = decode_datagrams(datagrams, decoder);
  assert(published.size() == 3);
  assert(contains_frame(published, frames[1]));
  assert(decoder.counters().recovered == 1);
  printf("test_fec_partial_generation_recovers OK\n");
}

static void test_fec_out_of_order_recovers() {
  const uint16_t k = 8;
  std::vector<std::vector<uint8_t>> frames;
  fec::BuiltGeneration built = build_generation(k, 60, frames, k);
  std::vector<std::vector<uint8_t>> datagrams{built.parity};
  for (int i = k - 1; i >= 0; --i) {
    if (i != 2) datagrams.push_back(built.data[static_cast<size_t>(i)]);
  }
  fec::Decoder decoder(64);
  auto published = decode_datagrams(datagrams, decoder);
  assert(published.size() == k);
  assert(contains_frame(published, frames[2]));
  assert(decoder.counters().recovered == 1);
  printf("test_fec_out_of_order_recovers OK\n");
}

static void test_fec_rejects_invalid_envelopes() {
  auto frame = make_trade(1, 0);
  auto datagram = fec::data_datagram(1, 0, 8, frame.data(),
                                    static_cast<uint16_t>(frame.size()));
  auto header = fec::read_envelope(datagram.data());
  auto* envelope = &header;
  envelope->k = 0;
  fec::write_envelope(datagram.data(), header);
  fec::Decoder decoder(4);
  assert(!decoder.receive(datagram.data(), static_cast<uint32_t>(datagram.size()),
                          100, [](const uint8_t*, uint32_t) { return true; }));

  envelope->k = fec::kMaxGeneration + 1;
  fec::write_envelope(datagram.data(), header);
  assert(!decoder.receive(datagram.data(), static_cast<uint32_t>(datagram.size()),
                          100, [](const uint8_t*, uint32_t) { return true; }));
  printf("test_fec_rejects_invalid_envelopes OK\n");
}


static void test_wire_golden_and_roundtrip() {
  msg::Trade trade{};
  trade.header={0x0102030405060708ull,9,1,msg::kVersion,sizeof(trade),11,12};
  trade.price=1.0; trade.price_ticks=-123; trade.quantity_lots=INT64_MIN;
  std::memcpy(trade.symbol,"BTC",3);
  auto bytes=wire::encode(&trade,sizeof(trade));
  const uint8_t prefix[]={0x50,0x55,0x4c,0x33,1,2,3,4,5,6,7,8};
  assert(std::equal(std::begin(prefix),std::end(prefix),bytes.begin()));
  assert(bytes.size()==200);
  // Canonical binary64 1.0: field offset fixed independently of ABI.
  assert(bytes[132]==0x3f && bytes[133]==0xf0);
  msg::Trade decoded{}; uint32_t length=0;
  assert(wire::decode(bytes.data(),bytes.size(),&decoded,length));
  assert(decoded.header.seq_id==trade.header.seq_id && decoded.price==1.0);
  assert(decoded.price_ticks==-123 && decoded.quantity_lots==INT64_MIN);
  assert(wire::encode(&decoded,length)==bytes);
  for(size_t n=0;n<bytes.size();++n)
    assert(!wire::decode(bytes.data(),n,&decoded,length));
  auto corrupt=bytes; corrupt[0]=0;
  assert(!wire::decode(corrupt.data(),corrupt.size(),&decoded,length));
  corrupt=bytes; corrupt[23]=99;
  assert(!wire::decode(corrupt.data(),corrupt.size(),&decoded,length));
  msg::Bbo bbo{}; bbo.header={5,6,2,msg::kVersion,sizeof(bbo),11,12};
  bbo.ask_price=987.25; bbo.bid_size_lots=-999;
  auto bb=wire::encode(&bbo,sizeof(bbo)); msg::Bbo bbo_out{};
  assert(wire::decode(bb.data(),bb.size(),&bbo_out,length));
  assert(bbo_out.ask_price==bbo.ask_price && bbo_out.bid_size_lots==-999);
  msg::OrderBook book{}; book.header={7,8,3,msg::kVersion,sizeof(book),11,12};
  book.asks[4].size=42.5; book.bids[3].order_count=123;
  auto bk=wire::encode(&book,sizeof(book)); msg::OrderBook book_out{};
  assert(bk.size()==540);
  assert(wire::decode(bk.data(),bk.size(),&book_out,length));
  assert(book_out.asks[4].size==42.5 && book_out.bids[3].order_count==123);
  printf("test_wire_golden_and_roundtrip OK\n");
}

static void test_fec_multiple_erasures() {
  for(unsigned mask=0;mask<(1u<<9);++mask) {
    if(__builtin_popcount(mask)!=3) continue;
    fec::Encoder encoder(6,3);
    std::vector<std::vector<uint8_t>> source;
    for(int i=0;i<6;++i) {
      source.push_back(make_trade(i+1,i));
      assert(encoder.add(source.back().data(),source.back().size()));
    }
    auto built=encoder.close(7,0);
    auto packets=built.data; packets.push_back(built.parity);
    packets.insert(packets.end(),built.extra_parity.begin(),built.extra_parity.end());
    std::vector<std::vector<uint8_t>> delivered;
    for(int i=8;i>=0;--i) if(!(mask&(1u<<i))) delivered.push_back(packets[i]);
    fec::Decoder decoder;
    auto output=decode_datagrams(delivered,decoder);
    for(auto& frame:source) assert(contains_frame(output,frame));
  }
  // Partial generation, all data missing, recovered solely from parity.
  fec::Encoder partial(8,3);
  auto frame=make_trade(19,2);
  partial.add(frame.data(),frame.size()); partial.add(frame.data(),frame.size());
  auto built=partial.close(22,1);
  fec::Decoder decoder;
  auto output=decode_datagrams(built.extra_parity,decoder);
  assert(output.size()==2 && output[0]==frame && output[1]==frame);
  assert(decoder.counters().recovered==2);
  printf("test_fec_multiple_erasures OK\n");
}


static void test_fec_conflicting_closure_does_not_poison() {
  fec::Encoder encoder(4,2);
  std::vector<std::vector<uint8_t>> source;
  for(int i=0;i<4;++i) {
    source.push_back(make_trade(i+1,0));
    encoder.add(source.back().data(),source.back().size());
  }
  auto built=encoder.close(42,0);
  fec::Decoder decoder;
  auto output=decode_datagrams({built.parity,built.data[0]},decoder);
  auto bad=built.extra_parity[0];
  auto header=fec::read_envelope(bad.data()); header.k=2; header.index=3;
  fec::write_envelope(bad.data(),header);
  assert(!decoder.receive(bad.data(),bad.size(),200,[](const uint8_t*,uint32_t){return true;}));
  auto rest=decode_datagrams({built.data[1],built.extra_parity[0]},decoder);
  assert(contains_frame(rest,source[2]) && contains_frame(rest,source[3]));
  printf("test_fec_conflicting_closure_does_not_poison OK\n");
}


struct TestRegion {
  void* data;
  size_t size;
  explicit TestRegion(uint32_t slots):size(shm::region_size(slots)) {
    data=mmap(nullptr,size,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0);
    assert(data!=MAP_FAILED);
  }
  ~TestRegion(){munmap(data,size);}
};
static void test_ring_concurrent_overwrite_integrity() {
  TestRegion region(4);
  shm::Ring writer,reader;writer.attach(region.data,4,true);writer.set_backpressure(false);
  reader.attach(region.data,4,false);
  std::atomic<bool> done{false};
  std::thread thread([&] {
    for(uint64_t n=1;n<=30000;++n) {
      uint64_t frame[32];std::fill(std::begin(frame),std::end(frame),n);
      writer.publish(frame,sizeof(frame));
    }
    done.store(true);
  });
  uint64_t index=0,ok=0;
  while(!done.load() || index<writer.live_edge()) {
    uint64_t frame[32],resume=0;uint32_t len=0;
    auto status=reader.read(index,frame,&len,&resume);
    if(status==shm::Ring::FrameStatus::kOk) {
      assert(len==sizeof(frame));
      for(auto value:frame) assert(value==index+1);
      ++index;++ok;
    } else if(status==shm::Ring::FrameStatus::kLapped) index=resume;
    else assert(status==shm::Ring::FrameStatus::kEmpty);
  }
  thread.join();assert(ok>0);
  printf("test_ring_concurrent_overwrite_integrity OK\n");
}
static void test_ring_backpressure_and_dead_lock_owner() {
  TestRegion region(2);
  shm::Ring writer,reader;writer.attach(region.data,2,true);reader.attach(region.data,2,false);
  uint64_t frame=1;writer.publish(&frame,8);writer.publish(&frame,8);
  std::atomic<bool> published{false};
  std::thread blocked([&]{writer.publish(&frame,8);published.store(true);});
  std::this_thread::sleep_for(std::chrono::milliseconds(10));assert(!published.load());
  uint64_t out,resume=0;uint32_t len=0;
  assert(reader.read(0,&out,&len,&resume)==shm::Ring::FrameStatus::kOk);
  blocked.join();assert(published.load());
  // A dead process holding a read-side slot mutex must not wedge the ring.
  auto* slots=reinterpret_cast<shm::Slot*>(static_cast<uint8_t*>(region.data)+sizeof(shm::Header));
  pid_t child=fork();assert(child>=0);
  if(child==0) {pthread_mutex_lock(&slots[1].mutex);_exit(0);}
  int status=0;assert(waitpid(child,&status,0)==child);
  assert(reader.read(1,&out,&len,&resume)==shm::Ring::FrameStatus::kOk);
  printf("test_ring_backpressure_and_dead_lock_owner OK\n");
}
static void test_bounded_metrics_and_stream_isolation() {
  metrics::Accumulator acc(100000000);
  for(uint64_t i=1;i<=200000;++i) acc.record(i,i);
  assert(acc.retained_samples()==metrics::kSampleLimit);
  auto report=acc.report();assert(report.received==200000 && report.lat_max==200000 && report.lat_min==1);
  multiplex::Streams streams(2,4,64);
  auto* a=streams.get({1,1});auto* b=streams.get({2,1});
  assert(a && b && a!=b);
  assert(a->dedupe.observe(1)==dedupe::ObserveResult::kAccept);
  assert(b->dedupe.observe(1)==dedupe::ObserveResult::kAccept);
  assert(!streams.get({3,1}));
  assert(streams.get({1,1})->dedupe.observe(1)==dedupe::ObserveResult::kTooOld ||
         a->dedupe.counters().duplicates+a->dedupe.counters().too_old>0);
  printf("test_bounded_metrics_and_stream_isolation OK\n");
}

int main() {
  test_ring_concurrent_overwrite_integrity();
  test_ring_backpressure_and_dead_lock_owner();
  test_bounded_metrics_and_stream_isolation();
  test_fec_conflicting_closure_does_not_poison();
  test_wire_golden_and_roundtrip();
  test_fec_multiple_erasures();
  test_metrics_basic();
  test_metrics_drops();
  test_ring_roundtrip();
  test_ring_lapping();
  test_frame_roundtrip_preserves_header();
  test_frame_validation();
  test_ring_rejects_invalid_metadata_and_lengths();
  test_dedupe_in_order();
  test_dedupe_swapped_adjacent();
  test_dedupe_delayed_w_minus_one();
  test_dedupe_delayed_w_plus_one_too_old();
  test_dedupe_duplicate();
  test_dedupe_gap_lost_once();
  test_dedupe_base_advances_full_window();
  test_fec_disabled_raw_stream();
  test_fec_single_drop_each_index();
  test_fec_two_drops_unrecoverable();
  test_fec_drop_parity();
  test_fec_partial_generation_recovers();
  test_fec_out_of_order_recovers();
  test_fec_rejects_invalid_envelopes();
  printf("ALL TESTS PASSED\n");
  return 0;
}
