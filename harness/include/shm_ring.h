#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>

#include "message.h"

namespace shm {

inline constexpr uint32_t kFrameCap = msg::kMaxFrame;

inline constexpr uint32_t kCacheLine = 64;

struct alignas(kCacheLine) Slot {


  std::atomic<uint64_t> seq;
  uint32_t frame_len;
  uint8_t frame[kFrameCap];
};

struct alignas(kCacheLine) Header {
  uint32_t magic;
  uint32_t slot_count;
  uint64_t slot_size;
  alignas(kCacheLine) std::atomic<uint64_t> write_index;
};

inline constexpr uint32_t kMagic = 0x53484d31;

inline size_t region_size(uint32_t slots) {
  return sizeof(Header) + static_cast<size_t>(slots) * sizeof(Slot);
}


class Ring {
 public:
  Ring() = default;


  void attach(void* base, uint32_t slots, bool init) {
    if (base == nullptr || slots == 0 || (slots & (slots - 1)) != 0) {
      throw std::invalid_argument("shared-memory slot count must be a power of two");
    }
    header_ = static_cast<Header*>(base);
    slots_ = reinterpret_cast<Slot*>(static_cast<uint8_t*>(base) +
                                     sizeof(Header));
    if (init) {
      header_->magic = kMagic;
      header_->slot_count = slots;
      header_->slot_size = sizeof(Slot);
      header_->write_index.store(0, std::memory_order_relaxed);
      for (uint32_t i = 0; i < slots; ++i) {
        slots_[i].seq.store(0, std::memory_order_relaxed);
      }
    } else if (header_->magic != kMagic || header_->slot_count != slots ||
               header_->slot_size != sizeof(Slot)) {
      throw std::runtime_error("shared-memory ring metadata mismatch");
    }
    mask_ = header_->slot_count - 1;
  }

  uint32_t slot_count() const { return header_->slot_count; }

  void publish(const void* frame, uint32_t len) {
    if (frame == nullptr || len == 0 || len > kFrameCap) {
      throw std::length_error("invalid shared-memory frame length");
    }
    const uint64_t idx = header_->write_index.load(std::memory_order_relaxed);
    Slot& s = slots_[idx & mask_];
    s.frame_len = len;
    std::memcpy(s.frame, frame, len);


    s.seq.store(idx + 1, std::memory_order_release);
    header_->write_index.store(idx + 1, std::memory_order_release);
  }


  uint64_t live_edge() const {
    return header_->write_index.load(std::memory_order_acquire);
  }

  enum class FrameStatus { kOk, kEmpty, kLapped, kCorrupt };


  FrameStatus read(uint64_t read_index, void* out, uint32_t* out_len,
                   uint64_t* resume_at) {
    Slot& s = slots_[read_index & mask_];
    const uint64_t seq = s.seq.load(std::memory_order_acquire);
    const uint64_t want = read_index + 1;

    if (seq < want) return FrameStatus::kEmpty;
    if (seq > want) {
      const uint64_t edge = live_edge();
      *resume_at = edge > slot_count() ? edge - slot_count() : 0;
      return FrameStatus::kLapped;
    }

    const uint32_t len = s.frame_len;
    if (len == 0 || len > kFrameCap) return FrameStatus::kCorrupt;
    std::memcpy(out, s.frame, len);

    if (s.seq.load(std::memory_order_acquire) != want) {
      const uint64_t edge = live_edge();
      *resume_at = edge > slot_count() ? edge - slot_count() : 0;
      return FrameStatus::kLapped;
    }
    *out_len = len;
    return FrameStatus::kOk;
  }

 private:
  Header* header_ = nullptr;
  Slot* slots_ = nullptr;
  uint64_t mask_ = 0;
};

}
