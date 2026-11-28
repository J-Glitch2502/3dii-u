// cfg: system configuration blocks (language, region, user name, ...).
#include <fstream>

#include "../loader.h"
#include "../service.h"
#include "../settings.h"
#include "services.h"
#include <fstream>

namespace kernel {

int g_system_language = -1;

u32 cfg_languages() { return regions_languages(g_game.regions); }

int cfg_menu_language(int chosen) {
    static const int built = [] {
        std::ifstream file(g_game.dir + "/language.txt");
        int value;
        char extra;
        return file >> value && value >= 0 && value < 12 && !(file >> extra) ? value : -1;
    }();
    return pick_language(cfg_languages(), chosen, g_system_language, built);
}

// 0 JPN, 1 USA, 2 EUR, 3 AUS, 4 CHN, 5 KOR, 6 TWN: of the game's own regions (SMDH), one that has its language (languages.h)
static u32 cfg_region() {
    if (const char *r = getenv("RECOMP_REGION")) return (u32)atoi(r);
    return pick_region(g_game.regions, cfg_language());
}

// The game's language, the same all run, as a game may ask more than once (languages.h): the one picked in the options menu
// (settings.ini), then the build default and the console's own language.
int cfg_language() {
    static const int language = [] {
        if (const char *l = getenv("RECOMP_LANGUAGE")) {
            char *end;
            const long value = strtol(l, &end, 10);
            if (end != l && !*end && value >= 0 && value < 12) return (int)value;
        }
        return cfg_menu_language(g_settings.language);
    }();
    return language;
}

// country (CFG country code) and names matching the region
struct Country { u8 code; const char *name, *province; };
static Country cfg_country() {
    switch (cfg_region()) {
    case 0: return {1, "Japan", "Tokyo"};
    case 1: return {49, "United States", "New York"};
    case 4: return {160, "China", "Beijing"};
    case 5: return {136, "Korea", "Seoul"};
    case 6: return {128, "Taiwan", "Taipei"};
    default: return {110, "United Kingdom", "England"};
    }
}

static void fill_block(u32 id, u32 dst, u32 size) {
    memset(gptr(dst), 0, size);
    auto put_utf16 = [&](u32 at, const char *s, u32 max) {
        for (u32 i = 0; s[i] && i < max; i++) w16(at + 2 * i, (u8)s[i]);
    };
    switch (id) {
    case 0x00050005: {  // stereo camera settings
        float v[8] = {62.0f, 289.0f, 76.80020141601563f, 46.08010101318359f, 10.0f, 5.0f, 55.58000183105469f, 21.56999969482422f};
        for (u32 i = 0; i < 8 && (i + 1) * 4 <= size; i++) {
            u32 u;
            memcpy(&u, &v[i], 4);
            w32(dst + 4 * i, u);
        }
        break;
    }
    case 0x00070001:  // sound output mode: stereo
        w8(dst, 1);
        break;
    case 0x00090000:  // console unique id
    case 0x00090001:
        if (size >= 8) w64(dst, 0x0000123456789ABCull);
        break;
    case 0x000A0000:  // user name
        put_utf16(dst, "Mario", std::min<u32>(size / 2, 10));
        break;
    case 0x000A0001:  // birthday
        if (size >= 2) { w8(dst, 1); w8(dst + 1, 1); }
        break;
    case 0x000A0002:  // language
        w8(dst, (u8)cfg_language());
        break;
    case 0x000B0000:  // country
        if (size >= 4) { w8(dst + 2, 2); w8(dst + 3, cfg_country().code); }
        break;
    case 0x000B0001:  // country name (16 languages x 0x40 utf16)
        for (u32 l = 0; l < 16 && (l + 1) * 0x80 <= size; l++) put_utf16(dst + l * 0x80, cfg_country().name, 0x40);
        break;
    case 0x000B0002:  // province name
        for (u32 l = 0; l < 16 && (l + 1) * 0x80 <= size; l++) put_utf16(dst + l * 0x80, cfg_country().province, 0x40);
        break;
    case 0x000C0000:  // parental controls: none
    case 0x000C0001:
    case 0x000C0002:
        break;
    case 0x000D0000:  // EULA version agreed
        if (size >= 4) w32(dst, 0x00007F7F);
        break;
    case 0x000F0004:  // system model: CTR
        w8(dst, 0);
        break;
    case 0x00100001:
        break;
    case 0x00110000:
        break;
    case 0x00130000:  // debug
        break;
    case 0x00160000:
        break;
    case 0x00170000:
        break;
    default:
        VLOG("[cfg] block %08x (size %x) zeroed\n", id, size);
    }
}

struct Cfg : Service {
    SERVICE_CHECKPOINT(Cfg)
    using Service::Service;
    void handle(IPC &ipc) override {
        switch (ipc.cmd()) {
        case 0x0001: {  // GetConfigInfoBlk2(size, id, mapped buffer)
            u32 size = ipc[1], id = ipc[2], dst = ipc[4];
            VLOG("[cfg] GetConfigInfoBlk2 %08x size %x\n", id, size);
            fill_block(id, dst, size);
            ipc.ok(1, 2);
            ipc.set(2, (size << 4) | 0xC);
            ipc.set(3, dst);
            break;
        }
        case 0x0401: {  // GetConfigInfoBlk8 (cfg:s / cfg:i)
            u32 size = ipc[1], id = ipc[2], dst = ipc[4];
            fill_block(id, dst, size);
            ipc.ok(1, 2);
            ipc.set(2, (size << 4) | 0xC);
            ipc.set(3, dst);
            break;
        }
        case 0x0002:  // SecureInfoGetRegion
        case 0x0406:
            ipc.ok(2);
            ipc.set(2, cfg_region());
            break;
        case 0x0003:  // GenHashConsoleUnique
            ipc.ok(3);
            ipc.set(2, 0x33646D6F ^ ipc[1]);
            ipc.set(3, 0x6F534841);
            break;
        case 0x0004:  // GetRegionCanadaUSA
            ipc.ok(2);
            ipc.set(2, 0);
            break;
        case 0x0005:  // GetSystemModel
            ipc.ok(2);
            ipc.set(2, 0);
            break;
        case 0x0006:  // GetModelNintendo2DS (1: not a 2DS)
            ipc.ok(2);
            ipc.set(2, 1);
            break;
        case 0x0009:  // GetCountryCodeString
            ipc.ok(2);
            ipc.set(2, 'G' | ('B' << 8));
            break;
        case 0x000A:  // GetCountryCodeID
            ipc.ok(2);
            ipc.set(2, 110);
            break;
        case 0x000B:  // IsFangateSupported
            ipc.ok(2);
            ipc.set(2, 1);
            break;
        default:
            unimplemented(ipc);
        }
    }
};

void register_cfg() {
    register_service(std::make_shared<Cfg>("cfg:u"));
    register_service(std::make_shared<Cfg>("cfg:s"));
    register_service(std::make_shared<Cfg>("cfg:i"));
}

}  // namespace kernel
