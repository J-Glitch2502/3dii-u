"""Who 3Dii U Builder is built on: what pipeline.py --credits prints and the window's About shows. The full notices and
licence texts are THIRD_PARTY_LICENSES.md, which the app ships beside itself (make_app.py) and the repository keeps at
its root; keep the two lists in step."""

APP, VERSION = '3Dii U Builder', '1.0'
LICENCE = 'EUPL-1.2'
SOURCE = 'https://github.com/J-Glitch2502/3dii-u'

# (what, by whom, licence, where): the projects whose code is in the app or in the games it makes
UPSTREAM = [
    ('3dsrecomp, the recompiler and runtime 3Dii U is built on', 'Nathan Soufflet (nathsou)', 'EUPL-1.2',
     'https://github.com/nathsou/3dsrecomp'),
    ('devkitPPC (GCC, Binutils, newlib) and wut', 'devkitPro and the wut authors', 'GPL-3.0, BSD-style, Zlib',
     'https://devkitpro.org'),
    ('wut-tools (elf2rpl)', 'devkitPro', 'GPL-2.0-or-later', 'https://github.com/devkitPro/wut-tools'),
    ('CafeGLSL, the Wii U shader compiler', 'Exzap, on Mesa', 'MIT', 'https://github.com/Exzap/CafeGLSL'),
    ('FFmpeg\'s AAC decoder', 'the FFmpeg developers', 'LGPL-2.1-or-later', 'https://ffmpeg.org'),
    ('makerom (Project_CTR)', '3DSGuy, applestash, Jakcron', 'MIT', 'https://github.com/3DSGuy/Project_CTR'),
    ('pyctr', 'Ian Burgwin (ihaveamac)', 'MIT', 'https://github.com/ihaveamac/pyctr'),
    ('PyCryptodome, cryptography', 'Helder Eijs; the PyCA contributors', 'BSD-2-Clause; Apache-2.0 or BSD-3-Clause',
     'https://www.pycryptodome.org'),
    ('Qt and PySide6', 'The Qt Company and contributors', 'LGPL-3.0', 'https://www.qt.io'),
    ('Python', 'Python Software Foundation', 'PSF-2.0', 'https://www.python.org'),
    ('PyInstaller\'s bootloader', 'PyInstaller Development Team', 'GPL-2.0-or-later with its bootloader exception',
     'https://pyinstaller.org'),
    ('woomïnstaller\'s boot movie', 'shinyquagsire23', 'LGPL-2.1', 'https://github.com/shinyquagsire23/woominstaller'),
]

NOT_AFFILIATED = ('3Dii U is not affiliated with, endorsed by or sponsored by Nintendo. It is for games and system files you '
                  'dumped yourself from consoles you own, and for homebrew.')


def text():
    """the credits as plain text, for the command line"""
    lines = [f'{APP} {VERSION}, free and open-source under the {LICENCE}: {SOURCE}', '', 'Built on:']
    lines += [f'  {what}: {who} ({lic}), {url}' for what, who, lic, url in UPSTREAM]
    lines += ['', 'Full notices and licence texts: THIRD_PARTY_LICENSES.md, beside the app and in the source.', '', NOT_AFFILIATED]
    return '\n'.join(lines)


def about():
    """the credits for the About dialog, short enough to fit it: the projects by name; --credits and the file have the rest"""
    return (f'Version {VERSION}. Free and open-source under the {LICENCE}: {SOURCE}\n\n'
            'Built on 3dsrecomp by Nathan Soufflet (nathsou), devkitPro\'s devkitPPC and wut, CafeGLSL by Exzap, FFmpeg, '
            'makerom by 3DSGuy, applestash and Jakcron, pyctr by Ian Burgwin, PyCryptodome, cryptography, Qt and PySide6, '
            'Python, PyInstaller, and woomïnstaller by shinyquagsire23.\n\n'
            'Their notices and full licence texts are in THIRD_PARTY_LICENSES.md, beside the app.\n\n' + NOT_AFFILIATED)
