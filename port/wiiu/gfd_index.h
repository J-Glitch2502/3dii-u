#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

// Validate file/block extents before either our streaming reader or libwhb sees
// the file. All arithmetic is bounded by the remaining bytes, not head + size.
namespace gfd_index {
struct Shader { uint32_t head_at, head_len, prog_at, prog_len; };
struct Index {
    std::vector<Shader> vs, ps;
    uint32_t header_len = 0, end_at = 0, end_len = 0;
};
inline const char *scan(const uint8_t *data, size_t size, Index &out) {
    out = {};
    if (size < 32 || size > UINT32_MAX || std::memcmp(data, "Gfx2", 4)) return "invalid GFD header";
    auto word = [&](size_t at) {
        return uint32_t(data[at]) << 24 | uint32_t(data[at + 1]) << 16 |
               uint32_t(data[at + 2]) << 8 | data[at + 3];
    };
    const uint32_t first = word(4);
    if (first < 32 || first > size) return "invalid GFD header extent";
    out.header_len = first;
    for (size_t at = first; at < size;) {
        if (size - at < 32 || std::memcmp(data + at, "BLK{", 4)) return "invalid GFD block header";
        const uint32_t head = word(at + 4), kind = word(at + 16), payload = word(at + 20);
        if (head < 32 || head > size - at || payload > size - at - head) return "invalid GFD block extent";
        const uint32_t length = head + payload;
        if (kind == 3 || kind == 6) {
            if (!payload) return "empty GFD shader header";
            auto &shaders = kind == 3 ? out.vs : out.ps;
            shaders.push_back({uint32_t(at), length, 0, 0});
        } else if (kind == 5 || kind == 7) {
            auto &shaders = kind == 5 ? out.vs : out.ps;
            if (!payload || shaders.empty() || shaders.back().prog_len) return "unmatched GFD shader program";
            shaders.back().prog_at = uint32_t(at);
            shaders.back().prog_len = length;
        } else if (kind == 1) {
            for (const auto &s : out.vs) if (!s.prog_len) return "missing GFD vertex program";
            for (const auto &s : out.ps) if (!s.prog_len) return "missing GFD pixel program";
            out.end_at = uint32_t(at);
            out.end_len = length;
            return nullptr;  // bytes after END are not consumed by the shader loader
        }
        at += length;
    }
    return "missing GFD end block";
}
inline bool table_fits(size_t bytes, size_t header, size_t entry, uint32_t ps, uint32_t vs) {
    if (bytes < header || !entry) return false;
    const size_t available = (bytes - header) / entry;
    return ps <= available && vs <= available - ps;
}
}  // namespace gfd_index
