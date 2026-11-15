"""A game's art, for its start-up screens and Wii U Menu icon in place of the 48x48 icon scaled up, kept in a folder so each is fetched once.
With a SteamGridDB key (each user's own, from steamgriddb.com > Preferences > API; the app keeps it in its settings file) a banner and an
icon come from SteamGridDB, found by the game's name, or by its names in other regions where the site doesn't know this one: those names
and the key are all it sends there. Without one, or where SteamGridDB has nothing, the cover comes from GameTDB (art.gametdb.com), by the
four letters at the end of the game's product code (CTR-P-AMKE: AMKE), which are all it sends. GameTDB's list of 3DS games, fetched once for
all, gives the year a game came out, for the Virtual Console's start-up screens, and its names in other regions. These are the only things
the builder downloads."""
import http.client
import io
import json
import os
import unicodedata
import urllib.error
import urllib.parse
import urllib.request
import zipfile
from xml.etree import ElementTree

URL = 'https://art.gametdb.com/3ds/coverHQ/{region}/{game}.jpg'
# GameTDB files covers by language; the product code's last letter says where the game was sold
REGIONS = {'E': ('US', 'EN'), 'P': ('EN', 'FR', 'DE', 'ES', 'IT'), 'J': ('JA',), 'K': ('KO',), 'W': ('ZH',)}
ANY = ('US', 'EN', 'JA')
TIMEOUT = 15
SGDB = 'https://www.steamgriddb.com/api/v2'
PICTURES = {b'\x89PNG': '.png', b'\xff\xd8\xff': '.jpg'}  # what a download may start with, and the file it becomes


class BadKey(Exception):
    """SteamGridDB turned the key down"""


class NoRedirect(urllib.request.HTTPRedirectHandler):
    """urllib carries a request's headers along a redirect, to whatever host it names: the key goes to the site it's for and no further"""
    def redirect_request(self, *a, **k):
        return None


KEYED = urllib.request.build_opener(NoRedirect)


def game_code(product):
    """AMKE from CTR-P-AMKE, or None for a product code that isn't a game's"""
    code = (product or '').rsplit('-', 1)[-1].upper()
    return code if len(code) == 4 and code.isalnum() else None


def fetch(url, key=None):
    request = urllib.request.Request(url, headers={'User-Agent': '3DiiU-Builder', **({'Authorization': 'Bearer ' + key} if key else {})})
    with (KEYED.open if key else urllib.request.urlopen)(request, timeout=TIMEOUT) as r:
        return r.read(16 << 20)


def keep(data, stem):
    """the picture at stem plus its kind's ending, or None if the bytes aren't a PNG or a JPEG"""
    ending = next((e for magic, e in PICTURES.items() if data.startswith(magic)), None)
    if not ending:
        return None
    os.makedirs(os.path.dirname(stem), exist_ok=True)
    with open(stem + '.part', 'wb') as f:
        f.write(data)
    os.replace(stem + '.part', stem + ending)
    return stem + ending


def kept(stem):
    return next((stem + e for e in PICTURES.values() if os.path.isfile(stem + e)), None)


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
            data = fetch(URL.format(region=region, game=code))
        except urllib.error.HTTPError:  # no cover under this language
            continue
        except OSError:  # no connection: nothing to wait for
            return None
        if data[:3] == b'\xff\xd8\xff':
            return keep(data, os.path.join(folder, code))
    return None


TDB = 'https://www.gametdb.com/3dstdb.zip'
# where a game was sold, by the product code's last letter, as the Virtual Console's screens name it ("Released in Europe: 2007")
PLACES = {'E': 'North America', 'P': 'Europe', 'J': 'Japan', 'K': 'Korea', 'U': 'Australia', 'D': 'Germany', 'F': 'France', 'I': 'Italy', 'S': 'Spain',
          'W': 'Taiwan', 'C': 'China'}


def tdb_entries(code, folder, url=TDB):
    """GameTDB's list of 3DS games (3dstdb.zip, fetched into folder the first time): the entries of this game in every region, those whose
    code starts with the same three letters (AGGP's: AGGE, AGGJ, AGGP), as (code, element), each element cleared once the next is asked
    for; none if the list can't be had"""
    path = os.path.join(folder, '3dstdb.zip')
    if not os.path.isfile(path):
        try:
            data = fetch(url)
            zipfile.ZipFile(io.BytesIO(data)).close()  # a short or wrong body isn't kept: the next build asks again
            os.makedirs(folder, exist_ok=True)
            with open(path + '.part', 'wb') as f:
                f.write(data)
            os.replace(path + '.part', path)
        except (OSError, http.client.HTTPException, zipfile.BadZipFile):  # no connection, no such file, or cut short: nothing this time
            return
    try:
        with zipfile.ZipFile(path) as z:
            xml = next((n for n in z.namelist() if n.lower().endswith('.xml')), None)
            if not xml:
                return
            for _, e in ElementTree.iterparse(io.BytesIO(z.read(xml))):
                if e.tag != 'game':
                    continue
                gid = (e.findtext('id') or '').strip().upper()
                if gid[:3] == code[:3]:
                    yield gid, e
                e.clear()
    except (OSError, zipfile.BadZipFile, ElementTree.ParseError):
        return


