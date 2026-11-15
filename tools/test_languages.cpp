// Checks, on a PC, which language a game is told (runtime/services/languages.h): the player's choice, the console's own, or the region's.
// `python tools/selftest.py` builds and runs it.
#include <cstdio>

#include "services/languages.h"

using namespace kernel;

static int failures;
static void expect(bool ok, const char *what) {
    if (!ok) {
        failures++;
        printf("FAILED: %s\n", what);
    }
}

int main() {
    enum { JP = 0, EN = 1, FR = 2, DE = 3, IT = 4, ES = 5, ZH = 6, KO = 7, NL = 8, PT = 9, RU = 10, TW = 11 };
    enum { JPN = 0, USA = 1, EUR = 2, AUS = 3, CHN = 4, KOR = 5, TWN = 6 };
    const uint32_t eur = region_languages(EUR), usa = region_languages(USA);
    expect(pick_language(eur, -1, DE) == DE, "a European game follows a German Wii U");
    expect(pick_language(usa, -1, DE) == EN, "an American game on a German Wii U plays in English");
    expect(pick_language(usa, -1, FR) == FR && pick_language(usa, -1, ES) == ES, "an American game takes French and Spanish");
    expect(pick_language(eur, IT, DE) == IT, "the player's choice comes before the console's");
    expect(pick_language(usa, DE, FR) == FR, "a choice the region lacks (a hand-edited settings.ini) is passed over");
    expect(pick_language(eur, 12, -1) == EN && pick_language(eur, -1, 99) == EN, "values out of range are passed over");
    expect(pick_language(region_languages(JPN), -1, EN) == JP, "a Japanese game is told Japanese");
    expect(pick_language(region_languages(KOR), EN, EN) == KO && pick_language(region_languages(CHN), -1, -1) == ZH &&
               pick_language(region_languages(TWN), -1, -1) == TW,
           "Korea, China and Taiwan have one language each");
    expect(region_languages(AUS) == eur && (eur >> NL & 1) && (eur >> PT & 1) && (eur >> RU & 1) && !(usa >> PT & 1),
           "Australia is Europe's set, with Dutch, Portuguese and Russian; America's has no Portuguese");
    if (failures) return 1;
    printf("game languages ok\n");
    return 0;
}
