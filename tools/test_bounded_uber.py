"""Check exact-stage specialization and non-lossy light-capacity selection."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
cpp = r'''
#include <cassert>
#include <set>
#include "shadergen.h"
#include "uber_family.h"
int main() {
    std::set<std::string> keys;
    for (bool lit : {false, true}) for (unsigned mask = 0; mask < 8; ++mask)
        for (int stages = 0; stages <= 6; ++stages) for (unsigned bucket = 0; bucket < (lit ? 5u : 1u); ++bucket) {
            const auto cap = gpu::kUberLightCaps[bucket];
            const auto source = gpu::gen_uber_fs(lit, mask, stages, cap);
            const auto ordinary = gpu::gen_uber_fs(lit, mask);
            assert(keys.insert(gpu::uber_family_key(lit, mask, stages, cap)).second);
            assert(source.find("void main()") != std::string::npos);
            assert(source.find("if (uUber.y >= 0)") != std::string::npos); // alpha test
            assert(source.find("if (uUber.z != 0)") != std::string::npos); // fog
            assert(source.find("uKonst[6]") != std::string::npos); // same uniform layout
            const auto body = source.substr(source.find("void main()"));
            assert(body.find("if (uUber.x >") == std::string::npos);
            for (int i = 0; i < 6; ++i)
                assert((body.find("stage(" + std::to_string(i) + ");") != std::string::npos) == (i < stages));
            for (unsigned i = 0; i < 8; ++i)
                assert((source.find("if (uLit.x > " + std::to_string(i) + ") light(" + std::to_string(i) + ");") != std::string::npos) == (lit && i < cap));
            if (lit) {
                assert(source.find("uLight[8]") != std::string::npos);
                assert(source.find("if (uLit2.w != 0)") != std::string::npos); // shadow alpha
                assert(source.find("if (bump)") != std::string::npos);
                assert(source.find("if (i == uLit.x - 1 && uSel[3].x != 0)") != std::string::npos); // Fresnel last light
            }
            // Dynamic material/texture arithmetic is unchanged by specialization.
            assert(source.substr(0, source.find("void lighting()")) == ordinary.substr(0, ordinary.find("void lighting()")) || !lit);
        }
    assert(keys.size() == 336);
    for (unsigned lights = 0; lights <= 8; ++lights) {
        const auto bucket = gpu::uber_light_bucket(lights);
        assert(bucket < 5 && gpu::kUberLightCaps[bucket] >= lights);
        if (bucket) assert(gpu::kUberLightCaps[bucket - 1] < lights);
        // Every selected shader executes exactly the real active lights.
        for (unsigned i = 0; i < 8; ++i)
            assert((i < gpu::kUberLightCaps[bucket] && lights > i) == (lights > i));
    }
}
'''
with tempfile.TemporaryDirectory() as directory:
    p = Path(directory)
    (p / 'test.cpp').write_text(cpp)
    subprocess.run(['c++', '-std=c++20', '-O1', '-ffunction-sections', '-fdata-sections',
                    '-I' + str(root / 'port/wiiu'), '-I' + str(root / 'third_party/3dsrecomp/runtime'),
                    str(p / 'test.cpp'), str(root / 'port/wiiu/shadergen.cpp'), '-Wl,--gc-sections', '-o', str(p / 'test')], check=True)
    subprocess.run([str(p / 'test')], check=True)
print('PASS: 336 unique families; exact TEV stages; all 0..8 active lights retained; material feature preservation')
