#pragma once
#include <cstdio>
#include <cstring>
#include <string>

namespace wiiu::wup_update {
enum class Match { Standalone, Matched, Invalid, WrongBase };
inline bool marker(FILE *file, char (&id)[65]) {
    char bytes[67]{};
    const size_t count = std::fread(bytes, 1, sizeof bytes, file);
    if (std::ferror(file) || (count != 64 && !(count == 65 && bytes[64] == '\n') && !(count == 66 && bytes[64] == '\r' && bytes[65] == '\n'))) return false;
    for (size_t i=0; i<64; ++i)
        if (!((bytes[i]>='0' && bytes[i]<='9') || (bytes[i]>='a' && bytes[i]<='f'))) return false;
    std::memcpy(id, bytes, 64); id[64]=0; return true;
}
inline Match check(const std::string &content) {
    FILE *expected = std::fopen((content+"/3dii-update-base-id.txt").c_str(), "rb");
    if (!expected) return Match::Standalone; // old/full titles have no update requirement
    char wanted[65]{}, actual[65]{};
    bool ok = marker(expected, wanted); std::fclose(expected);
    if (!ok) return Match::Invalid;
    FILE *base = std::fopen((content+"/3dii-base-id.txt").c_str(), "rb");
    if (!base) return Match::WrongBase;
    ok = marker(base, actual); std::fclose(base);
    return ok && std::strcmp(wanted, actual)==0 ? Match::Matched : Match::WrongBase;
}
inline bool menu_enabled(const std::string &content, bool legacy_enabled) {
    FILE *file = std::fopen((content+"/menu_enabled.txt").c_str(), "rb");
    if (!file) return legacy_enabled;
    char bytes[4]{}; const size_t count=std::fread(bytes,1,sizeof bytes,file);
    const bool valid=!std::ferror(file) && (count==1 || (count==2 && bytes[1]=='\n') || (count==3 && bytes[1]=='\r' && bytes[2]=='\n'));
    std::fclose(file);
    return valid && (bytes[0]=='0' || bytes[0]=='1') ? bytes[0]=='1' : legacy_enabled;
}
}
