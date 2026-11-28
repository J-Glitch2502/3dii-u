"""Exercise the production checkpoint storage selection with mocked FS backends."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
def function(file, signature):
    text = (ROOT / file).read_text()
    start = text.index(signature)
    return text[start:text.index('\n}', start) + 2]

source = r'''
#include <cassert>
#include <cstring>
#include <mutex>
using u32 = unsigned;
using FSAClientHandle = int;
using FSError = int;
constexpr int FS_ERROR_OK=0, FS_ERROR_FLAG_ALL=0, COMMON=255;
constexpr u32 FS_STAT_QUOTA=0x60000000u;
struct FSStat { u32 flags; unsigned long long quotaSize; };
static int stat_error, stat_flags, direct_error, hooked_error, calls;
static bool direct;
int FSAAddClient(void*) { return 1; }
void FSADelClient(int) {}
int __wrap_FSAGetStat(int,const char*,FSStat *s) { s->flags=stat_flags;return stat_error; }
void OSReport(const char*,...) {}
bool checkpoint_quota_required=true;
std::mutex quota_mutex;
int client,block;
int hook(int*,int*,int account,int) { assert(account==COMMON);++calls;return hooked_error; }
int (*flush_quota)(int*,int*,int,int)=hook;
void fs_begin(int) {}
void fs_end() {}
bool fsa_direct_flush_quota(const char *path,int &result) {
 assert(!strcmp(path,"/vol/save/common"));
 if(!direct)return false;
 ++calls;result=direct_error;return true;
}
'''
source += function('port/wiiu/fsa_direct.cpp', 'int fsa_report_save_quota(')
source += '\n' + function('port/wiiu/save_storage.cpp', 'int flush_checkpoint_storage(')
source += r'''
int main() {
 stat_flags=0x80000000u;
 checkpoint_quota_required=fsa_report_save_quota("/vol/save/common")!=0;
 hooked_error=-6;
 assert(!checkpoint_quota_required && flush_checkpoint_storage()==0 && calls==0);
 stat_flags=0xe0000000u;
 checkpoint_quota_required=fsa_report_save_quota("/vol/save/common")!=0;
 assert(checkpoint_quota_required && flush_checkpoint_storage()==-6 && calls==1);
 hooked_error=0;assert(flush_checkpoint_storage()==0 && calls==2);
 direct=true;direct_error=-9;assert(flush_checkpoint_storage()==-9 && calls==3);
 direct_error=0;assert(flush_checkpoint_storage()==0 && calls==4);
 stat_error=-6;stat_flags=0;
 checkpoint_quota_required=fsa_report_save_quota("/vol/save/common")!=0;
 assert(checkpoint_quota_required); // a failed probe must not suppress real failures
 flush_quota=nullptr;assert(flush_checkpoint_storage()<0);
}
'''
with tempfile.TemporaryDirectory() as directory:
    cpp = Path(directory) / 'storage.cpp'
    exe = Path(directory) / 'storage'
    cpp.write_text(source)
    subprocess.run(['c++', '-std=c++17', '-pthread', str(cpp), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print('PASS: SD directories omit quota calls; WUP commits common only; backend failures propagate; unknown storage stays strict')
