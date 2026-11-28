"""Compare the actual tiled movie fast path byte-for-byte with its scalar fallback."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

from test_y2r_coefficients import ROOT, STUBS

HARNESS = r'''
int main() {
 using namespace kernel;
 const s16 extra[4][8] = {
  {-32768,32767,-32768,32767,-32768,32767,-32768,32767},
  {32767,-32768,32767,-32768,32767,-32768,32767,-32768},
  {0,0,0,0,0,-1,1,-24}, {301,411,209,101,523,-7135,4321,-8803}
 };
 for(u32 width:{8u,16u,512u,1024u})
 for(u32 height:{1u,7u,8u,240u})
 for(u32 unit:{0u,3u,16384u})
 for(u32 table=0;table<8;table++) {
  for(u32 i=0;i<memory.size();i++) memory[i]=(u8)(i*17+(i>>7)+table*31);
  Y2rService s("y2r:u");
  s.input_format=1;s.output_format=0;s.block=1;s.rotation=0;
  s.width=width;s.lines=height;s.alpha=table*37;
  memcpy(s.co,table<4?kStandard[table]:extra[table-4],sizeof s.co);
  s.y={0x1000,width*height,width,3};
  s.u={0x80000,width*((height+1)/2)/2,width/2,5};
  s.v={0xa0000,width*((height+1)/2)/2,width/2,7};
  s.out={0x100000,width*height*4,unit,unit?1u:0u};
  s.convert(0);
  // Include every output byte, patterned-DMA gaps and the surrounding guards.
  const u32 bytes=((height+7)/8)*width*8*4;
  const u32 span=bytes+(unit?(bytes-1)/unit:0);
  fwrite(memory.data()+s.out.addr-32,1,span+64,stdout);
 }
}
'''


def main():
    source = (ROOT / 'third_party/3dsrecomp/runtime/services/y2r.cpp').read_text()
    source = source.replace('#include "../service.h"', '').replace('#include "services.h"', '')
    scalar = source.replace(
        'const bool direct_movie = input_format == 1 && output_format == 0 && block && !rotation;',
        'const bool direct_movie = false;')
    assert scalar != source, 'Scalar reference selector changed'
    compiler = shlex.split(os.environ.get('CXX', 'c++'))
    stubs = STUBS.replace('16384', '4194304')
    with tempfile.TemporaryDirectory(prefix='y2r-movie-blocks-') as tmp:
        processes = []
        for name, body in [('scalar', scalar), ('fast', source)]:
            cpp = Path(tmp) / (name + '.cpp')
            exe = Path(tmp) / name
            cpp.write_text(stubs + "\n#define __BIG_ENDIAN__ 1\n" + body + HARNESS)
            subprocess.run(compiler + ['-std=c++20', '-O2', str(cpp), '-o', str(exe)], check=True)
            processes.append(subprocess.Popen([str(exe)], stdout=subprocess.PIPE))
        compared = 0
        try:
            while True:
                before, after = [p.stdout.read(65536) for p in processes]
                assert before == after, f'Movie conversion differs near output byte {compared}'
                if not before:
                    break
                compared += len(before)
            assert all(p.wait() == 0 for p in processes)
        finally:
            for p in processes:
                if p.poll() is None:
                    p.kill()
                p.wait()
        print(f'PASS: 384 movie cases, {compared:,} identical bytes; coefficients, dimensions, partial rows and DMA gaps')


if __name__ == '__main__':
    main()
