// Which language a game is told (cfg.cpp), apart from the service so a PC test can check it (tools/test_languages.cpp).
#pragma once
#include <cstdint>
#include <initializer_list>

namespace kernel {
// Languages as a 3DS and a Wii U both number them: 0 Japanese, 1 English, 2 French, 3 German, 4 Italian, 5 Spanish, 6 Chinese,
// 7 Korean, 8 Dutch, 9 Portuguese, 10 Russian, 11 Chinese (Taiwan). A 3DS offers the ones of where it was sold, and a game is made
// for the ones of its region. region: CFG's (0 JPN, 1 USA, 2 EUR, 3 AUS, 4 CHN, 5 KOR, 6 TWN); a bit each.
inline uint32_t region_languages(uint32_t region) {
    switch (region) {
    case 0: return 1u << 0;
    case 1: return 1u << 1 | 1u << 2 | 1u << 5;  // English, French, Spanish
    case 4: return 1u << 6;
    case 5: return 1u << 7;
    case 6: return 1u << 11;
    default: return 1u << 1 | 1u << 2 | 1u << 3 | 1u << 4 | 1u << 5 | 1u << 8 | 1u << 9 | 1u << 10;  // and German, Italian, Dutch, Portuguese, Russian
    }
}

// chosen (the options menu's, -1 for none), else the console's own, if offered has it; else English, or the region's one language
inline int pick_language(uint32_t offered, int chosen, int console) {
    for (int l : {chosen, console})
        if (l >= 0 && l < 12 && (offered >> l & 1)) return l;
    return offered & 2 ? 1 : __builtin_ctz(offered);
}
}  // namespace kernel
