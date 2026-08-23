#pragma once

#include <cstdint>
#include <cstdlib>
#include <vector>

namespace dedupe {

enum class ObserveResult {
  kAccept,
  kDuplicate,
  kTooOld,
  kAdvanced,
};

struct Counters {
  uint64_t accepted = 0;
  uint64_t duplicates = 0;
  uint64_t too_old = 0;
  uint64_t lost_confirmed = 0;
  uint64_t window_slides = 0;
  uint64_t max_reorder_depth = 0;
  uint64_t receiver_seq_jump_raw = 0;
};

class Window {
 public:
  explicit Window(uint64_t size = 65536) : size_(size), mask_(size - 1), bits_(size / 64, 0) {
    if (size == 0 || (size & (size - 1)) != 0 || size < 64) std::abort();
  }

  ObserveResult observe(uint64_t seq) {
    if (!initialized_) {
      initialized_ = true;
      base_seq_ = seq;
    }
    if (seq >= base_seq_) {
      const uint64_t raw_jump = seq - base_seq_;
      if (raw_jump > counters_.receiver_seq_jump_raw) counters_.receiver_seq_jump_raw = raw_jump;
    }
    if (seq < base_seq_) {
      ++counters_.too_old;
      return ObserveResult::kTooOld;
    }
    ObserveResult result = ObserveResult::kAccept;
    if (seq >= base_seq_ + size_) {
      slide_to(seq - size_ + 1);
      ++counters_.window_slides;
      result = ObserveResult::kAdvanced;
    }
    const uint64_t depth = seq - base_seq_;
    if (depth > counters_.max_reorder_depth) counters_.max_reorder_depth = depth;
    if (test(seq)) {
      ++counters_.duplicates;
      return ObserveResult::kDuplicate;
    }
    set(seq);
    ++counters_.accepted;
    advance_contiguous();
    return result;
  }

  uint64_t base_seq() const { return base_seq_; }
  const Counters& counters() const { return counters_; }

 private:
  bool test(uint64_t seq) const {
    const uint64_t pos = seq & mask_;
    return (bits_[pos >> 6] & (uint64_t{1} << (pos & 63))) != 0;
  }

  void set(uint64_t seq) {
    const uint64_t pos = seq & mask_;
    bits_[pos >> 6] |= uint64_t{1} << (pos & 63);
  }

  void clear(uint64_t seq) {
    const uint64_t pos = seq & mask_;
    bits_[pos >> 6] &= ~(uint64_t{1} << (pos & 63));
  }

  void slide_to(uint64_t next_base) {
    while (base_seq_ < next_base) {
      if (!test(base_seq_)) ++counters_.lost_confirmed;
      clear(base_seq_);
      ++base_seq_;
    }
  }

  void advance_contiguous() {
    while (test(base_seq_)) {
      clear(base_seq_);
      ++base_seq_;
    }
  }

  uint64_t size_;
  uint64_t mask_;
  std::vector<uint64_t> bits_;
  uint64_t base_seq_ = 0;
  bool initialized_ = false;
  Counters counters_;
};

}
