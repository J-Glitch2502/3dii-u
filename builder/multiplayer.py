"""Shared SD-card room configuration, emitted for every output format."""
import json
from pathlib import Path

DEFAULT_WEB_API = "http://88.198.47.46:5000"


def write_config(sd_root, defaults):
    settings = json.loads(Path(defaults).read_text(encoding='utf-8'))
    host = settings['host']
    port = settings['port']
    if (not isinstance(host, str) or any(c not in
            'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-' for c in host)):
        raise ValueError('Invalid multiplayer room hostname')
    if not isinstance(port, int) or isinstance(port, bool) or not 1 <= port <= 65535:
        raise ValueError('Invalid multiplayer room port')
    target = Path(sd_root) / '3dii-u' / 'multiplayer-room.txt'
    target.parent.mkdir(parents=True, exist_ok=True)
    try:
        with target.open('x', encoding='utf-8', newline='\n') as output:
            output.write(f'enabled=0\nhost={host}\nport={port}\nnickname=auto\nconsole_id=auto\npassword=\nweb_api_url={DEFAULT_WEB_API}\nweb_api_username=\nnetwork_token=\nchat_side=right\n')
    except FileExistsError:
        # Keep custom endpoints and credentials. Repair only the missing/blank
        # directory field, including templates emitted by older builds.
        text = target.read_text(encoding='utf-8')
        lines = text.splitlines(keepends=True)
        found = False
        for i, line in enumerate(lines):
            if line.startswith('web_api_url='):
                found = True
                if not line.split('=', 1)[1].strip():
                    lines[i] = f'web_api_url={DEFAULT_WEB_API}\n'
        if not found:
            if lines and not lines[-1].endswith('\n'): lines[-1] += '\n'
            lines.append(f'web_api_url={DEFAULT_WEB_API}\n')
        updated = ''.join(lines)
        if updated != text: target.write_text(updated, encoding='utf-8', newline='\n')
    return target
