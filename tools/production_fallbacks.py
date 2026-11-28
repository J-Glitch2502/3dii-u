"""Required PC fallback compilation, shared by native and Windows game builds."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import tempfile

from build_fallback_pack import build
from fallback_pack import read_packet, write_packet, replace_generics

ROOT = Path(__file__).resolve().parents[1]


def compiler_path():
    platform = 'windows-x86_64' if os.name == 'nt' else 'linux-x86_64'
    name = 'glslcompiler.exe' if os.name == 'nt' else 'glslcompiler'
    path = Path(os.environ.get('THREEDII_FALLBACK_GLSLC') or
                ROOT / 'third_party/cafeglsl_fallback' / platform / 'bin' / name)
    if not path.is_file():
        raise ValueError('Missing CafeGLSL 1.0.0-rc1 fallback compiler. Run tools/setup_fallback_compiler.py; '
                         'the legacy shader compiler is not a compatible replacement.')
    return path.resolve()


def prefix_packet(source, destination, budget, max_entries=2048):
    """Retain complete programs only; omitted optimizations use bounded fallback."""
    from build_fallback_pack import builder_module
    builder = builder_module()
    keys, data = read_packet(source)
    blocks = list(builder.gfd_blocks(data))
    selected, pieces, used = 0, [], 32 + 32
    pair = []
    for kind, head, body in blocks:
        if kind == 1: break
        pair.append((head, body))
        if kind != 7: continue
        cost = sum(len(h) + len(b) for h, b in pair) + 8
        if used + cost > budget or selected == max_entries: break
        pieces.extend(pair);pair=[];used += cost;selected += 1
    if not selected: return 0, len(keys)
    end = next((h,b) for k,h,b in blocks if k == 1)
    write_packet(destination, keys[:selected], data[:32] + b''.join(h+b for h,b in pieces) + b''.join(end))
    return selected, len(keys)-selected


def integrate(shaders, generator, work, jobs, *, generator_args=('--fallback',), compiler=None):
    shaders, generator, work = Path(shaders), Path(generator), Path(work)
    compiler = Path(compiler) if compiler else compiler_path()
    identity = hashlib.sha256(generator.read_bytes() + compiler.read_bytes()).hexdigest()[:24]
    baseline = work / 'fallback-cache' / identity
    baseline.parent.mkdir(parents=True, exist_ok=True)
    from build_fallback_pack import builder_module
    builder_module()  # supplies the shared cross-platform build lock
    from objcache import file_locks
    with file_locks(baseline.with_suffix('.lock')):
        if not (baseline/'complete.json').is_file():
            if baseline.is_symlink(): raise ValueError('Fallback cache cannot be a symlink')
            if baseline.exists(): shutil.rmtree(baseline)
            with tempfile.TemporaryDirectory(prefix='fallback-', dir=baseline.parent) as temp:
                temp = Path(temp)
                build(generator,compiler,temp/'full',jobs,full=True,generator_args=generator_args)
                build(generator,compiler,temp/'bounded',jobs,generator_args=generator_args)
                (temp/'complete.json').write_text(json.dumps({'compiler':'CafeGLSL 1.0.0-rc1','full':16,'bounded':336}))
                ready = temp.parent/(temp.name+'-ready')
                temp.rename(ready)
                ready.replace(baseline)
    with tempfile.TemporaryDirectory(prefix='fallback-game-',dir=work) as temp:
        temp = Path(temp)
        inputs = shaders/'fragment-states.bin'
        if not inputs.is_file(): raise ValueError('Missing discovered fragment inputs; regenerate shaders first')
        parts = [baseline/'bounded/fallback-shaders.bin']
        report = {'compiler':'CafeGLSL 1.0.0-rc1, block uniforms + fit_program',
                  'full_families':16,'bounded_families':336,'material_families':0,'lighting_families':0,
                  'optimization_limits':{},'hardware_validation':'not tested',
                  'coverage_note':'Existing specialized programs and their index stay unchanged. Omitted families use bounded/general fallback.'}
        # These limits concern optional speedups, never removal of exact shaders.
        _, bounded = read_packet(parts[0]); budget=(16<<20)-len(bounded)-336*8-12
        key_budget=2048-336
        generated=[]
        for kind,lighting in [('material',False),('lighting',True)]:
            try:
                path=build(generator,compiler,temp/kind,jobs,materials=inputs,lighting=lighting,
                           generator_args=generator_args,max_families=1024)
            except ValueError as error:
                if str(error)!='No fallback families generated': raise
                continue
            generated.append((kind,path))
        for index,(kind,path) in enumerate(generated):
            allocation=budget//(len(generated)-index)
            selected=temp/(kind+'-selected.bin')
            kept,omitted=prefix_packet(path,selected,allocation,key_budget//(len(generated)-index))
            report[kind+'_families']=kept
            candidate=json.loads((temp/kind/'fallback-build-report.json').read_text())
            report['optimization_limits'][kind]=omitted+candidate.get('uncompiled_families',0)
            key_budget-=kept
            if kept:
                parts.append(selected); budget-=selected.stat().st_size
        from fallback_pack import merge_packets
        combined=parts[0]
        for i,part in enumerate(parts[1:]):
            dest=temp/('merged-'+str(i)+'.bin');merge_packets(combined,part,dest);combined=dest
        final=temp/'corrected'
        replace_generics(shaders,baseline/'full/fallback-shaders.bin',final)
        # Only generic PS blocks change; exact shaders, VS and index are checked above.
        shutil.copy2(final/'shaders.gsh',shaders/'shaders.gsh')
        shutil.copy2(combined,shaders/'fallback-shaders.bin')
        report['pack_bytes']=(shaders/'fallback-shaders.bin').stat().st_size
        (shaders/'fallback-build-report.json').write_text(json.dumps(report,indent=2)+'\n')
        print('Optimized fallback pack: '+json.dumps(report),flush=True)
        return report
