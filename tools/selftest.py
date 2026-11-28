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


def depth_viewport():
    exe = build3ds.WORK + '/selftest/test_depth_viewport.exe'
    os.makedirs(os.path.dirname(exe), exist_ok=True)
    return [build3ds.HOST_CXX + ['-std=c++20', '-O1', '-w', f'-I{build3ds.PORT}/wiiu', '-static', ROOT + '/tools/test_depth_viewport.cpp', '-o', exe], [exe]]


def surface_readback():
    """GPU readback preserves newer CPU writes and overlapping surfaces."""
    exe = build3ds.WORK + '/selftest/test_surface_readback.exe'
    os.makedirs(os.path.dirname(exe), exist_ok=True)
    return [build3ds.HOST_CXX + ['-std=c++20', '-O1', '-w', f'-I{build3ds.PORT}/wiiu', '-static', ROOT + '/tools/test_surface_readback.cpp', '-lpthread', '-o', exe], [exe], [sys.executable, ROOT + '/tools/test_surface_lifetime.py']]


def shader_native_cache():
    exe = build3ds.WORK + '/selftest/test_shader_native_cache.exe'
    os.makedirs(os.path.dirname(exe), exist_ok=True)
    return [build3ds.HOST_CXX + ['-std=c++20', '-O1', '-w', f'-I{build3ds.PORT}/wiiu', '-static', ROOT + '/tools/test_shader_native_cache.cpp', '-lpthread', '-o', exe], [exe]]


def mii_selection():
    exe = build3ds.WORK + '/selftest/test_mii_selection.exe'
    os.makedirs(os.path.dirname(exe), exist_ok=True)
    return [build3ds.HOST_CXX + ['-std=c++20', '-O1', '-w', f'-I{build3ds.RT}', '-static', ROOT + '/tools/test_mii_selection.cpp', '-lpthread', '-o', exe], [exe]]


def apt_mii_qr():
    exe = build3ds.WORK + '/selftest/test_apt_mii_qr.exe'
    os.makedirs(os.path.dirname(exe), exist_ok=True)
    return [build3ds.HOST_CXX + ['-std=c++20', '-O1', f'-I{build3ds.RT}', '-static', ROOT + '/tools/test_apt_mii_qr.cpp', '-lbcrypt', '-o', exe], [exe]]


def memory_fills():
    """port/wiiu/memory_fills.h against made-up fills and writes of a made-up memory, built for this PC"""
    exe = build3ds.WORK + '/selftest/test_memory_fills.exe'
    os.makedirs(os.path.dirname(exe), exist_ok=True)
    return [build3ds.HOST_CXX + ['-std=c++20', '-O1', '-w', f'-I{build3ds.PORT}/wiiu', '-static', ROOT + '/tools/test_memory_fills.cpp', '-lpthread', '-o', exe], [exe]]


def motion_input():
    """GamePad motion axes and units against the 3DS HID contract, built for this PC"""
    exe = build3ds.WORK + '/selftest/test_motion_input.exe'
    os.makedirs(os.path.dirname(exe), exist_ok=True)
    return [build3ds.HOST_CXX + ['-std=c++20', '-O1', '-w', f'-I{build3ds.PORT}/wiiu', '-static', ROOT + '/tools/test_motion_input.cpp', '-o', exe], [exe]]


def audio_dsp():
    """Production DSP coverage plus serialized Wii U queue/lifetime checks."""
    return [[sys.executable, ROOT + '/tools/test_audio_stability.py']]

def microphone():
    exe = build3ds.WORK + '/selftest/test_mic_stream.exe'
    os.makedirs(os.path.dirname(exe), exist_ok=True)
    return [build3ds.HOST_CXX + ['-std=c++20', '-O1', '-static', ROOT + '/tools/test_mic_stream.cpp', '-o', exe], [exe]]


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
    exe = build3ds.WORK + '/selftest/test_cpu_parallel.exe'
    os.makedirs(os.path.dirname(exe), exist_ok=True)
    return [build3ds.HOST_CXX + ['-std=c++20', '-O1', '-static', ROOT + '/tools/test_cpu_parallel.cpp', '-o', exe], [exe]]


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


