#pragma once

#include <chrono>
#include <csignal>
#include <cstdint>

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

inline volatile std::sig_atomic_t stop_requested = 0;

inline void request_stop(int) { stop_requested = 1; }

inline void install_signal_handlers() {
  std::signal(SIGINT, request_stop);
  std::signal(SIGTERM, request_stop);
}

inline bool should_stop() { return stop_requested != 0; }

inline bool is_power_of_two(uint64_t value) {
  return value != 0 && (value & (value - 1)) == 0;
}

}
