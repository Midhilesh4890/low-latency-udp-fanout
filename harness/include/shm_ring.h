#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <new>
#include <chrono>
#include <pthread.h>
#include <unistd.h>
#include <cerrno>
#include <cstdlib>
#include "message.h"
#include "util.h"

namespace shm {
inline constexpr uint32_t kFrameCap=msg::kMaxFrame;
inline constexpr uint32_t kCacheLine=64;
inline constexpr uint32_t kMaxReaders=64;
inline constexpr uint32_t kMagic=0x53484d31;

inline void init_mutex(pthread_mutex_t& mutex) {
  pthread_mutexattr_t attr;
  if(pthread_mutexattr_init(&attr)) throw std::runtime_error("mutex attributes");
  int rc=pthread_mutexattr_setpshared(&attr,PTHREAD_PROCESS_SHARED);
  if(!rc) rc=pthread_mutexattr_setrobust(&attr,PTHREAD_MUTEX_ROBUST);
  if(!rc) rc=pthread_mutex_init(&mutex,&attr);
  pthread_mutexattr_destroy(&attr);
  if(rc) throw std::runtime_error("process-shared robust mutex unavailable");
}
class Lock {
  pthread_mutex_t* mutex_=nullptr;
 public:
  explicit Lock(pthread_mutex_t& mutex,bool wait=true) {
    const auto deadline=util::steady_now_ns()+2000000000ull;
    for(;;) {
      int rc=pthread_mutex_trylock(&mutex);
      if(rc==0 || rc==EOWNERDEAD) {
        if(rc==EOWNERDEAD && pthread_mutex_consistent(&mutex)) {
          pthread_mutex_unlock(&mutex);
          throw std::runtime_error("unrecoverable shared-memory mutex");
        }
        mutex_=&mutex; return;
      }
      if(rc!=EBUSY) throw std::runtime_error("shared-memory mutex failed");
      if(!wait) return;
      if(util::should_stop() || util::steady_now_ns()>=deadline)
        throw std::runtime_error("shared-memory mutex deadline exceeded");
      std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
  }
  explicit operator bool() const {return mutex_!=nullptr;}
  ~Lock() {if(mutex_) pthread_mutex_unlock(mutex_);}
  Lock(const Lock&)=delete;
};
struct alignas(kCacheLine) Slot {
  pthread_mutex_t mutex;
  uint64_t seq;
  uint32_t frame_len;
  uint8_t frame[kFrameCap];
};
struct ReaderState {
  std::atomic<uint32_t> pid;
  std::atomic<uint64_t> cursor;
};
struct alignas(kCacheLine) Header {
  uint32_t magic;
  uint32_t layout_version;
  uint64_t epoch;
  pthread_mutex_t registry_mutex;
  ReaderState readers[kMaxReaders];
  uint32_t slot_count;
  uint64_t slot_size;
  uint32_t backpressure;
  alignas(kCacheLine) std::atomic<uint64_t> write_index;
};
inline size_t region_size(uint32_t slots) {
  return sizeof(Header)+static_cast<size_t>(slots)*sizeof(Slot);
}
class Ring {
  Header* header_=nullptr;
  Slot* slots_=nullptr;
  uint64_t mask_=0;
  int reader_=-1;
 public:
  Ring()=default;
  Ring(const Ring&)=delete;
  Ring& operator=(const Ring&)=delete;
  void attach(void* base,uint32_t count,bool init) {
    if(!base || !util::is_power_of_two(count) ||
       reinterpret_cast<uintptr_t>(base)%alignof(Header))
      throw std::invalid_argument("ring requires aligned memory and power-of-two slots");
    header_=static_cast<Header*>(base);
    slots_=reinterpret_cast<Slot*>(static_cast<uint8_t*>(base)+sizeof(Header));
    if(init) {
      new(header_) Header{};
      header_->magic=kMagic;header_->layout_version=3;
      header_->epoch=util::steady_now_ns();
      header_->slot_count=count;header_->slot_size=sizeof(Slot);
      const char* policy=std::getenv("PULSEFANOUT_RING_POLICY");
      header_->backpressure=!(policy && std::strcmp(policy,"overwrite")==0);
      init_mutex(header_->registry_mutex);
      for(uint32_t i=0;i<count;++i) {
        new(&slots_[i]) Slot{};
        init_mutex(slots_[i].mutex);
      }
    } else {
      if(header_->magic!=kMagic || header_->layout_version!=3 ||
         header_->slot_count!=count || header_->slot_size!=sizeof(Slot))
        throw std::runtime_error("shared-memory ring metadata mismatch");
      Lock lock(header_->registry_mutex);
      for(uint32_t i=0;i<kMaxReaders;++i) if(!header_->readers[i].pid.load()) {
        header_->readers[i].cursor.store(0);
        header_->readers[i].pid.store(static_cast<uint32_t>(getpid()));
        reader_=static_cast<int>(i);break;
      }
      if(reader_<0) throw std::runtime_error("shared-memory reader limit exceeded");
    }
    mask_=count-1;
  }
  ~Ring() {
    if(reader_>=0) header_->readers[reader_].pid.store(0,std::memory_order_release);
  }
  uint32_t slot_count() const {return header_->slot_count;}
  uint32_t reader_count() const {
    uint32_t count=0;
    for(const auto& r:header_->readers) if(r.pid.load(std::memory_order_acquire)) ++count;
    return count;
  }
  void set_backpressure(bool enabled) {
    if(reader_>=0) throw std::logic_error("only the creator sets ring policy");
    header_->backpressure=enabled;
  }
  uint64_t live_edge() const {return header_->write_index.load(std::memory_order_acquire);}
  void publish(const void* frame,uint32_t len) {
    if(!frame || !len || len>kFrameCap) throw std::length_error("invalid shared-memory frame length");
    if(reader_>=0) throw std::logic_error("ring has one creator/writer");
    const uint64_t idx=live_edge();
    const auto deadline=util::steady_now_ns()+2000000000ull;
    if(header_->backpressure && idx>=slot_count()) {
      for(;;) {
        bool blocked=false, any=false;
        for(const auto& r:header_->readers) if(r.pid.load(std::memory_order_acquire)) {
          any=true;
          if(r.cursor.load(std::memory_order_acquire)<=idx-slot_count()) blocked=true;
        }
        if(any && !blocked) break;
        if(util::should_stop() || util::steady_now_ns()>=deadline)
          throw std::runtime_error("ring backpressure deadline exceeded (slow, absent, or dead reader)");
        std::this_thread::sleep_for(std::chrono::microseconds(50));
      }
    }
    Slot& slot=slots_[idx&mask_]; Lock lock(slot.mutex);
    // Invalidate before copying so recovery after a dead writer cannot expose
    // a partially copied frame. Readers and writers hold the same robust lock.
    slot.seq=0;slot.frame_len=0;
    std::memcpy(slot.frame,frame,len);slot.frame_len=len;slot.seq=idx+1;
    header_->write_index.store(idx+1,std::memory_order_release);
  }
  enum class FrameStatus {kOk,kEmpty,kLapped,kCorrupt};
  FrameStatus read(uint64_t index,void* out,uint32_t* len,uint64_t* resume) {
    Slot& slot=slots_[index&mask_]; Lock lock(slot.mutex,false);
    if(!lock) return FrameStatus::kEmpty;
    if(slot.seq<index+1) return FrameStatus::kEmpty;
    if(slot.seq>index+1) {
      const auto edge=live_edge();
      *resume=edge>slot_count()?edge-slot_count():0;
      return FrameStatus::kLapped;
    }
    if(!slot.frame_len || slot.frame_len>kFrameCap) return FrameStatus::kCorrupt;
    std::memcpy(out,slot.frame,slot.frame_len);*len=slot.frame_len;
    if(reader_>=0) header_->readers[reader_].cursor.store(index+1,std::memory_order_release);
    return FrameStatus::kOk;
  }
};
}
