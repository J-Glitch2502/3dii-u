#include "../port/wiiu/wup_update_guard.h"
#include <filesystem>
#include <fstream>
#include <cassert>
using namespace wiiu::wup_update;
int main(int argc,char **argv) {
    assert(argc==2);std::filesystem::path dir=argv[1];std::filesystem::create_directories(dir);
    auto write=[&](const char *name,const std::string &value){std::ofstream(dir/name,std::ios::binary)<<value;};
    assert(check(dir.string())==Match::Standalone);
    write("3dii-update-base-id.txt",std::string(64,'a')+"\n");assert(check(dir.string())==Match::WrongBase);
    write("3dii-base-id.txt",std::string(64,'b')+"\n");assert(check(dir.string())==Match::WrongBase);
    write("3dii-base-id.txt",std::string(64,'a')+"\n");assert(check(dir.string())==Match::Matched);
    write("3dii-update-base-id.txt",std::string(63,'a')+"\n");assert(check(dir.string())==Match::Invalid);
    write("3dii-update-base-id.txt",std::string(64,'a')+"\nextra");assert(check(dir.string())==Match::Invalid);
    write("3dii-update-base-id.txt",std::string(64,'a')+"\r\n");
    write("3dii-base-id.txt",std::string(64,'a')+"\r\n");assert(check(dir.string())==Match::Matched);
    write("menu_enabled.txt","1\r\n");assert(menu_enabled(dir.string(),false));
    write("menu_enabled.txt","0\r\n");assert(!menu_enabled(dir.string(),true));
    write("menu_enabled.txt","1\n");assert(menu_enabled(dir.string(),false));
    write("menu_enabled.txt","0\n");assert(!menu_enabled(dir.string(),true));
    write("menu_enabled.txt","bad");assert(menu_enabled(dir.string(),true));
    std::filesystem::remove_all(dir);
}
