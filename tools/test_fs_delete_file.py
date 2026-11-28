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
std::errc forced_error{};
unsigned remove_calls = 0;
namespace fsys {
using namespace std::filesystem;
bool remove(const std::string &path, std::error_code &ec) {
    ++remove_calls;
    if (forced_error != std::errc{}) { ec = std::make_error_code(forced_error); return false; }
    return std::filesystem::remove(path, ec);
}
}
CONSTANTS_AND_HELPERS

struct Archive { std::string host; u32 id = 9; } archive;
bool valid = true;
unsigned settled = 0, changed = 0, errors = 0, forgotten = 0;
std::string requested;
Archive *arch(u32, u32) { return valid ? &archive : nullptr; }
struct LowPath { std::string str() const { return requested; } };
LowPath read_path(u32, u32, u32) { return {}; }
std::string sanitize(const std::string &s) { return s; }
void save_changed(const std::string &) { ++changed; }
#define SAVE_SETTLE(path, forget) (++settled, forgotten += (forget), true)
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
    assert(ipc.value == 0 && changed == 1 && settled == 2 && forgotten == 1);
    assert(!fsys::exists(archive.host + requested));
    const auto calls = remove_calls;
    forced_error = std::errc::io_error; // Simulate Aroma's remove(ENOENT)->EIO bug.
    handle(ipc);
    assert(ipc.value == ERR_FILE_NOT_FOUND && remove_calls == calls);
    assert(changed == 1 && forgotten == 1);
    requested = "/keep";
    std::ofstream(archive.host + requested) << "unchanged";
    handle(ipc); // Real I/O failures on an existing file must not look absent.
    assert(ipc.value == ERR_FS_IO && fsys::file_size(archive.host + requested) == 9);
    assert(forgotten == 1 && changed == 1);
    for(auto error : {std::errc::permission_denied, std::errc::read_only_file_system,
                      std::errc::operation_not_permitted}) {
        forced_error = error; handle(ipc);
        assert(ipc.value == ERR_FS_PERMISSION && forgotten == 1 && changed == 1);
    }
    forced_error = std::errc{};
    requested = "/nonempty";
    fsys::create_directory(archive.host + requested);
    std::ofstream(archive.host + requested + "/keep") << "unchanged";
    handle(ipc);
    assert(ipc.value == fs_err(250, 9)); // DeleteFile must not remove directories.
    assert(fsys::file_size(archive.host + requested + "/keep") == 9);
    requested = "/loop/child";
    fsys::create_symlink("loop", archive.host + "/loop");
    handle(ipc);
    assert(ipc.value == ERR_FS_IO && changed == 1 && forgotten == 1);
    assert(delete_error(std::make_error_code(std::errc::directory_not_empty)) == fs_err(240, 9));
    assert(delete_error(std::make_error_code(std::errc::no_space_on_device)) == fs_err(210, 3));
    valid = false;
    const unsigned before = settled;
    handle(ipc);
    assert(ipc.value == ERR_ARCHIVE && settled == before);
    puts("PASS: actual DeleteFile IPC distinguishes missing, I/O, permission, directory and path failures; missing preflight avoids Aroma EIO; failed deletes preserve data/cache");
}
'''


def main():
    source = (ROOT / "third_party/3dsrecomp/runtime/services/fs.cpp").read_text()
    start = source.index("        case 0x0804: {  // DeleteFile")
    end = source.index("        case 0x0805:", start)
    with tempfile.TemporaryDirectory(prefix="3dii-delete-file-") as tmp:
        tmp = Path(tmp)
        cpp = tmp / "test.cpp"
        helpers_start = source.index('static u32 fs_err(')
        helpers_end = source.index('}  // namespace kernel', helpers_start)
        preamble = PRE.replace('CONSTANTS_AND_HELPERS', source[helpers_start:helpers_end])
        cpp.write_text(preamble + source[start:end] + POST)
        exe = tmp / "test"
        subprocess.run(shlex.split(os.environ.get("CXX", "g++")) +
                       ["-std=c++20", "-O2", "-fno-exceptions", str(cpp), "-o", str(exe)], check=True)
        data = tmp / "data"
        data.mkdir()
        subprocess.run([str(exe), str(data)], check=True)


if __name__ == "__main__":
    main()
