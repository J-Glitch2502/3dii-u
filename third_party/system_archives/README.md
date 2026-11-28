# Open-source country-list archive

`0004009b00010402.romfs` is an open-source replacement for the 3DS country list,
not a Nintendo system dump. The Builder supplies it only when a game needs this
archive and the selected build cache has no valid copy. User-provided copies take
precedence. Other system archives retain their existing import behavior.

Source: https://github.com/B3n30/citra_system_archives
Pinned revision: `bbe60b5477fc8d7f317d9a82a9f2e6a789af931c`.
Copyright 2018 Weiyi Wang; GPL-2.0-or-later, see LICENSE.
Corresponding generator and input data are included under `source/`.

Rebuild from this directory:

```sh
cd source/country_list
python3 country-archive.py -c country.json ../../0004009b00010402.romfs
```
