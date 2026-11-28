"""Run the actual checkpoint coordinator against controlled CPU/GPU fixtures."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
RUNTIME = ROOT / 'third_party/3dsrecomp/runtime'
source = r'''
#include "checkpoint.h"
#include "loader.h"
#include <cassert>
#include <array>
#include <memory>
#include <thread>
#include <chrono>
GameInfo g_game;
std::string g_save_base;
u8 *g_mem;
void rt_log(const char *, ...) {}
extern "C" { volatile u32 rt_checkpoint_requested; }
extern "C" volatile u32 g_3dii_state_status;
extern "C" volatile u32 g_3dii_state_command;
static std::atomic<bool> hold_writer{false},writer_entered{false};
namespace checkpoint { void test_writer_gate() {
 writer_entered=true;
 while(hold_writer.load())std::this_thread::yield();
} }
static unsigned epoch, cpu=4, gpu=8, cleared;
static bool fail, fail_memory, resource_valid=true;
static std::array<u8,32> ram{};
static std::shared_ptr<unsigned> handle = std::make_shared<unsigned>(12);
static std::weak_ptr<unsigned> identity;
namespace audio { void pause(bool) {} void checkpoint_clear_output() { ++cleared; }
void checkpoint_audio(checkpoint::Image &) {} }
namespace mem { void checkpoint_allocator(checkpoint::Image &) {} }
namespace kernel {
bool guest_readable(u32 a,u32 n) { return n && (u64)a+n<=ram.size(); }
bool guest_writable(u32 a,u32 n) { return guest_readable(a,n) && a<16; }
void checkpoint_services(checkpoint::Image &) {}
void checkpoint_hid(checkpoint::Image &) {}
void checkpoint_dsp(checkpoint::Image &) {}
void checkpoint_gsp(checkpoint::Image &) {}
void checkpoint_fs(checkpoint::Image &) {}
bool checkpoint_apt(checkpoint::Image &) { return true; }
}
namespace checkpoint {
bool kernel_capture(Image &im) {
 im.take(cpu); im.take(handle); if(fail_memory) im.max_bytes=1; im.memory(ram.data(),ram.size());
 const auto e=epoch; im.validate.push_back([e] {return e==epoch;});
 return !fail;
}
bool kernel_persist(Image &im) {
 im.take(cpu); im.memory(ram.data(),ram.size()); return im.ok && !fail;
}
bool gpu_capture(Image &im) {
 im.take(gpu); im.validate.push_back([] {return resource_valid;}); return true;
}
}
static void command(unsigned n) { checkpoint::request(n); checkpoint::pump(); }
static void wait_true(const std::atomic<bool> &value) {
 auto until=std::chrono::steady_clock::now()+std::chrono::seconds(2);
 while(!value.load()) { assert(std::chrono::steady_clock::now()<until); std::this_thread::yield(); }
}
static void check_barrier() {
 std::atomic<bool> first{false},second{false},release_readers{false},writer_inside{false},release_writer{false},late_reader{false};
 auto reader=[&](std::atomic<bool>&flag) { checkpoint::Scope lock(checkpoint::gate()); flag=true; wait_true(release_readers); };
 std::thread a(reader,std::ref(first)); wait_true(first);
 std::thread b(reader,std::ref(second)); wait_true(second); // readers really execute concurrently
 std::thread w([&] { checkpoint::ExclusiveScope lock(checkpoint::gate()); writer_inside=true; wait_true(release_writer); });
 auto until=std::chrono::steady_clock::now()+std::chrono::seconds(2);
 while(!rt_checkpoint_requested) {assert(std::chrono::steady_clock::now()<until);std::this_thread::yield();}
 assert(!writer_inside); release_readers=true; a.join(); b.join(); wait_true(writer_inside);
 std::thread late([&] {checkpoint::Scope lock(checkpoint::gate());late_reader=true;});
 std::this_thread::sleep_for(std::chrono::milliseconds(10)); assert(!late_reader);
 release_writer=true; w.join(); late.join(); assert(late_reader && !rt_checkpoint_requested);
}
int main(int argc, char **argv) {
 assert(argc==2);g_save_base=argv[1];g_mem=ram.data();
 // Debug RAM operations queue until the same barrier pump, check mappings,
 // and compare the expected word before changing anything.
 w32(0,0x12345678);
 assert(!checkpoint::request_memory32(1,true,0,1));
 assert(checkpoint::request_memory32(0,true,0x12345678,0x87654321));
 assert(!checkpoint::request_memory32(4,false));
 assert(r32(0)==0x12345678);checkpoint::pump();assert(r32(0)==0x87654321);
 assert(checkpoint::request_memory32(0,true,0,9));checkpoint::pump();assert(r32(0)==0x87654321);
 assert(checkpoint::request_memory32(16,true,0,9));checkpoint::pump();assert(r32(16)==0);
 assert(checkpoint::request_memory32(0xfffffffcu,true,0,9));checkpoint::pump();
 assert(checkpoint::request_memory32(0,false));checkpoint::pump();
 ram.fill(0);
 check_barrier();
 g_game.program_id=0x000400000010bf00ULL;
 ram[3]=42; identity=handle;
 command(1); assert(g_3dii_state_status==1 && checkpoint::has_state());
 cpu=99; gpu=77; ram[3]=13; handle.reset(); assert(!identity.expired());
 command(2); assert(g_3dii_state_status==2 && cpu==4 && gpu==8 && ram[3]==42);
 assert(handle && handle.get()==identity.lock().get() && *handle==12 && cleared==1);
 cpu=99; gpu=77; ram[3]=13; ++epoch;
 command(2); assert(g_3dii_state_status==0x80000003);
 assert(cpu==99 && gpu==77 && ram[3]==13 && cleared==1); // no partial restore
 --epoch; resource_valid=false; command(2);
 assert(g_3dii_state_status==0x80000003 && cpu==99 && gpu==77);
 resource_valid=true; fail=true; command(1);
 assert(g_3dii_state_status==0x80000002 && !checkpoint::has_state());
 assert(checkpoint::Image::Bytes::live==0);
 fail=false; fail_memory=true; command(1);
 assert(g_3dii_state_status==0x80000004 && !checkpoint::has_state() && cpu==99);
 assert(checkpoint::Image::Bytes::live==0);
 fail_memory=false;
 for(unsigned i=0;i<20;++i) {
  cpu=i; ram[3]=i; command(1);
  assert(g_3dii_state_status==1 && checkpoint::Image::Bytes::live==ram.size());
  assert(checkpoint::Image::Bytes::peak==ram.size()); // never two payloads at once
  cpu=999; ram[3]=255; command(2);
  assert(g_3dii_state_status==2 && cpu==i && ram[3]==i);
 }
 command(3); assert(!checkpoint::has_state() && checkpoint::Image::Bytes::live==0);
 {
  std::array<u8,150000> buffer{}; buffer[140000]=37;
  checkpoint::Image image; image.memory(buffer.data(),buffer.size());
  assert(image.ok && image.restore.size()==3); buffer[140000]=0;
  for(auto &restore:image.restore) restore();
  assert(buffer[140000]==37);
 }
 assert(checkpoint::Image::Bytes::live==0);
 command(2); assert(g_3dii_state_status==0x80000003);
 g_game.program_id=0x00040000001a4800ULL; command(1); assert(g_3dii_state_status==1);
 command(3);
 checkpoint::initialize_persistent();
 {
  checkpoint::Archive a(nullptr,false);checkpoint::Image im;im.archive=&a;im.max_bytes=1;
  im.memory(ram.data(),ram.size());assert(!im.ok&&a.staged.empty());
 }
 // Save replacement requests coalesce while a writer owns the immutable chunks.
 for(unsigned i=0;i<30;++i){cpu=i;command(4);if(i%3==0)command(2);command(4); }
 while(checkpoint::saving_to_disk() || g_3dii_state_command) {checkpoint::pump();std::this_thread::yield();}
 checkpoint::finish_pending_write();command(3);cpu=999;command(5);
 assert(g_3dii_state_status==5 && cpu==29);
 command(3);
 cpu=123;gpu=456;ram[3]=71;
 hold_writer=true;writer_entered=false;
 command(4);assert(g_3dii_state_status==4);wait_true(writer_entered);
 assert(checkpoint::saving_to_disk());
 cpu=987;gpu=654;ram[3]=19;command(2);
 assert(g_3dii_state_status==2&&cpu==123&&gpu==456&&ram[3]==71);
 assert(checkpoint::saving_to_disk()); // restore didn't wait for the deliberately blocked writer
 cpu=888;ram[3]=27; // changing live RAM must not alter the pending file
 hold_writer=false;checkpoint::finish_pending_write();command(3); // discard all RAM checkpoint ownership
 cpu=987;gpu=654;ram[3]=19;
 command(5);assert(g_3dii_state_status==5&&cpu==123&&gpu==456&&ram[3]==71);
 command(3);
 FILE*f=fopen((g_save_base+"/checkpoint.bin").c_str(),"r+b");assert(f);fseek(f,-1,SEEK_END);int ch=fgetc(f);fseek(f,-1,SEEK_END);fputc(ch^1,f);fclose(f);
 cpu=111;gpu=222;ram[3]=33;command(5);
 assert((g_3dii_state_status&0x80000000)&&cpu==111&&gpu==222&&ram[3]==33);
 // Restore the original byte, then reject a valid-checksum payload with trailing data.
 f=fopen((g_save_base+"/checkpoint.bin").c_str(),"r+b");assert(f);fseek(f,-1,SEEK_END);fputc(ch,f);fclose(f);
 f=fopen((g_save_base+"/checkpoint.bin").c_str(),"ab");assert(f);fputc(0,f);fclose(f);
 cpu=111;gpu=222;ram[3]=33;command(5);
 assert((g_3dii_state_status&0x80000000)&&cpu==111&&gpu==222&&ram[3]==33);
 // A truncated payload must also reject without applying any restore callback.
 f=fopen((g_save_base+"/checkpoint.bin").c_str(),"wb");assert(f);fputc(0,f);fclose(f);
 cpu=111;gpu=222;ram[3]=33;command(5);
 assert((g_3dii_state_status&0x80000000)&&cpu==111&&gpu==222&&ram[3]==33); // corrupt file never partially restores

 // A failed storage write must retain a usable RAM checkpoint.
 g_save_base += "/missing-folder";cpu=432;gpu=876;ram[3]=54;
 command(4);checkpoint::finish_pending_write();
 assert(std::string(checkpoint::result_message()).find("RAM STATE RETAINED")!=std::string::npos);
 cpu=1;gpu=2;ram[3]=3;command(2);
 assert(g_3dii_state_status==2&&cpu==432&&gpu==876&&ram[3]==54);
}
'''
with tempfile.TemporaryDirectory(prefix='checkpoint-transaction-') as tmp:
    p = Path(tmp)
    (p/'coreinit').mkdir(); (p/'gx2').mkdir()
    (p/'coreinit/debug.h').write_text('#pragma once\ninline void OSReport(const char *, ...) {}\n')
    (p/'gx2/event.h').write_text('#pragma once\ninline void GX2DrawDone() {}\n')
    (p/'test.cpp').write_text(source)
    subprocess.run(['c++','-std=c++20','-DRECOMP_RESUMABLE','-DCHECKPOINT_TEST_WORKER_GATE','-O2','-pthread',
                    '-I'+str(p),'-I'+str(RUNTIME),str(p/'test.cpp'),
                    str(ROOT/'port/wiiu/checkpoint.cpp'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test'),str(p)],check=True)
print('PASS: CPU/RAM/GPU rollback, pinned handle identity, preflight rejection, 20 replacement saves/loads without overlapping payloads, memory-budget rejection, chunked restore, discard and non-Kirby title availability')
