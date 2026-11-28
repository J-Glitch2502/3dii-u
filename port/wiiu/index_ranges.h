#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <bit>
#include <new>

namespace gx2_indices {
struct Range { std::uint32_t first=0,last=0; bool reused=false; };
inline Range scan(const std::uint8_t *source,std::uint32_t count,bool wide,bool prefetch) {
    if (!count) return {};
    std::uint32_t low[4]={65535,65535,65535,65535}, high[4]={};
    auto add=[&](unsigned lane,std::uint32_t value){
        if(value<low[lane])low[lane]=value;
        if(value>high[lane])high[lane]=value;
    };
    auto word=[](const std::uint8_t *p){
        std::uint32_t value;std::memcpy(&value,p,4);
        if constexpr(std::endian::native==std::endian::big) value=__builtin_bswap32(value);
        return value;
    };
    const unsigned stride=wide ? 2u : 1u;
    std::uint32_t at=0;
    // Real index buffers are aligned; retain the byte-safe path for odd inputs.
    while(at<count && (reinterpret_cast<std::uintptr_t>(source+std::size_t(at)*stride)&3u)) {
        add(at&3u,wide ? source[2u*at]|(std::uint32_t(source[2u*at+1])<<8) : source[at]);at++;
        if(wide && (reinterpret_cast<std::uintptr_t>(source)&1u)) break;
    }
    if(!(reinterpret_cast<std::uintptr_t>(source+std::size_t(at)*stride)&3u)) {
        for(;count-at>=16;at+=16) {
            const auto *p=source+std::size_t(at)*stride;
            if(prefetch && std::uint64_t(count-at)*stride>128) __builtin_prefetch(p+128,0,3);
            if(wide) {
                for(unsigned j=0;j<8;j++) { const auto v=word(p+4*j);add(j&3u,v&65535u);add(j&3u,v>>16); }
            } else {
                for(unsigned j=0;j<4;j++) {const auto v=word(p+4*j);add(0,v&255u);add(1,(v>>8)&255u);add(2,(v>>16)&255u);add(3,v>>24);}
            }
        }
    }
    for(;at<count;at++) add(at&3u,wide ? source[2u*at]|(std::uint32_t(source[2u*at+1])<<8) : source[at]);
    Range result{low[0],high[0],false};
    for(unsigned j=1;j<4;j++) {if(low[j]<result.first)result.first=low[j];if(high[j]>result.last)result.last=high[j];}
    return result;
}
// Bound the auxiliary CPU storage. A byte-for-byte comparison on every hit
// detects unannounced guest writes and rewinds, unlike a flush-only cache.
class Ranges {
    struct Entry {
        std::uint32_t address=0,count=0;
        bool wide=false;
        Range range;
        std::unique_ptr<std::uint8_t[]> data;
        std::size_t size=0;
    };
    std::array<Entry,512> entries;
    std::size_t held=0;
public:
    static constexpr std::size_t budget=2u<<20, max_entry=64u<<10;
    std::size_t bytes() const { return held; }
    Range get(std::uint32_t address,const std::uint8_t *source,std::uint32_t count,bool wide) {
        if (!count) return {};
        const std::uint64_t size=std::uint64_t(count)*(wide ? 2u : 1u);
        const unsigned slot=((address>>2)^(count*0x9e3779b9u)^(wide ? 0x85ebca6bu : 0u))&511u;
        Entry &entry=entries[slot];
        if (size<=max_entry && entry.address==address && entry.count==count && entry.wide==wide &&
            entry.size==size && !std::memcmp(source,entry.data.get(),std::size_t(size))) {
            auto found=entry.range;found.reused=true;return found;
        }
        Range result{0xffffu,0,false};
        for(std::uint32_t i=0;i<count;i++) {
            const std::uint32_t value=wide ? source[2u*i]|(std::uint32_t(source[2u*i+1])<<8) : source[i];
            if(value<result.first)result.first=value;
            if(value>result.last)result.last=value;
        }
        if(size<=max_entry && held-entry.size+size<=budget) {
            // Exact-size buffers avoid retained vector capacity escaping the
            // budget after a large entry is replaced with a smaller one.
            std::unique_ptr<std::uint8_t[]> data(new(std::nothrow) std::uint8_t[std::size_t(size)]);
            if (!data) return result;
            std::memcpy(data.get(),source,std::size_t(size));
            held=held-entry.size+std::size_t(size);
            entry.data=std::move(data);entry.size=std::size_t(size);
            entry.address=address;entry.count=count;entry.wide=wide;entry.range=result;
        }
        return result;
    }
};
}
