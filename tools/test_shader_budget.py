"""Exercise production fitter, partition, install, scan and offline gates without a game."""
import argparse
import contextlib
import importlib.util
import io
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile

ROOT = next(p for p in Path(__file__).resolve().parents
            if (p / 'third_party/3dsrecomp/runtime/gpu/pica.h').exists())
sys.dont_write_bytecode = True
sys.path.insert(0, str(ROOT / 'tools/win'))
import build3ds as original


def function(text, signature):
    masked = re.sub(r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"',
                    lambda match: ' ' * len(match[0]), text)
    start = text.index(signature)
    body = masked.index('{', start)
    depth, end = 1, body + 1
    while depth:
        depth += (masked[end] == '{') - (masked[end] == '}')
        end += 1
    return text[start:end]


def block(kind, body):
    return struct.pack('>8I', 0x424c4b7b, 32, 1, 0, kind, len(body), 0, 0) + body


def exported(register, burst=0):
    return [register << 15, 0x688 | burst << 17 | 40 << 23 | 1 << 21]


def control(kind, target=0):
    return [target, kind << 23]


def gfd(vs, ps=1, vs_words=None, ps_words=None, omitted_header=None):
    data = struct.pack('>8I', 0x47667832, 32, 0, 0, 0, 0, 0, 0)
    for kind, count in [(3, vs), (6, ps)]:
        if count is None:
            continue
        words = vs_words if kind == 3 else ps_words
        if words is None:
            words = exported(max(count - 1, 0))
        if kind != omitted_header:
            data += block(kind, struct.pack('>I', count))
        data += block(5 if kind == 3 else 7, struct.pack(f'<{len(words)}I', *words))
    return data


def fixtures():
    constant_pos = [0, 0, 0, 0, 0xc0002000, 0xb24 | 40 << 23 | 1 << 21]
    constant_pixel = [0xc0000000, 0x94200924]
    escaped = [2, 10 << 23, *exported(0), 4, 8 << 26,
               *exported(48), 1 << 31, 16 | 25 << 7 | 48 << 21]
    nested_branch = [*control(6, 7), *control(10, 4), *control(10, 4),
                     *control(0, 0xffffffff), *control(13, 6),
                     *control(9, 6), *control(5, 1), *exported(0)]
    nested_loop = [*control(6, 7), *control(6, 5), *control(8, 4),
                   *control(9, 4), *control(5, 2), *control(8, 6),
                   *control(5, 1), *exported(0)]

    def texture(src, dst):
        return [2, 1 << 23, *exported(0), 16 | src << 16, dst, 0, 0]

    cases = [(n, gfd(n), n in (48, 49, 124, None), {0} if n in (49, 124) else set())
             for n in (48, 49, 124, 125, 0, None)]
    cases += [
        ('TEX source', gfd(4, vs_words=texture(4, 0)), False, set()),
        ('TEX destination', gfd(4, vs_words=texture(0, 4)), False, set()),
        ('clause temporary export', gfd(124, vs_words=exported(123, 1)), False, set()),
        ('burst overflow', gfd(124, vs_words=exported(127, 1)), False, set()),
        ('truncated ALU', gfd(4, vs_words=[2, 8 << 26, *exported(0)]), False, set()),
        ('short size', gfd(4, vs_words=[0]), False, set()),
        ('zero pixel', gfd(49, 0), False, set()),
        ('maximum pixel ordinary count', gfd(124, 124), True, set()),
        ('pixel exceeds physical124', gfd(124, 125, ps_words=exported(0)), False, set()),
        ('PS150 cannot use the allocation as ordinary GPRs', gfd(100, 150, ps_words=exported(0)), False, set()),
        ('constant zero pixel', gfd(49, 0, ps_words=constant_pixel), True, set()),
        ('unary zero VS', gfd(0, vs_words=[2, 8 << 26, 0, 0x924 | 40 << 23 | 1 << 21,
                                         248 | 253 << 13 | 3 << 23 | 1 << 31,
                                         16 | 25 << 7 | 124 << 21]), True, set()),
        ('constant POS zero VS', gfd(0, vs_words=constant_pos), True, set()),
        ('jump past first EOP hides R48', gfd(1, vs_words=escaped), False, set()),
        ('jump out of bounds', gfd(1, vs_words=[*control(10, 0xffffffff), *exported(0)]), False, set()),
        ('ordinary CALL', gfd(1, vs_words=[*control(18), *exported(0)]), False, set()),
        ('ordinary RET', gfd(1, vs_words=[*control(20), *exported(0)]), False, set()),
        ('noncanonical CALL_FS address', gfd(1, vs_words=[*control(19, 100), *exported(0)]), False, set()),
        ('noncanonical CALL_FS count', gfd(1, vs_words=[0, 19 << 23 | 1 << 13, *exported(0)]), False, set()),
        ('nested branches', gfd(1, vs_words=nested_branch), True, set()),
        ('nested loops', gfd(1, vs_words=nested_loop), True, set()),
        ('explicit zero headers', gfd(0, 0, constant_pos, constant_pixel), True, set()),
        ('orphan VS', gfd(0, 0, constant_pos, constant_pixel, 3), False, set()),
        ('orphan PS', gfd(0, 0, constant_pos, constant_pixel, 6), False, set()),
        ('invalid GSX policy', gfd(125), True, {0}),
    ]
    return cases


