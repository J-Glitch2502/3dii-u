"""Check the actual scanout routine against GX2 copies that clobber state."""
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[1]
s=(root/'port/wiiu/gx2_backend.cpp').read_text()
a=s.index('void to_screen(GX2ColorBuffer *from, GX2ScanTarget target) {')
b=s.index('\n}\n',a)+2
source=r'''
#include <cassert>
#include <cstdlib>
#include <cstring>
#define RECOMP_RESUMABLE
#define LOG(...) ((void)0)
using u32=unsigned;
struct Surface {u32 width=0,height=0,format=0,alignment=64,imageSize=64;void *image=nullptr;};
struct GX2ColorBuffer {Surface surface;};
enum GX2ScanTarget {GX2_SCAN_TARGET_TV=1,GX2_SCAN_TARGET_DRC=4};
constexpr int GX2_INVALIDATE_MODE_COLOR_BUFFER=1;
static bool context=true, saving,loading;
static unsigned draws,scans,copies;
static GX2ColorBuffer *expected;
namespace mem {void host_free(void *p){free(p);}void *host_alloc(u32,u32 n){return malloc(n);}}
void GX2InitColorBufferRegs(GX2ColorBuffer*){}
void GX2CopySurface(Surface *from,int,int,Surface *to,int,int){
 memcpy(to->image,from->image,from->imageSize);context=false;++copies;
}
void restore_context(){context=true;}
void GX2Invalidate(int,void*,u32){}
void GX2CopyColorBufferToScanBuffer(GX2ColorBuffer *cb,GX2ScanTarget){
 assert(((char*)cb->surface.image)[0]==(saving||loading ? 9:3));++scans;
}
namespace checkpoint {bool saving_to_disk(){return saving;}}
namespace wiiu {
bool state_loading_visible(){return loading;}
void draw_state_saving_on(GX2ColorBuffer *cb){
 assert(context && cb==expected);((char*)cb->surface.image)[0]=9;++draws;
}
}
'''+s[a:b]+r'''
int main(){
 GX2ColorBuffer tv,pad;tv.surface.width=pad.surface.width=854;tv.surface.height=pad.surface.height=480;tv.surface.format=pad.surface.format=1;char tvdata[64]={3},paddata[64]={3};
 tv.surface.image=tvdata;pad.surface.image=paddata;
 to_screen(&tv,GX2_SCAN_TARGET_TV);to_screen(&pad,GX2_SCAN_TARGET_DRC);
 saving=true;
 for(int i=0;i<3;++i){
 expected=&tv;to_screen(&tv,GX2_SCAN_TARGET_TV);assert(tvdata[0]==3);
 expected=&pad;to_screen(&pad,GX2_SCAN_TARGET_DRC);assert(paddata[0]==3);
 }
 saving=false;loading=true;expected=&pad;to_screen(&pad,GX2_SCAN_TARGET_DRC);assert(paddata[0]==3);
 loading=false;to_screen(&tv,GX2_SCAN_TARGET_TV);to_screen(&pad,GX2_SCAN_TARGET_DRC);
 assert(draws==7&&scans==11);
}
'''
with tempfile.TemporaryDirectory() as d:
    p=Path(d);(p/'test.cpp').write_text(source)
    subprocess.run(['c++','-std=c++17',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
print('PASS: copy-clobbered context reset, TV/DRC save/load indicators scanned out, clean paused pictures restored')
