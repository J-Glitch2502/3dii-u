"""A game's cover art, for its start-up screens and Wii U Menu icon in place of the 48x48 icon scaled up. It comes from GameTDB
(art.gametdb.com), by the four letters at the end of the game's product code (CTR-P-AMKE: AMKE), and is kept in a folder so each is fetched
once. This is the one thing the builder downloads: the four letters are all it sends."""
import os
import urllib.error
import urllib.request

URL = 'https://art.gametdb.com/3ds/coverHQ/{region}/{game}.jpg'
# GameTDB files covers by language; the product code's last letter says where the game was sold
REGIONS = {'E': ('US', 'EN'), 'P': ('EN', 'FR', 'DE', 'ES', 'IT'), 'J': ('JA',), 'K': ('KO',), 'W': ('ZH',)}
ANY = ('US', 'EN', 'JA')
TIMEOUT = 15


def game_code(product):
    """AMKE from CTR-P-AMKE, or None for a product code that isn't a game's"""
    code = (product or '').rsplit('-', 1)[-1].upper()
    return code if len(code) == 4 and code.isalnum() else None


def cover(product, folder):
    """The path of this game's cover in folder, downloaded if it isn't there yet. None if GameTDB has none, or there's no connection."""
    code = game_code(product)
    if not code:
        return None
    path = os.path.join(folder, code + '.jpg')
    if os.path.isfile(path):
        return path
    regions = REGIONS.get(code[3], ()) + tuple(r for r in ANY if r not in REGIONS.get(code[3], ()))
    for region in regions:
        try:
            request = urllib.request.Request(URL.format(region=region, game=code), headers={'User-Agent': '3DiiU-Builder'})
            with urllib.request.urlopen(request, timeout=TIMEOUT) as r:
                data = r.read(8 << 20)
        except urllib.error.HTTPError:  # no cover under this language
            continue
        except OSError:  # no connection: nothing to wait for
            return None
        if data[:3] != b'\xff\xd8\xff':  # not a JPEG
            continue
        os.makedirs(folder, exist_ok=True)
        with open(path + '.part', 'wb') as f:
            f.write(data)
        os.replace(path + '.part', path)
        return path
    return None
