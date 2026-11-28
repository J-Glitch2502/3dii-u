"""Run the production C++ regression and compare both log tools' normalization with its keys."""
from pathlib import Path
import ast
import struct
import subprocess
import sys
import tempfile
import types

import tev_to_log

# Exercise the production key functions without requiring optional cache decompression on Python < 3.14.
azahar_to_log = types.SimpleNamespace(struct=struct)
source = ast.parse(Path(__file__).with_name('azahar_to_log.py').read_text())
exec(compile(ast.Module(body=[n for n in source.body if isinstance(n, ast.FunctionDef)], type_ignores=[]),
             'azahar_to_log.py', 'exec'), vars(azahar_to_log))


def main():
    with tempfile.TemporaryDirectory(prefix='3dii-fs-normal-') as temp:
        records = Path(temp) / 'keys.bin'
        subprocess.run([sys.argv[1], str(records)], check=True)
        data, pos, cases = records.read_bytes(), 0, 0
        while pos < len(data):
            pair = []
            for _ in range(2):
                size, = struct.unpack_from('<I', data, pos)
                pos += 4
                pair.append(data[pos:pos + size])
                pos += size
            raw, normal = pair
            assert tev_to_log.encode(tev_to_log.normal(tev_to_log.decode(raw))) == normal, f'TEV normalization differs in case {cases}'
            azahar = azahar_to_log.normalize_logged(raw)
            if struct.unpack_from('<I', raw)[0] & 1:
                assert azahar == normal, f'Azahar normalization differs in case {cases}'
            else:
                assert azahar is None  # This importer skips unshaded states.
            cases += 1
        assert pos == len(data) and cases == 16545
        print(f'Python normalization parity: {cases} production C++ keys')
    configs = 0
    for lights in (1, 8):
        for alpha in range(4):
            for shadow in (0, 1):
                keys = []
                for shadow_alpha in (0, 1):
                    config = bytearray(272)
                    struct.pack_into('<I', config, 0, 1 | 1 << 5)  # alpha Always, Z-buffer
                    struct.pack_into('<I', config, 48, 1 | 1 << 16)  # stage 0 reads lit primary colour/alpha
                    for stage in range(1, 6):
                        struct.pack_into('<I', config, 48 + 16 * stage, 15 | 15 << 16)  # passthrough
                    struct.pack_into('<I', config, 168, 1 | lights << 1 | (alpha & 1) << 15 |
                                     (alpha >> 1) << 16 | shadow << 17 | shadow_alpha << 21)
                    key = azahar_to_log.key_from_config(config)
                    assert key is not None and azahar_to_log.normalize_logged(key) == key
                    assert tev_to_log.encode(tev_to_log.normal(tev_to_log.decode(key))) == key
                    keys.append(key)
                    configs += 1
                assert (keys[0] != keys[1]) == bool(shadow), 'only enabled shadow alpha keeps distinct converted keys'
    print(f'Azahar generated-config normalization: {configs} cases')
    spot_configs = 0
    for lights in range(9):
        for mask in range(1 << lights):
            for selector in range(8):
                config = bytearray(272)
                struct.pack_into('<I', config, 0, 1 | 1 << 5)
                struct.pack_into('<I', config, 48, 1 | 1 << 16)
                for stage in range(1, 6):
                    struct.pack_into('<I', config, 48 + 16 * stage, 15 | 15 << 16)
                struct.pack_into('<I', config, 168, 1 | lights << 1)
                struct.pack_into('<I', config, 172 + 2 * 8, 3 | selector << 2)
                for i in range(8):
                    spot = bool(mask & (1 << i)) if i < lights else True
                    struct.pack_into('<H', config, 228 + i * 2, int(spot) << 6)
                key = azahar_to_log.key_from_config(config)
                assert key is not None and azahar_to_log.normalize_logged(key) == key
                normalized = tev_to_log.normal(tev_to_log.decode(key))
                assert tev_to_log.encode(normalized) == key
                assert normalized['lsel'][2] == (3 | selector << 2)
                spot_configs += 1
    assert spot_configs == 4088
    print(f'Azahar active-light spotlight configurations: {spot_configs}')



if __name__ == '__main__':
    main()
