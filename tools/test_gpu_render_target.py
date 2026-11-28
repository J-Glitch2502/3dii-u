#!/usr/bin/env python3
"""Build a copied synthetic software/GX2 replay title. Developer check; never launches Cemu.

    python tools/test_gpu_render_target.py --out build_win/gpu_replay_probe

The output directory must be new. Review build_proof.json, create an isolated
Cemu profile with check_ports.cemu_profile(), and launch with cemu_run.ps1.
Run tools/gpu_replay_verify.py on the guest's common-save output with
--backend productionGX2 and --reference <out>/analytical.
An empty common directory selects diagnostic240; a default480 marker selects
the production480 setting. Both must report six native draws and zero fallback.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

sys.dont_write_bytecode = True
TOOLS = Path(__file__).resolve().parent
ROOT = next(parent for parent in TOOLS.parents if (parent / 'tools/win/build3ds.py').is_file())


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    output = args.out.resolve()
    if output.exists():
        parser.error('--out must be a new directory')
    scanned_roots = ('port', 'third_party/3dsrecomp/runtime', 'third_party/3dsrecomp/tools/gpu_test')
    if any(output.is_relative_to((ROOT / folder).resolve()) for folder in scanned_roots):
        parser.error('--out must be outside scanned source roots')
    if not output.parent.is_dir():
        parser.error('--out parent must already exist')
    if shutil.disk_usage(output.parent).free < (8 << 30) + (1 << 30):
        parser.error('leave at least8GiB disk reserve')
    output.mkdir()
    source = output / 'source'
    copied = []

    def copy(path, relative):
        target = source / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        before = sha(path)
        shutil.copy2(path, target)
        assert before == sha(target) == sha(path) and target.stat().st_nlink == 1
        copied.append({'path': relative.as_posix(), 'sha256': before})

    for directory in scanned_roots:
        for path in sorted((ROOT / directory).rglob('*')):
            if path.is_file() and path.suffix in ('.cpp', '.h', '.c', '.inc', '.py'):
                copy(path, path.relative_to(ROOT))
    for name in ('tools/fallbackgen.cpp', 'tools/shadergen_main.cpp', 'tools/shadergen_lists.cpp',
                 'tools/win/build3ds.py', 'tools/win/objcache.py', 'tools/win/shadergen_stubs.cpp',
                 'tools/wup_safety.py', 'tools/gsh_rules.py', 'tools/test_gpu_render.cpp',
                 'tools/gpu_replay_verify.py', 'third_party/cafeglsl/glslcompiler.exe',
                 'third_party/cafeglsl/libwinpthread-1.dll', 'third_party/cafeglsl/zlib1.dll',
                 'third_party/stb/stb_truetype.h'):
        copy(ROOT / name, Path(name))
    for name in ('target_backend.cpp', 'target_support.cpp'):
        copy(TOOLS / 'gpu_replay' / name, Path('tools/gpu_replay') / name)
    (output / 'source_manifest.json').write_text(json.dumps(copied, indent=2) + '\n')
    sys.path.insert(0, str(source / 'tools/win'))
    spec = importlib.util.spec_from_file_location('replay_builder', source / 'tools/win/build3ds.py')
    builder = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(builder)
    builder.WORK = str(output / 'build').replace('\\', '/')
    builder.CAFEGLSL_PPC = str(ROOT / 'third_party/cafeglsl_ppc/cafeglsl-1.0.0-rc1-wiiu-ppc').replace('\\', '/')
    work = output / 'build'
    work.mkdir()
    sdk_files = [Path(builder.HOST_CXX[0]), Path(builder.BIN + 'g++.exe'),
                 Path(builder.WUT) / 'lib/libwut.a', Path(builder.WUT) / 'share/wut.specs',
                 Path(builder.WUT) / 'share/wut.ld', Path(builder.CAFEGLSL_PPC) / 'lib/libcafeglsl.a']
    sdk_before = {str(path): sha(path) for path in sdk_files}
    (output / 'sdk_manifest.json').write_text(json.dumps(sdk_before, indent=2) + '\n')
    commands = []

    def run(argv, label, cwd=output, env=None):
        assert shutil.disk_usage(output).free >= 8 << 30
        commands.append({'label': label, 'cwd': str(cwd), 'argv': list(map(str, argv))})
        process = subprocess.run(argv, cwd=cwd, env=env, capture_output=True, text=True, errors='replace',
                                 creationflags=builder.BELOW_NORMAL | subprocess.CREATE_NO_WINDOW)
        (work / (label + '.log')).write_text(process.stdout + process.stderr, encoding='utf-8')
        return process

    def compile_files(sources, flags, label):
        folder = work / label
        folder.mkdir()
        objects, jobs = [], []
        for path in sources:
            obj = folder / (path.stem + '.o')
            objects.append(str(obj))
            argv = flags + ['-c', str(path), '-o', str(obj)]
            jobs.append((path.name, lambda argv=argv, name=label + '_' + path.stem: run(argv, name), None, None))
        assert not builder.run_jobs(jobs, 1, label), 'see per-file build logs'
        return objects

    runtime, port = source / 'third_party/3dsrecomp/runtime', source / 'port'
    gpu = [runtime / 'gpu' / (name + '.cpp') for name in
           ('gpu', 'fragment', 'raster', 'shader', 'texture', 'transfer', 'vertex')]
    fixture = source / 'tools/test_gpu_render.cpp'
    host_sources = gpu + [port / 'pc/stubs.cpp', port / 'pc/vs_log.cpp', port / 'pc/win/win_compat.cpp',
                          runtime / 'gpu_weak.cpp', runtime.parent / 'tools/gpu_test/stub.cpp', fixture]
    host_flags = builder.HOST_CXX + ['-std=c++20', '-O1', '-ffp-contract=off', '-DRECOMP_VS_LOG',
        '-I' + str(port / 'pc/win'), '-I' + str(port / 'pc'), '-I' + str(port / 'wiiu'),
        '-I' + str(runtime), '-I' + str(runtime / 'gpu'), '-include', str(port / 'pc/shim.h')]
    host_objects = compile_files(host_sources, host_flags, 'host')
    executable = work / 'graphics_replay.exe'
    assert run(builder.HOST_CXX + ['-static', *host_objects, '-lpthread', '-lwinmm', '-lgdi32',
                                   '-o', str(executable)], 'host_link').returncode == 0
    host_output = output / 'host_output'
    host_output.mkdir()
    env = {key: value for key, value in os.environ.items() if not key.startswith(('GPU_', 'RECOMP_'))}
    env.update(GPU_THREADS='1', GPU_BACKEND='sw')
    assert run([str(executable), str(host_output)], 'host_run', cwd=host_output, env=env).returncode == 0
    sys.path.insert(0, str(source / 'tools'))
    oracle_spec = importlib.util.spec_from_file_location('target_oracle', source / 'tools/gpu_replay_verify.py')
    oracle = importlib.util.module_from_spec(oracle_spec)
    oracle_spec.loader.exec_module(oracle)
    analytical = output / 'analytical'
    oracle.make_analytical(analytical)
    host_proof = oracle.verify(host_output, 'software', analytical, output / 'host_png')
    assert host_proof['match']
    (output / 'host_proof.json').write_text(json.dumps(host_proof, indent=2) + '\n')

    shadergen = builder.build_shadergen(argparse.Namespace(jobs=1, force=False))
    logs = output / 'shader_logs'
    logs.mkdir()
    for name in ('vs_log.bin', 'fs_log.bin'):
        shutil.copy2(host_output / name, logs / name)
    empty = output / 'empty_pipelines.bin'
    empty.write_bytes(b'')
    title = output / 'title'
    content, code = title / 'content', title / 'code'
    content.mkdir(parents=True)
    code.mkdir()
    assert run([shadergen, str(empty), str(logs), str(content), '-'], 'shadergen').returncode == 0
    builder.compile_shaders(str(content).replace('\\', '/'), 1)

    target_sources = gpu + [port / 'wiiu' / name for name in
        ('shadergen.cpp', 'shaderkeys.cpp', 'shader_compile.cpp', 'ui_font.cpp', 'stubs.cpp')]
    target_sources += [source / 'tools/gpu_replay/target_backend.cpp',
                       source / 'tools/gpu_replay/target_support.cpp', fixture]
    flags = [builder.BIN + 'g++.exe'] + builder.runtime_flags(
        ['-DRECOMP_MEASURE', '-fdata-sections', '-ffp-contract=off'])
    objects = compile_files(target_sources, flags, 'target')
    builder.link_setup()
    (work / 'target.rsp').write_text('\n'.join('"' + path.replace('\\', '/') + '"' for path in objects))
    elf = work / 'graphics_replay.elf'
    link = [builder.BIN + 'g++.exe', '-mcpu=750', '-meabi', '-mhard-float', '-specs=rpx/rpx.specs',
        '-Wl,--gc-sections', '-Wl,--relax', '-Wl,--no-warn-rwx-segments', '@target.rsp', '-Wl,--start-group',
        '-L' + builder.WUT + '/lib', '-L' + builder.CAFEGLSL_PPC + '/lib', '-lcafeglsl', '-lwut',
        '-lstdc++', '-lm', '-lc', '-lsysbase', '-lgcc', '-Wl,--end-group', '-o', str(elf)]
    assert run(link, 'target_link', cwd=work).returncode == 0
    rpx = code / 'graphics_replay.rpx'
    elf2rpl = shutil.which('elf2rpl.exe') or builder.DKP + '/tools/bin/elf2rpl.exe'
    assert run([elf2rpl, str(elf), str(rpx)], 'elf2rpl').returncode == 0
    meta = title / 'meta'
    meta.mkdir()
    for relative in ('code/app.xml', 'code/cos.xml', 'meta/meta.xml'):
        template = ROOT / 'phase0/title_code' / relative
        text = template.read_text().replace('0005000010334455', '000500001052504c')
        text = text.replace('bench.rpx', 'graphics_replay.rpx').replace('3DS Recomp Code Test', 'Synthetic PICA Replay')
        (title / relative).write_text(text)
    check = run([sys.executable, '-B', str(ROOT / 'tools/rpx_check.py'), str(rpx), str(code / 'cos.xml')], 'loader_check')
    assert check.returncode == 0 and 'RESULT: no problems found' in check.stdout
    assert all(sha(source / row['path']) == row['sha256'] for row in copied)
    assert sdk_before == {str(path): sha(path) for path in sdk_files}
    proof = {'rpx': str(rpx), 'rpx_sha256': sha(rpx), 'source_stable': True, 'sdk_stable': True,
        'host_analytical': True, 'target_execution': 'NOT RUN', 'title_id': '000500001052504c',
        'fixture_sha256': sha(fixture), 'shader_pack': {name: sha(content / name) for name in ('shaders.gsh', 'shaders.idx')},
        'fixture_input_sha256': {path.name: sha(path) for path in host_output.glob('*.bin')
            if path.name.endswith(('_commands.bin', '_vertices.bin')) or path.name == 'texture.bin'},
        'maximum_compiler_jobs': 1, 'free_bytes': shutil.disk_usage(output).free,
        'launch_argv_template': ['powershell', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
            '<unchanged runner in isolated Cemu tree>', '-Rpx', str(rpx), '-Low', '1052504c', '-Seconds', '20',
            '-Shots', '1', '-Out', '<new capture directory>'],
        'verify_argv': [sys.executable, str(ROOT / 'tools/gpu_replay_verify.py'), '<common-save-output>',
            '--backend', 'productionGX2', '--reference', str(analytical), '--png-output', '<new PNG directory>'],
        'limits': 'Native240 is diagnostic1x; default480 uses production scaling/readback. Cemu is not console hardware. Texture coverage uses one nearest-filtered 8x8 Morton tile.'}
    (output / 'commands.json').write_text(json.dumps(commands, indent=2) + '\n')
    (output / 'build_proof.json').write_text(json.dumps(proof, indent=2) + '\n')
    print(json.dumps({'rpx': str(rpx), 'rpx_sha256': sha(rpx), 'target_execution': 'NOT RUN'}), flush=True)


if __name__ == '__main__':
    main()
