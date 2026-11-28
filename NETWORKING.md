# Network menu and shared room settings

Open Network from either left-stick menu. B returns one level without disconnecting.
Wireless is Off by default, including older files without an `enabled` field. Off
starts no room socket, DNS lookup, public-directory request, or recurring UDS timer.
Enabling wireless initializes nsysnet independently of the optional debugger.

Homebrew and installed titles share `SD:/3dii-u/multiplayer-room.txt`. Missing files
are created. An absent or empty `web_api_url` is filled with the community directory
`http://88.198.47.46:5000`; custom URLs, endpoints, tokens and unknown keys are kept.
This is a community service, not an official Azahar service. Its availability is
outside the project's control. Wireless stays Off when creating a default file.

```
enabled=0
host=
port=24872
nickname=auto
console_id=auto
password=
web_api_url=http://88.198.47.46:5000
web_api_username=
network_token=
chat_side=right
```

`host` and `port` select one room server; `web_api_url` selects the public directory.
Browse and Refresh fetch its `/lobby` endpoint. Username/token are optional account
credentials for directories requiring verified joins. They remain on the SD card;
room-scoped JWTs are temporary. HTTP credentials are not forwarded on redirects.
Use an HTTPS directory when sending credentials over an untrusted network.

A selects or edits. Left/Right change toggles and choices; Right on a room or the
server-address row measures an ENet connection round trip, without joining that
room. This is not an ICMP ping. Text uses the system keyboard where available,
with the existing overlay keyboard as fallback. Touch taps select, swipes scroll,
the scrollbar jumps through long lists, and the right stick scrolls.

Public-room filters include search, only the running game (Off initially), hide
full/empty rooms, player-count limits and name/player-count/measured-ping sorting.
The connected-room menu lists members and their games, shows chat, sends messages
and disconnects. Disconnect leaves the virtual wireless switch On. Turning wireless
Off also disables in-game wireless. Gameplay continues while the Network submenu
is open so browsing or chatting does not pause an active multiplayer session.

## GamePad chat

Recent room messages appear as white cards with black text. GamePad chat cycles
between Left, Right and Off using A/touch or either D-pad direction. Cards expire
after 15 seconds; full recent messages remain in the room menu.

| GamePad layout | Card position |
| --- | --- |
| Bottom screen | Selected side border, compact text |
| Both screens stacked | Selected side border |
| Both screens side by side | Selected corner of the top/bottom letterbox |
| Top screen only | Room menu only: side borders are too narrow |
| Stretched top screen | Room menu only: no unused border |

Cards never overlap either game screen. They are drawn for scanout and the clean
image is restored afterward, preventing stale rectangles on paused/static screens.
They are hidden while menus, the keyboard, or the Mii selector use the GamePad.

## Validation

`tools/test_network_menu.cpp` checks parsing, touch/scroll gestures and card bounds
against every GamePad layout on both sides. `tools/test_network_lobby.cpp` checks
malformed/oversized directory data and can fetch a real directory when given a URL.
`PYTHONPATH=builder python3 -m unittest builder.test_multiplayer` checks generated
files, default migration, preserved custom configuration and invalid endpoints.
System-keyboard/GX2 behavior still requires real-console testing; Cemu is not a
substitute for that check.
