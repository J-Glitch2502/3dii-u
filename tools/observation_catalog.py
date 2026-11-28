"""The portable shader observations required by source and packaged builds.

Keep this outside games/: that folder also holds private dumps and is ignored by
Git. The checked manifest makes an incomplete source archive a build failure.
"""
import hashlib
import json
from pathlib import Path, PurePosixPath
import shutil


def directory(root):
    return Path(root) / 'tools/shader_observations'


def validate(root):
    catalog = directory(root)
    try:
        manifest = json.loads((catalog / 'manifest.json').read_text(encoding='utf-8'))
    except (OSError, ValueError) as exc:
        raise ValueError('Shader observation inventory is missing or invalid; restore tools/shader_observations from this release') from exc
    if manifest.get('version') != 1 or not isinstance(manifest.get('files'), dict) or not manifest['files']:
        raise ValueError('Invalid shader observation inventory')
    for name, expected in manifest['files'].items():
        parts = PurePosixPath(name).parts
        if (len(parts) != 4 or parts[0] != 'games' or parts[2] != 'shader_logs'
                or any(p in ('.', '..') for p in parts) or '\\' in name
                or Path(name).suffix not in {'.bin', '.vsf', '.vsr', '.json'}):
            raise ValueError('Invalid shader observation path: ' + name)
        path = catalog / name
        if (not path.is_file() or any((catalog.joinpath(*parts[:i])).is_symlink() for i in range(1, len(parts) + 1))
                or path.stat().st_size != expected.get('bytes')
                or hashlib.sha256(path.read_bytes()).hexdigest() != expected.get('sha256')):
            raise ValueError('Shader observation missing or changed: ' + name + '; restore the catalog or deliberately update its inventory')
    return manifest


def stage_own(root, slug, logs):
    """Keep shipped data separate from private learned files and predictions."""
    source = directory(root) / 'games' / slug / 'shader_logs'
    destination = Path(logs)
    if not source.is_dir():
        return
    destination.mkdir(parents=True, exist_ok=True)
    for path in sorted(source.iterdir()):
        if path.suffix == '.json':
            # A newer local observed-GLSL catalog takes precedence. Its own
            # generator fingerprint is checked before it is injected.
            target = destination / path.name
            if not target.exists():
                shutil.copyfile(path, target)
            continue
        predicted = path.name in {'confirmed.bin', 'models_fs.bin', 'variants.bin', 'neighborhood_fs.bin'} or path.name.startswith('preserved_')
        prefix = 'preserved_catalog_' if predicted else 'catalog_'
        shutil.copyfile(path, destination / (prefix + path.name))
