"""A 3DS save packed into a game as it is built: the files of a save, as Checkpoint, JKSM or an emulator keep them, go into the title as
save_import, and the game takes them in on the console once it has made a save of its own (port/wiiu/save_storage.cpp, save_from_content).
No SD card: the save travels inside the game."""
import os
import re
import shutil

ROOM = 8 << 20  # more than any 3DS game's save, and copied while the game starts within the watchdog's time (frontend_wiiu.cpp)


class SaveError(Exception):
    """what to tell the player"""


def junk(name):
    """what Windows and macOS leave in folders: a game that finds a file it doesn't know in its save formats it (Mario Kart 7 in Cemu)"""
    return name.lower() in ('desktop.ini', 'thumbs.db', '.ds_store') or name.startswith('._')


def files_in(folder):
    """(path relative to folder, size) of every file under it but junk"""
    return [(os.path.relpath(os.path.join(d, f), folder), os.path.getsize(os.path.join(d, f))) for d, _, names in os.walk(folder) for f in names
            if not junk(f)]


def the_save(folder):
    """The folder that holds the save's own files, from the one the player chose: an emulator's title folder holds it in data/00000001, and
    Checkpoint's folder for a game holds one dated folder per backup (20261114-050000), the newest taken. Any other is taken as it is."""
    inner = os.path.join(folder, 'data', '00000001')
    if os.path.isdir(inner):
        return inner
    names = [n for n in os.listdir(folder) if not junk(n)]
    if names and all(re.fullmatch(r'\d{8}-\d{6}', n) and os.path.isdir(os.path.join(folder, n)) for n in names):
        return os.path.join(folder, max(names))
    return folder


def checked(chosen):
    """the folder with the save's own files in the one the player chose, and how many there are; SaveError if it can't be a save"""
    source = the_save(chosen)
    found = files_in(source)
    if not found:
        raise SaveError('There are no files in that folder. Choose the folder with the save in it: Checkpoint\'s or JKSM\'s folder for the game, '
                        'or the emulator\'s folder of it.')
    if sum(size for _, size in found) > ROOM:
        raise SaveError('That is bigger than any save of a 3DS game: choose the folder of this game\'s save alone.')
    return source, len(found)


def pack(chosen, content):
    """the save's files into a title's content folder, as save_import: the folder they went to"""
    source, _ = checked(chosen)
    target = os.path.join(content, 'save_import')
    shutil.rmtree(target, ignore_errors=True)
    shutil.copytree(source, target, ignore=lambda _, names: [n for n in names if junk(n)])
    return target
