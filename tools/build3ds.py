#!/usr/bin/env python3
import os
import runpy

runpy.run_path(os.path.join(os.path.dirname(__file__), 'win', 'build3ds.py'), run_name='__main__')