def fs_normal():
    """The production fragment normalizer, plus both log converters' key parity."""
    shadergen()
    exe = build3ds.WORK + '/selftest/test_fs_normal.exe'
    os.makedirs(os.path.dirname(exe), exist_ok=True)
    objs = [f'{build3ds.WORK}/shadergen/{n}.o' for n in ('shadergen.cpp', 'shaderkeys.cpp', 'shader.cpp', 'shadergen_stubs.cpp')]
    return [build3ds.HOST_CXX + ['-std=c++20', '-O1', '-w', f'-I{build3ds.PORT}/wiiu', f'-I{build3ds.PORT}/pc/win', f'-I{build3ds.PORT}/pc', f'-I{build3ds.RT}',
                                 f'-I{build3ds.RT}/gpu', '-include', f'{build3ds.PORT}/pc/shim.h', '-static', ROOT + '/tools/test_fs_normal.cpp', *objs,
                                 '-lpthread', '-o', exe], [sys.executable, ROOT + '/tools/test_fs_normal.py', exe], [sys.executable, ROOT + '/tools/test_fragment_requests.py']]


def gsx():
    """tools/test_gsx.cpp with the shader generator's own objects"""
    shadergen()
    exe = build3ds.WORK + '/selftest/test_gsx.exe'
    os.makedirs(os.path.dirname(exe), exist_ok=True)
    objs = [f'{build3ds.WORK}/shadergen/{n}.o' for n in ('shadergen.cpp', 'shaderkeys.cpp', 'shader.cpp', 'shadergen_stubs.cpp')]
    return [build3ds.HOST_CXX + ['-std=c++20', '-O1', '-w', f'-I{build3ds.PORT}/wiiu', f'-I{build3ds.PORT}/pc/win', f'-I{build3ds.PORT}/pc', f'-I{build3ds.RT}',
                                 f'-I{build3ds.RT}/gpu', '-include', f'{build3ds.PORT}/pc/shim.h', '-static', ROOT + '/tools/test_gsx.cpp', *objs,
                                 '-lpthread', '-o', exe], [exe]]


def camera():
    """Real camera IPC with a made-up sensor, padded NV12 conversion, and native buffer ownership."""
    commands = []
    for name, sources in (
        ('camera', [ROOT + '/tools/test_camera.cpp', build3ds.RT + '/services/cam.cpp', build3ds.RT + '/services/y2r.cpp']),
        ('camera_queue', [ROOT + '/tools/test_camera_queue.cpp']),
    ):
        exe = build3ds.WORK + '/selftest/test_' + name + '.exe'
        os.makedirs(os.path.dirname(exe), exist_ok=True)
        commands += [build3ds.HOST_CXX + ['-std=c++20', '-O1', '-w', f'-I{build3ds.RT}', f'-I{build3ds.PORT}/wiiu', '-static',
                                        *sources, '-lpthread', '-o', exe], [exe]]
    exe = build3ds.WORK + '/selftest/test_camera_memory.exe'
    commands += [build3ds.HOST_CXX + ['-std=c++20', '-O1', '-w', f'-I{build3ds.PORT}/pc/win', f'-I{build3ds.PORT}/pc', f'-I{build3ds.RT}',
                                    '-include', f'{build3ds.PORT}/pc/shim.h', '-static', ROOT + '/tools/test_camera_memory.cpp', '-lpthread', '-o', exe], [exe]]
    exe = build3ds.WORK + '/selftest/test_camera_backend.exe'
    commands += [build3ds.HOST_CXX + ['-std=c++20', '-O1', '-w', '-static', f'-I{ROOT}/tools/camera_mock', f'-I{build3ds.WUT}/include',
                                    f'-I{build3ds.PORT}/wiiu', f'-I{build3ds.RT}', f'-I{build3ds.RT}/services',
                                    ROOT + '/tools/test_camera_backend.cpp', '-lpthread', '-o', exe], [exe]]
    return commands


def mapped_output():
    exe = build3ds.WORK + '/selftest/test_mapped_output.exe'
    os.makedirs(os.path.dirname(exe), exist_ok=True)
    return [build3ds.HOST_CXX + ['-std=c++20', '-O1', '-w', '-static', ROOT + '/tools/test_mapped_output.cpp', '-lpthread', '-o', exe], [exe]]


