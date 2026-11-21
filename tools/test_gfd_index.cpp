#include "gfd_index.h"
#include <cassert>
#include <fstream>
#include <iterator>
#include <iostream>
using Bytes = std::vector<uint8_t>;
void word(Bytes &b, size_t at, uint32_t value) {
    for (int k = 0; k < 4; ++k) b[at + k] = value >> (24 - k * 8);
}
void block(Bytes &b, uint32_t kind, uint32_t payload) {
    const size_t at = b.size(); b.resize(at + 32 + payload);
    std::memcpy(b.data() + at, "BLK{", 4);
    word(b, at + 4, 32); word(b, at + 16, kind); word(b, at + 20, payload);
}
int main(int argc, char **argv) {
    gfd_index::Index out;
    Bytes good(32); std::memcpy(good.data(), "Gfx2", 4); word(good, 4, 32);
    block(good, 3, 4); block(good, 5, 4); block(good, 6, 4); block(good, 7, 4); block(good, 1, 0);
    assert(!gfd_index::scan(good.data(), good.size(), out));
    assert(out.vs.size() == 1 && out.ps.size() == 1 && out.vs[0].prog_len == 36);
    for (size_t n = 0; n < good.size(); ++n) assert(gfd_index::scan(good.data(), n, out));
    for (auto field : {size_t(4), size_t(36), size_t(52)}) {
        for (uint32_t value : {0u, 1u, 31u, 0xffffffefu, 0xffffffffu}) {
            auto bad = good; word(bad, field, value);
            assert(gfd_index::scan(bad.data(), bad.size(), out));
        }
    }
    auto bad = good; word(bad, 32 + 16, 5); assert(gfd_index::scan(bad.data(), bad.size(), out));
    bad = good; word(bad, 68 + 16, 2); assert(gfd_index::scan(bad.data(), bad.size(), out));
    assert(gfd_index::table_fits(64, 32, 16, 1, 1));
    assert(!gfd_index::table_fits(63, 32, 16, 1, 1));
    assert(!gfd_index::table_fits(64, 32, 16, UINT32_MAX, 2));
    assert(!gfd_index::table_fits(64, 32, 16, 1, UINT32_MAX));
    // Mutate all fields under ASan/UBSan; any result must terminate without an invalid access.
    uint32_t seed = 42;
    for (int run = 0; run < 20000; ++run) {
        bad = good;
        for (int j = 0; j < 4; ++j) { seed = seed * 1664525 + 1013904223; bad[seed % bad.size()] ^= seed >> 24; }
        gfd_index::scan(bad.data(), bad.size(), out);
    }
    for (int i = 1; i < argc; ++i) {
        std::ifstream file(argv[i], std::ios::binary);
        if (!file) return 2;
        Bytes data((std::istreambuf_iterator<char>(file)), {});
        if (const char *error = gfd_index::scan(data.data(), data.size(), out)) { std::cerr << argv[i] << ": " << error << '\n'; return 1; }
        std::cout << argv[i] << ": " << out.vs.size() << " VS, " << out.ps.size() << " PS\n";
    }
    std::cout << "GFD validation passed\n";
}
