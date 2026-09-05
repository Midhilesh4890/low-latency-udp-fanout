#pragma once
#include <sys/socket.h>
#include <sys/un.h>
#include <poll.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include "util.h"
namespace transport {
inline int unix_socket(const std::string& path,bool listener) {
  sockaddr_un address{}; address.sun_family=AF_UNIX;
  if(path.empty() || path.size()>=sizeof(address.sun_path))
    throw std::invalid_argument("invalid Unix socket path");
  std::memcpy(address.sun_path,path.c_str(),path.size()+1);
  int fd=socket(AF_UNIX,SOCK_STREAM,0);
  if(fd<0) throw std::runtime_error("Unix socket creation failed");
  int rc=listener ? bind(fd,reinterpret_cast<sockaddr*>(&address),sizeof(address))
                  : connect(fd,reinterpret_cast<sockaddr*>(&address),sizeof(address));
  if(rc<0 || (listener && listen(fd,1)<0)) {
    close(fd); throw std::runtime_error("Unix socket bind/connect failed");
  }
  return fd;
}
// One fixed deadline covers the complete frame, including partial I/O.
inline bool transfer(int fd,uint8_t* data,size_t size,bool sending,uint64_t deadline) {
  while(size && !util::should_stop()) {
    if(util::steady_now_ns()>=deadline) return false;
    pollfd event{fd,static_cast<short>(sending?POLLOUT:POLLIN),0};
    int rc=poll(&event,1,10);
    if(rc<0 && errno==EINTR) continue;
    if(rc<0) return false;
    if(!rc) continue;
    ssize_t n=sending ? send(fd,data,size,MSG_DONTWAIT|MSG_NOSIGNAL)
                      : recv(fd,data,size,MSG_DONTWAIT);
    if(n<0 && (errno==EAGAIN || errno==EWOULDBLOCK || errno==EINTR)) continue;
    if(n<=0) return false;
    data+=n;size-=static_cast<size_t>(n);
  }
  return size==0;
}
inline bool send_frame(int fd,const uint8_t* data,uint32_t len) {
  uint8_t prefix[2]{static_cast<uint8_t>(len>>8),static_cast<uint8_t>(len)};
  const auto deadline=util::steady_now_ns()+2000000000ull;
  return transfer(fd,prefix,2,true,deadline) &&
         transfer(fd,const_cast<uint8_t*>(data),len,true,deadline);
}
inline int receive_frame(int fd,uint8_t* data,uint32_t cap,uint64_t timeout_ns) {
  uint8_t prefix[2];
  const auto deadline=util::steady_now_ns()+timeout_ns;
  if(!transfer(fd,prefix,2,false,deadline)) return -1;
  uint32_t len=(static_cast<uint32_t>(prefix[0])<<8)|prefix[1];
  if(!len || len>cap || !transfer(fd,data,len,false,deadline)) return -1;
  return static_cast<int>(len);
}
}
