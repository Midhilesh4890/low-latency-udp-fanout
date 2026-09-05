#pragma once
#include <map>
#include <memory>
#include "wire.h"
#include "fec.h"
#include "dedupe_window.h"
namespace multiplex {
inline constexpr uint32_t magic=0x50465333; // PFS3
inline constexpr size_t header_size=20;
struct Key {
  uint64_t stream=0,epoch=0;
  bool operator<(const Key& other) const {
    return stream<other.stream || (stream==other.stream && epoch<other.epoch);
  }
};
inline std::vector<uint8_t> wrap(Key key,const uint8_t* data,uint32_t len) {
  wire::Writer w;auto tag=magic;w(tag,key.stream,key.epoch);
  w.bytes.insert(w.bytes.end(),data,data+len);return w.bytes;
}
inline bool unwrap(const uint8_t*& data,uint32_t& len,Key& key) {
  if(len<header_size) return false;
  wire::Reader r{data,len};uint32_t tag;r(tag,key.stream,key.epoch);
  if(tag!=magic || !key.stream || !key.epoch) return false;
  data+=header_size;len-=header_size;return true;
}
struct Stream {
  fec::Decoder decoder;
  dedupe::Window dedupe;
  uint64_t last_seq=0;
  explicit Stream(uint32_t generations,uint64_t window):decoder(generations),dedupe(window){}
};
// Never evict a live identity and accidentally accept old replay as new.
// At the limit new identities are rejected until the operator starts a new run.
class Streams {
  std::map<Key,std::unique_ptr<Stream>> values_;
  size_t limit_;
  uint32_t generations_;
  uint64_t window_;
 public:
  Streams(size_t limit,uint32_t generations,uint64_t window):
    limit_(limit),generations_(generations),window_(window) {}
  Stream* get(Key key) {
    auto it=values_.find(key);
    if(it!=values_.end()) return it->second.get();
    if(values_.size()>=limit_) return nullptr;
    return values_.emplace(key,std::make_unique<Stream>(generations_,window_)).first->second.get();
  }
  auto& values() {return values_;}
};
}
