#!/usr/bin/env python3
"""Exercise the actual Y2R service's coefficient replies and video conversion.

Host-independent IPC stubs replace only the kernel; the whole service source
is compiled. No ROM, console, external emulator, or SDK is needed.
"""
from pathlib import Path
import os, shlex, subprocess, sys, tempfile
ROOT = Path(__file__).resolve().parents[1]
STUBS = r'''
#include <cstdint>
#include <memory>
#include <string>
#include <initializer_list>
#include <array>
#include <functional>
#include <cassert>
#include <cstdio>
using u8=uint8_t; using u16=uint16_t; using s16=int16_t; using u32=uint32_t; using s32=int32_t;
#define LOG(...) ((void)0)
#define VLOG(...) ((void)0)
std::array<u8,16384> memory{};
u8 *gptr(u32 at){assert(at<memory.size());return memory.data()+at;}
void gsp_cpu_wrote(u32,u32){}
namespace kernel {
struct Event {void signal(){}};
constexpr int ONESHOT=0;
std::shared_ptr<Event> make_event(int,const char*){return std::make_shared<Event>();}
struct IPC {
 u32 req[64]{},reply[64]{};int lock=0;int *lk=&lock;
 explicit IPC(u16 cmd){req[0]=(u32)cmd<<16;for(auto &v:reply)v=0xA55AA55A;}
 u16 cmd()const{return req[0]>>16;}
 u32 operator[](int i)const{return req[i];}
 void set(int i,u32 v){reply[i]=v;}
 void result(u32 res,u32 n=1){reply[0]=(u32)cmd()<<16|n<<6;reply[1]=res;}
 void ok(u32 n=1,u32=0){result(0,n);}
 void handles(int,std::initializer_list<std::shared_ptr<Event>>){}
};
struct Service {
 explicit Service(std::string){};virtual ~Service()=default;virtual void handle(IPC&)=0;
 void unimplemented(IPC&i){i.result(0xDEADBEEF);}
};
void register_service(std::shared_ptr<Service>){}
void run_blocked(int&,const std::function<void()>&f){f();}
}
'''
TESTS = r'''
int main(){
 using namespace kernel;
 Y2rService s("y2r:u");
 // All four standard tables have exact wire ordering and leave active state unchanged.
 for(u32 mode=0;mode<4;mode++){
  IPC select(0x20);select.req[1]=3;s.handle(select);
  s16 active[8];memcpy(active,s.co,sizeof active);
  IPC q(0x21);q.req[1]=mode;s.handle(q);
  assert(q.reply[0]==0x00210140 && q.reply[1]==0);
  assert(memcmp(s.co,active,sizeof active)==0);
  for(u32 k=0;k<8;k++)assert((s16)(q.reply[2+k/2]>>(16*(k&1)))==kStandard[mode][k]);
  // MH4U copies the query's payload into SetCoefficient.
  IPC set(0x1e);for(u32 k=0;k<4;k++)set.req[k+1]=q.reply[k+2];s.handle(set);
  assert(memcmp(s.co,kStandard[mode],sizeof s.co)==0);
  IPC get(0x1f);s.handle(get);assert(get.reply[0]==0x001f0140);
  for(u32 k=0;k<4;k++)assert(get.reply[k+2]==q.reply[k+2]);
 }
 IPC zero(0x21);s.handle(zero);
 assert(zero.reply[2]==0x01660100 && zero.reply[3]==0x005800b6);
 for(u32 invalid:{4u,0xffffffffu}){
  s16 before[8];memcpy(before,s.co,sizeof before);
  IPC q(0x21);q.req[1]=invalid;s.handle(q);
  assert(q.reply[0]==0x00210040 && q.reply[1]==0xe0e053ed);
  assert(memcmp(before,s.co,sizeof before)==0);
 }
 // Known limited-range BT.601 neutral black/white frames through real conversion.
 IPC standard(0x21);standard.req[1]=2;s.handle(standard);
 IPC set(0x1e);for(u32 k=0;k<4;k++)set.req[k+1]=standard.reply[k+2];s.handle(set);
 s.input_format=1;s.output_format=0;s.width=8;s.lines=8;s.block=0;s.alpha=255;
 s.y={128,64,8,0};s.u={256,16,4,0};s.v={384,16,4,0};s.out={1024,256,32,0};
 memset(gptr(256),128,16);memset(gptr(384),128,16);
 for(u8 luma:{u8(16),u8(235)}){
  memset(gptr(128),luma,64);s.convert();
  for(u32 px=0;px<64;px++){
   const u8* p=gptr(1024+px*4);assert(p[0]==255);
   for(u32 c=1;c<4;c++)assert(p[c]==(luma==16?0:255));
  }
 }
 // Batches may cross multiple DMA units; gaps must remain untouched, even
 // when the next call starts part-way through a transfer unit.
 for(u32 unit:{0u,3u,7u,8u,64u,192u}){
  memset(memory.data(),0xA5,memory.size());u8 stream[257];
  for(u32 i=0;i<257;i++)stream[i]=(u8)(i*19);
  DmaWriter writer(Dma{2048,257,unit,5});
  writer.put(stream,13);writer.put(stream+13,0);writer.put(stream+13,244);
  assert(writer.end==2048+257+(unit?256/unit*5:0));
  for(u32 i=0;i<257;i++)assert(memory[2048+i+(unit?i/unit*5:0)]==stream[i]);
  if(unit)for(u32 i=0;i<257/unit;i++)for(u32 gap=0;gap<5;gap++)
   assert(memory[2048+(i+1)*unit+i*5+gap]==0xA5);
 }
 // Continuous and patterned DMA readers retain their position across calls.
 for(u32 step:{1u,2u})for(u32 unit:{0u,8u,64u}){
  for(u32 i=0;i<memory.size();++i)memory[i]=(u8)(i*19+(i>>7));
  DmaReader reader(Dma{2048,1024,unit,6},step);u8 read[129];
  reader.read(read,17);reader.read(read+17,112);
  for(u32 i=0;i<129;++i){
   const u32 pos=i*step;
   assert(read[i]==memory[2048+pos+(unit?pos/unit*6:0)]);
  }
 }
 puts("PASS: Y2R coefficient replies, video levels and DMA reader/writer boundaries");
}
'''
def main():
    source=(ROOT/'third_party/3dsrecomp/runtime/services/y2r.cpp').read_text()
    source=source.replace('#include "../service.h"','').replace('#include "services.h"','')
    cxx=shlex.split(os.environ.get('CXX','c++'))
    if os.name=='nt' and 'CXX' not in os.environ:
        sys.path.insert(0,str(ROOT/'tools/win'));import build3ds
        cxx=build3ds.HOST_CXX
    with tempfile.TemporaryDirectory(prefix='3dii_y2r_') as tmp:
        p=Path(tmp);cpp=p/'test.cpp';exe=p/('test.exe' if os.name=='nt' else 'test')
        cpp.write_text(STUBS+source+TESTS)
        subprocess.run(cxx+['-std=c++20','-O1',str(cpp),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True)
if __name__=='__main__':main()
