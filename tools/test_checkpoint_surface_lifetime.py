"""Exercise the renderer's real idle-eviction path with live checkpoint pins."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
text = (root/'port/wiiu/gx2_backend.cpp').read_text()
pin = text[text.index('static std::shared_ptr<Surface> checkpoint_pin('):text.index('\n#endif', text.index('static std::shared_ptr<Surface> checkpoint_pin('))]
start = text.index('void forget_unused() {')
evict = text[start:text.index('\n}\n', start)+2]
source = r'''
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <vector>
#include "surface_readback.h"
using u32=uint32_t; using u64=uint64_t;
#define RECOMP_RESUMABLE
struct Surface {
 u32 checkpoint_pins=0,addr=0; u64 last_use=0,drawn=0,written_after=0;
 bool guest_newer=false,first_use=false; surface_readback::Spans written; u32 bytes()const{return 16;}
 bool in_list=false, shared_depth=false, depth=true, gpu_newer=false, mem1=false;
 void *image() {return nullptr;}
};
std::vector<std::unique_ptr<Surface>> surfaces;
Surface *cur_color, *cur_depth, *depth_owner;
size_t next_surface_retirement;
bool transfer_source(const Surface&){return false;}
u64 use_counter=2000;
bool state_sent_valid=true,state_lost=false;
void (*GfxHeapFreeMEM1)(void*);
PIN
EVICT
int main() {
 surfaces.push_back(std::make_unique<Surface>());
 auto *saved=surfaces[0].get();
 auto pin=checkpoint_pin(saved);
 assert(saved->checkpoint_pins==1);
 surfaces.push_back(std::make_unique<Surface>());
 forget_unused(); // an unpinned idle target is still eligible
 assert(surfaces.size()==1 && surfaces[0].get()==saved);
 use_counter=100000;forget_unused(); // elapsed time cannot invalidate a saved state
 assert(surfaces.size()==1);
 auto second=checkpoint_pin(saved);pin.reset();
 assert(saved->checkpoint_pins==1);forget_unused();assert(surfaces.size()==1);
 second.reset();assert(saved->checkpoint_pins==0);
 forget_unused();assert(surfaces.empty()); // replacement/discard re-enables normal reclamation
 surfaces.push_back(std::make_unique<Surface>());
 {auto rejected=checkpoint_pin(surfaces[0].get());}
 forget_unused();assert(surfaces.empty()); // failed capture leaves no pin behind
}
'''.replace('PIN', pin).replace('EVICT', evict)
with tempfile.TemporaryDirectory(prefix='checkpoint-surface-') as tmp:
 p=Path(tmp);(p/'test.cpp').write_text(source)
 subprocess.run(['c++','-std=c++20','-O2','-Wall','-Werror','-I'+str(root/'port/wiiu'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('PASS: saved surfaces survive idle eviction; other surfaces retire; final pin release and failed capture permit reclamation')
