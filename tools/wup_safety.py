"""Keep generated titles and their output folders away from existing console titles and NAND data."""
from contextlib import ExitStack, contextmanager
from pathlib import Path
import hashlib
import os
import re
import shutil
import tempfile

# Installed releases keep these IDs. Native-update-capable games use 0x1D;
# legacy 0x3D titles remain accepted for existing Homebrew/development builds.
LEGACY_IDS = {0x10334453, 0x10334455, 0x10334467, 0x10334477, 0x10334478, 0x10334479,
              0x10334480, 0x10334483, 0x10334484, 0x10334485, 0x10334486}
NAND_DIRS = {'nand', 'slc', 'slccmpt', 'mlc', 'mlc01', 'storage_slc', 'storage_slccmpt',
             'storage_mlc01', 'storage_usb01', 'ticket', 'tickets'}


def check_title_id(title_id):
    low = title_id & 0xffffffff
    if title_id >> 32 not in (0x00050000, 0x0005000e) or (low >> 24 not in (0x1d, 0x3d) and low not in LEGACY_IDS):
        raise ValueError('Refusing a title ID outside 3Dii U: existing console tickets must not be replaced.')


def check_output(path, source=None, replace=False):
    target = Path(path).resolve()
    if any(part.casefold() in NAND_DIRS for part in target.parts):
        raise ValueError('Choose an output folder outside NAND and ticket storage.')
    if source is not None:
        original = Path(source).resolve()
        if target == original or target in original.parents or original in target.parents:
            raise ValueError('The WUP output and source title must be separate folders.')
    if replace and os.path.lexists(path):
        def reparse(entry):
            return entry.is_symlink() or bool(getattr(entry.lstat(), 'st_file_attributes', 0) & 0x400)
        original = Path(path)
        if reparse(original) or not original.is_dir():
            raise ValueError('The existing WUP output must be an ordinary folder.')
        for entry in original.iterdir():
            known = entry.name in ('title.tik', 'title.tmd', 'title.cert') or re.fullmatch(r'[0-9A-Fa-f]{8}\.(app|h3)', entry.name)
            if reparse(entry) or not entry.is_file() or entry.lstat().st_nlink != 1 or not known:
                raise ValueError('Refusing to replace a folder containing unknown files or shared file links.')


def wup_title_id(folder):
    """Read the title identity from a Builder WUP's RSA-2048/SHA-256 TMD."""
    path = Path(folder) / 'title.tmd'
    try:
        with path.open('rb') as f:
            head = f.read(0x204)
            size = os.fstat(f.fileno()).st_size
    except FileNotFoundError as e:
        raise ValueError(f'The WUP in {folder} has no title.tmd.') from e
    count = int.from_bytes(head[0x1DE:0x1E0], 'big')
    if len(head) != 0x204 or head[:4] != b'\0\1\0\4' or not count or size != 0xB04 + count * 0x30:
        raise ValueError(f'The WUP in {folder} has an invalid title.tmd.')
    title_id = int.from_bytes(head[0x18C:0x194], 'big')
    check_title_id(title_id)
    return title_id


def check_title_replacement(target, title_id):
    check_output(target, replace=True)
    if os.path.lexists(target) and os.listdir(target) and wup_title_id(target) != title_id:
        raise ValueError(f'A different Wii U title already uses the folder {target}. Choose another output folder.')


def publication_lock(*targets):
    """Keep publication locks on the PC, shared by the account's Builder processes."""
    from win.objcache import file_locks
    local = Path(os.environ.get('LOCALAPPDATA') or Path.home() / '.local' / 'share')
    locks = local / '3Dii U Builder' / 'publication_locks'
    paths = []
    for target in targets:
        check_output(target, replace=True)
        canonical = Path(os.path.normcase(Path(target).resolve()))
        name = canonical.name.rstrip(' .') if os.name == 'nt' else canonical.name
        key = hashlib.sha256(os.fsencode(canonical.with_name(name))).hexdigest()
        paths.append(locks / f'{key}.lock')
    return file_locks(*paths)


@contextmanager
def staged_outputs(*targets):
    """Stage and verify all replacements before changing any destination."""
    with publication_lock(*targets):
        with stage_locked_outputs(*targets) as new:
            yield new


@contextmanager
def stage_locked_outputs(*targets):
    """Replace outputs whose publication locks the caller holds."""
    swaps = []
    try:
        for target in targets:
            check_output(target, replace=True)
            target = os.path.abspath(target)
            parent = os.path.dirname(target)
            os.makedirs(parent, exist_ok=True)
            stage = tempfile.mkdtemp(prefix='.3diiu-', dir=parent)
            swaps.append((target, os.path.join(stage, 'new'), os.path.join(stage, 'previous')))
        yield [new for _, new, _ in swaps]
        for target, new, _ in swaps:
            check_output(new, replace=True)
            check_title_replacement(target, wup_title_id(new))
        try:
            for target, new, previous in swaps:
                if os.path.lexists(target):
                    print(f'Keeping the previous build at {previous} until replacement finishes.', flush=True)
                    os.rename(target, previous)
                os.rename(new, target)
        except BaseException:
            failed = []
            for target, new, previous in reversed(swaps):
                try:
                    if not os.path.lexists(new) and os.path.lexists(target):
                        os.rename(target, new)
                    if os.path.lexists(previous):
                        os.rename(previous, target)
                except OSError:
                    kept = previous if os.path.lexists(previous) else target
                    failed.append(kept)
            if failed:
                raise OSError(f'The previous build could not be restored. The retained output is safe at {", ".join(failed)}.') from None
            raise
        for _, _, previous in swaps:
            shutil.rmtree(previous, ignore_errors=True)
    finally:
        for _, new, previous in swaps:
            if not os.path.lexists(previous):
                shutil.rmtree(os.path.dirname(new), ignore_errors=True)