def replay_validation():
    exe = build3ds.WORK + '/selftest/test_replay_validation.exe'
    os.makedirs(os.path.dirname(exe), exist_ok=True)
    return [build3ds.HOST_CXX + ['-std=c++20', '-O1', '-DNOMINMAX', '-ffunction-sections', '-fdata-sections',
            '-Wl,--gc-sections', '-static', f'-I{build3ds.RT}', ROOT + '/tools/test_replay_validation.cpp',
            '-lpthread', '-o', exe], [exe, build3ds.WORK + '/selftest/replay_validation_fixtures']]


def unanswered():
    exe = build3ds.WORK + '/selftest/test_unanswered.exe'
    os.makedirs(os.path.dirname(exe), exist_ok=True)
    return [build3ds.HOST_CXX + ['-std=c++20', '-O1', '-w', '-static', ROOT + '/tools/test_unanswered.cpp', '-lpthread', '-o', exe], [exe]]


def room_wire():
    exe = build3ds.WORK + '/selftest/test_uds_wire.exe'
    os.makedirs(os.path.dirname(exe), exist_ok=True)
    return [build3ds.HOST_CXX + ['-std=c++20', '-O1', '-static', '-I' + ROOT + '/third_party/3ds-link/include',
            ROOT + '/tools/tests/test_uds_wire.cpp', ROOT + '/third_party/3ds-link/src/room_proto.cpp', '-o', exe], [exe]]


def uds_room():
    exe = build3ds.WORK + '/selftest/test_uds_room.exe'
    os.makedirs(os.path.dirname(exe), exist_ok=True)
    return [build3ds.HOST_CXX + ['-std=c++20', '-O1', '-w', '-static', ROOT + '/tools/test_uds_room.cpp', '-lpthread', '-o', exe], [exe]]


def gsp_lcd():
    exe = build3ds.WORK + '/selftest/test_gsp_lcd.exe'
    os.makedirs(os.path.dirname(exe), exist_ok=True)
    return [build3ds.HOST_CXX + ['-std=c++20', '-O1', '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                               '-I' + build3ds.PORT + '/pc/win', '-include', build3ds.PORT + '/pc/shim.h',
                               '-static', ROOT + '/tools/test_gsp_lcd.cpp', '-lpthread', '-o', exe], [exe]]


def ir_ipc():
    exe = build3ds.WORK + '/selftest/test_ir.exe'
    os.makedirs(os.path.dirname(exe), exist_ok=True)
    return [build3ds.HOST_CXX + ['-std=c++20', '-O1', '-w', '-static', ROOT + '/tools/test_ir.cpp', '-lpthread', '-o', exe], [exe]]


def ir_memory():
    exe = build3ds.WORK + '/selftest/test_ir_memory.exe'
    os.makedirs(os.path.dirname(exe), exist_ok=True)
    return [build3ds.HOST_CXX + ['-std=c++20', '-O1', '-w', f'-I{build3ds.PORT}/pc/win', f'-I{build3ds.PORT}/pc', f'-I{build3ds.RT}',
                              '-include', f'{build3ds.PORT}/pc/shim.h', '-static', ROOT + '/tools/test_ir_memory.cpp', '-lpthread', '-o', exe], [exe]]


def save_backups():
    exe = build3ds.WORK + '/selftest/test_save_backup.exe'
    os.makedirs(os.path.dirname(exe), exist_ok=True)
    return [build3ds.HOST_CXX + ['-std=c++20', '-O1', '-static', ROOT + '/tools/test_save_backup.cpp', '-lpthread', '-o', exe],
            [exe], [sys.executable, ROOT + '/tools/test_backup_menu.py']]


