#!/usr/bin/env python3
"""Exercise production fragment selection and generated depth/scissor semantics."""
import argparse
from pathlib import Path
import subprocess
import sys

sys.dont_write_bytecode = True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--backend', type=Path)
    parser.add_argument('--out', type=Path)
    args = parser.parse_args()
    root = args.root.resolve()
    sys.path.insert(0, str(root / 'tools'))
    from test_vertex_cache import function
    sys.path.insert(0, str(root / 'tools/win'))
    import build3ds

    out = args.out or Path(build3ds.WORK) / 'selftest/fragment_requests'
    out.mkdir(parents=True, exist_ok=True)
    backend = (args.backend or root / 'port/wiiu/gx2_backend.cpp').read_text()
    (out / 'ps_entry_production.inc').write_text(function(backend, 'struct PsEntry {') + ';\n')
    (out / 'ps_cache_production.inc').write_text(backend[backend.index('const DrawState *ps_last_ds;'):backend.index('GX2PixelShader *find_ps(')])
    (out / 'find_ps_production.inc').write_text(function(backend, 'GX2PixelShader *find_ps(') + '\n')
    generator = (root / 'port/wiiu/shadergen.cpp').read_text()
    # GLSL string literals contain braces, so retain the whole generator prefix.
    fragment = generator[:generator.index('std::string gen_uber_fs(')]
    (out / 'fragment_generator.inc').write_text(fragment + '\n} // namespace gpu\n')
    keys = (root / 'port/wiiu/shaderkeys.cpp').read_text()
    (out / 'fragment_key_hash.inc').write_text(function(keys, 'u64 fnv64(') + '\n')

    exe = out / 'test_fragment_requests.exe'
    subprocess.run(build3ds.HOST_CXX + ['-std=c++20', '-O1', '-w', '-static', '-I' + str(out),
                   '-I' + str(root / 'port/wiiu'), '-I' + str(root / 'port/pc/win'),
                   '-I' + str(root / 'port/pc'), '-I' + str(root / 'third_party/3dsrecomp/runtime'),
                   '-include', str(root / 'port/pc/shim.h'), str(Path(__file__).with_suffix('.cpp')),
                   '-lpthread', '-o', str(exe)], check=True, timeout=120)
    return subprocess.run([str(exe)], timeout=15).returncode


if __name__ == '__main__':
    raise SystemExit(main())
