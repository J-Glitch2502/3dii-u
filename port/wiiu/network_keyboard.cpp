#include "network_keyboard.h"
#include "wiiu.h"
#include <nn/swkbd.h>
#include <coreinit/filesystem.h>
#include <coreinit/dynload.h>
#include <gx2/event.h>
#include <malloc.h>
#include <mutex>
#include <algorithm>
#include <cstring>
namespace wiiu::network_keyboard {
namespace {
std::mutex mutex;
bool requested,opened,fallback,finished,ok,native_created;
std::string heading,text,original;
unsigned maximum=128;
bool password,numpad;
VPADStatus pad{};
void *memory;
FSClient *client;
char16_t initial_text[513]{};
void release_native() {
    if(native_created){GX2DrawDone();nn::swkbd::Destroy();}
    native_created=false;
    if(client){FSDelClient(client,FS_ERROR_FLAG_NONE);free(client);client=nullptr;}
    free(memory);memory=nullptr;
}
}
void request(const std::string &title,const std::string &value,unsigned length,bool hidden,bool digits) {
    std::lock_guard lock(mutex);if(requested||opened)return;
    heading=title;original=value;maximum=std::min(512u,length);password=hidden;numpad=digits;
    requested=true;fallback=false;finished=false;ok=false;pad={};
}
void input(const VPADStatus &p){std::lock_guard lock(mutex);const auto trigger=pad.trigger;pad=p;pad.trigger|=trigger;}
bool active(){std::lock_guard lock(mutex);return requested||opened;}
bool take(std::string &out,bool &accepted){std::lock_guard lock(mutex);if(!finished)return false;out=text;accepted=ok;finished=false;return true;}
bool draw(){
    std::lock_guard lock(mutex);
    if(!requested&&!opened)return false;
    if(fallback)return false;
    if(requested){
        requested=false;
        // WUT dynamically loads swkbd.rpl and restores its temporary allocator.
        // Cemu and consoles without that system module use the existing overlay.
        OSDynLoad_Module module{};void *entry=nullptr;
        if(wiiu::debug_flag("cemu") || OSDynLoad_Acquire("swkbd.rpl",&module)!=OS_DYNLOAD_OK){fallback=opened=true;return false;}
        const bool available=OSDynLoad_FindExport(module,OS_DYNLOAD_EXPORT_FUNC,"SwkbdCreate__3RplFPUcQ3_2nn5swkbd10RegionTypeUiP8FSClient",&entry)==OS_DYNLOAD_OK&&entry;
        OSDynLoad_Release(module);
        if(!available){fallback=opened=true;return false;}
        const auto bytes=nn::swkbd::GetWorkMemorySize(0);
        if(bytes<=16*1024*1024)memory=memalign(64,bytes);
        client=static_cast<FSClient*>(memalign(64,sizeof(FSClient)));
        if(!memory||!client){if(client){free(client);client=nullptr;}free(memory);memory=nullptr;fallback=opened=true;return false;}
        std::memset(client,0,sizeof *client);
        if(FSAddClient(client,FS_ERROR_FLAG_NONE)<0){free(client);client=nullptr;free(memory);memory=nullptr;fallback=opened=true;return false;}
        nn::swkbd::CreateArg create;create.workMemory=memory;create.fsClient=client;create.regionType=nn::swkbd::RegionType::USA;
        native_created=nn::swkbd::Create(create);
        if(!native_created){release_native();fallback=opened=true;return false;}
        nn::swkbd::AppearArg appear{};
        appear.keyboardArg.configArg.keyboardMode=numpad?nn::swkbd::KeyboardMode::Numpad:nn::swkbd::KeyboardMode::Utf8;
        appear.keyboardArg.configArg.disableNewLine=true;
        appear.keyboardArg.configArg.showWordSuggestions=false;
        appear.inputFormArg.maxTextLength=maximum;
        appear.inputFormArg.passwordMode=password?nn::swkbd::PasswordMode::Hide:nn::swkbd::PasswordMode::Clear;
        appear.inputFormArg.higlightInitialText=true;
        std::fill(std::begin(initial_text),std::end(initial_text),0);
        for(size_t i=0;i<std::min<size_t>(original.size(),maximum);i++)initial_text[i]=static_cast<unsigned char>(original[i]);
        appear.inputFormArg.initialText=initial_text;
        if(!nn::swkbd::AppearInputForm(appear)){release_native();fallback=opened=true;return false;}
        opened=true;
    }
    nn::swkbd::ControllerInfo info;info.vpad=&pad;
    nn::swkbd::Calc(info);pad.trigger=0;
    if(nn::swkbd::IsNeedCalcSubThreadFont())nn::swkbd::CalcSubThreadFont();
    if(nn::swkbd::IsNeedCalcSubThreadPredict())nn::swkbd::CalcSubThreadPredict();
    const bool accept=nn::swkbd::IsDecideOkButton(nullptr);
    if(accept||nn::swkbd::IsDecideCancelButton(nullptr)){
        text.clear();
        if(accept){const char16_t *p=nn::swkbd::GetInputFormString();for(unsigned i=0;p&&p[i]&&i<maximum;i++){unsigned c=p[i];if(c>=32&&c<127)text+=char(c);}}
        ok=accept;nn::swkbd::DisappearInputForm();release_native();opened=false;finished=true;return true;
    }
    nn::swkbd::DrawTV();nn::swkbd::DrawDRC();pad.trigger=0;return true;
}
bool fallback_pending(){std::lock_guard lock(mutex);return fallback&&opened;}
void fallback_finish(const std::string &value,bool accepted){std::lock_guard lock(mutex);text=value;ok=accepted;opened=requested=fallback=false;finished=true;}
std::string label(){std::lock_guard lock(mutex);return heading;}
std::string initial(){std::lock_guard lock(mutex);return original;}
unsigned limit(){std::lock_guard lock(mutex);return maximum;}
bool secret(){std::lock_guard lock(mutex);return password;}
bool numeric(){std::lock_guard lock(mutex);return numpad;}
}
