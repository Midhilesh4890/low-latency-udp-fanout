#pragma once
#include <cstdint>
#include <vector>
namespace metrics {
inline constexpr size_t kSampleLimit=65536;
inline void sample(std::vector<uint64_t>& values,uint64_t value,uint64_t seen) {
  if(values.size()<kSampleLimit) {values.push_back(value);return;}
  uint64_t x=seen+0x9e3779b97f4a7c15ull;
  x=(x^(x>>30))*0xbf58476d1ce4e5b9ull;
  x=(x^(x>>27))*0x94d049bb133111ebull;
  x^=x>>31;
  const auto index=x%seen;
  if(index<values.size()) values[index]=value;
}
}
