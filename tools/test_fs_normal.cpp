// Shared offline/runtime fragment normalization: disabled shadows' fields reuse the existing shader.
#include <cstdio>
#include <map>
#include <string>

#include "shadergen.h"

using namespace gpu;

static int failures, cases;
static void expect(bool ok, const char *what) {
    if (!ok) {
        if (failures < 12) std::printf("FAILED: %s\n", what);
        ++failures;
    }
}

int main(int argc, char **argv) {
    if (argc > 2) return 2;
    FILE *records = argc == 2 ? std::fopen(argv[1], "wb") : nullptr;
    if (argc == 2 && !records) return 2;
    for (bool shade : {false, true})
        for (bool lighting : {false, true})
            for (bool shadow : {false, true})
                for (u32 alpha = 0; alpha < 4; ++alpha)
                    for (bool fresnel : {false, true})
                        for (u32 lights : {0u, 1u, 8u}) {
                            DrawState a{};
                            a.need_shade = shade;
                            a.lighting = lighting;
                            a.logic_op = 3;
                            a.light.num_lights = lights;
                            a.light.shadow_enable = shadow;
                            a.light.primary_alpha = alpha & 1;
                            a.light.secondary_alpha = alpha & 2;
                            a.light.fr.enabled = fresnel;
                            a.light.fr.input = 1;
                            a.tev_count = 1;
                            a.tev_list[0] = 0;
                            a.tev[0].csrc[0] = a.tev[0].asrc[0] = 1;
                            a.tev[0].cmul = a.tev[0].amul = 1;
                            DrawState b = a;
                            b.light.shadow_alpha = b.light.shadow_invert = b.light.shadow_primary = b.light.shadow_secondary = true;
                            b.light.shadow_sel = 2;
                            for (u32 i = 0; i < lights; i++) b.light.lights[i].shadow = true;
                            const std::string raw_a = fs_key(a), raw_b = fs_key(b);
                            const DrawState na = fs_normal(a), nb = fs_normal(b);
                            const std::string ka = fs_key(na), kb = fs_key(nb);
                            const std::string sa = gen_fs_glsl(na), sb = gen_fs_glsl(nb);
                            const bool active_lighting = shade && lighting;
                            if (records) {
                                for (const std::string *k : {&raw_b, &kb}) {
                                    const u32 size = (u32)k->size();
                                    if (std::fwrite(&size, sizeof size, 1, records) != 1 || std::fwrite(k->data(), size, 1, records) != 1) return 2;
                                }
                            }

                            if (active_lighting && !shadow) {
                                expect(!nb.light.shadow_alpha && !nb.light.shadow_invert && !nb.light.shadow_primary && !nb.light.shadow_secondary &&
                                           !nb.light.shadow_sel && (lights == 0 || !nb.light.lights[lights - 1].shadow),
                                       "disabled shadows discard every shadow field");
                                expect(ka == kb && sa == sb, "disabled shadow alpha shares the production key and GLSL");
                                // The runtime's normalized lookup must find an already-shipped equivalent shader.
                                const std::map<std::string, u32> shipped = {{ka, 17}};
                                const auto found = shipped.find(fs_key(fs_normal(b)));
                                expect(found != shipped.end() && found->second == 17, "normalized lookup reuses the existing shader");
                            } else {
                                expect(nb.light.shadow_alpha && nb.light.shadow_invert && nb.light.shadow_primary && nb.light.shadow_secondary &&
                                           nb.light.shadow_sel == 2 && (lights == 0 || nb.light.lights[lights - 1].shadow),
                                       "enabled shadows and inactive lighting retain their fields");
                            }
                            if (active_lighting && shadow) {
                                expect(ka != kb, "enabled shadow alpha keeps its distinct key");
                                expect(sa == gen_fs_glsl(a) && sb == gen_fs_glsl(b), "enabled shadows keep generated behavior");
                                if (alpha) expect(sa != sb, "enabled shadow alpha still changes alpha-producing shader code");
                            }
                            if (!active_lighting) expect(ka == kb && sa == sb, "inactive lighting flags do not affect shader selection");
                            for (const DrawState *n : {&na, &nb}) {
                                const DrawState twice = fs_normal(*n);
                                expect(fs_key(twice) == fs_key(*n) && gen_fs_glsl(twice) == gen_fs_glsl(*n), "normalization is idempotent");
                            }
                            expect(fs_key(a) == raw_a && fs_key(b) == raw_b && b.light.shadow_alpha, "normalization leaves caller state unchanged");
                            ++cases;
                        }
    auto emit = [&](const DrawState &d) {
        if (!records) return true;
        const std::string raw = fs_key(d), normal = fs_key(fs_normal(d));
        for (const std::string *key : {&raw, &normal}) {
            const u32 size = (u32)key->size();
            if (std::fwrite(&size, sizeof size, 1, records) != 1 || std::fwrite(key->data(), size, 1, records) != 1) return false;
        }
        return true;
    };
    // Captured Mirai miss 5bc241f058947ff7 and the existing shipped shader-11 state.
    const u32 captured_words[] = {
        0x000006c3, 0x00004001, 0x00000000, 0x00000000, 0x00000002, 0x00000000, 0x00000001,
        0x00000033, 0x000000ee, 0x00001111, 0x00000001, 0x000000f2, 0x000000ff, 0x000000ff,
        0x00001102, 0x00000001, 0x00004001, 0x00000002, 0x0000000a, 0x00000013, 0x00000002,
        0x00000002, 0x00000002, 0x00000002, 0x00000008, 0x00000008, 0x00010101, 0xffffffff,
    };
    const u32 shipped_words[] = {
        0x000006c3, 0x00004001, 0x00000000, 0x00000000, 0x00000002, 0x00000000, 0x00000001,
        0x00000033, 0x000000ee, 0x00001111, 0x00000001, 0x000000f2, 0x000000ff, 0x000000ff,
        0x00001102, 0x00000001, 0x00004001, 0x00000002, 0x0000000a, 0x00000003, 0x00000002,
        0x00000002, 0x00000002, 0x00000002, 0x00000008, 0x00000008, 0x00010101, 0xffffffff,
    };

    const DrawState captured = decode_key(std::string((const char *)captured_words, sizeof captured_words));
    const DrawState shipped = decode_key(std::string((const char *)shipped_words, sizeof shipped_words));
    const std::string raw_capture = fs_key(captured);
    expect(fnv64(raw_capture.data(), raw_capture.size()) == 0x5bc241f058947ff7ull, "fixture is the actual captured miss");
    expect(gen_fs_glsl(fs_normal(captured)) != gen_fs_glsl(fs_normal(shipped)), "live spotlight selectors keep distinct LUT behavior");
    expect(fs_key(fs_normal(captured)) != fs_key(fs_normal(shipped)), "live spotlight selector is retained even when this draw disables every spotlight");
    expect(fs_normal(captured).light.sp.enabled && fs_normal(captured).light.sp.abs, "inactive selector keeps spotlight enable and absolute bits");
    expect(fs_key(captured) == raw_capture, "spotlight normalization leaves caller state unchanged");
    if (!emit(captured)) return 2;
    ++cases;
    for (u32 lights = 0; lights <= 8; lights++)
        for (u32 mask = 0; mask < (1u << lights); mask++)
            for (bool enabled : {false, true})
                for (bool absolute : {false, true})
                    for (u32 input = 0; input < 8; input++) {
                        DrawState d = fs_normal(captured);
                        d.light.num_lights = lights;
                        d.light.sp.enabled = enabled;
                        d.light.sp.abs = absolute;
                        d.light.sp.input = input;
                        for (u32 i = 0; i < 8; i++) d.light.lights[i].spot_atten = i < lights ? bool(mask & (1u << i)) : true;
                        const DrawState normal = fs_normal(d);
                        expect(normal.light.sp.input == (enabled ? input : 0), "live spotlight LUT keeps its selector while enabled");
                        expect(normal.light.sp.enabled == enabled && normal.light.sp.abs == (enabled && absolute), "spotlight enable/abs retain existing normalization");
                        expect(gen_fs_glsl(normal) == gen_fs_glsl(fs_normal(normal)), "spotlight normalization preserves generated behavior on repeated normalization");
                        expect(fs_key(fs_normal(normal)) == fs_key(normal), "spotlight normalization is idempotent");
                        if (!enabled) {
                            DrawState zero = d;
                            zero.light.sp.input = 0;
                            expect(fs_key(normal) == fs_key(fs_normal(zero)), "all inactive selector values share one key");
                        }
                        if (!emit(d)) return 2;
                        ++cases;
                    }
    DrawState active = captured;
    active.light.lights[0].spot_atten = true;
    DrawState active_zero = active;
    active_zero.light.sp.input = 0;
    expect(fs_key(fs_normal(active)) != fs_key(fs_normal(active_zero)), "active spotlight selector keeps distinct keys");
    expect(gen_fs_glsl(active) != gen_fs_glsl(active_zero), "active spotlight selector changes actual GLSL");
    if (records && std::fclose(records)) return 2;
    std::printf("fragment normalization: %d cases, %d failures\n", cases, failures);
    return failures ? 1 : 0;
}
