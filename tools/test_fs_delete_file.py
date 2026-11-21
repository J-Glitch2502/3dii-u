#!/usr/bin/env python3
"""Exercise the actual DeleteFile IPC case, including host filesystem failures."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PRE = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
using u32 = uint32_t;
namespace fsys = std::filesystem;
constexpr u32 ERR_ARCHIVE = 1, ERR_FILE_NOT_FOUND = 2;
struct Archive { std::string host; u32 id = 9; } archive;
bool valid = true;
unsigned settled = 0, changed = 0, errors = 0;
std::string requested;
Archive *arch(u32, u32) { return valid ? &archive : nullptr; }
struct LowPath { std::string str() const { return requested; } };
LowPath read_path(u32, u32, u32) { return {}; }
std::string sanitize(const std::string &s) { return s; }
void save_changed(const std::string &) { ++changed; }
#define SAVE_SETTLE(path, forget) (++settled)
#define LOG(...) (++errors)
struct IPC {
    u32 value = 99;
    u32 operator[](unsigned) const { return 0; }
    void result(u32 v) { value = v; }
};
void handle(IPC &ipc) { switch (0x0804) {
'''
POST = r'''
} }
int main(int argc, char **argv) {
    archive.host = argv[1];
    IPC ipc;
    requested = "/present";
    std::ofstream(archive.host + requested) << "save bytes";
    handle(ipc);
    assert(ipc.value == 0 && changed == 1 && settled == 1);
    assert(!fsys::exists(archive.host + requested));
    handle(ipc); // Missing file is a guest error, with no save-change notification.
    assert(ipc.value == ERR_FILE_NOT_FOUND && changed == 1 && settled == 2);
    requested = "/nonempty";
    fsys::create_directory(archive.host + requested);
    std::ofstream(archive.host + requested + "/keep") << "unchanged";
    handle(ipc); // Real ENOTEMPTY/EEXIST: formerly terminated -fno-exceptions builds.
    assert(ipc.value == ERR_FILE_NOT_FOUND && errors == 1 && changed == 1);
    assert(fsys::file_size(archive.host + requested + "/keep") == 9);
    requested = "/loop/child";
    fsys::create_symlink("loop", archive.host + "/loop");
    handle(ipc); // Real ELOOP, a second OS error, also must return normally.
    assert(ipc.value == ERR_FILE_NOT_FOUND && errors == 2 && changed == 1);
    valid = false;
    const unsigned before = settled;
    handle(ipc);
    assert(ipc.value == ERR_ARCHIVE && settled == before);
    puts("PASS: DeleteFile success, missing file, ENOTEMPTY, ELOOP, invalid archive; failed operations preserve data and return IPC errors");
}
'''


def main():
    source = (ROOT / "third_party/3dsrecomp/runtime/services/fs.cpp").read_text()
    start = source.index("        case 0x0804: {  // DeleteFile")
    end = source.index("        case 0x0805:", start)
    with tempfile.TemporaryDirectory(prefix="3dii-delete-file-") as tmp:
        tmp = Path(tmp)
        cpp = tmp / "test.cpp"
        cpp.write_text(PRE + source[start:end] + POST)
        exe = tmp / "test"
        subprocess.run(shlex.split(os.environ.get("CXX", "g++")) +
                       ["-std=c++20", "-O2", "-fno-exceptions", str(cpp), "-o", str(exe)], check=True)
        data = tmp / "data"
        data.mkdir()
        subprocess.run([str(exe), str(data)], check=True)


if __name__ == "__main__":
    main()
