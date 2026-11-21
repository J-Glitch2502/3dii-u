"""Execute a generated mixed-mode indirect call across a split safe callee."""
from pathlib import Path
import subprocess
import sys
import tempfile
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'third_party/3dsrecomp'))
from tools.recomp.gen import Emitter
from tools.recomp.ins import Ins, ICALL

em = Emitter(SimpleNamespace(funcs=set()), '')
em.abi = em.cur_abi = True
em.safe_ranges = [(0x2000, 0x2100)]
i = Ins(0x1000, 4, 0, 0)
i.kind, i.link = ICALL, 0x1004
i.code = ['r14 = 0x1004u;']
body = em.emit_ins(i, 0x1000, {})
# The stub safe function yields once to another translated entry before returning.
# ABI callbacks return directly, retaining the existing fast path.
source = r'''
#include <stdint.h>
#include <assert.h>
typedef uint32_t u32;
struct CPU { u32 pc, r0, r4, lr; };
static unsigned calls;
static void rt_dispatch(struct CPU *c, u32 unused) {
 (void)unused; ++calls;
 if (c->pc == 0x2001) {
  assert(c->r4 == 77); c->r4 = 88; c->pc = 0x2005;
 } else if (c->pc == 0x2005) { c->r0 = 42; c->pc = c->lr; }
 else { assert(c->pc == 0x3000); c->r0 = 9; c->pc = c->lr; }
}
static int completed;
static void run(struct CPU *c, u32 tpc) {
 u32 r0=1, r4=77, r14=0;
#define SOC() do {c->r0=r0; c->lr=r14;} while(0)
#define SIC() do {r0=c->r0;} while(0)
#define SO() do {SOC(); c->r4=r4;} while(0)
#define SI() do {SIC(); r4=c->r4;} while(0)
BODY
 completed=1;
 if (tpc==0x2001) {assert(r0==42); assert(r4==88);}
 else {assert(r0==9); assert(r4==77);}
}
int main(void) {
 struct CPU c={0}; run(&c,0x2001); assert(completed && calls==2);
 completed=0; calls=0; run(&c,0x3000); assert(completed && calls==1);
 return 0;
}
'''.replace('BODY', body)
with tempfile.TemporaryDirectory(prefix='mixed-icall-') as tmp:
    p=Path(tmp); (p/'test.c').write_text(source)
    subprocess.run(['cc','-O2','-Wall','-Werror',str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
print('mixed indirect safe callback and ABI callback: passed')