def main():
    py = sys.executable
    checks = {
        'save backups': save_backups(),
        'save scheduling and cache': [[py, ROOT + '/tools/test_fs_save.py']],
        'shader compiler recipe': [[py, ROOT + '/tools/test_cafeglsl_recipe.py']],
        'dependency setup': [[py, ROOT + '/tools/test_setup_deps.py']],
        'SDK paths': [[py, ROOT + '/tools/test_sdk_path.py']],
        'IR IPC': ir_ipc(),
        'IR memory': ir_memory(),
        'IR input': [[py, ROOT + '/tools/test_ir_input.py']],
        'PC autoplay': [[py, ROOT + '/tools/test_pc_autoplay.py']],
        'mapped output': mapped_output(),
        'unanswered service replies': unanswered(),
        'GSP LCD registers': gsp_lcd(),
        'module memory ownership': [[py, '-B', ROOT + '/tools/test_module_memory.py'],
                                    [py, '-B', ROOT + '/tools/test_cro_modules.py']],
        'offline UDS IPC': uds_room(),
        'room packet encoding': room_wire(),
        'Wii U UDP adapter': [[py, ROOT + '/tools/test_room_udp.py']],
        'multiplayer configuration': [[py, ROOT + '/builder/test_multiplayer.py']],
        'Builder package inventory': [[py, ROOT + '/builder/test_package_audit.py']],
        'relative callbacks': [[py, ROOT + '/tools/test_relative_callbacks.py']],
        'multiply wrap': [[py, ROOT + '/tools/test_multiply_wrap.py']],
        'object cache safety': [[py, ROOT + '/tools/test_objcache_safety.py']],
        'generated object cache': [[py, ROOT + '/tools/test_generated_objects.py']],
        'camera': camera(),
        'compiler policy': [[py, ROOT + '/builder/test_compiler_policy.py']],
        'trial tools': [[py, ROOT + '/tools/test_trial_tools.py']],
        'homebrew dev publisher': [[py, ROOT + '/tools/test_ftp_homebrew_dev.py']],
        'vertex arrays': vertex_arrays(),
        'vertex cache': [[py, ROOT + '/tools/test_vertex_cache.py'], [py, ROOT + '/tools/test_native_immediate.py']],
        'Wii U vertex worker scheduling': [[py, ROOT + '/tools/test_wiiu_pool_idle.py']],
        'GX2 lookup texture allocation': [[py, ROOT + '/tools/test_gx2_lut_alloc.py']],
        'present rule': present_rule(),
        'screen layouts': screen_layouts(),
        'depth viewport': depth_viewport(),
        'surface readback': surface_readback(),
        'Mii selection': mii_selection(),
        'APT Mii QR': apt_mii_qr(),
        'Wii U Mii database and picker': [[py, ROOT + '/tools/test_wiiu_miis.py']],
        'native shader cache': shader_native_cache() + [[py, ROOT + '/tools/test_shader_cache_check.py'],
                                                       [py, ROOT + '/tools/test_shader_install.py']],
        'draw-state cache eviction': [[py, ROOT + '/tools/test_draw_state_cache.py']],
        'fragment cache transitions': [[py, ROOT + '/tools/test_fragment_requests.py']],
        'shader learning': [[py, ROOT + '/tools/test_shader_learning.py']],
        'shader budget': [[py, ROOT + '/tools/test_shader_budget.py']],
        'memory fills': memory_fills(),
        'surface retirement': [[py, ROOT + '/tools/test_surface_retirement.py']],
        'handwriting latency log': [[py, ROOT + '/tools/wiiu_diagnostics/test_handwriting_latency.py']],
        'shader file validation': gfd_index(),
        'motion input': motion_input(),
        'audio DSP': audio_dsp(),
        'GamePad microphone': microphone(),
        'module exports': ro_tree(),
        'legacy extra data': [[py, ROOT + '/tools/test_fs_extdata.py']],
        'game languages': languages(),
        'CPU byte operations': cpu_parallel(),
        'Wii U audio ring': [[py, ROOT + '/tools/test_wiiu_audio_ring.py']],
        'RomFS read-ahead': [[py, ROOT + '/tools/test_romfs_readahead.py']],
        'filesystem delete errors': [[py, ROOT + '/tools/test_fs_delete_file.py']],
        'compiled programs': latte_program(),
        'open bools': open_bools(),
        'geometry shaders': gsx() + [[py, ROOT + '/tools/gsx_to_gsl.py', '--self-check']],
        'tagged shader materials': [[py, ROOT + '/tools/test_tagged_materials.py']],
        'native geometry conditionals': [[py, ROOT + '/tools/test_gs_native_conditionals.py']],
        'command lists': [[py, ROOT + '/tools/test_shadergen_lists.py', shadergen()]],
        'program rules': [[py, ROOT + '/tools/gsh_rules.py', '--self-check']],
        'fragment variants': [[py, ROOT + '/tools/fs_variants.py', '--self-check']],
        'fragment normalization': fs_normal(),
        'overwritten TEV arithmetic': [[py, ROOT + '/tools/test_tev_prefix.py']],
        'picture check': [[py, ROOT + '/tools/test_check_pictures.py']],
        'graphics replay comparison': [[py, ROOT + '/tools/test_gpu_pngdiff.py'],
                                       [py, ROOT + '/tools/test_gpu_replay_check.py']],
        'graphics replay rendering': [[py, ROOT + '/tools/test_gpu_render.py']],
        'graphics replay validation': replay_validation(),
        'deferred transfer readback': [[py, ROOT + '/tools/test_deferred_transfer.py']],
        'Cemu capture deadline': [[py, ROOT + '/tools/test_cemu_capture.py']],
        'failure categories': [[py, ROOT + '/tools/test_failure_categories.py']],
        'missed code': [[py, ROOT + '/tools/test_learn_missed.py']],
        'switch through a helper': [[py, ROOT + '/tools/test_recomp_switch.py']],
        'mixed indirect safe callback': [[py, ROOT + '/tools/test_recomp_mixed_icall.py']],
        'ARM bulk copy equivalence': [[py, ROOT + '/tools/test_recomp_bulk_copy.py']],
        'VFP modified immediates': [[py, ROOT + '/tools/test_vfp_immediate.py']],
        'Y2R coefficient replies': [[py, ROOT + '/tools/test_y2r_coefficients.py']],
        'Y2R tiled movie blocks': [[py, ROOT + '/tools/test_y2r_movie_blocks.py']],
        'dispatcher transitions': [[py, ROOT + '/tools/test_transition_contract.py']],
        'fallback instruction groups': [[py, ROOT + '/tools/test_fallback_groups.py']],
        'build markers': [[py, ROOT + '/tools/test_mark_built.py']],
        'WUP packer': [[py, ROOT + '/tools/test_pack_wup.py']],
        'ticket safety': [[py, ROOT + '/tools/test_ticket_safety.py'],
                          build3ds.HOST_CXX + ['-std=c++20', '-O1', '-static', f'-I{build3ds.PORT}/wiiu',
                              ROOT + '/tools/test_write_policy.cpp', '-o', build3ds.WORK + '/selftest/test_write_policy.exe'],
                          [build3ds.WORK + '/selftest/test_write_policy.exe']],
        'boot sound': [[py, ROOT + '/tools/test_boot_sound.py']],
        'game art': [[py, ROOT + '/builder/test_covers.py'], [py, ROOT + '/builder/test_title_art.py']],
        'game detection': [[py, ROOT + '/tools/win/test_detect.py']],
        'first game trial': [[py, ROOT + '/tools/test_trial_tools.py']],
        'shader logs': [[py, ROOT + '/builder/test_shaderlogs.py'], [py, ROOT + '/tools/test_dvlb_to_log.py']],
        'user files': [[py, ROOT + '/builder/test_user_files.py']],
        '3DS saves in a build': [[py, ROOT + '/builder/test_saves.py'], *save_from_content()],
        'save migration failures': [[py, ROOT + '/tools/test_save_migration.py']],
        'system archives': [[py, ROOT + '/builder/test_system_archives.py']],
        'builder app': [[py, ROOT + '/builder/test_app.py'], [py, ROOT + '/builder/test_pipeline_outputs.py']],
        'builder input validation': [[py, ROOT + '/builder/test_input_validation.py']],
        'builder lifecycle': [[py, ROOT + '/tools/test_builder_lifecycle.py']],
        'builder window': [[py, ROOT + '/builder/test_window.py'], [py, ROOT + '/builder/test_cards.py']],
        'builder updater': [[py, ROOT + '/builder/test_updater.py']],
        'DLC reader': [[py, ROOT + '/builder/selftest.py']],
        'Download Play content': [[py, ROOT + '/builder/test_dlp_content.py']],
        'CIA conversion': [[py, ROOT + '/builder/test_cia_conversion.py']],
    }
    failed = []
    filter_args = [a.lower() for a in sys.argv[1:] if not a.startswith('-')]
    for name, commands in checks.items():
        if filter_args and not any(f in name.lower() for f in filter_args):
            continue
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
