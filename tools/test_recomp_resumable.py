"""Execute generated nested calls and rewind at every dispatcher boundary.

This tests CPU/test-RAM continuations only, not kernel or whole-game states.
"""
from pathlib import Path
import subprocess
import sys
import tempfile
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'third_party/3dsrecomp'))
from tools.recomp.gen import Emitter
from tools.recomp.ins import Ins, B, CALL, ICALL, JMP, SVC

def ins(addr, code=(), kind='op', **kw):
    i = Ins(addr, 2 if addr >= 0x3000 else 4, int(addr >= 0x3000), 0)
    i.code, i.kind = list(code), kind
    for k, v in kw.items():
        setattr(i, k, v)
    return i

# Nested ARM -> ARM -> Thumb calls, a backward conditional branch, an SVC
# that changes registers, and an indirect jump-table continuation.
program = {
    0x1000: [
        ins(0x1000, ['r13 -= 4; WR32(r13, r14);']),
        ins(0x1004, ['r14 = 0x1008u;'], CALL, target=0x2000, link=0x1008),
        ins(0x1008, ['r14 = RD32(r13); r13 += 4; r0 += 7;']),
        ins(0x100c, ['tpc = r14;'], JMP, ret=True),
    ],
    0x2000: [
        ins(0x2000, ['r13 -= 4; WR32(r13, r14); r4 = 3;']),
        ins(0x2004, ['r14 = 0x2008u; tpc = 0x3001u;'], ICALL, link=0x2008),
        ins(0x2008, ['r0 += r4; S17 += 0.5f; r4 -= 1; zf = (r4 == 0);']),
        ins(0x200c, kind=B, target=0x2008, cond=1),
        ins(0x2010, kind=SVC, imm=0x42),
        ins(0x2014, ['r0 += r7; tpc = 0x201cu;'], JMP, jt=[0x201c]),
        ins(0x201c, ['r14 = RD32(r13); r13 += 4; tpc = r14;'], JMP, ret=True),
    ],
    0x3001: [
        ins(0x3000, ['r0 += 2; nf = 1; cf = 1; vf = 1; qf = 1; ge = 9; fpsr = 0x800000u; S0 = 1.25f;']),
        ins(0x3002, ['tpc = r14;'], JMP, ret=True),
    ],
}
an = SimpleNamespace(funcs=set(program), explore=lambda k: ({i.addr: i for i in program[k]}, None))
em = Emitter(an, '')
em.resumable = True
generated = '\n'.join(em.emit_function(k) for k in program)
assert 'rt_dispatch(' not in generated and 'musttail' not in generated
assert len({a for a, _, _ in em.entries}) == len(em.entries)
for a in (0x1008, 0x2008, 0x2014, 0x201c):
    assert a in {a for a, _, _ in em.entries}
entries = '\n'.join('{0x%xu, %s, %d},' % e for e in em.entries)
source = r'''
#include "cpu.h"
#include <assert.h>
#include <stdio.h>
static u8 ram[4096];
u8 *g_mem = ram;
volatile u32 rt_preempt_flag = 1;
volatile u32 rt_checkpoint_requested;
static unsigned preemptions, services;
void rt_preempt(CPU *c) { assert(c->pc); ++preemptions; }
void rt_svc(CPU *c, u32 imm) {
 assert(imm == 0x42 && c->pc == 0x2014);
 assert(c->nf == 1 && c->cf == 1 && c->vf == 1 && c->zf == 1);
 assert(c->qf == 1 && c->ge == 9 && c->fpscr == 0x800000);
 assert(c->s[0] == 1.25f && c->s[17] == 1.5f);
 c->r[7] = 11; c->s[0] = 2.5f; ++services;
}
GENERATED
static const GuestEntry entries[] = { ENTRIES };
static void step(CPU *c) {
 for (unsigned i = 0; i < sizeof entries / sizeof entries[0]; ++i)
  if (entries[i].addr == c->pc) { entries[i].fn(c, entries[i].entry); return; }
 assert(!"missing continuation");
}
static void finish(CPU *c) {
 unsigned limit = 100;
 while(c->pc != 0xdeadbeef && limit--) step(c);
 assert(c->pc == 0xdeadbeef && c->r[0] == 26 && c->r[13] == 4096);
 assert(c->s[0] == 2.5f && c->s[17] == 1.5f);
}
int main(void) {
 CPU c = {0}, final, snapshot;
 u8 final_ram[sizeof ram], saved_ram[sizeof ram];
 c.pc = 0x1000; c.r[13] = 4096; c.r[14] = 0xdeadbeef;
 snapshot = c; finish(&c); final = c; memcpy(final_ram, ram, sizeof ram);
 assert(services == 1 && preemptions == 0);
 c = snapshot; memset(ram, 0, sizeof ram);
 unsigned boundaries = 0;
 while(c.pc != 0xdeadbeef) {
  snapshot = c; memcpy(saved_ram, ram, sizeof ram);
  finish(&c);
  assert(!memcmp(&c, &final, sizeof c) && !memcmp(ram, final_ram, sizeof ram));
  /* The generated C frame has returned before this rewind. */
  c = snapshot; memcpy(ram, saved_ram, sizeof ram);
  step(&c); ++boundaries;
 }
 printf("PASS: %u rewind boundaries; nested ARM/Thumb calls, loop, SVC, jump table, CPU and RAM\n", boundaries);
}
'''.replace('GENERATED', generated).replace('ENTRIES', entries)
with tempfile.TemporaryDirectory(prefix='resumable-') as tmp:
    p = Path(tmp)
    (p / 'test.c').write_text(source)
    subprocess.run(['cc', '-std=gnu11', '-O2', '-I' + str(ROOT / 'third_party/3dsrecomp/runtime'),
                    str(p / 'test.c'), '-o', str(p / 'test')], check=True)
    subprocess.run([str(p / 'test')], check=True)

em.abi = True
try:
    em.emit_function(0x1000)
except ValueError:
    pass
else:
    raise AssertionError('mixed ABI/resumable generation must be rejected')
