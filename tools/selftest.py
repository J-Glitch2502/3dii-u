#!/usr/bin/env python3
"""Every check that needs no game and no Cemu, in a few seconds: selftest.py. Exit code 0 if all passed.
The checks of whole games are tools/check_ports.py's."""
import contextlib
import io
import os
import subprocess
import sys
import types

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__))).replace('\\', '/')
sys.path.insert(0, ROOT + '/tools/win')
import build3ds  # noqa: E402  the PC's compiler and where the sources are


def vertex_arrays():
    """port/wiiu/vertex_arrays.h, built for this PC"""
    exe = build3ds.WORK + '/selftest/test_vertex_arrays.exe'
    os.makedirs(os.path.dirname(exe), exist_ok=True)
    return [build3ds.HOST_CXX + ['-std=c++20', '-w', '-D__WIIU__', f'-I{build3ds.PORT}/wiiu', f'-I{build3ds.PORT}/pc/win', f'-I{build3ds.PORT}/pc', f'-I{build3ds.RT}',
                                 f'-I{build3ds.RT}/gpu', '-include', f'{build3ds.PORT}/pc/shim.h', '-static', ROOT + '/tools/test_vertex_arrays.cpp', '-lpthread',
                                 '-o', exe], [exe]]


def present_rule():
    """port/wiiu/present_rule.h against made-up frame times, built for this PC"""
    exe = build3ds.WORK + '/selftest/test_present_rule.exe'
    os.makedirs(os.path.dirname(exe), exist_ok=True)
    return [build3ds.HOST_CXX + ['-std=c++20', '-O1', '-w', f'-I{build3ds.PORT}/wiiu', '-static', ROOT + '/tools/test_present_rule.cpp', '-lpthread', '-o', exe], [exe]]


def screen_layouts():
    """port/wiiu/screen_layouts.h on the Wii U's TV sizes and its GamePad, built for this PC"""
    exe = build3ds.WORK + '/selftest/test_screen_layouts.exe'
    os.makedirs(os.path.dirname(exe), exist_ok=True)
    return [build3ds.HOST_CXX + ['-std=c++20', '-O1', '-w', f'-I{build3ds.PORT}/wiiu', '-static', ROOT + '/tools/test_screen_layouts.cpp', '-o', exe], [exe]]


def gfd_index():
    """Shader file extents, missing programs, and truncated index tables."""
    exe = build3ds.WORK + '/selftest/test_gfd_index.exe'
    os.makedirs(os.path.dirname(exe), exist_ok=True)
    return [build3ds.HOST_CXX + ['-std=c++20', '-O1', f'-I{build3ds.PORT}/wiiu', '-static', ROOT + '/tools/test_gfd_index.cpp', '-o', exe], [exe]]


def memory_fills():
    """port/wiiu/memory_fills.h against made-up fills and writes of a made-up memory, built for this PC"""
    exe = build3ds.WORK + '/selftest/test_memory_fills.exe'
    os.makedirs(os.path.dirname(exe), exist_ok=True)
    return [build3ds.HOST_CXX + ['-std=c++20', '-O1', '-w', f'-I{build3ds.PORT}/wiiu', '-static', ROOT + '/tools/test_memory_fills.cpp', '-lpthread', '-o', exe], [exe]]


def ro_tree():
    """third_party/3dsrecomp/runtime/services/ro_tree.h against an export tree made up for three names, built for this PC"""
    exe = build3ds.WORK + '/selftest/test_ro_tree.exe'
    os.makedirs(os.path.dirname(exe), exist_ok=True)
    return [build3ds.HOST_CXX + ['-std=c++20', '-O1', '-w', f'-I{build3ds.RT}/services', '-static', ROOT + '/tools/test_ro_tree.cpp', '-o', exe], [exe]]


def languages():
    """third_party/3dsrecomp/runtime/services/languages.h: which language a game is told, built for this PC"""
    exe = build3ds.WORK + '/selftest/test_languages.exe'
    os.makedirs(os.path.dirname(exe), exist_ok=True)
    return [build3ds.HOST_CXX + ['-std=c++20', '-O1', '-w', f'-I{build3ds.RT}', '-static', ROOT + '/tools/test_languages.cpp', '-o', exe], [exe]]


def cpu_parallel():
    """ARM byte-lane operations against an independent scalar reference."""
    exe = build3ds.WORK + '/selftest/test_cpu_parallel.exe'
    os.makedirs(os.path.dirname(exe), exist_ok=True)
    return [build3ds.HOST_CXX + ['-std=c++17', '-O1', '-static', ROOT + '/tools/test_cpu_parallel.cpp', '-o', exe], [exe]]


