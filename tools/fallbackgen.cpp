// Host-side generation and finalization of the small generic fragment families.
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <vector>
#include <set>
#include "shadergen.h"
#include "latte_program.h"
#include "uber_family.h"

static uint32_t be(const uint8_t *p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
static void put(uint8_t *p, uint32_t x) {
    for (int i = 3; i >= 0; --i) { p[i] = x; x >>= 8; }
}
static uint64_t key(const std::string &s) {
    uint64_t h = 14695981039346656037ull;
    for (unsigned char c : s) h = (h ^ c) * 1099511628211ull;
    return h;
}
static int fit(const char *input, const char *output) {
    std::ifstream f(input, std::ios::binary);
    if (!f) return 1;
    std::vector<uint8_t> d{std::istreambuf_iterator<char>(f), {}};
    if (d.size() < 32 || memcmp(d.data(), "Gfx2", 4)) return 2;
    size_t at = be(d.data() + 4), resource = 0;
    unsigned count = 0;
    bool end = false;
    while (at < d.size()) {
        if (d.size() - at < 32 || memcmp(d.data() + at, "BLK{", 4)) return 3;
        const auto head = be(d.data() + at + 4), kind = be(d.data() + at + 16), bytes = be(d.data() + at + 20);
        if (head < 32 || head > d.size() - at || bytes > d.size() - at - head) return 4;
        if (kind == 6) {
            if (resource || bytes < 4) return 5;
            resource = at + head;
        } else if (kind == 7) {
            if (!resource || bytes % 4) return 6;
            const auto old = be(d.data() + resource);
            auto regs = old & 255;
            if (regs > gpu::kPixelRegisters || !gpu::fit_program(d.data() + at + head, bytes, regs, gpu::kPixelRegisters)) return 7;
            put(d.data() + resource, (old & ~255u) | regs);
            resource = 0;
            ++count;
        } else if (kind == 1) {
            end = true;
            if (at + head + bytes != d.size()) return 8;
        } else return 9;  // This tool finalizes pixel-only packs.
        at += head + bytes;
    }
    if (!count || resource || !end) return 10;
    std::ofstream out(output, std::ios::binary);
    out.write(reinterpret_cast<const char *>(d.data()), d.size());
    if (!out) return 11;
    std::cout << "Fitted " << count << " pixel programs with the runtime's GPU corrections\n";
    return 0;
}
int fallbackgen_main(int argc, char **argv) {
    if (argc == 4 && std::string(argv[1]) == "--fit") return fit(argv[2], argv[3]);
    const bool lighting = argc == 4 && std::string(argv[1]) == "--lighting";
    if (argc != 2 && argc != 3 && !lighting) { std::cerr << "fallbackgen <output-directory> | --fit <input.gsh> <output.gsh>\n"; return 1; }
    const bool full = argc == 3 && std::string(argv[1]) == "--full";
    const std::filesystem::path dir(argv[full || lighting ? 2 : 1]);
    std::filesystem::create_directories(dir);
    std::ofstream manifest(dir / "families.tsv");
    if (full) {
        for (bool lit : {false, true}) for (unsigned mask = 0; mask < 8; ++mask) {
            const auto name = "generic_" + std::to_string(lit) + "_" + std::to_string(mask) + ".frag";
            std::ofstream file(dir / name); file << gpu::gen_uber_fs(lit, mask);
            if (!file) return 20;
            manifest << std::hex << std::setw(16) << std::setfill('0') << key(gpu::uber_family_key(lit, mask)) << '\t' << name << '\n';
        }
        return manifest ? 0 : 21;
    }
    if (argc == 3 || lighting) {
        std::ifstream input(argv[lighting ? 3 : 2], std::ios::binary);
        if (!input) return 14;
        std::set<uint64_t> seen;
        while (input.peek() != EOF) {
            uint8_t prefix[4];
            if (!input.read(reinterpret_cast<char *>(prefix), 4)) return 15;
            const uint32_t n = uint32_t(prefix[0]) | uint32_t(prefix[1]) << 8 | uint32_t(prefix[2]) << 16 | uint32_t(prefix[3]) << 24;
            if (n < 4 || n > gpu::FS_KEY_WORDS * 4 || n % 4) return 16;
            std::string bytes(n, '\0');
            if (!input.read(bytes.data(), n)) return 17;
            const auto state = gpu::decode_key(bytes);
            if (!state.need_shade) continue;
            const auto cap = state.lighting ? gpu::kUberLightCaps[gpu::uber_light_bucket(state.light.num_lights)] : 0;
            const auto hash = lighting ? gpu::uber_lighting_material_key(state, cap) : gpu::uber_material_key(state, cap);
            if (lighting && !state.lighting) continue;
            if (!seen.insert(hash).second) continue;
            const auto source = lighting ? gpu::gen_uber_lighting_material_fs(state, cap) : gpu::gen_uber_material_fs(state, cap);
            if (source.empty()) continue;
            const auto name = "material_" + std::to_string(seen.size()) + ".frag";
            std::ofstream file(dir / name); file << source;
            if (!file) return 18;
            manifest << std::hex << std::setw(16) << std::setfill('0') << hash << '\t' << name << '\n';
        }
        return manifest ? 0 : 19;
    }
    for (bool lit : {false, true}) for (unsigned mask = 0; mask < 8; ++mask)
        for (int stages = 0; stages <= 6; ++stages) for (unsigned bucket = 0; bucket < (lit ? 5u : 1u); ++bucket) {
            const auto cap = gpu::kUberLightCaps[bucket];
            const auto name = "fallback_" + std::to_string(lit) + "_" + std::to_string(mask) + "_" + std::to_string(stages) + "_" + std::to_string(cap) + ".frag";
            std::ofstream file(dir / name);
            file << gpu::gen_uber_fs(lit, mask, stages, cap);
            if (!file) return 12;
            manifest << std::hex << std::setw(16) << std::setfill('0') << key(gpu::uber_family_key(lit, mask, stages, cap)) << '\t' << name << '\n';
        }
    return manifest ? 0 : 13;
}

#ifndef THREEDII_COMBINED_SHADERGEN
int main(int argc, char **argv) { return fallbackgen_main(argc, argv); }
#endif
