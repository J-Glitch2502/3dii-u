"""Apply the compiler's checked copy fast path to an existing safe generated cache.

Normal regeneration emits this automatically. This helper avoids regenerating
unrelated functions for a hardware trial and backs up the affected generated file.
"""
from pathlib import Path
import argparse
import re
import shutil
import struct
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'third_party/3dsrecomp'))
from tools.recomp.image import Image
from tools.recomp.bulk_copy import COPY32_WORDS, copy32_fastpath


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('game_dir', type=Path)
    args = ap.parse_args()
    image = Image(str(args.game_dir))
    signature = struct.pack('<29I', *COPY32_WORDS)
    changed = 0
    for segment in image.texts:
        offset = segment.data.find(signature)
        while offset >= 0:
            address = segment.addr + offset
            name = f'f_{address:08x}'
            files = [p for p in (args.game_dir/'gen_safe').glob('code_*.c') if f'void {name}(' in p.read_text()]
            if len(files) != 1:
                raise RuntimeError(f'Expected one generated definition of {name}')
            p = files[0]; text = p.read_text()
            match = re.search(r'(?ms)^void '+name+r'\(.*?(?=^void |\Z)', text)
            assert match
            body = match.group()
            old_body = body
            line = next(l for l in body.splitlines() if f'/* {address+12:x}:' in l)
            sub = next(l for l in body.splitlines() if f'/* {address+0x1c:x}:' in l)
            fast = copy32_fastpath(address, 'vf =' in sub)
            if '__builtin_memcpy' in body:
                body, count = re.subn(r'(?ms)^#if defined\(__WIIU__\)[^\n]*\n.*?^#endif', lambda _:fast, body, count=1)
                assert count == 1
            else:
                body = body.replace(line, fast + '\n' + line, 1)
            if body != old_body:
                backup = p.with_suffix('.c.before-bulk-copy')
                if not backup.exists(): shutil.copy2(p, backup)
                p.write_text(text[:match.start()] + body + text[match.end():])
                changed += 1
                print(f'Updated {p.name}: {name} (whole routine signature checked)')
            offset = segment.data.find(signature, offset + 4)
    print(f'{changed} generated functions updated')


if __name__ == '__main__': main()
