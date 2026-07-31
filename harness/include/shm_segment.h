// Create / open a POSIX shared-memory segment and mmap it. RAII wrapper.
#pragma once

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace shm {

class Segment {
 public:
  // Create (producer) or open (reader) the named segment. On failure, prints to
  // stderr and exits -- these are simple CLI tools.
  static Segment open(const std::string& name, size_t size, bool create) {
    int flags = create ? (O_CREAT | O_RDWR) : O_RDWR;
    int fd = shm_open(name.c_str(), flags, 0600);
    if (fd < 0) {
      fprintf(stderr, "shm_open(%s) failed: %s\n", name.c_str(),
              std::strerror(errno));
      std::exit(1);
    }
    if (create && ftruncate(fd, static_cast<off_t>(size)) != 0) {
      fprintf(stderr, "ftruncate failed: %s\n", std::strerror(errno));
      std::exit(1);
    }
    void* base = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) {
      fprintf(stderr, "mmap failed: %s\n", std::strerror(errno));
      std::exit(1);
    }
    return Segment(name, fd, base, size);
  }

  void* base() const { return base_; }

  void unlink() { shm_unlink(name_.c_str()); }

  ~Segment() {
    if (base_ && base_ != MAP_FAILED) munmap(base_, size_);
    if (fd_ >= 0) close(fd_);
  }

  Segment(Segment&& o) noexcept
      : name_(std::move(o.name_)), fd_(o.fd_), base_(o.base_), size_(o.size_) {
    o.fd_ = -1;
    o.base_ = nullptr;
  }
  Segment(const Segment&) = delete;
  Segment& operator=(const Segment&) = delete;

 private:
  Segment(std::string name, int fd, void* base, size_t size)
      : name_(std::move(name)), fd_(fd), base_(base), size_(size) {}

  std::string name_;
  int fd_ = -1;
  void* base_ = nullptr;
  size_t size_ = 0;
};

}  // namespace shm
