"""Turn actual shader translator rejections into a reviewable GitHub report.

No network requests, raw shader instructions, generated GLSL, paths from the
user's computer, or console identifiers are put in the issue template.
"""
import csv
import json
from pathlib import Path
from urllib.parse import urlencode

ISSUES = 'https://github.com/J-Glitch2502/3dii-u/issues/new'
STAGES = {'vertex', 'vertex_open_bools', 'geometry_pipeline'}
FEATURES = {'translation_rejected', 'triangle_count_limit'}


def write_report(folder, game_name):
    folder = Path(folder)
    source = folder / 'shader-diagnostics.tsv'
    if not source.exists():
        return None
    rows, seen = [], set()
    with source.open(encoding='utf-8', newline='') as f:
        for row in csv.DictReader(f, delimiter='\t'):
            if row.get('stage') not in STAGES or row.get('feature') not in FEATURES:
                raise ValueError('unrecognized shader diagnostic category')
            clean = {k: int(row[k]) for k in ('entry', 'variant', 'pc', 'translator_line')}
            if any(v < 0 or v > 0xffffffff for v in clean.values()):
                raise ValueError('shader diagnostic number out of range')
            clean['program_id'] = int(row['program_id'])
            if not 0 <= clean['program_id'] <= 0xffffffffffffffff:
                raise ValueError('shader diagnostic identity out of range')
            clean.update(stage=row['stage'], feature=row['feature'])
            key = tuple(sorted(clean.items()))
            if key not in seen:
                seen.add(key)
                rows.append(clean)
    report = {'format': 1, 'scope': 'discovered shader variants only; not complete gameplay coverage',
              'fallback': 'CPU interpreter remains available; these are GPU translation limitations, not proof of a crash',
              'failures': rows}
    target = folder / 'shader-feature-report.json'
    target.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    if not rows:
        (folder / 'report-shader-issue.txt').unlink(missing_ok=True)
        print('No GPU translation rejections among the discovered shader variants. Gameplay coverage is incomplete.', flush=True)
        return target
    # The URL stays short; the full structured report is a separate attachment.
    # Use only the basename of the internal game identifier, never a dump path.
    name = str(game_name).replace('\\', '/').rsplit('/', 1)[-1][:80]
    body = ('Game/build identifier: ' + name + '\n\n'
            f'The Builder found {len(rows)} shader translation limitations. CPU fallback remains available.\n'
            'Attach shader-feature-report.json and add your Builder version, game region/update, '
            'and any visible glitches or slow scenes. Do not attach the game dump.\n\n'
            'This build-time scan covers discovered variants; it cannot certify every gameplay state.\n')
    url = ISSUES + '?' + urlencode({'title': f'Shader translation report: {name}', 'body': body})
    (folder / 'report-shader-issue.txt').write_text(url + '\n\n' + body, encoding='utf-8')
    print(f'Shader translation limitations: {len(rows)}. CPU fallback may be slower. Report: {target}', flush=True)
    print('To report this on GitHub, open the link in report-shader-issue.txt and attach shader-feature-report.json.', flush=True)
    return target
