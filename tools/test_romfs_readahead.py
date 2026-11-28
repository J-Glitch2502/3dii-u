#!/usr/bin/env python3
"""Run the actual direct-read loop with deterministic backing storage."""
from pathlib import Path
import os,re,shlex,subprocess,tempfile
ROOT=Path(__file__).resolve().parents[1]
PRE=r'''#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <random>
#include <vector>
#include "direct_source.h"
using u8=uint8_t;using u32=uint32_t;using u64=uint64_t;
#define OSReport(...) ((void)0)
constexpr u32 CHUNK=1<<20;
std::vector<u8> backing(3*CHUNK+17), buffer(CHUNK);
u8 *chunk=buffer.data();bool on=true,read_ahead=true,fail=false;
u64 source_offset=0,source_size=backing.size(),cached_at,previous_end=~0ULL;
u32 cached_len,calls;
std::mutex read_m;
bool direct_read(void*,u32,u64,bool *cached=nullptr);
u64 slept;struct IPC {int lock=0;int *lk=&lock;};
void sleep_current(u64 ns,int&){slept=ns;}
bool read_chunk(u32 n,u32 at){calls++;assert(n<=CHUNK && at<=backing.size() && n<=backing.size()-at);if(fail){buffer[0]=0;return false;}memcpy(chunk,backing.data()+at,n);return true;}
'''
MAIN=r'''
void verify(u64 off,u32 n){std::vector<u8> out(n);assert(direct_read(out.data(),n,off));assert(!memcmp(out.data(),backing.data()+off,n));}
void reset(bool enabled){on=true;fail=false;read_ahead=enabled;cached_len=0;cached_at=0;previous_end=~0ULL;calls=0;}
int main(){
 for(size_t i=0;i<backing.size();i++)backing[i]=(u8)(i*31+(i>>9));
 reset(false);for(int i=0;i<20;i++)verify(i*65536,65536);assert(calls==20);
 reset(true);for(int i=0;i<20;i++)verify(i*65536,65536);assert(calls==6);
 reset(true);bool cached=true;std::vector<u8> probe(65536);
 assert(direct_read(probe.data(),65536,0,&cached) && !cached);
 assert(direct_read(probe.data(),65536,65536,&cached) && !cached);
 assert(direct_read(probe.data(),65536,131072,&cached) && cached);
 IPC ipc;const bool delays=atoi(getenv("RECOMP_FS_DELAY"))!=0;
 assert(romfs_read_delay(ipc,65536,0,false)==(delays ? 6743162u : 0u));
 assert(romfs_read_delay(ipc,65536,1000,true)==(delays ? 662124u : 0u));
 assert(romfs_read_delay(ipc,65536,1000000,true)==0);
 assert(romfs_read_delay(ipc,65536,20000000,false)==0);
 // Seek, cached range, crossing a cache boundary, multi-chunk and EOF reads.
 verify(17,99);verify(116,300000);verify(300110,400);verify(0,2*CHUNK+3);
 verify(backing.size()-100,100);verify(backing.size(),0);
 std::vector<u8> out(100);assert(!direct_read(out.data(),100,backing.size()-99));
 std::mt19937 rng(123);
 for(int i=0;i<2000;i++){u32 n=rng()%300000;u32 off=rng()%(backing.size()-n+1);verify(off,n);}
 // A failed physical read invalidates the cache and disables direct IO so the
 // caller can retry the entire request through the ordinary filesystem.
 reset(true);verify(0,65536);fail=true;assert(!direct_read(out.data(),100,65536));assert(!on && cached_len==0);
 puts("PASS: actual RomFS read loop preserves bytes across cache hits, seeks, boundaries, EOF and failure; 20 sequential reads use 6 physical reads");
}
'''
def main():
 text=(ROOT/'port/wiiu/direct_read.cpp').read_text();start=text.index('bool direct_read(void *dst');end=text.index('\n}  // namespace wiiu',start);loop=text[start:end]
 with tempfile.TemporaryDirectory(prefix='3dii_romfs_cache_') as d:
  p=Path(d)/'test.cpp';fs=(ROOT/'third_party/3dsrecomp/runtime/services/fs.cpp').read_text();a=fs.index('static u64 romfs_read_delay(');b=fs.index('\n}\n',a)+3;delay=fs[a:b];p.write_text(PRE+loop+delay+MAIN);exe=Path(d)/'test'
  subprocess.run(shlex.split(os.environ.get('CXX','g++'))+['-std=c++20','-O2','-I'+str(ROOT/'port/wiiu'),str(p),'-o',str(exe)],check=True)
  for mode in ('1','0'):
   subprocess.run([str(exe)],check=True,env=dict(os.environ,RECOMP_FS_DELAY=mode))
if __name__=='__main__':main()