def latte_program():
    """port/wiiu/latte_program.h against small programs made up word by word, built for this PC"""
    exe = build3ds.WORK + '/selftest/test_latte_program.exe'
    os.makedirs(os.path.dirname(exe), exist_ok=True)
    return [build3ds.HOST_CXX + ['-std=c++20', '-O1', '-w', f'-I{build3ds.PORT}/wiiu', '-static', ROOT + '/tools/test_latte_program.cpp', '-lpthread', '-o', exe], [exe]]


def save_from_content():
    """port/wiiu/save_storage.cpp's save_from_content on folders of this PC, built for this PC with the Wii U's headers stood in for"""
    exe = build3ds.WORK + '/selftest/test_save_from_content.exe'
    fake = build3ds.WORK + '/selftest/fake_wut'
    os.makedirs(fake + '/coreinit', exist_ok=True)
    for h in ('dynload.h', 'filesystem.h', 'time.h'):
        open(f'{fake}/coreinit/{h}', 'a').close()
    return [build3ds.HOST_CXX + ['-std=c++20', '-O1', '-w', f'-I{fake}', f'-I{build3ds.PORT}/wiiu', f'-I{build3ds.PORT}/pc/win', f'-I{build3ds.PORT}/pc',
                                 f'-I{build3ds.RT}', '-include', f'{build3ds.PORT}/pc/shim.h', '-static', ROOT + '/tools/test_save_from_content.cpp',
                                 f'{build3ds.PORT}/pc/win/win_compat.cpp', '-lpthread', '-lwinmm', '-o', exe], [exe]]


def shadergen():
    """the shader generator, which build3ds keeps built for this PC"""
    with contextlib.redirect_stdout(io.StringIO()):
        return build3ds.build_shadergen(types.SimpleNamespace(jobs=4, force=False, extra_flags=None, only=None, opt=None))


def open_bools():
    """tools/test_open_bools.cpp with the shader generator's own objects"""
    shadergen()
    exe = build3ds.WORK + '/selftest/test_open_bools.exe'
    os.makedirs(os.path.dirname(exe), exist_ok=True)
    objs = [f'{build3ds.WORK}/shadergen/{n}.o' for n in ('shadergen.cpp', 'shaderkeys.cpp', 'shader.cpp', 'shadergen_stubs.cpp')]
    return [build3ds.HOST_CXX + ['-std=c++20', '-O1', '-w', f'-I{build3ds.PORT}/wiiu', f'-I{build3ds.PORT}/pc/win', f'-I{build3ds.PORT}/pc', f'-I{build3ds.RT}',
                                 f'-I{build3ds.RT}/gpu', '-include', f'{build3ds.PORT}/pc/shim.h', '-static', ROOT + '/tools/test_open_bools.cpp', *objs,
                                 '-lpthread', '-o', exe], [exe]]


def gsx():
    """tools/test_gsx.cpp with the shader generator's own objects"""
    shadergen()
    exe = build3ds.WORK + '/selftest/test_gsx.exe'
    os.makedirs(os.path.dirname(exe), exist_ok=True)
    objs = [f'{build3ds.WORK}/shadergen/{n}.o' for n in ('shadergen.cpp', 'shaderkeys.cpp', 'shader.cpp', 'shadergen_stubs.cpp')]
    return [build3ds.HOST_CXX + ['-std=c++20', '-O1', '-w', f'-I{build3ds.PORT}/wiiu', f'-I{build3ds.PORT}/pc/win', f'-I{build3ds.PORT}/pc', f'-I{build3ds.RT}',
                                 f'-I{build3ds.RT}/gpu', '-include', f'{build3ds.PORT}/pc/shim.h', '-static', ROOT + '/tools/test_gsx.cpp', *objs,
                                 '-lpthread', '-o', exe], [exe]]



def ir_ipc():
    exe = build3ds.WORK + '/selftest/test_ir.exe'
    os.makedirs(os.path.dirname(exe), exist_ok=True)
    return [build3ds.HOST_CXX + ['-std=c++20', '-O1', '-w', '-static', ROOT + '/tools/test_ir.cpp', '-lpthread', '-o', exe], [exe]]

