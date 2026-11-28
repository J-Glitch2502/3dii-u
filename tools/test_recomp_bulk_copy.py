"""Execute original and optimized ARM copy translations; compare all CPU state and memory."""
from pathlib import Path
import json
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'third_party/3dsrecomp'))
from tools.recomp import gen, image
from tools.recomp.bulk_copy import COPY32_WORDS, matches_copy32

BASE = 0x1000
with tempfile.TemporaryDirectory(prefix='copy32-') as tmp:
    d = Path(tmp)
    (d/'code.bin').write_bytes(struct.pack('<29I', *COPY32_WORDS))
    (d/'info.json').write_text(json.dumps({'text': {'addr': BASE, 'size': 116},
        'rodata': {'addr': BASE+0x1000, 'size': 0}, 'data': {'addr': BASE+0x2000, 'size': 0}}))
    an = gen.Analyzer(image.Image(str(d)), log=lambda *a: None)
    an.funcs[BASE] = True
    before = gen.Emitter(an, str(d)); before.bulk_copy = False
    reference = before.emit_function(BASE).replace('f_00001000', 'reference')
    optimized = gen.Emitter(an, str(d)).emit_function(BASE).replace('f_00001000', 'optimized')
    bf = gen.Emitter(an, str(d)); bf.bulk_copy = False; bf.flag_returning.add(BASE)
    af = gen.Emitter(an, str(d)); af.flag_returning.add(BASE)
    reference_flags = bf.emit_function(BASE).replace('f_00001000', 'reference_flags')
    optimized_flags = af.emit_function(BASE).replace('f_00001000', 'optimized_flags')
    resumable = gen.Emitter(an, str(d)); resumable.resumable = True
    resumed = resumable.emit_function(BASE).replace('f_00001000', 'resumable_copy')
    entries = '\n'.join('case 0x%xu: resumable_copy(c, %d); break;' % (a,k) for a,_,k in resumable.entries)
    assert '__builtin_memcpy' in optimized
    for offset in range(29):
        changed = image.Image(str(d))
        b = bytearray(changed.text.data); b[offset*4] ^= 1; changed.text.data = bytes(b)
        assert not matches_copy32(changed, BASE, list(range(BASE, BASE+116, 4)))
    harness = r'''
#include "cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
static u8 a[4<<20], b[4<<20], seed[4<<20];
u8 *g_mem;
volatile u32 rt_preempt_flag;
volatile u32 rt_checkpoint_requested;
u32 rt_bulk_copy_enabled=1;
static u32 native_calls, preemptions;
void rt_preempt(CPU *c) { (void)c; ++preemptions; }
static void *native_copy(void *d, const void *s, size_t n) { ++native_calls; return memcpy(d,s,n); }
#define __DEVKITPPC__ 1 /* generated PPC units do not define __WIIU__ */
#define __builtin_memcpy native_copy
REFERENCE
OPTIMIZED
REF_FLAGS
OPT_FLAGS
RESUMABLE
static void resume_run(CPU *c) {
 const u32 done=c->r[14]; c->pc=0x1000;
 unsigned budget=100000;
 while(c->pc!=done && budget--) {
  switch(c->pc) { RESUME_ENTRIES default: assert(c->pc<0x1000 || c->pc>=0x1074); return; }
 }
 assert(c->pc==done);
}
static unsigned cases;
static void check_one(u32 n,u32 src,u32 dst,u32 pending,int flags) {
 CPU x={0},y;
 for(u32 i=0;i<16;++i)x.r[i]=i*0x7654321u+0x12345678u;
 x.r[0]=dst;x.r[1]=src;x.r[2]=n;x.r[13]=0x3f0000;x.r[14]=0x12345678;
 x.nf=cases&1;x.zf=(cases>>1)&1;x.cf=(cases>>2)&1;x.vf=(cases>>3)&1;
 y=x;memcpy(a,seed,sizeof a);memcpy(b,seed,sizeof b);
 rt_preempt_flag=pending;g_mem=a;if(flags)reference_flags(&x,0);else reference(&x,0);
 native_calls=0;preemptions=0;g_mem=b;if(flags==2)resume_run(&y);else if(flags)optimized_flags(&y,0);else optimized(&y,0);
 if(memcmp(&x,&y,sizeof x)||memcmp(a,b,sizeof a)) {
  fprintf(stderr,"copy mismatch n=%u src=%x dst=%x pending=%u\n",n,src,dst,pending);
  for(u32 i=0;i<sizeof x/4;++i)if(((u32*)&x)[i]!=((u32*)&y)[i])fprintf(stderr,"state word %u: %08x != %08x\n",i,((u32*)&x)[i],((u32*)&y)[i]);
  for(u32 i=0;i<sizeof a;++i)if(a[i]!=b[i]){fprintf(stderr,"memory %x: %02x != %02x\n",i,a[i],b[i]);break;}
  abort();
 }
 if(pending)assert(!native_calls);
 if(!rt_bulk_copy_enabled)assert(!native_calls);
 if(rt_bulk_copy_enabled && n>=32 && n<=1048576 && dst==0x200000 && !pending)assert(native_calls==1);
 ++cases;
}
static void check(u32 n,u32 src,u32 dst,u32 pending) {
 check_one(n,src,dst,pending,0);check_one(n,src,dst,pending,1);check_one(n,src,dst,pending,2);
}
int main(void) {
 for(u32 i=0;i<sizeof seed;++i)seed[i]=(u8)(i*37u+(i>>9));
 for(u32 n=0;n<=96;++n)for(u32 align=0;align<4;++align) {
  check(n,0x10000+align,0x200000,0);
  check(n,0x10000+align,0x200000,1);
  check(n,0x10000+align,0x10010,0);
 }
 const u32 sizes[]={127,128,129,255,256,511,512,65535,65536,98304,491520,1048575,1048576,1048577};
 for(u32 i=0;i<sizeof sizes/sizeof sizes[0];++i)for(u32 align=0;align<4;++align) {
  check(sizes[i],0x10000+align,0x200000,0);
  check(sizes[i],0x10000+align,0x10010,0);
 }
 check(128,0x10000,0x3effe0,0); // destination overlaps saved registers
 rt_bulk_copy_enabled=0;check(491520,0x10000,0x200000,0);
 printf("PASS: %u copies; complete CPU state, flags, memory, overlap, tails and pending preemption\n",cases);
}
'''.replace('REFERENCE', reference).replace('OPTIMIZED', optimized).replace('REF_FLAGS', reference_flags).replace('OPT_FLAGS', optimized_flags).replace('RESUMABLE', resumed).replace('RESUME_ENTRIES', entries)
    (d/'test.c').write_text(harness)
    subprocess.run(['cc', '-std=gnu11', '-O2', '-fno-strict-aliasing',
        '-I'+str(ROOT/'third_party/3dsrecomp/runtime'), str(d/'test.c'), '-o', str(d/'test')], check=True)
    subprocess.run([str(d/'test')], check=True)
