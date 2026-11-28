"""Reject mixed or incomplete source trees before shipping a Builder engine."""
from pathlib import Path
import filecmp


def inventory(root, folders):
    root = Path(root)
    result = set()
    for folder in folders:
        base = root / folder
        for path in ([base] if base.is_file() else base.rglob('*')):
            rel = path.relative_to(root)
            if path.is_symlink() or not path.is_file() or '__pycache__' in rel.parts or path.suffix == '.pyc':
                continue
            if rel.parent.as_posix() == 'tools' and path.name.startswith('console_'):
                continue
            result.add(rel.as_posix())
    return result


def verify(source, engine, folders):
    """Require the packaged source set and bytes to match the selected checkout."""
    source, engine = Path(source), Path(engine)
    expected, actual = inventory(source, folders), inventory(engine, folders)
    problems = [('missing', expected - actual), ('unexpected', actual - expected)]
    problems.append(('different', {n for n in expected & actual
                                   if not filecmp.cmp(source / n, engine / n, shallow=False)}))
    bad = [kind + ': ' + name for kind, names in problems for name in sorted(names)]
    if bad:
        raise ValueError('Packaged sources do not match the selected checkout:\n' + '\n'.join(bad))
    return len(expected)
