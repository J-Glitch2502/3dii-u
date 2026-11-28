#!/usr/bin/env python3
"""Exercise production cache trimming without rebuilding every material at once."""
from pathlib import Path
import subprocess
import sys
import tempfile
from test_vertex_cache import function


def main():
    root=Path(__file__).resolve().parents[1]
    sys.path.insert(0,str(root/'tools/win'))
    import build3ds
    trim=function((root/'third_party/3dsrecomp/runtime/gpu/raster.cpp').read_text(),
                  'void draw_state_cache_trim()')
    source=r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <deque>
#include <unordered_map>
using u64=uint64_t;
std::unordered_map<u64,int> ds_cache;
std::deque<u64> ds_cache_order;
'''+trim+r'''
int main() {
 for(u64 i=0;i<4200;i++){ds_cache[i]=int(i);ds_cache_order.push_back(i);}
 draw_state_cache_trim();
 assert(ds_cache.size()==4136 && ds_cache_order.size()==ds_cache.size());
 assert(!ds_cache.count(0) && !ds_cache.count(63));
 assert(ds_cache.count(64) && ds_cache.count(4199));
 draw_state_cache_trim();
 assert(ds_cache.size()==4096 && ds_cache_order.size()==4096);
 assert(!ds_cache.count(103) && ds_cache.count(104) && ds_cache.count(4199));
 auto retained=ds_cache;
 draw_state_cache_trim();
 assert(ds_cache==retained && ds_cache_order.front()==104);
 ds_cache.clear();ds_cache_order.clear();draw_state_cache_trim();
 assert(ds_cache.empty() && ds_cache_order.empty());
}
'''
    with tempfile.TemporaryDirectory(prefix='3dii-draw-cache-') as temp:
        p=Path(temp);(p/'test.cpp').write_text(source)
        exe=p/'test.exe'
        subprocess.run(build3ds.HOST_CXX+['-std=c++20','-O1',str(p/'test.cpp'),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True)
    print('Draw-state cache: bounded eviction preserves retained materials and stops at the limit')


if __name__=='__main__':
    main()
