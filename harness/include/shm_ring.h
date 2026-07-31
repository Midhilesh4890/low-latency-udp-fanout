// Shared-memory broadcast ring buffer (single producer, one or more readers).
//
// The producer never blocks on a reader: once the ring is full it overwrites
// the oldest slot. A reader that falls too far behind is "lapped" -- it detects
// the overwrite via the per-slot sequence number and skips ahead. That is the
// drop mechanism, modelling the 0.1-1% loss the task assumes on the channel.
//
// Publication is one release-store of the slot sequence; readers spin on an
// acquire-load. No locks, no syscalls on the hot path.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "message.h"

namespace shm {

inline constexpr uint32_t kFrameCap = msg::kMaxFrame;

inline constexpr uint32_t kCacheLine = 64;

struct alignas(kCacheLine) Slot {
  // Publication sequence for this slot; 0 means "never written". The producer
  // writes the frame, then release-stores seq = write_index + 1.
  std::atomic<uint64_t> seq;
  uint32_t frame_len;
  uint8_t frame[kFrameCap];
};

struct alignas(kCacheLine) Header {
  uint32_t magic;
  uint32_t slot_count;  // power of two
  uint64_t slot_size;
  alignas(kCacheLine) std::atomic<uint64_t> write_index;
};

inline constexpr uint32_t kMagic = 0x53484d31;  // "SHM1"

inline size_t region_size(uint32_t slots) {
  return sizeof(Header) + static_cast<size_t>(slots) * sizeof(Slot);
}

// A thin view over an already-mapped region; does not own the mapping.
class Ring {
 public:
  Ring() = default;

  // Producer passes init=true to (re)initialise the header; readers pass false.
  void attach(void* base, uint32_t slots, bool init) {
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
    }
    mask_ = header_->slot_count - 1;
  }

  uint32_t slot_count() const { return header_->slot_count; }

  void publish(const void* frame, uint32_t len) {
    const uint64_t idx = header_->write_index.load(std::memory_order_relaxed);
    Slot& s = slots_[idx & mask_];
    s.frame_len = len;
    std::memcpy(s.frame, frame, len);
    // Release so the frame writes are visible before the seq flip. seq is idx+1
    // so 0 stays reserved for "never written".
    s.seq.store(idx + 1, std::memory_order_release);
    header_->write_index.store(idx + 1, std::memory_order_release);
  }

  // The producer's live write edge -- where a fresh reader should start.
  uint64_t live_edge() const {
    return header_->write_index.load(std::memory_order_acquire);
  }

  enum class FrameStatus { kOk, kEmpty, kLapped };

  // Try to read the slot at logical position read_index. On kLapped, resume_at
  // gives a safe position to jump to.
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
    std::memcpy(out, s.frame, len);
    // Re-check seq: if the producer began overwriting mid-copy, we were lapped.
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

}  // namespace shm
