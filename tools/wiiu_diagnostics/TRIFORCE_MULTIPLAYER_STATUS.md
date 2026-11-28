# Experimental room multiplayer

The Wii U runtime can relay emulated 3DS local wireless through an Azahar-compatible
room server. This is local-play tunnelling, not Nintendo internet-service support.
The bridge uses 3DS-Link's UDS engine and room transport, with Wii U service and
socket adapters. Its source revision and local changes are in
[`third_party/3ds-link/UPSTREAM`](../../third_party/3ds-link/UPSTREAM).

## Shared configuration

Every rebuilt game, whether Homebrew or WUP, reads one optional file:
`SD:/3dii-u/multiplayer-room.txt`. Missing files and blank hosts leave room
networking disabled. Old per-title content files are not consulted.

```ini
host=
port=64000
nickname=auto
console_id=auto
password=
```

Set `host` to the room server's hostname or IPv4 address and `port` to its UDP
port. Relaunch the game after editing. `nickname=auto` uses the active Wii U
account's Mii name, adjusted to the room's ASCII and minimum-length rules.
`console_id=auto` creates a persistent public uniqueness token in the title's
save storage. It does not publish OTP or SEEPROM data. Each client needs a unique
nickname and identity; do not copy one client's generated identity to another.

Rebuilt titles advertise their original 3DS SMDH display name in the room's
member list (for example, `MARIO KART 7`), alongside the selected nickname.
If the display name is unavailable, the runtime uses the executable's short
title, then `3Dii U` if both are missing. The game ID is unchanged. The startup
log records the advertised name and ID as `[uds-room] playing ...`.
Existing executables need rebuilding to receive this display-only correction.

All Builder output modes generate this blank template under `3dii-u/` alongside
the output, or at the SD root when copying directly to a card. Existing settings
are preserved. No personal or public server address is shipped in the defaults.
The normal Windows packager includes `builder/multiplayer-defaults.json` in its
engine as well as the frozen Python module.

For clients outside the host's LAN, the room's UDP port must reach the server PC.
Clients on the host's LAN can use its LAN address. Joining an emulator room and
joining a game's in-game lobby are separate operations. This shared configuration
is available to every game; command compatibility is not universal.

## Implementation

- UDS service calls use 3DS-Link's engine and bridge its events into guest handles.
  Outgoing frames use the room-assigned transmitter MAC so directed replies work.
- AC command IDs distinguish GetStatus (0x0C), GetWifiStatus (0x0D), and
  GetLastErrorCode (0x0A). The shared radio-status byte is enabled for a configured
  room; transport readiness is reported separately.
- The Wii U UDP adapter uses nonblocking receives and aligns both the receive
  buffer and capacity to 64 bytes, as native nsysnet requires. Cemu did not enforce
  that alignment requirement. Oversized datagrams are rejected.
- Mario Kart 7's supported regional title IDs enable Download Play lobby services
  before the game's UDS session. IPC adapters explicitly convert host words and
  guest little-endian names, node records and reboot passphrases.
- The room worker pumps Download Play while the resumable dispatcher waits;
  optional adapters are created before that worker starts. The room member is
  destroyed before the engine and lobby objects its callbacks use.
- APT profile wrapping uses AES-CCM with the shared compatibility key used by the
  tested Azahar peer. An optional SD override remains supported. This is separate
  from console-unique credentials and from room identity.
- Wireless IPC diagnostics require verbose logging. Socket diagnostics are bounded.

## Verification on 26–27 November 2026

- Real Wii U: ENet connection and room join succeeded after fixing receive
  alignment, using the active account's Mii name.
- Tri Force Heroes: Cemu joined an Azahar-hosted in-game lobby after correcting
  the radio-status byte and outbound transmitter MAC. The user confirmed the join.
- Mario Kart 7: Cemu and a locally patched Azahar reached an in-game lobby and
  started a race. Controls, a complete race and long-session stability were not
  established by that startup test. Azahar needed a fresh Download Play discovery
  timestamp in this test; that emulator change is outside this repository.
- The native Wii U Mario Kart runtime compiled and linked. The packaged Windows
  toolchain compiled all 71 runtime sources and compiled/linked Tri Force Heroes.
- The actual frozen GUI passed its window checks under Steam Proton. This is not
  a native Windows hardware test.
- Host checks cover room packet byte order and native UDP alignment, scatter/gather,
  truncation and nonblocking receives. Builder tests cover blank configuration,
  existing-file preservation and rejection of invalid defaults.
- A local fixture from an actual Azahar APT packet authenticated and round-tripped;
  changing its authentication tag was rejected without changing the output buffer.
  That private packet fixture is not distributed.

## Remaining test gaps

The application now stops the room worker before process cleanup, with a bounded
wait and callbacks suppressed during shutdown. Mario Kart 7 was observed joining
a live room and returning successfully to the real Wii U Menu. This is one normal
exit test; repeated exits and interrupted-session cleanup still need coverage.

Full on-console multiplayer gameplay, three-player synchronization,
scan cancellation, reconnection and compatibility
across other games still need testing. Some service waits synchronously block the
resumable dispatcher until completion or timeout. Do not use save states during
multiplayer sessions: live network peers are not part of a checkpoint.

Do not infer multiplayer correctness from compilation, room join, UDP reachability
or a single successful lobby/race startup alone.
