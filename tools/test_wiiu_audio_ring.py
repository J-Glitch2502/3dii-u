#!/usr/bin/env python3
"""Exercise actual Wii U queue/recovery code with deterministic AX cursors."""
from pathlib import Path
import os, re, shlex, subprocess, tempfile
ROOT = Path(__file__).resolve().parents[1]
STUBS = r'''#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <array>
#include <atomic>
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <malloc.h>
#include "audio_ring.h"
using u32=uint32_t; using s16=int16_t;
#define LOG(...) ((void)0)
namespace audio::hle { using Sample=std::array<s16,2>; }
struct AXVoice{};
struct AXInitParams{int renderer,pipeline;};
struct AXVoiceVeData{int volume;};
struct AXVoiceDeviceMixData{struct {int volume;} bus[1];};
struct AXVoiceOffsets {int dataType,loopingEnabled;u32 loopOffset,endOffset,currentOffset;void *data;};
constexpr int AX_INIT_RENDERER_48KHZ=0,AX_INIT_PIPELINE_SINGLE=0,AX_VOICE_STATE_PLAYING=1,AX_VOICE_STATE_STOPPED=0;
constexpr int AX_DEVICE_TYPE_TV=0,AX_DEVICE_TYPE_DRC=1,AX_VOICE_FORMAT_LPCM16=0,AX_VOICE_LOOP_ENABLED=1,AX_VOICE_SRC_TYPE_LINEAR=0;
u32 cursor=0;
void DCFlushRange(void*,size_t){}
void AXVoiceBegin(AXVoice*){} void AXVoiceEnd(AXVoice*){}
void AXSetVoiceState(AXVoice*,int){} void AXSetVoiceType(AXVoice*,int){}
void AXSetVoiceVe(AXVoice*,AXVoiceVeData*){}
void AXSetVoiceDeviceMix(AXVoice*,int,int,AXVoiceDeviceMixData*){}
void AXSetVoiceOffsets(AXVoice*,AXVoiceOffsets*){}
void AXGetVoiceOffsets(AXVoice*,AXVoiceOffsets *o){o->currentOffset=cursor;}
void AXSetVoiceSrcType(AXVoice*,int){} void AXSetVoiceSrcRatio(AXVoice*,float){}
int AXGetInputSamplesPerSec(){return 48000;}
void AXInitWithParams(AXInitParams*){} void AXQuit(){} void AXFreeVoice(AXVoice*){}
AXVoice *AXAcquireVoice(int,void*,void*){static AXVoice v;return &v;}
'''
MAIN = r'''
int main(){
 audio::init();
 std::array<audio::hle::Sample,160> samples; for(auto &s:samples)s={1234,-1234};
 for(int i=0;i<10;i++)audio::hle::host_output(samples.data(),samples.size());
 assert(playing && write_pos==3200);
 assert(audio::hle::host_queued()==3200);
 // AX passes the writer during a producer scheduling stall. The old queue
 // result was 7792 and blocked refill until ~189 ms of stale data had played.
 cursor=3600;
 assert(audio::hle::host_queued()==0);
 audio::hle::host_output(samples.data(),samples.size());
 assert(wrapped_recoveries==1 && audio::hle::host_queued()==1760);
 assert(ring[0][5200]==1234 && ring[1][5200]==-1234 && ring[0][0]==0);
 // Normal wrap is a real queue, not an underrun.
 write_pos=128;cursor=8000;
 assert(audio::hle::host_queued()==320);
 // A near-empty queue still uses the existing cushion recovery.
 write_pos=8100;cursor=8020;
 assert(audio::hle::host_queued()==80);
 audio::hle::host_output(samples.data(),samples.size());
 assert(low_recoveries==1 && audio::hle::host_queued()==1760);
 for(u32 raw=0;raw<8192;raw++){
  write_pos=(cursor+raw)%8192;
  assert(audio::hle::host_queued()==(raw>4096 ? 0 : (int)raw));
 }
 audio::pause(true);audio::pause(false);audio::shutdown();
 assert(audio::hle::host_queued()==-1);
 puts("PASS: actual AX queue code refills after wrapped underrun; normal wrap, low recovery, all distances and shutdown");
}
'''
def main():
 source=(ROOT/'port/wiiu/audio_wiiu.cpp').read_text()
 source=re.sub(r'^#include[^\n]*\n','',source,flags=re.M)
 with tempfile.TemporaryDirectory(prefix='3dii_audio_ring_') as tmp:
  cpp=Path(tmp)/'test.cpp';exe=Path(tmp)/'test';cpp.write_text(STUBS+source+MAIN)
  subprocess.run(shlex.split(os.environ.get('CXX','g++'))+['-std=c++17','-O2','-I'+str(ROOT/'port/wiiu'),str(cpp),'-o',str(exe)],check=True)
  subprocess.run([str(exe)],check=True)
if __name__=='__main__':main()
