"""Exercise actual range cache against changed bytes, aliases, rewind and budget."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1]
source=r'''
#include <cassert>
#include <random>
#include <vector>
#include <algorithm>
#include "index_ranges.h"
int main(){
 gx2_indices::Ranges cache;
 std::mt19937 random(77);
 for(bool wide:{false,true})for(unsigned trial=0;trial<1000;trial++){
  unsigned n=1+random()%3000;
  std::vector<uint8_t> data(n*(wide?2:1));for(auto &v:data)v=random();
  unsigned low=65535,high=0;
  for(unsigned i=0;i<n;i++){unsigned v=wide?data[2*i]|(unsigned(data[2*i+1])<<8):data[i];low=std::min(low,v);high=std::max(high,v);}
  for(unsigned offset=0;offset<4;offset++) {
   std::vector<uint8_t> unaligned(offset);unaligned.insert(unaligned.end(),data.begin(),data.end());
   for(bool prefetch:{false,true}) {
    auto fast=gx2_indices::scan(unaligned.data()+offset,n,wide,prefetch);
    assert(fast.first==low&&fast.last==high);
   }
  }
  unsigned address=0x20000000+(trial%61)*4096;
  auto a=cache.get(address,data.data(),n,wide);assert(a.first==low&&a.last==high&&!a.reused);
  auto b=cache.get(address,data.data(),n,wide);assert(b.first==low&&b.last==high&&b.reused);
  auto saved=data;data[0]^=255;
  auto changed=cache.get(address,data.data(),n,wide);assert(!changed.reused);
  auto restored=cache.get(address,saved.data(),n,wide);assert(!restored.reused&&restored.first==low&&restored.last==high);
  assert(cache.bytes()<=cache.budget);
 }
 assert(cache.get(0,nullptr,0,true).last==0);
 std::vector<uint8_t> huge(cache.max_entry+1,42);
 auto h=cache.get(7,huge.data(),huge.size(),false);assert(!h.reused&&h.first==42&&h.last==42);
 assert(!cache.get(7,huge.data(),huge.size(),false).reused);
 for(unsigned i=0;i<1500;i++){
  std::vector<uint8_t> data(16000+i%50000,uint8_t(i));
  cache.get(0x21000000+i*16,data.data(),data.size(),false);assert(cache.bytes()<=cache.budget);
 }
}
'''
with tempfile.TemporaryDirectory() as tmp:
 p=Path(tmp);(p/'test.cpp').write_text(source)
 subprocess.run(['c++','-std=c++20','-O2','-fsanitize=address,undefined','-I'+str(root/'port/wiiu'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('PASS: exact bounds, immediate byte changes, rewind, collisions, empty/oversized inputs and bounded storage')
