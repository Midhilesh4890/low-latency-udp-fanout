#pragma once

#include <chrono>
#include <atomic>
#include <csignal>
#include <cstdint>
#include <thread>
#include <cstdlib>
#include <algorithm>

namespace util {

inline uint64_t now_ns() {
  using namespace std::chrono;
  return static_cast<uint64_t>(
      duration_cast<nanoseconds>(system_clock::now().time_since_epoch())
          .count());
}

inline uint64_t steady_now_ns() {
  using namespace std::chrono;
  return static_cast<uint64_t>(
      duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count());
}

static_assert(std::atomic<bool>::is_always_lock_free,"signal flag must be lock-free");
inline std::atomic<bool> stop_requested{false};

inline void request_stop(int) { stop_requested.store(true,std::memory_order_relaxed); }

inline void install_signal_handlers() {
  std::signal(SIGINT, request_stop);
  std::signal(SIGTERM, request_stop);
}

inline bool should_stop() { return stop_requested.load(std::memory_order_relaxed); }

inline void idle_wait(uint64_t last_progress) {
  static const uint64_t spin_us=[] {
    const char* p=std::getenv("PULSEFANOUT_SPIN_US");
    return p ? std::min<uint64_t>(std::strtoull(p,nullptr,10),1000000) : 50;
  }();
  static const uint64_t sleep_us=[] {
    const char* p=std::getenv("PULSEFANOUT_SLEEP_US");
    return p ? std::min<uint64_t>(std::strtoull(p,nullptr,10),10000) : 50;
  }();
  if(sleep_us && steady_now_ns()-last_progress>=spin_us*1000)
    std::this_thread::sleep_for(std::chrono::microseconds(sleep_us));
}

inline bool is_power_of_two(uint64_t value) {
  return value != 0 && (value & (value - 1)) == 0;
}

}
