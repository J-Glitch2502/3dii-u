"""Exercise the real shader generator's own-game/borrowed observation ordering."""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile
from test_fallback_material_analysis import state
from tev_to_log import records
import struct


def check(generator):
    with tempfile.TemporaryDirectory() as directory:
        root=Path(directory);logs=root/'logs';logs.mkdir();out=root/'out';empty=root/'pipelines.bin';empty.touch()
        foreign=state(source=3);own=state(source=1)
        pack=lambda key:struct.pack('<I',len(key))+key
        # Deliberately create the borrowed observation first.
        (logs/'other_borrowed.bin').write_bytes(pack(foreign))
        (logs/'catalog_own.bin').write_bytes(pack(own))
        subprocess.run([str(generator),str(empty),str(logs),str(out),'-'],check=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
        assert records(str(out/'fragment-states.bin'))==[own,foreign]
        assert json.loads((out/'fallback-input-report.json').read_text())=={'preferred_game_states':1,'total_unique_states':2}
        print('PASS: own-game observations precede borrowed inputs without dropping either state')


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('generator',type=Path);check(p.parse_args().generator)
