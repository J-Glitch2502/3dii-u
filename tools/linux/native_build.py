#!/usr/bin/env python3
"""Drive the existing builder with native Linux host/PPC tools and optional Wine GLSLC."""
import argparse
import hashlib
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[2]


def load_builder(work, compiler=None):
    original_path = os.environ.get("PATH", "")
    sys.path.insert(0, str(ROOT / "tools/win"))
    import build3ds
    os.environ["PATH"] = original_path
    build3ds.WORK = str(Path(work).resolve())
    build3ds.HOST_CXX = shlex.split(os.environ.get("CXX", "g++"))
    build3ds.HOST_CXX[0] = shutil.which(build3ds.HOST_CXX[0]) or build3ds.HOST_CXX[0]
    def free_mb():
        for line in Path("/proc/meminfo").read_text().splitlines():
            if line.startswith("MemAvailable:"): return int(line.split()[1]) // 1024
        raise RuntimeError("Cannot determine available RAM")
    build3ds.free_mb = free_mb
    def native_run(argv, **kwargs):
        executable = Path(argv[0])
        pe = executable.is_file() and executable.open("rb").read(2) == b"MZ"
        if pe:
            argv = ["wine", str(executable)] + ["Z:" + a.replace("/", "\\") if isinstance(a, str) and a.startswith("/") else a for a in argv[1:]]
            kwargs["env"] = dict(kwargs.get("env", os.environ), WINEDEBUG="-all")
        return subprocess.run(argv, **kwargs)
    build3ds.run = native_run
    if compiler: build3ds.GLSLC = str(Path(compiler).resolve())
    return build3ds


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("command", choices=["code", "shaders", "selftest"])
    ap.add_argument("--game-dir", type=Path)
    ap.add_argument("--name")
    ap.add_argument("--output", type=Path)
    ap.add_argument("--work", type=Path, default=ROOT / "build_native")
    ap.add_argument("--devkitpro", type=Path, default=Path(os.environ.get("DEVKITPRO", "/opt/devkitpro")))
    ap.add_argument("--wut", type=Path)
    ap.add_argument("--cafeglsl-ppc", type=Path)
    ap.add_argument("--compiler", type=Path)
    ap.add_argument("--sdk-alias", type=Path)
    ap.add_argument("--tools-dir", type=Path)
    ap.add_argument("--jobs", type=int, default=min(3, os.cpu_count() or 1))
    a = ap.parse_args()
    if a.jobs < 1: ap.error("--jobs must be positive")
    b = load_builder(a.work, a.compiler)
    if a.command == "selftest":
        sys.path.insert(0, str(ROOT / "tools"))
        import selftest
        selftest.main()
        return
    if not a.game_dir or not a.output: ap.error("--game-dir and --output are required")
    if a.command == "code":
        sdk = a.devkitpro.resolve(); wut = (a.wut or sdk / "wut").resolve()
        targets = {"devkitPPC": sdk / "devkitPPC", "tools": sdk / "tools", "wut": wut}
        digest = hashlib.sha256(str(targets).encode()).hexdigest()[:12]
        alias = a.sdk_alias or Path(tempfile.gettempdir()) / ("3dii-native-sdk-" + digest)
        alias.mkdir(parents=True, exist_ok=True)
        for name, target in targets.items():
            if not target.is_dir(): ap.error("Missing SDK directory: " + str(target))
            link = alias / name
            if link.exists():
                if link.resolve() != target: ap.error("SDK alias already points elsewhere: " + str(link))
            else: link.symlink_to(target, target_is_directory=True)
        tools = a.tools_dir or a.work / "native-tools"; tools.mkdir(parents=True, exist_ok=True)
        sources = list((sdk / "devkitPPC/bin").glob("powerpc-eabi-*")) + [sdk / "tools/bin/elf2rpl", sdk / "tools/bin/wuhbtool"]
        for source in sources:
            if not source.is_file(): continue
            link = tools / (source.name + ".exe")
            if not link.exists(): link.symlink_to(source)
        b.DKP = str(alias); b.WUT = str(alias / "wut"); b.BIN = str(tools.resolve() / "powerpc-eabi-")
        if a.cafeglsl_ppc: b.CAFEGLSL_PPC = str(a.cafeglsl_ppc.resolve())
        os.environ.update(DEVKITPRO=str(alias), DEVKITPPC=str(alias / "devkitPPC"), PATH=str(tools.resolve()) + os.pathsep + str(alias / "devkitPPC/bin") + os.pathsep + str(alias / "tools/bin") + os.pathsep + os.environ["PATH"])
    game = b.Game(a.name or a.game_dir.name, str(a.game_dir.resolve()))
    game.out = str(a.output.resolve()); game.rpx = game.out + "/" + game.name + ".rpx"; game.nm = game.out + "/game.nm"; game.shaders = game.out + "/shaders"
    args = SimpleNamespace(jobs=a.jobs, force=False, extra_flags=None, only=None, opt=None, link_flags=None)
    if a.command == "code": b.build_code(game, args)
    else: b.build_shaders(game, args)


if __name__ == "__main__": main()
