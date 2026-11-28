#include "direct_source.h"
#include <vector>
#include <cassert>
#include <fstream>
#include <iterator>
void word(std::vector<uint8_t> &b, size_t at, uint64_t n) {
    for (int i = 0; i < 8; ++i) b[at + i] = n >> (56 - i * 8);
}
int main(int argc, char **argv) {
    const char path[] = "/vol/external01/wiiu/apps/Any Homebrew.wuhb";
    std::vector<uint8_t> b(32);std::memcpy(b.data(),"DROMFS01",8);
    word(b,8,512);word(b,16,1000);word(b,24,2000);
    b.insert(b.end(),path,path+sizeof path);
    direct_source::Source s;
    assert(direct_source::parse(b.data(),b.size(),s));assert(s.offset==512 && s.length==1000);
    for(size_t i=0;i<b.size();++i)assert(!direct_source::parse(b.data(),i,s));
    auto bad=b;word(bad,8,UINT64_MAX);assert(!direct_source::parse(bad.data(),bad.size(),s));
    bad=b;word(bad,16,UINT64_MAX);assert(!direct_source::parse(bad.data(),bad.size(),s));
    bad=b;word(bad,24,uint64_t(UINT32_MAX)+1);assert(!direct_source::parse(bad.data(),bad.size(),s));
    bad=b;bad[40]=0;assert(!direct_source::parse(bad.data(),bad.size(),s));
    assert(direct_source::fits(100,100,0));assert(!direct_source::fits(100,99,2));assert(!direct_source::fits(100,UINT64_MAX,1));
    if(argc>1){std::ifstream f(argv[1],std::ios::binary);std::vector<uint8_t>d((std::istreambuf_iterator<char>(f)),{});assert(direct_source::parse(d.data(),d.size(),s));}
}