def gsx_caller_fixture(builder, out):
    folder = out / 'gsx_caller'
    glsl = folder / 'glsl'
    glsl.mkdir(parents=True, exist_ok=True)
    (glsl / 'list.txt').write_text('fixture.vert\nfixture.frag\n')
    recorded = []
    saved = builder.run_jobs, builder.merge_gfd, builder.check_registers

    def merged(order, path):
        Path(path).write_bytes(gfd(49))
        return {builder.VS_HEAD: 1, builder.PS_HEAD: 1}

    try:
        builder.run_jobs = lambda *args: []
        builder.merge_gfd = merged
        builder.check_registers = lambda path, droppable=frozenset(): recorded.append(set(droppable))
        (glsl / 'gsx.txt').write_text('0\n')
        with contextlib.redirect_stdout(io.StringIO()):
            builder.compile_shaders(str(folder), 1)
        (glsl / 'gsx.txt').unlink()
        with contextlib.redirect_stdout(io.StringIO()):
            builder.compile_shaders(str(folder), 1)
    finally:
        builder.run_jobs, builder.merge_gfd, builder.check_registers = saved
    return [] if recorded == [{0}, set()] else [('GSX caller forwarding', recorded)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', type=Path, default=ROOT)
    parser.add_argument('--baseline', action='store_true')
    parser.add_argument('--out', type=Path)
    args = parser.parse_args()
    selected = args.source_root.resolve()

    def source(name):
        return selected / name if (selected / name).exists() else ROOT / name

    with tempfile.TemporaryDirectory(prefix='3dii-shader-budget-') as temp:
        out = args.out or Path(temp)
        out.mkdir(parents=True, exist_ok=True)
        if not args.baseline:
            assert re.search(r'kOrdinary\s*=\s*124\b', source('port/wiiu/shader_budget.h').read_text())
            backend = source('port/wiiu/gx2_backend.cpp').read_text()
            modes = function(backend, 'void shader_mode()') + '\n' + function(backend, 'void require_vs_registers(')
            (out / 'shader_budget_gpu.inc').write_text(modes)
            (out / 'shader_budget_scan.inc').write_text(function(backend, 'static void gfd_scan('))
            install = function(backend, 'void install_compiled()')
            (out / 'shader_budget_install.inc').write_text(install)
            assert install.index('require_vs_registers(') < install.index('v.shader = d.vs_shader')
            main_source = source('tools/shadergen_main.cpp').read_text()
            start = main_source.index('// The runtime reserves this VS maximum once;')
            standalone = function(main_source[start:], '// The runtime reserves this VS maximum once;')
            (out / 'shader_budget_standalone.inc').write_text(standalone)
            assert 'GX2SetShaderMode(' not in backend
            for name in ('void restore_context()', 'void begin_draw_common(',
                         'void begin_blit()', 'void gfx_thread_init()'):
                assert 'shader_mode();' in function(backend, name), name
            worker = source('port/wiiu/shader_compile.cpp').read_text()
            assert worker.count('registers, shader_budget::kOrdinary') == 1  # Physical-SD GLSL journal; redirected native-GFD cache deliberately deferred.
            assert 'GX2SetShaderMode' not in worker and 'GX2DrawDone' not in worker

        exe = out / 'test_shader_budget.exe'
        command = original.HOST_CXX + [
            '-std=c++20', '-O1', '-w', '-static', '-I' + str(out),
            '-I' + str(selected / 'port/wiiu'), '-I' + str(ROOT / 'port/wiiu'),
        ]
        if args.baseline:
            command.append('-DBUDGET_BASELINE')
        command += [str(Path(__file__).with_suffix('.cpp')), '-lpthread', '-o', str(exe)]
        subprocess.run(command, check=True)
        cpp_status = subprocess.run([str(exe)]).returncode

        spec = importlib.util.spec_from_file_location('budget_builder', source('tools/win/build3ds.py'))
        builder = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(builder)
        builder.ROOT = selected.as_posix()
        failures = []
        cases = fixtures()
        for name, data, accepted, droppable in cases:
            path = out / 'shaders.gsh'
            path.write_bytes(data)
            (out / 'glsl').mkdir(exist_ok=True)
            (out / 'glsl/list.txt').write_text('fixture.vert\nfixture.frag\n')
            idx = out / 'shaders.idx'
            idx.write_bytes(struct.pack('>8I', 0x53484458, 1, 0, 1, 0, 0, 0, 0)
                            + struct.pack('>4I', 0, 1, 0, 1))
            before, good, message = idx.read_bytes(), True, ''
            try:
                with contextlib.redirect_stdout(io.StringIO()):
                    builder.check_registers(str(path), droppable)
            except (SystemExit, IndexError, ValueError, struct.error) as error:
                good, message = False, str(error)
            if good != accepted or (good and name != 'invalid GSX policy' and idx.read_bytes() != before):
                failures.append((name, 'accept/drop mismatch', message))
            if name == 'invalid GSX policy' and good and struct.unpack_from('>I', idx.read_bytes(), 12)[0] != 0:
                failures.append((name, 'invalid GSX not dropped', message))
            if name is None and not good and 'empty shader pack' not in message:
                failures.append((name, 'empty diagnostic', message))
            if name == 0 and not good and 'zero-register metadata' not in message:
                failures.append((name, 'zero diagnostic', message))
            if name == 125 and not good and not all(part in message for part in ('vertex program 0', 'fixture.vert', '125')):
                failures.append((name, 'stage/index/source/count diagnostic', message))
        failures.extend(gsx_caller_fixture(builder, out))
        print(f'offline budget: {len(cases)} fixtures plus GSX caller present/absent, {len(failures)} failures', failures)
        return 1 if cpp_status or failures else 0


if __name__ == '__main__':
    raise SystemExit(main())
