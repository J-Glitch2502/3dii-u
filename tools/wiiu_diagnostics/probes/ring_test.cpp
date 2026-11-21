#include <cstdint>
#include <cassert>
#include <thread>
#include <atomic>
#include <cstdio>
thread_local uint32_t fakeCore=0;
thread_local uint64_t tick=0;
static bool OSDisableInterrupts(){return true;}
static void OSRestoreInterrupts(bool){}
static uint32_t OSGetCoreId(){return fakeCore;}
static uint64_t OSGetTime(){return ++tick;}
static void *OSGetCurrentThread(){return (void*)(uintptr_t)(0x1000+fakeCore*0x100);}
#define FLIGHT_HOST_TEST
#include "../MemoryMappingModule/source/flight_recorder.h"
int main(){
 assert(flight::record(10,1,2,3,4)==0);
 assert(flight::load(&flight::rings[0].head)==0);
 flight::store(&flight::enabled,1);
 {flight::Scope s(10,0x1234,0x80000000,2368,32);s.result=0x9000;}
 uint32_t out[flight::Words];
 assert(flight::read(0,1,out) && out[1]==10 && out[6]==0x80000000);
 assert(flight::read(0,2,out) && out[1]==11 && out[6]==0x9000 && out[9]==1);
 for(auto &ring:flight::rings)flight::store(&ring.head,0);
 std::atomic<unsigned> done{0},valid{0};
 std::thread writers[3];
 for(unsigned c=0;c<3;c++) writers[c]=std::thread([c,&done]{
   fakeCore=c;
   for(unsigned i=1;i<=100000;i++) flight::record(30,0x1234,i,i^0xdeadbeef,~i);
   ++done;
 });
 while(done.load()!=3){
   for(unsigned c=0;c<3;c++){
     unsigned head=flight::load(&flight::rings[c].head);
     if(head && flight::read(c,head,out)){
       assert(out[1]==30 && out[7]==(out[6]^0xdeadbeef) && out[8]==~out[6]); ++valid;
     }
   }
 }
 for(auto &t:writers)t.join();
 for(unsigned c=0;c<3;c++){
   assert(flight::load(&flight::rings[c].head)==100000);
   assert(!flight::read(c,1,out));
   assert(flight::read(c,100000,out) && out[6]==100000);
 }
 assert(valid.load()>0);
 printf("PASS: gating, scope pairing, 300000 events, concurrent snapshots, overwrite detection (%u snapshots)\n",valid.load());
}
