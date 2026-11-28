"""Check priority with a sustained stream of pixel misses and cached work."""
from pathlib import Path
import subprocess,tempfile
r=Path(__file__).resolve().parents[1]
s=r'''
#include "shader_queue.h"
#include <deque>
#include <cassert>
struct Job { bool vs,demanded; unsigned key; };
int main(){
 std::deque<Job> q;
 for(unsigned i=0;i<100;++i)gpu::compile_queue::enqueue(q,Job{bool(i%2),false,i});
 gpu::compile_queue::enqueue(q,Job{true,true,1000});
 for(unsigned i=0;i<1000;++i)gpu::compile_queue::enqueue(q,Job{false,true,2000+i});
 assert(q.front().key==1000);q.pop_front();
 for(unsigned i=0;i<1000;++i){assert(!q.front().vs&&q.front().demanded);q.pop_front();}
 for(auto &j:q)assert(!j.demanded);
 // A cached vertex becomes urgent when a real draw needs it.
 auto promoted=q.back();q.pop_back();promoted.vs=true;promoted.demanded=true;
 gpu::compile_queue::enqueue(q,promoted);assert(q.front().key==promoted.key);
 unsigned rank=0;
 for(auto &j:q){auto p=gpu::compile_queue::priority(j);assert(p>=rank);rank=p;}
}
'''
with tempfile.TemporaryDirectory() as t:
 p=Path(t);(p/'test.cpp').write_text(s)
 subprocess.run(['c++','-std=c++17','-O2','-I'+str(r/'port/wiiu'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('PASS: demanded vertices survive 1000 pixel arrivals; pixels precede speculative cache; cached vertex promotion')
