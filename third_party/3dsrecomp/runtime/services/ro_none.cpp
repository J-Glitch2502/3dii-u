// For a game with no CRO modules, whose generated table.c has no slots. In a file of its own: gcc would fold these into ro.cpp if it could
// see them there, and a game's real table would be ignored.
#include "../cpu.h"

#ifndef RECOMP_MODULES
extern "C" {
extern __attribute__((weak)) const GuestCro guest_cros[1] = {};  // extern: otherwise gcc takes a const for private, and refuses
extern __attribute__((weak)) const u32 guest_cro_count = 0;
}
#endif
