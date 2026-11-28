#!/usr/bin/env python3
"""Exercise the production Wii U UDP adapter using POSIX host sockets."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile


def main():
    if os.name != 'posix':
        print('SKIP: UDP adapter host test requires POSIX sockets; cross-compilation is checked separately')
        return
    root = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix='3dii-udp-') as temp:
        exe = Path(temp)/'udp-test'
        subprocess.run(shlex.split(os.environ.get('CC', 'cc')) + [
            '-std=gnu11', '-O1', '-D__WIIU__', '-DHAS_FCNTL=1', '-DHAS_SOCKLEN_T=1',
            '-DENET_TEST_WRAP_RECV', '-I'+str(root/'third_party/3ds-link/include'),
            str(root/'tools/tests/test_enet_wiiu_udp.c'),
            str(root/'third_party/3ds-link/enet/wiiu.c'),
            '-Wl,--wrap=recvfrom', '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True, timeout=10)
    print('Wii U UDP adapter: alignment, scatter/gather, truncation and nonblocking checks passed')


if __name__ == '__main__':
    main()