def ir_memory():
    exe = build3ds.WORK + '/selftest/test_ir_memory.exe'
    os.makedirs(os.path.dirname(exe), exist_ok=True)
    return [build3ds.HOST_CXX + ['-std=c++20', '-O1', '-w', f'-I{build3ds.PORT}/pc/win', f'-I{build3ds.PORT}/pc', f'-I{build3ds.RT}',
                              '-include', f'{build3ds.PORT}/pc/shim.h', '-static', ROOT + '/tools/test_ir_memory.cpp', '-lpthread', '-o', exe], [exe]]

def main():
    py = sys.executable
    checks = {
        'IR IPC': ir_ipc(),
        'IR memory': ir_memory(),
        'IR input': [[py, ROOT + '/tools/test_ir_input.py']],
        'PC autoplay': [[py, ROOT + '/tools/test_pc_autoplay.py']],
        'compiler policy': [[py, ROOT + '/builder/test_compiler_policy.py']],
        'trial tools': [[py, ROOT + '/tools/test_trial_tools.py']],
        'homebrew dev publisher': [[py, ROOT + '/tools/test_ftp_homebrew_dev.py']],
        'vertex arrays': vertex_arrays(),
        'present rule': present_rule(),
        'screen layouts': screen_layouts(),
        'memory fills': memory_fills(),
        'surface retirement': [[py, ROOT + '/tools/test_surface_retirement.py']],
        'handwriting latency log': [[py, ROOT + '/tools/wiiu_diagnostics/test_handwriting_latency.py']],
        'shader file validation': gfd_index(),
        'module exports': ro_tree(),
        'game languages': languages(),
        'CPU byte operations': cpu_parallel(),
        'Wii U audio ring': [[py, ROOT + '/tools/test_wiiu_audio_ring.py']],
        'RomFS read-ahead': [[py, ROOT + '/tools/test_romfs_readahead.py']],
        'filesystem delete errors': [[py, ROOT + '/tools/test_fs_delete_file.py']],
        'compiled programs': latte_program(),
        'open bools': open_bools(),
        'geometry shaders': gsx(),
        'native geometry conditionals': [[py, ROOT + '/tools/test_gs_native_conditionals.py']],
        'command lists': [[py, ROOT + '/tools/test_shadergen_lists.py', shadergen()]],
        'program rules': [[py, ROOT + '/tools/gsh_rules.py', '--self-check']],
        'fragment variants': [[py, ROOT + '/tools/fs_variants.py', '--self-check']],
        'picture check': [[py, ROOT + '/tools/test_check_pictures.py']],
        'missed code': [[py, ROOT + '/tools/test_learn_missed.py']],
        'switch through a helper': [[py, ROOT + '/tools/test_recomp_switch.py']],
        'mixed indirect safe callback': [[py, ROOT + '/tools/test_recomp_mixed_icall.py']],
        'ARM bulk copy equivalence': [[py, ROOT + '/tools/test_recomp_bulk_copy.py']],
        'VFP modified immediates': [[py, ROOT + '/tools/test_vfp_immediate.py']],
        'Y2R coefficient replies': [[py, ROOT + '/tools/test_y2r_coefficients.py']],
        'Y2R tiled movie blocks': [[py, ROOT + '/tools/test_y2r_movie_blocks.py']],
        'build markers': [[py, ROOT + '/tools/test_mark_built.py']],
        'WUP packer': [[py, ROOT + '/tools/test_pack_wup.py']],
        'boot sound': [[py, ROOT + '/tools/test_boot_sound.py']],
        'game art': [[py, ROOT + '/builder/test_covers.py'], [py, ROOT + '/builder/test_title_art.py']],
        'game detection': [[py, ROOT + '/tools/win/test_detect.py']],
        'shader logs': [[py, ROOT + '/builder/test_shaderlogs.py']],
        'user files': [[py, ROOT + '/builder/test_user_files.py']],
        '3DS saves in a build': [[py, ROOT + '/builder/test_saves.py'], *save_from_content()],
        'system archives': [[py, ROOT + '/builder/test_system_archives.py']],
        'builder app': [[py, ROOT + '/builder/test_app.py']],
        'builder window': [[py, ROOT + '/builder/test_window.py']],
        'builder updater': [[py, ROOT + '/builder/test_updater.py']],
        'DLC reader': [[py, ROOT + '/builder/selftest.py']],
    }
    failed = []
    for name, commands in checks.items():
        for argv in commands:
            r = subprocess.run(argv, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors='replace')
            if r.returncode:
                failed.append(name)
                print(f'FAILED {name}:\n{r.stdout[-1500:]}')
                break
        else:
            print(f'ok     {name}')
    sys.exit(1 if failed else 0)


if __name__ == '__main__':
    main()