def released(product, folder, url=TDB):
    """'Released in North America: 2011' for the game of this product code, from GameTDB's list of 3DS games, or '' if the list has no year
    for it or can't be had"""
    code = game_code(product)
    if not code:
        return ''
    for gid, e in tdb_entries(code, folder, url):
        if gid.startswith(code):
            year = (e.find('date').get('year') if e.find('date') is not None else '') or ''
            if not year.isdigit():
                return ''
            return f'Released in {PLACES[code[3]]}: {year}' if code[3] in PLACES else f'Released: {year}'
    return ''


def other_names(product, folder, url=TDB):
    """the game's English names in GameTDB's other regions (AGGP's "Luigi's Mansion 2" is North America's "Luigi's Mansion: Dark Moon"),
    North America's first: a site may know a game by another region's name"""
    code = game_code(product)
    if not code:
        return []
    found = [(gid, (e.findtext("locale[@lang='EN']/title") or '').strip()) for gid, e in tdb_entries(code, folder, url) if gid[:4] != code]
    return list(dict.fromkeys(name for _, name in sorted(found, key=lambda f: f[0][3:4] != 'E') if name))


def plain(name):
    """a name as two spellings of it compare: letters and digits only, without accents, case or trademark signs"""
    return ''.join(c for c in unicodedata.normalize('NFKD', name.translate({0x2122: None, 0xAE: None, 0xA9: None})).casefold() if c.isalnum())


def asked(path, key, base):
    """what SteamGridDB answers: its list, empty where it has nothing"""
    try:
        reply = json.loads(fetch(base + path, key))
    except urllib.error.HTTPError as e:
        if e.code in (401, 403):
            raise BadKey from e
        return []
    return (reply.get('data') or []) if isinstance(reply, dict) and reply.get('success') else []


def first(items, sizes):
    """the site lists its best first: the first of the earliest size it has any of, or None"""
    for size in sizes:
        for i in items:
            if (i.get('width'), i.get('height')) == size and i.get('url'):
                return i
    return None


def steamgriddb(name, key, folder, title_id, base=SGDB, also=None):
    """This game's banner and icon from SteamGridDB, as {'banner': path or None, 'icon': path or None}, downloaded into folder if they
    aren't there yet. The game is the one of that name, or of one of its names elsewhere (also: a function giving them, asked only when the
    site has nothing by this one), or else the first whose name holds one of them or is held by one (a subtitle more or less): the site's
    other answers are other games. None without a connection; BadKey if the key is wrong."""
    stems = {what: os.path.join(folder, f'sgdb_{title_id}_{what}') for what in ('banner', 'icon')}
    have = {what: kept(stem) for what, stem in stems.items()}
    if all(have.values()):
        return have
    try:
        answers = []  # (a name of the game, the site's games for it)

        def ask(n):
            """the site's games for this name, kept in answers: whether one has exactly that name"""
            wanted = plain(n)
            games = [(plain(g.get('name', '')), g) for g in asked('/search/autocomplete/' + urllib.parse.quote(n, safe=''), key, base)]
            answers.append((wanted, games))
            return any(found == wanted for found, _ in games)

        if not ask(name) and also:
            for other in also():
                if plain(other) not in (wanted for wanted, _ in answers) and ask(other):
                    break
        game = next((g for wanted, games in answers for found, g in games if found == wanted),
                    next((g for wanted, games in answers for found, g in games if found and (found in wanted or wanted in found)), None))
        if not game:
            return have
        filters = 'types=static&nsfw=false&humor=false&mimes=image/png,image/jpeg'
        wide = first(asked(f'/grids/game/{game["id"]}?dimensions=920x430,460x215&{filters}', key, base), ((920, 430), (460, 215)))
        icons = [i for i in asked(f'/icons/game/{game["id"]}?types=static&nsfw=false&humor=false&mimes=image/png', key, base) if i.get('url')]
        # an icon big enough for the Wii U Menu's 128x128 as the site ranks them, or its largest; a square grid where the game has no icon
        square = next((i for i in icons if i.get('width', 0) >= 128), max(icons, key=lambda i: i.get('width', 0), default=None))
        square = square or first(asked(f'/grids/game/{game["id"]}?dimensions=1024x1024,512x512&{filters}', key, base), ((1024, 1024), (512, 512)))
        for what, item in (('banner', wide), ('icon', square)):
            if item and not have[what]:
                have[what] = keep(fetch(item['url']), stems[what])
    except BadKey:
        raise
    except (OSError, ValueError, KeyError, TypeError):  # no connection, or an answer that isn't what the site documents
        return have if any(have.values()) else None
    return have


def is_case_scan(path, folder):
    """whether path is a GameTDB cover that cover() fetched into folder, not a picture the user picked or SteamGridDB's"""
    return os.path.dirname(os.path.abspath(path)) == os.path.abspath(folder) and not os.path.basename(path).startswith('sgdb_')
