"""Check bounded logical serialization without admitting native resources."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1]
source=r'''
#include "state_archive.h"
#include <cassert>
struct State { std::string name; std::vector<unsigned> q; std::map<unsigned,std::string> m;
 void state_fields(checkpoint::Archive&a){a.fields(name,q,m);} };
int main(){
 std::array<u8,70000> staged_data{};staged_data[69999]=71;
 checkpoint::Archive staged(nullptr,false);staged.raw(staged_data.data(),staged_data.size());
 assert(staged.ok&&staged.staged.size()==2&&staged.staged.back()->back()==71);
 staged_data[69999]=0;assert(staged.staged.back()->back()==71);
 FILE*f=tmpfile();assert(f);State s{"Mario",{1,3,7},{{4,"event"}}};
 checkpoint::Archive out(f,false);out.io(s);assert(out.ok);auto hash=out.hash, size=out.bytes;
 rewind(f);State restored;checkpoint::Archive in(f,true);in.io(restored);
 assert(in.ok&&in.hash==hash&&in.bytes==size&&restored.name==s.name&&restored.q==s.q&&restored.m==s.m);
 rewind(f);checkpoint::Archive truncated(f,true);std::array<char,512> too_big{};truncated.raw(too_big.data(),too_big.size());assert(!truncated.ok);
 rewind(f);uint32_t huge=0xffffffffu;fwrite(&huge,4,1,f);rewind(f);
 checkpoint::Archive hostile(f,true);std::vector<unsigned> bad;hostile.io(bad);assert(!hostile.ok&&bad.empty());
 checkpoint::Archive pointer(f,false);int x=1;int*p=&x;pointer.io(p);assert(!pointer.ok);
 struct Hidden {int *ptr;}; Hidden hidden{p};checkpoint::Archive aggregate(f,false);aggregate.io(hidden);assert(!aggregate.ok);
 CPU cpu{};cpu.pc=0x1234;cpu.thread=p;FILE*cf=tmpfile();checkpoint::Archive cw(cf,false);cw.io(cpu);rewind(cf);CPU other{};checkpoint::Archive cr(cf,true);cr.io(other);assert(cr.ok&&other.pc==cpu.pc&&other.thread==nullptr);fclose(cf);
 checkpoint::Archive callback(f,false);std::function<void()> fn=[]{};callback.io(fn);assert(!callback.ok);
 fclose(f);
}
'''
with tempfile.TemporaryDirectory(prefix='state-archive-') as t:
 p=Path(t);(p/'test.cpp').write_text(source)
 subprocess.run(['c++','-std=c++20','-O2','-Wall','-Werror','-I'+str(root/'third_party/3dsrecomp/runtime'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('PASS: logical collections round trip; checksum agrees; oversized/truncated data, native pointers and callbacks rejected')
