#pragma once

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <chrono>
#include <thread>
#include <stdexcept>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace shm {

class Segment {
 public:


  static Segment open(const std::string& name,size_t size,bool create) {
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    int fd=-1;
    do {
      fd=shm_open(name.c_str(),create?(O_CREAT|O_EXCL|O_RDWR):O_RDWR,0600);
      if(fd>=0 || create || errno!=ENOENT) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while(std::chrono::steady_clock::now()<deadline);
    if(fd<0) throw std::runtime_error("shm_open("+name+"): "+std::strerror(errno));
    void* base=MAP_FAILED;
    try {
      for(;;) {
        if(flock(fd,(create?LOCK_EX:LOCK_SH)|LOCK_NB)==0) {
          struct stat info{};
          if(fstat(fd,&info)) throw std::runtime_error("shared-memory fstat failed");
          if(create || info.st_size!=0) {
            if(!create && static_cast<size_t>(info.st_size)!=size)
              throw std::runtime_error("shared-memory size mismatch");
            break;
          }
          flock(fd,LOCK_UN);
        } else if(errno!=EWOULDBLOCK && errno!=EINTR) {
          throw std::runtime_error("shared-memory initialization lock failed");
        }
        if(std::chrono::steady_clock::now()>=deadline)
          throw std::runtime_error("shared-memory initialization timed out");
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
      if(create && ftruncate(fd,static_cast<off_t>(size)))
        throw std::runtime_error("shared-memory ftruncate failed");
      base=mmap(nullptr,size,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
      if(base==MAP_FAILED) throw std::runtime_error("shared-memory mmap failed");
      if(!create) flock(fd,LOCK_UN);
      return Segment(name,fd,base,size,create);
    } catch(...) {
      if(base!=MAP_FAILED) munmap(base,size);
      close(fd);
      if(create) shm_unlink(name.c_str());
      throw;
    }
  }

  void* base() const { return base_; }

  void ready() { if(owner_) flock(fd_,LOCK_UN); }

  void unlink() { if(owner_) { shm_unlink(name_.c_str()); owner_=false; } }

  ~Segment() {
    unlink();
    if (base_ && base_ != MAP_FAILED) munmap(base_, size_);
    if (fd_ >= 0) close(fd_);
  }

  Segment(Segment&& o) noexcept
      : name_(std::move(o.name_)), fd_(o.fd_), base_(o.base_), size_(o.size_), owner_(o.owner_) {
    o.owner_=false;
    o.fd_ = -1;
    o.base_ = nullptr;
  }
  Segment(const Segment&) = delete;
  Segment& operator=(const Segment&) = delete;

 private:
  Segment(std::string name, int fd, void* base, size_t size, bool owner)
      : name_(std::move(name)), fd_(fd), base_(base), size_(size), owner_(owner) {}

  std::string name_;
  int fd_ = -1;
  void* base_ = nullptr;
  size_t size_ = 0;
  bool owner_ = false;
};

}
