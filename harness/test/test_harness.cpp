#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "dedupe_window.h"
#include "fec.h"
#include "message.h"
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
  std::vector<uint8_t> mem(shm::region_size(slots));
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
  std::vector<uint8_t> mem(shm::region_size(slots));
  shm::Ring prod;
  prod.attach(mem.data(), slots, true);
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
  std::vector<uint8_t> mem(shm::region_size(slots));
  shm::Ring prod;
  prod.attach(mem.data(), slots, true);
  shm::Ring cons;
  cons.attach(mem.data(), slots, false);

  msg::Trade frame{};
  frame.header.seq_id = 42;
  frame.header.send_ts_ns = 123456789;
  frame.header.type = static_cast<uint16_t>(msg::Type::Trade);
  frame.header.version = 1;
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
  frame.header.version = 1;
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

int main() {
  test_metrics_basic();
  test_metrics_drops();
  test_ring_roundtrip();
  test_ring_lapping();
  test_frame_roundtrip_preserves_header();
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
  printf("ALL TESTS PASSED\n");
  return 0;
}
