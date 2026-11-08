#!/usr/bin/env python3
"""Runs the recompiler with the same output as on other systems: text files with plain newlines, not Windows' CRLF, so the C is byte for byte what WSL's run writes.
Run from third_party/3dsrecomp with the arguments of tools.recomp.gen."""
import builtins
import runpy
import sys

plain_open = builtins.open


def lf_open(file, mode='r', buffering=-1, encoding=None, errors=None, newline=None, closefd=True, opener=None):
    if newline is None and 'b' not in mode and any(c in mode for c in 'wax'):
        newline = '\n'
    return plain_open(file, mode, buffering, encoding, errors, newline, closefd, opener)


builtins.open = lf_open
sys.path.insert(0, '.')
sys.argv[0] = 'tools.recomp.gen'
runpy.run_module('tools.recomp.gen', run_name='__main__', alter_sys=True)
