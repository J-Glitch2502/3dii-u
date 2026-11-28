// Grow the current WUP's own native save quota through nn_save, never by
// replacing a quota or moving its files. Homebrew directories remain unchanged.
// The CFW capability protocol is documented by wiiu-env/libmocha's
// Mocha_CheckAPIVersion / Mocha_UnlockFSClientEx. Only our private client is unlocked.
#include <coreinit/debug.h>
#include <coreinit/filesystem_fsa.h>
#include <coreinit/ios.h>
#include <coreinit/title.h>
#include <nn/save/save.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <malloc.h>
#include <string>
#include <vector>
#include "rt.h"
#include "wiiu.h"
#include "quota_policy.h"
namespace wiiu {
namespace {
struct FileScope { FileScope(){fs_begin(0xC011);} ~FileScope(){fs_end();} };
struct Client {
    FSAClientHandle fd=-1;
    bool unlocked=false;
    Client(){fd=FSAAddClient(nullptr);}
    ~Client(){if(fd>=0)FSADelClient(fd);}
    bool unlock(){
        if(unlocked)return true;
        if(fd<0)return false;
        alignas(64) u32 request[16]{};request[0]=0xF8;
        const int mcp=IOS_Open("/dev/mcp",IOS_OPEN_READ);
        if(mcp<0)return false;
        const int result=IOS_Ioctl(mcp,100,request,4,request,8);IOS_Close(mcp);
        if(result || request[0]!=0xCAFEBABE || request[1]<1)return false;
        memset(request,0,sizeof request);
        unlocked=IOS_Ioctl(fd,0x28,request,sizeof request,request,sizeof request)==0;
        return unlocked;
    }
};
struct Journal {char magic[8];u64 title;u64 target;u64 previous;u32 account;u32 reserved;};
constexpr const char *journal_path="/vol/save/common/.3dii-quota-request";
std::map<std::string,u64> available;
std::string strip_device(std::string path){if(path.compare(0,3,"fs:")==0)path.erase(0,3);return path;}
std::string quota_root(const std::string &path){
    if(path.compare(0,10,"/vol/save/") || path.find("/../")!=std::string::npos || path.find("/./")!=std::string::npos)return {};
    const size_t end=path.find('/',10);if(end==std::string::npos)return {};
    const auto name=path.substr(10,end-10);
    if(name!="common" && (name.size()!=8 || name.find_first_not_of("0123456789abcdefABCDEF")!=std::string::npos))return {};
    return path.substr(0,end);
}
bool read_file(Client &c,const std::string &path,std::string &data){
    FSAStat stat{};if(FSAGetStat(c.fd,path.c_str(),&stat) || stat.size>131072)return false;
    auto *buf=static_cast<char*>(memalign(64,std::max<size_t>(64,stat.size)));
    if(!buf)return false;
    FSAFileHandle f;bool ok=FSAOpenFileEx(c.fd,path.c_str(),"r",static_cast<FSMode>(0x660),FS_OPEN_FLAG_NONE,0,&f)==0;
    if(ok){ok=FSAReadFile(c.fd,buf,1,stat.size,f,0)==(int)stat.size;ok=FSACloseFile(c.fd,f)==0 && ok;}
    if(ok)data.assign(buf,stat.size);free(buf);return ok;
}
bool write_at(Client &c,const std::string &path,const void *data,u32 bytes,u32 offset,bool create){
    auto *buf=static_cast<char*>(memalign(64,std::max(64u,bytes)));if(!buf)return false;
    memcpy(buf,data,bytes);FSAFileHandle f;bool ok=FSAOpenFileEx(c.fd,path.c_str(),create?"w":"r+",static_cast<FSMode>(0x660),FS_OPEN_FLAG_NONE,0,&f)==0;
    if(ok){if(offset)ok=FSASetPosFile(c.fd,f,offset)==0;if(ok)ok=FSAWriteFile(c.fd,buf,1,bytes,f,0)==(int)bytes;ok=FSACloseFile(c.fd,f)==0 && ok;}
    free(buf);return ok;
}
bool field(const std::string &xml,const char *tag,size_t &at,u64 &value){
    at=xml.find(std::string("<")+tag+" ");if(at==std::string::npos)return false;
    at=xml.find('>',at);if(at==std::string::npos)return false;++at;
    if(at+16>=xml.size() || xml[at+16]!='<' || xml.substr(at,16).find_first_not_of("0123456789abcdefABCDEF")!=std::string::npos)return false;
    value=strtoull(xml.substr(at,16).c_str(),nullptr,16);return true;
}
bool own_metadata(Client &c,u64 title,std::string &path,std::string &volume,std::string &xml){
    if((title>>32)!=0x00050000 || (((title>>24)&255)!=0x3d && ((title>>24)&255)!=0x1d))return false;
    char tail[128];snprintf(tail,sizeof tail,"/usr/title/%08x/%08x/meta/meta.xml",(u32)(title>>32),(u32)title);
    for(const char *v:{"/vol/storage_usb01","/vol/storage_mlc01"}){
        std::string candidate=std::string(v)+tail;
        if(!read_file(c,candidate,xml))continue;
        size_t at;u64 id;if(!field(xml,"title_id",at,id) || id!=title)return false;
        path=candidate;volume=v;return true;
    }
    return false;
}
bool apply(Client &c,const Journal &j){
    if(j.title!=OSGetTitleID() || j.account>1 || !j.target || j.target>quota_policy::maximum || j.target%quota_policy::unit)return false;
    if(j.account){
        const auto current=strip_device(save_account_dir());
        char expected[32];snprintf(expected,sizeof expected,"/vol/save/%08x",j.reserved);
        if(current!=expected)return false;
    }else if(j.reserved)return false;
    std::string meta,volume,xml;if(!own_metadata(c,j.title,meta,volume,xml))return false;
    size_t offset;u64 declared;const char *tag=j.account?"account_save_size":"common_save_size";
    if(!field(xml,tag,offset,declared) || declared>quota_policy::maximum)return false;
    // Fixed-width hexadecimal update keeps the title XML present and well-formed.
    // The durable save journal records the full intended value for retry after interruption.
    const u64 target=j.target;char digits[17],before[17];
    snprintf(digits,sizeof digits,"%016llX",(unsigned long long)target);
    snprintf(before,sizeof before,"%016llX",(unsigned long long)j.previous);
    // A torn field write may contain a mixture of old/new digits. Repair only
    // that known transition; leave unrelated metadata changes for investigation.
    char actual[17];snprintf(actual,sizeof actual,"%016llX",(unsigned long long)declared);
    for(size_t i=0;i<16;++i)if(actual[i]!=before[i] && actual[i]!=digits[i])return false;
    if(declared!=target){
        if(!write_at(c,meta,digits,16,(u32)offset,false) || FSAFlushVolume(c.fd,volume.c_str()))return false;
        std::string verify;if(!read_file(c,meta,verify) || verify.substr(offset,16)!=digits)return false;
    }
    int needed=-1;uint64_t space=~uint64_t{};
    const int checked=SAVECheckSaveDirRequiredUpdate(&needed,&space);
    OSReport("[save-quota] native check=%d needed=%d temporary-space=%llu target=%llu\n",checked,needed,(unsigned long long)space,(unsigned long long)target);
    // Cemu's unimplemented stub reports zero without populating either output.
    if(checked || needed<0 || needed>1)return false;
    const int result=SAVEUpdateSaveDir();
    OSReport("[save-quota] native update=%d\n",result);
    if(result)return false;
    const std::string root=j.account ? save_account_dir() : "/vol/save/common";
    FSAStat after{};
    return FSAGetStat(c.fd,strip_device(root).c_str(),&after)==0 && after.quotaSize>=target;
}
bool recover(Client &c){
    FSAStat status{};const int stat=FSAGetStat(c.fd,journal_path,&status);
    if(stat==FS_ERROR_NOT_FOUND)return true;
    if(stat)return false;
    std::string bytes;if(!read_file(c,journal_path,bytes) || bytes.size()!=sizeof(Journal))return false;
    Journal j;memcpy(&j,bytes.data(),sizeof j);
    if(memcmp(j.magic,"3DQUOTA",8) || !apply(c,j))return false;
    if(FSARemove(c.fd,journal_path) || FSAFlushQuota(c.fd,"/vol/save/common"))return false;
    available.clear();return true;
}
}
bool ensure_save_capacity(const std::string &file,u64 incoming){
    const auto path=strip_device(file),root=quota_root(path);
    if(root.empty())return true; // SD-backed Homebrew is not native title-quota storage.
    FileScope serial;
    auto found=available.find(root);
    if(found!=available.end() && found->second>=incoming){found->second-=incoming;return true;}
    Client c;if(c.fd<0)return false;
    FSAStat status{};int result=FSAGetStat(c.fd,root.c_str(),&status);
    if(!result && (status.flags&FS_STAT_QUOTA)!=FS_STAT_QUOTA){available[root]=~u64{};return true;}
    if(!c.unlock()){OSReport("[save-quota] CFW filesystem access unavailable; no quota metadata changed\n");return false;}
    if(!recover(c))return false;
    if(FSAGetStat(c.fd,root.c_str(),&status))return false;
    if((status.flags&FS_STAT_QUOTA)!=FS_STAT_QUOTA){available[root]=~u64{};return true;}
    uint64_t free=0;if(FSAGetFreeSpaceSize(c.fd,root.c_str(),&free))return false;
    FSAStat old{};u64 old_size=FSAGetStat(c.fd,path.c_str(),&old)==0?old.size:0;
    const u64 target=quota_policy::required(status.quotaSize,free,old_size,incoming);
    if(!target)return false;
    const u64 extra=incoming>old_size?incoming-old_size:0;
    if(target==status.quotaSize){available[root]=free>=extra+quota_policy::headroom?free-extra-quota_policy::headroom:0;return true;}
    Journal j{};memcpy(j.magic,"3DQUOTA",8);j.title=OSGetTitleID();j.target=target;j.account=root!="/vol/save/common";
    if(j.account)j.reserved=strtoul(root.substr(10).c_str(),nullptr,16);
    std::string meta,volume,xml;size_t offset;
    if(!own_metadata(c,j.title,meta,volume,xml) || !field(xml,j.account?"account_save_size":"common_save_size",offset,j.previous))return false;
    if(j.previous>quota_policy::maximum)return false;
    j.target=std::max(target,j.previous);
    OSReport("[save-quota] grow %s: old=%llu free=%llu incoming=%llu target=%llu\n",root.c_str(),(unsigned long long)status.quotaSize,(unsigned long long)free,(unsigned long long)incoming,(unsigned long long)target);
    if(!write_at(c,journal_path,&j,sizeof j,0,true) || FSAFlushQuota(c.fd,"/vol/save/common"))return false;
    if(!apply(c,j))return false;
    FSAStat after{};if(FSAGetStat(c.fd,root.c_str(),&after) || after.quotaSize<target)return false;
    if(FSARemove(c.fd,journal_path) || FSAFlushQuota(c.fd,"/vol/save/common"))return false;
    available.clear();return true;
}
}
